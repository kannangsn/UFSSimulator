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

		//The host freed CQ slots. Deliver a completion that was blocked on a full ring
		if (cq_context->Completed_user_requests.size() > 0) {
			User_Request* request = cq_context->Completed_user_requests.front();
			cq_context->Completed_user_requests.pop_front();
			inform_host_request_completed(request->Stream_id, request);
			DELETE_REQUEST_UFS(request);
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
		Input_Stream_UFS* stream = (Input_Stream_UFS*)input_streams[request->Stream_id];
		stream->Submission_head_informed_to_host++;
		if (stream->Submission_head_informed_to_host == stream->Submission_queue_size) {
			stream->Submission_head_informed_to_host = 0;//circular ring
		}
		if (request->Type == UserRequestType::READ) {
			stream->Waiting_user_requests.push_back(request);
			stream->STAT_number_of_read_requests++;
			segment_user_request(request);
			((Host_Interface_UFS*)host_interface)->broadcast_user_request_arrival_signal(request);
		} else {//write: the data phase (DATA OUT UPIUs) must arrive before segmentation
			stream->Waiting_user_requests.push_back(request);
			stream->STAT_number_of_write_requests++;
			((Host_Interface_UFS*)host_interface)->request_fetch_unit->Fetch_write_data(request);
		}
	}

	void Input_Stream_Manager_UFS::Handle_arrived_write_data(User_Request* request)
	{
		segment_user_request(request);
		((Host_Interface_UFS*)host_interface)->broadcast_user_request_arrival_signal(request);
	}

	void Input_Stream_Manager_UFS::Handle_serviced_request(User_Request* request)
	{
		stream_id_type stream_id = request->Stream_id;
		Input_Stream_UFS* stream = (Input_Stream_UFS*)input_streams[stream_id];
		stream->Waiting_user_requests.remove(request);
		stream->On_the_fly_requests--;
		total_active_commands--;//MCQConfig.MAC accounting (incremented at fetch issue)

		DEBUG("** UFS Host Interface: Request #" << request->ID << " from stream #" << request->Stream_id << " is finished")

		//Read data goes back to the host as DATA IN UPIU(s) before the completion
		if (request->Type == UserRequestType::READ) {
			((Host_Interface_UFS*)host_interface)->request_fetch_unit->Send_read_data(request);
		}

		//Freed MAC capacity: run the arbitration pump again (the UFS analogue of
		//NVMe's per-queue refetch-on-completion)
		fetch_pending_requests();

		UFS_Completion_Queue_Context* cq_context = find_completion_queue(stream->CQ_id);
		if (iacth == 0 && iatoval == 0) {
			//aggregation disabled: immediate delivery (the NVMe-equivalent baseline)
			deliver_or_block(cq_context, request);
			return;
		}

		//Interrupt aggregation (ufs-design.md section 6): hold the completion, flush
		//when the count threshold is reached, or when the IATOVAL timer fires
		if (cq_context->Pending_completions.empty()) {
			cq_context->Oldest_pending_completion_time = Simulator->Time();
			if (iatoval > 0) {
				Simulator->Register_sim_event(Simulator->Time() + iatoval, (Host_Interface_UFS*)host_interface,
					(void*)(intptr_t)cq_context->CQ_id, 0);
			}
		}
		cq_context->Pending_completions.push_back(request);
		if (iacth > 0 && cq_context->Pending_completions.size() >= iacth) {
			flush_pending_completions(cq_context);
		}
	}

	void Input_Stream_Manager_UFS::Aggregation_timeout(uint16_t cq_id)
	{
		UFS_Completion_Queue_Context* cq_context = find_completion_queue(cq_id);
		//Stale-timer guard: a count-based flush may have emptied the list, and a
		//timer armed by an older arrival must not flush a younger batch early
		if (cq_context->Pending_completions.empty()) {
			return;
		}
		if (Simulator->Time() >= cq_context->Oldest_pending_completion_time + iatoval) {
			flush_pending_completions(cq_context);
		}
	}

	void Input_Stream_Manager_UFS::flush_pending_completions(UFS_Completion_Queue_Context* cq_context)
	{
		while (!cq_context->Pending_completions.empty()) {
			User_Request* request = cq_context->Pending_completions.front();
			cq_context->Pending_completions.pop_front();
			deliver_or_block(cq_context, request);
		}
	}

	void Input_Stream_Manager_UFS::deliver_or_block(UFS_Completion_Queue_Context* cq_context, User_Request* request)
	{
		//One ring slot is always reserved (head == tail means empty, ufs-design.md
		//section 1): the CQ is full when advancing the tail would reach the head
		if ((uint16_t)((cq_context->Completion_tail + 1) % cq_context->Completion_queue_size) == cq_context->Completion_head) {
			cq_context->Completed_user_requests.push_back(request);//delivered when the host frees CQ slots
			return;
		}
		inform_host_request_completed(request->Stream_id, request);
		DELETE_REQUEST_UFS(request);
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
		//Protocol-neutral request segmentation, same logic as the NVMe/SATA path
		Input_Stream_UFS* stream = (Input_Stream_UFS*)input_streams[user_request->Stream_id];
		LHA_type lsa = user_request->Start_LBA;
		unsigned int req_size = user_request->SizeInSectors;

		page_status_type access_status_bitmap = 0;
		unsigned int handled_sectors_count = 0;
		unsigned int transaction_size = 0;
		while (handled_sectors_count < req_size) {
			//Check if LSA is in the correct range allocated to the stream
			if (lsa < stream->Start_logical_sector_address || lsa > stream->End_logical_sector_address) {
				lsa = stream->Start_logical_sector_address + (lsa % (stream->End_logical_sector_address - stream->Start_logical_sector_address));
			}
			LHA_type internal_lsa = lsa - stream->Start_logical_sector_address;//per-flow LSAs are translated into a range starting from zero

			transaction_size = host_interface->sectors_per_page - (unsigned int)(lsa % host_interface->sectors_per_page);
			if (handled_sectors_count + transaction_size >= req_size) {
				transaction_size = req_size - handled_sectors_count;
			}
			LPA_type lpa = internal_lsa / host_interface->sectors_per_page;

			page_status_type temp = ~(0xffffffffffffffff << (int)transaction_size);
			access_status_bitmap = temp << (int)(internal_lsa % host_interface->sectors_per_page);

			if (user_request->Type == UserRequestType::READ) {
				NVM_Transaction_Flash_RD* transaction = new NVM_Transaction_Flash_RD(Transaction_Source_Type::USERIO, user_request->Stream_id,
					transaction_size * SECTOR_SIZE_IN_BYTE, lpa, NO_PPA, user_request, user_request->Priority_class, 0, access_status_bitmap, CurrentTimeStamp);
				user_request->Transaction_list.push_back(transaction);
				stream->STAT_number_of_read_transactions++;
			} else {//UserRequestType::WRITE
				NVM_Transaction_Flash_WR* transaction = new NVM_Transaction_Flash_WR(Transaction_Source_Type::USERIO, user_request->Stream_id,
					transaction_size * SECTOR_SIZE_IN_BYTE, lpa, user_request, user_request->Priority_class, 0, access_status_bitmap, CurrentTimeStamp);
				user_request->Transaction_list.push_back(transaction);
				stream->STAT_number_of_write_transactions++;
			}

			lsa = lsa + transaction_size;
			handled_sectors_count += transaction_size;
		}
	}

	void Input_Stream_Manager_UFS::inform_host_request_completed(stream_id_type stream_id, User_Request* request)
	{
		Input_Stream_UFS* stream = (Input_Stream_UFS*)input_streams[stream_id];
		UFS_Completion_Queue_Context* cq_context = find_completion_queue(stream->CQ_id);
		((Request_Fetch_Unit_UFS*)((Host_Interface_UFS*)host_interface)->request_fetch_unit)->Send_completion_entry(request);
		cq_context->Completion_tail++;//next free slot in the completion queue
		if (cq_context->Completion_tail == cq_context->Completion_queue_size) {
			cq_context->Completion_tail = 0;//circular ring
		}
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

	//Per-UPIU header overhead for data transfers (ufs-design.md sections 2.1, 3
	//step 6): each DATA IN/OUT UPIU carries a 24-byte header, and one UPIU moves at
	//most 64 KB of data segment, so large transfers pay one header per 64 KB chunk
	static unsigned int data_upiu_header_overhead(unsigned int data_size_in_bytes)
	{
		unsigned int upiu_count = (data_size_in_bytes + 65535) / 65536;
		if (upiu_count == 0) {
			upiu_count = 1;
		}
		return 24 * upiu_count;
	}

	void Request_Fetch_Unit_UFS::Fetch_write_data(User_Request* request)
	{
		UFS_DMA_Req_Item* dma_req_item = new UFS_DMA_Req_Item;
		dma_req_item->Type = UFS_DMA_Req_Type::WRITE_DATA;
		dma_req_item->object = (void*)request;
		ufs_dma_list.push_back(dma_req_item);

		//The PRDT points at dummy data addresses (like NVMe's PRPs). DATA OUT UPIU
		//header overhead is added to the modeled transfer size
		host_interface->Send_read_message_to_host(DATA_MEMORY_REGION,
			request->Size_in_byte + data_upiu_header_overhead(request->Size_in_byte));
	}

	void Request_Fetch_Unit_UFS::Send_read_data(User_Request* request)
	{
		//DATA IN UPIU(s): payload plus one 24-byte header per 64 KB data segment
		host_interface->Send_write_message_to_host(DATA_MEMORY_REGION, request->Data,
			request->Size_in_byte + data_upiu_header_overhead(request->Size_in_byte));
	}

	void Request_Fetch_Unit_UFS::Send_completion_entry(User_Request* request)
	{
		//Completion write-back, ufs-design.md section 3 steps 7-8: the Response UPIU
		//(32 B) goes to the UCD response region, then the CQ entry (32 B) to the CQ
		//ring. Two link messages, so both transfers' occupancy is modeled. Only the
		//CQ entry is behaviorally consumed by the host.
		Host_Interface_UFS* hi = (Host_Interface_UFS*)host_interface;
		Input_Stream_UFS* stream = (Input_Stream_UFS*)hi->input_stream_manager->input_streams[request->Stream_id];
		UFS_Completion_Queue_Context* cq_context = ((Input_Stream_Manager_UFS*)hi->input_stream_manager)->find_completion_queue(stream->CQ_id);
		UFS_Command_UPIU* command_upiu = (UFS_Command_UPIU*)request->IO_command_info;

		UFS_Response_UPIU* response_upiu = new UFS_Response_UPIU;
		memset(response_upiu, 0, sizeof(UFS_Response_UPIU));
		response_upiu->Transaction_Type = 0x21;//RESPONSE UPIU
		response_upiu->LUN = command_upiu->LUN;
		response_upiu->Task_Tag = command_upiu->Task_Tag;
		response_upiu->Response = 0;//Target Success
		response_upiu->Status = 0; //SCSI GOOD
		//Response region address: slot chosen by task tag [ABSTRACTION: the actual
		//slot index has no timing content. Only the region kind and 32 B size matter]
		uint64_t response_address = UFS_UCD_MEMORY(stream->SQ_id, command_upiu->Task_Tag % stream->Submission_queue_size) + 0x40;
		host_interface->Send_write_message_to_host(response_address, response_upiu, sizeof(UFS_Response_UPIU));

		UFS_Completion_Queue_Entry* cqe = new UFS_Completion_Queue_Entry;
		memset(cqe, 0, sizeof(UFS_Completion_Queue_Entry));
		cqe->DW0 = (uint32_t)(stream->SQ_id & 0x1F);//SQID. UCDLBA carried as zero. The HBA identifies the command by SQID + Task Tag
		cqe->DW2 = (16 << 16) | 8;//Response UPIU offset/length (DWords), copied from the UTRD convention
		cqe->DW3 = (24 << 16) | 1;//PRDT offset/length
		cqe->DW4 = 0;//OCS = SUCCESS. In MCQ mode the OCS is here, not in UTRD.DW2
		cqe->DW5 = ((uint32_t)command_upiu->LUN << 8) | command_upiu->Task_Tag;
		host_interface->Send_write_message_to_host(
			cq_context->Completion_queue_base_address + (uint64_t)cq_context->Completion_tail * sizeof(UFS_Completion_Queue_Entry),
			cqe, sizeof(UFS_Completion_Queue_Entry));
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
		Host_Interface_UFS* hi = (Host_Interface_UFS*)host_interface;
		UFS_DMA_Req_Item* dma_req_item = ufs_dma_list.front();
		ufs_dma_list.pop_front();

		switch (dma_req_item->Type) {
			case UFS_DMA_Req_Type::UTRD:
			{
				//Step 1 arrived: the UTRD's interpreted content is the UCD pointer.
				//Issue step 2, the UCD read (Command UPIU + PRDT, 48 bytes).
				stream_id_type stream_id = (stream_id_type)(intptr_t)dma_req_item->object;
				UTP_Transfer_Request_Descriptor* utrd = (UTP_Transfer_Request_Descriptor*)payload;
				uint64_t ucd_address = ((uint64_t)utrd->DW5_UCD_Base_Upper << 32) | utrd->DW4_UCD_Base_Lower;
				delete utrd;

				UFS_DMA_Req_Item* ucd_item = new UFS_DMA_Req_Item;
				ucd_item->Type = UFS_DMA_Req_Type::UCD;
				ucd_item->object = (void*)(intptr_t)stream_id;
				ufs_dma_list.push_back(ucd_item);
				host_interface->Send_read_message_to_host(ucd_address, sizeof(UFS_Command_UPIU) + sizeof(UFS_PRDT_Entry));
				break;
			}
			case UFS_DMA_Req_Type::UCD:
			{
				//Step 2 arrived: parse the Command UPIU into a User_Request
				User_Request* new_request = new User_Request;
				new_request->IO_command_info = payload;//freed via DELETE_REQUEST_UFS
				new_request->Stream_id = (stream_id_type)(intptr_t)dma_req_item->object;
				new_request->Priority_class = ((Input_Stream_Manager_UFS*)host_interface->input_stream_manager)->Get_priority_class(new_request->Stream_id);
				new_request->STAT_InitiationTime = Simulator->Time();
				UFS_Command_UPIU* upiu = (UFS_Command_UPIU*)payload;
				//CDB: READ(16)/WRITE(16). 8-byte big-endian LBA at bytes 2-9,
				//4-byte big-endian transfer length (in sectors) at bytes 10-13
				LHA_type lba = 0;
				for (int i = 0; i < 8; i++) {
					lba = (lba << 8) | upiu->CDB[2 + i];
				}
				uint32_t lba_count = 0;
				for (int i = 0; i < 4; i++) {
					lba_count = (lba_count << 8) | upiu->CDB[10 + i];
				}
				switch (upiu->CDB[0]) {
					case UFS_READ_OPCODE:
						new_request->Type = UserRequestType::READ;
						break;
					case UFS_WRITE_OPCODE:
						new_request->Type = UserRequestType::WRITE;
						break;
					default:
						throw std::invalid_argument("UFS command is not supported!");
				}
				new_request->Start_LBA = lba;
				new_request->SizeInSectors = lba_count;
				new_request->Size_in_byte = new_request->SizeInSectors * SECTOR_SIZE_IN_BYTE;
				((Input_Stream_Manager_UFS*)(hi->input_stream_manager))->Handle_new_arrived_request(new_request);
				break;
			}
			case UFS_DMA_Req_Type::WRITE_DATA:
				COPYDATA(((User_Request*)dma_req_item->object)->Data, payload, payload_size);
				((Input_Stream_Manager_UFS*)(hi->input_stream_manager))->Handle_arrived_write_data((User_Request*)dma_req_item->object);
				break;
			default:
				break;
		}
		delete dma_req_item;
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
		//The only device-side events are per-CQ IATOVAL aggregation timers,
		//registered in Handle_serviced_request with the CQ id as the parameter
		((Input_Stream_Manager_UFS*)input_stream_manager)->Aggregation_timeout((uint16_t)(intptr_t)event->Parameters);
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
