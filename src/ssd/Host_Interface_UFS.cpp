#include "Host_Interface_UFS.h"
#include "../host/Host_Defs.h"
#include "../sim/Engine.h"

//Remaining stubs land in Phase B (plan.md section 7): Day 5 doorbells/arbitration,
//Day 6 command fetch, Day 7 completion path + interrupt aggregation.

//Spec-fidelity guards for the on-the-wire structures (ufs-design.md section 2):
//a padding surprise here would silently skew every link-transfer size
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
		PRINT_ERROR("Input_Stream_Manager_UFS::SQ_tail_pointer_update is not implemented yet (Phase B Day 5)")
	}

	void Input_Stream_Manager_UFS::CQ_head_pointer_update(uint16_t cq_id, uint16_t head_pointer_value)
	{
		PRINT_ERROR("Input_Stream_Manager_UFS::CQ_head_pointer_update is not implemented yet (Phase B Day 5)")
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

	stream_id_type Input_Stream_Manager_UFS::arbitrate_next_submission_queue()
	{
		PRINT_ERROR("Input_Stream_Manager_UFS::arbitrate_next_submission_queue is not implemented yet (Phase B Day 5)")
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
		PRINT_ERROR("Request_Fetch_Unit_UFS::Fetch_next_request is not implemented yet (Phase B Day 6)")
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
		PRINT_ERROR("Request_Fetch_Unit_UFS::Process_pcie_write_message is not implemented yet (Phase B Day 5)")
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
		//ufs-design.md section 8 invariants; the config-parse-level checks land in
		//Phase B Day 8 together with the XML parameters
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
