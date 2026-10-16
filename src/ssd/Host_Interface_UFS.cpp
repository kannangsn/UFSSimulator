#include <stdexcept>
#include "Host_Interface_UFS.h"
#include "../host/Host_Defs.h"
#include "../sim/Engine.h"

//Remaining stubs land in Phase B (plan.md section 7): Day 5 doorbells/arbitration,
//Day 6 command fetch, Day 7 completion path + interrupt aggregation.

//Spec-fidelity guards for the structures carried over the link (ufs-design.md
//section 2). A padding surprise here would silently skew every link-transfer size.
static_assert(sizeof(UTP_Transfer_Request_Descriptor) == 32, "UTRD must be 8 DWORDs (JESD223F 6.1.1)");
static_assert(sizeof(UFS_Completion_Queue_Entry) == 32, "MCQ CQ entry must be 8 DWORDs (JESD223F 6.2.2)");
static_assert(sizeof(UFS_Command_UPIU) == 32, "Command UPIU must be 32 bytes (JESD220G Table 10.13)");
static_assert(sizeof(UFS_Response_UPIU) == 32, "Response UPIU must be 32 bytes (JESD220G 10.7.2)");
static_assert(sizeof(UFS_PRDT_Entry) == 16, "PRDT entry must be 4 DWORDs (JESD223F 6.1)");

namespace SSD_Components
{
	Input_Stream_UFS::~Input_Stream_UFS()
	{
	}

	Input_Stream_Manager_UFS::Input_Stream_Manager_UFS(Host_Interface_Base* host_interface, uint16_t queue_fetch_size,
		UFS_Arbitration_Type arbitration, uint16_t max_active_commands,
		uint16_t completion_queue_count, uint8_t iacth, sim_time_type iatoval) :
		Input_Stream_Manager_Base(host_interface),
		Queue_fetch_size(queue_fetch_size), arbitration(arbitration),
		max_active_commands(max_active_commands), total_active_commands(0),
		rr_arbitration_pointer(0), iacth(iacth), iatoval(iatoval)
	{
		//completion_queues are created lazily in Create_new_stream, one context per
		//distinct CQ id (completion_queue_count of them in total)
		(void)completion_queue_count;
	}

	Input_Stream_Manager_UFS::~Input_Stream_Manager_UFS()
	{
		for (auto &cq : completion_queues) {
			delete cq;
		}
	}

	stream_id_type Input_Stream_Manager_UFS::Create_new_stream(IO_Flow_Priority_Class::Priority priority_class,
		LHA_type start_logical_sector_address, LHA_type end_logical_sector_address,
		uint64_t submission_queue_base_address, uint16_t submission_queue_size,
		uint64_t completion_queue_base_address, uint16_t completion_queue_size)
	{
		if (end_logical_sector_address < start_logical_sector_address) {
			PRINT_ERROR("Error in allocating address range to a stream in host interface: the start address should be smaller than the end address.")
		}

		//SQ id = stream id = flow id (ufs-design.md section 1.1: no admin-queue offset)
		uint16_t sq_id = (uint16_t)input_streams.size();
		if (sq_id >= 32) {
			PRINT_ERROR("UFS host interface: at most 32 submission queues are supported (MCQCAP.MAXQ)")
		}

		//Find-or-create the CQ context: several SQs may share one CQ (N:1 mapping,
		//ufs-design.md sections 1.2/6), so contexts are keyed by the CQ id decoded
		//from the ring's base address
		uint16_t cq_id = UFS_MEMORY_QUEUE_ID(completion_queue_base_address);
		UFS_Completion_Queue_Context* cq_context = NULL;
		for (auto &cq : completion_queues) {
			if (cq->CQ_id == cq_id) {
				cq_context = cq;
				break;
			}
		}
		if (cq_context == NULL) {
			cq_context = new UFS_Completion_Queue_Context(cq_id, completion_queue_base_address, completion_queue_size);
			completion_queues.push_back(cq_context);
		}

		Input_Stream_UFS* input_stream = new Input_Stream_UFS(priority_class,
			start_logical_sector_address, end_logical_sector_address,
			sq_id, cq_id, submission_queue_base_address, submission_queue_size);
		this->input_streams.push_back(input_stream);

		return (stream_id_type)(this->input_streams.size() - 1);
	}

	void Input_Stream_Manager_UFS::SQ_tail_pointer_update(stream_id_type stream_id, uint16_t tail_pointer_value)
	{
		if (stream_id >= input_streams.size()) {
			PRINT_ERROR("UFS host interface: SQ tail doorbell rung for an unknown submission queue!")
		}
		((Input_Stream_UFS*)input_streams[stream_id])->Submission_tail = tail_pointer_value;
		fetch_pending_requests();
	}

	void Input_Stream_Manager_UFS::CQ_head_pointer_update(uint16_t cq_id, uint16_t head_pointer_value)
	{
		UFS_Completion_Queue_Context* cq_context = find_completion_queue(cq_id);
		cq_context->Completion_head = head_pointer_value;

		//The host freed CQ slots. Deliver a completion that was blocked on a full
		//ring, if any (filled by Handle_serviced_request, Phase B Day 7)
		if (cq_context->Completed_user_requests.size() > 0) {
			User_Request* request = cq_context->Completed_user_requests.front();
			cq_context->Completed_user_requests.pop_front();
			inform_host_request_completed(request->Stream_id, request);
		}
	}

	UFS_Completion_Queue_Context* Input_Stream_Manager_UFS::find_completion_queue(uint16_t cq_id)
	{
		for (auto &cq : completion_queues) {
			if (cq->CQ_id == cq_id) {
				return cq;
			}
		}
		PRINT_ERROR("UFS host interface: reference to an unknown completion queue!")
	}

	stream_id_type Input_Stream_Manager_UFS::arbitrate_next_submission_queue()
	{
		//ufs-design.md section 4. Scanning starts one past the RR pointer so
		//consecutive decisions rotate. In strict-priority mode the scan keeps the
		//first-found queue of the lowest SQPL value (strictly-less comparison), which
		//realizes round-robin among queues of equal priority.
		uint16_t stream_count = (uint16_t)input_streams.size();
		stream_id_type winner = UFS_NO_ELIGIBLE_SQ;
		uint8_t winner_sqpl = 0xFF;
		for (uint16_t i = 1; i <= stream_count; i++) {
			stream_id_type candidate = (stream_id_type)((rr_arbitration_pointer + i) % stream_count);
			Input_Stream_UFS* stream = (Input_Stream_UFS*)input_streams[candidate];
			if (stream->Submission_head == stream->Submission_tail) {
				continue;//nothing new to fetch (head == tail means empty, ufs-design.md section 1)
			}
			if (stream->On_the_fly_requests >= Queue_fetch_size) {
				continue;//per-queue fetch window exhausted
			}
			if (arbitration == UFS_Arbitration_Type::ROUND_ROBIN) {
				return candidate;
			}
			if (stream->SQPL < winner_sqpl) {
				winner = candidate;
				winner_sqpl = stream->SQPL;
			}
		}
		return winner;
	}

	void Input_Stream_Manager_UFS::fetch_pending_requests()
	{
		//MCQConfig.MAC: device-wide cap on outstanding commands, counted from fetch
		//issue to completion (decremented in Handle_serviced_request, Phase B Day 7)
		while (total_active_commands < max_active_commands) {
			stream_id_type stream_id = arbitrate_next_submission_queue();
			if (stream_id == UFS_NO_ELIGIBLE_SQ) {
				return;
			}
			Input_Stream_UFS* stream = (Input_Stream_UFS*)input_streams[stream_id];
			//the fetch reads the UTRD at the current head, so issue before advancing
			((Host_Interface_UFS*)host_interface)->request_fetch_unit->Fetch_next_request(stream_id);
			stream->On_the_fly_requests++;
			total_active_commands++;
			stream->Submission_head++;
			if (stream->Submission_head == stream->Submission_queue_size) {
				stream->Submission_head = 0;//circular ring
			}
			rr_arbitration_pointer = stream_id;
		}
	}

	void Input_Stream_Manager_UFS::Handle_new_arrived_request(User_Request* request)
	{
		PRINT_ERROR("Input_Stream_Manager_UFS::Handle_new_arrived_request is not implemented yet (Phase B Day 6)")
	}

	void Input_Stream_Manager_UFS::Handle_arrived_write_data(User_Request* request)
	{
		PRINT_ERROR("Input_Stream_Manager_UFS::Handle_arrived_write_data is not implemented yet (Phase B Day 6)")
	}

	void Input_Stream_Manager_UFS::Handle_serviced_request(User_Request* request)
	{
		PRINT_ERROR("Input_Stream_Manager_UFS::Handle_serviced_request is not implemented yet (Phase B Day 7)")
	}

	uint16_t Input_Stream_Manager_UFS::Get_submission_queue_depth(stream_id_type stream_id)
	{
		return ((Input_Stream_UFS*)input_streams[stream_id])->Submission_queue_size;
	}

	uint16_t Input_Stream_Manager_UFS::Get_completion_queue_context_count()
	{
		return (uint16_t)completion_queues.size();
	}

	IO_Flow_Priority_Class::Priority Input_Stream_Manager_UFS::Get_priority_class(stream_id_type stream_id)
	{
		return ((Input_Stream_UFS*)input_streams[stream_id])->Priority_class;
	}

	void Input_Stream_Manager_UFS::segment_user_request(User_Request* user_request)
	{
		PRINT_ERROR("Input_Stream_Manager_UFS::segment_user_request is not implemented yet (Phase B Day 6)")
	}

	void Input_Stream_Manager_UFS::inform_host_request_completed(stream_id_type stream_id, User_Request* request)
	{
		PRINT_ERROR("Input_Stream_Manager_UFS::inform_host_request_completed is not implemented yet (Phase B Day 7)")
	}

	Request_Fetch_Unit_UFS::Request_Fetch_Unit_UFS(Host_Interface_Base* host_interface) :
		Request_Fetch_Unit_Base(host_interface)
	{
	}

	Request_Fetch_Unit_UFS::~Request_Fetch_Unit_UFS()
	{
		for (auto &item : ufs_dma_list) {
			delete item;
		}
	}

	void Request_Fetch_Unit_UFS::Fetch_next_request(stream_id_type stream_id)
	{
		//DMA step 1 of the two-step fetch (ufs-design.md section 3): read the 32-byte
		//UTRD at the SQ head. Step 2 (the UCD read) is issued when this one arrives
		//in Process_pcie_read_message (Phase B Day 6).
		UFS_DMA_Req_Item* dma_req_item = new UFS_DMA_Req_Item;
		dma_req_item->Type = UFS_DMA_Req_Type::UTRD;
		dma_req_item->object = (void*)(intptr_t)stream_id;
		ufs_dma_list.push_back(dma_req_item);

		Host_Interface_UFS* hi = (Host_Interface_UFS*)host_interface;
		Input_Stream_UFS* im = (Input_Stream_UFS*)hi->input_stream_manager->input_streams[stream_id];
		host_interface->Send_read_message_to_host(im->Submission_queue_base_address + im->Submission_head * sizeof(UTP_Transfer_Request_Descriptor),
			sizeof(UTP_Transfer_Request_Descriptor));
	}

	void Request_Fetch_Unit_UFS::Fetch_write_data(User_Request* request)
	{
		PRINT_ERROR("Request_Fetch_Unit_UFS::Fetch_write_data is not implemented yet (Phase B Day 6)")
	}

	void Request_Fetch_Unit_UFS::Send_read_data(User_Request* request)
	{
		PRINT_ERROR("Request_Fetch_Unit_UFS::Send_read_data is not implemented yet (Phase B Day 6)")
	}

	void Request_Fetch_Unit_UFS::Send_completion_entry(User_Request* request)
	{
		PRINT_ERROR("Request_Fetch_Unit_UFS::Send_completion_entry is not implemented yet (Phase B Day 7)")
	}

	void Request_Fetch_Unit_UFS::Process_pcie_write_message(uint64_t address, void* payload, unsigned int payload_size)
	{
		//Doorbell decode on the flat UFS register block (ufs-design.md section 1.1):
		//queue id from the 0x10 stride, SQ tail at +0x0, CQ head at +0x8
		Host_Interface_UFS* hi = (Host_Interface_UFS*)host_interface;
		uint64_t val = (uint64_t)payload;
		if (address < UFS_DOORBELL_BLOCK_BASE || address >= UFS_DOORBELL_BLOCK_BASE + 32 * UFS_DOORBELL_QUEUE_STRIDE) {
			throw std::invalid_argument("Unknown register is written!");
		}
		uint64_t offset = address - UFS_DOORBELL_BLOCK_BASE;
		uint16_t queue_id = (uint16_t)(offset / UFS_DOORBELL_QUEUE_STRIDE);
		switch (offset % UFS_DOORBELL_QUEUE_STRIDE) {
			case 0x0:
				((Input_Stream_Manager_UFS*)(hi->input_stream_manager))->SQ_tail_pointer_update(queue_id, (uint16_t)val);
				break;
			case 0x8:
				((Input_Stream_Manager_UFS*)(hi->input_stream_manager))->CQ_head_pointer_update(queue_id, (uint16_t)val);
				break;
			default:
				throw std::invalid_argument("Unknown UFS doorbell register offset is written!");
		}
	}

	void Request_Fetch_Unit_UFS::Process_pcie_read_message(uint64_t address, void* payload, unsigned int payload_size)
	{
		PRINT_ERROR("Request_Fetch_Unit_UFS::Process_pcie_read_message is not implemented yet (Phase B Day 6)")
	}

	Host_Interface_UFS::Host_Interface_UFS(const sim_object_id_type& id, LHA_type max_logical_sector_address,
		uint16_t queue_size, uint16_t completion_queue_count,
		uint16_t max_active_commands, UFS_Arbitration_Type arbitration,
		uint8_t iacth, sim_time_type iatoval,
		unsigned int no_of_input_streams, uint16_t queue_fetch_size,
		unsigned int sectors_per_page, Data_Cache_Manager_Base* cache) :
		Host_Interface_Base(id, HostInterface_Types::UFS, max_logical_sector_address, sectors_per_page, cache),
		queue_size(queue_size), completion_queue_count(completion_queue_count),
		max_active_commands(max_active_commands), arbitration(arbitration),
		iacth(iacth), iatoval(iatoval), no_of_input_streams(no_of_input_streams)
	{
		this->input_stream_manager = new Input_Stream_Manager_UFS(this, queue_fetch_size, arbitration,
			max_active_commands, completion_queue_count, iacth, iatoval);
		this->request_fetch_unit = new Request_Fetch_Unit_UFS(this);
	}

	stream_id_type Host_Interface_UFS::Create_new_stream(IO_Flow_Priority_Class::Priority priority_class,
		LHA_type start_logical_sector_address, LHA_type end_logical_sector_address,
		uint64_t submission_queue_base_address, uint64_t completion_queue_base_address)
	{
		return ((Input_Stream_Manager_UFS*)input_stream_manager)->Create_new_stream(priority_class,
			start_logical_sector_address, end_logical_sector_address,
			submission_queue_base_address, queue_size, completion_queue_base_address, queue_size);
	}

	void Host_Interface_UFS::Start_simulation()
	{
	}

	void Host_Interface_UFS::Validate_simulation_config()
	{
		//ufs-design.md section 8 validity conditions. The config-parse-level checks land
		//in Phase B Day 8 together with the XML parameters
		if (this->input_stream_manager == NULL) {
			PRINT_ERROR("UFS host interface bad configuration: no input stream manager is set!")
		}
		if (this->request_fetch_unit == NULL) {
			PRINT_ERROR("UFS host interface bad configuration: no request fetch unit is set!")
		}
		if (no_of_input_streams < 1 || no_of_input_streams > 32) {
			PRINT_ERROR("UFS host interface: the number of input streams (SQs) must be between 1 and 32 (MCQCAP.MAXQ)")
		}
		if (completion_queue_count < 1 || completion_queue_count > no_of_input_streams) {
			PRINT_ERROR("UFS host interface: the CQ count must be between 1 and the SQ count")
		}
		if (queue_size < 2 || queue_size > 8192) {
			PRINT_ERROR("UFS host interface: queue size must be between 2 and 8192 (SQATTRy.SIZE), one ring slot is always reserved")
		}
		if (max_active_commands < 1) {
			PRINT_ERROR("UFS host interface: MaxActiveCommands (MCQConfig.MAC) must be at least 1")
		}
	}

	void Host_Interface_UFS::Execute_simulator_event(MQSimEngine::Sim_Event* event)
	{
	}

	uint16_t Host_Interface_UFS::Get_queue_depth()
	{
		return queue_size;
	}

	uint16_t Host_Interface_UFS::Get_completion_queue_count()
	{
		return completion_queue_count;
	}

	uint16_t Host_Interface_UFS::Get_completion_queue_context_count()
	{
		return ((Input_Stream_Manager_UFS*)input_stream_manager)->Get_completion_queue_context_count();
	}

	void Host_Interface_UFS::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter)
	{
	}
}
