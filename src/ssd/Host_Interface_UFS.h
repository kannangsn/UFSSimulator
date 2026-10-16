#ifndef HOST_INTERFACE_UFS_H
#define HOST_INTERFACE_UFS_H

#include <vector>
#include <list>
#include <set>
#include "../sim/Sim_Event.h"
#include "Host_Interface_Base.h"
#include "User_Request.h"
#include "Host_Interface_Defs.h"

//UFS host interface, MCQ mode only (ufs-design.md, plan.md section 5.1: no legacy
//single-doorbell mode is built). Structure follows Host_Interface_NVMe, with the
//UFS-specific differences called out inline.

namespace SSD_Components
{
//Sentinel returned by SQ arbitration when no queue is eligible to fetch from
#define UFS_NO_ELIGIBLE_SQ ((stream_id_type)0xFFFF)

//UFS analogue of DELETE_REQUEST_NVME. The NVMe macro deletes IO_command_info
//through a Submission_Queue_Entry* cast. For UFS, IO_command_info holds the
//fetched UFS_Command_UPIU (ufs-design.md section 3 step 4) and must be deleted
//through that type.
#define DELETE_REQUEST_UFS(REQ) \
	delete (UFS_Command_UPIU*)REQ->IO_command_info; \
	if(Simulator->Is_integrated_execution_mode())\
		{if(REQ->Data != NULL) delete[] (char*)REQ->Data;} \
	if(REQ->Transaction_list.size() != 0) PRINT_ERROR("Deleting an unhandled user request in the host interface! MQSim thinks something is going wrong!")\
	delete REQ;

class Input_Stream_UFS : public Input_Stream_Base
{
public:
	Input_Stream_UFS(IO_Flow_Priority_Class::Priority priority_class,
					 LHA_type start_logical_sector_address, LHA_type end_logical_sector_address,
					 uint16_t sq_id, uint16_t cq_id,
					 uint64_t submission_queue_base_address, uint16_t submission_queue_size) : Input_Stream_Base(),
		Priority_class(priority_class),
		SQPL(priority_class == IO_Flow_Priority_Class::UNDEFINED ? 7 : (uint8_t)priority_class),//ufs-design.md section 4
		Start_logical_sector_address(start_logical_sector_address), End_logical_sector_address(end_logical_sector_address),
		SQ_id(sq_id), CQ_id(cq_id),
		Submission_queue_base_address(submission_queue_base_address), Submission_queue_size(submission_queue_size),
		Submission_head(0), Submission_head_informed_to_host(0), Submission_tail(0), On_the_fly_requests(0) {}
	~Input_Stream_UFS();
	IO_Flow_Priority_Class::Priority Priority_class;
	uint8_t SQPL;//flat 8-level strict priority (SQATTRy.SQPL), 0 = highest
	LHA_type Start_logical_sector_address;
	LHA_type End_logical_sector_address;
	uint16_t SQ_id; //= stream id = flow id (no admin-queue offset, unlike NVMe)
	uint16_t CQ_id; //SQ -> CQ mapping for N:1 topology (ufs-design.md section 5)
	uint64_t Submission_queue_base_address;
	uint16_t Submission_queue_size;
	uint16_t Submission_head;
	uint16_t Submission_head_informed_to_host;
	uint16_t Submission_tail;
	std::list<User_Request*> Waiting_user_requests;
	std::list<User_Request*> Waiting_write_data_transfers;
	uint16_t On_the_fly_requests;
};

//Completion-queue state is separate from the per-stream state because UFS allows
//N:1 SQ->CQ mapping (ufs-design.md sections 1.2, 6). Also carries the per-CQ
//interrupt-aggregation context (IACTH/IATOVAL) implemented in Phase B Day 7.
class UFS_Completion_Queue_Context
{
public:
	UFS_Completion_Queue_Context(uint16_t cq_id, uint64_t completion_queue_base_address, uint16_t completion_queue_size) :
		CQ_id(cq_id),
		Completion_queue_base_address(completion_queue_base_address), Completion_queue_size(completion_queue_size),
		Completion_head(0), Completion_tail(0),
		Aggregation_counter(0), Oldest_pending_completion_time(0) {}
	uint16_t CQ_id;
	uint64_t Completion_queue_base_address;
	uint16_t Completion_queue_size;
	uint16_t Completion_head;
	uint16_t Completion_tail;
	uint8_t Aggregation_counter;
	sim_time_type Oldest_pending_completion_time;
	std::list<User_Request*> Pending_completions;   //completed, gated behind IACTH/IATOVAL
	std::list<User_Request*> Completed_user_requests;//delivered but CQ ring full, waiting for head update
};

class Input_Stream_Manager_UFS : public Input_Stream_Manager_Base
{
	friend class Request_Fetch_Unit_UFS;//Send_completion_entry resolves the CQ context
public:
	Input_Stream_Manager_UFS(Host_Interface_Base* host_interface, uint16_t queue_fetch_size,
							 UFS_Arbitration_Type arbitration, uint16_t max_active_commands,
							 uint16_t completion_queue_count, uint8_t iacth, sim_time_type iatoval);
	~Input_Stream_Manager_UFS();
	stream_id_type Create_new_stream(IO_Flow_Priority_Class::Priority priority_class,
									 LHA_type start_logical_sector_address, LHA_type end_logical_sector_address,
									 uint64_t submission_queue_base_address, uint16_t submission_queue_size,
									 uint64_t completion_queue_base_address, uint16_t completion_queue_size);
	void SQ_tail_pointer_update(stream_id_type stream_id, uint16_t tail_pointer_value);
	void CQ_head_pointer_update(uint16_t cq_id, uint16_t head_pointer_value);
	void Handle_new_arrived_request(User_Request* request);
	void Handle_arrived_write_data(User_Request* request);
	void Handle_serviced_request(User_Request* request);
	//Fired by Host_Interface_UFS::Execute_simulator_event when a per-CQ IATOVAL
	//aggregation timer expires (ufs-design.md section 6)
	void Aggregation_timeout(uint16_t cq_id);
	uint16_t Get_submission_queue_depth(stream_id_type stream_id);
	uint16_t Get_completion_queue_context_count();//distinct CQ contexts created so far (N:1 sharing collapses several SQs onto one)
	IO_Flow_Priority_Class::Priority Get_priority_class(stream_id_type stream_id);

private:
	void segment_user_request(User_Request* user_request);
	void inform_host_request_completed(stream_id_type stream_id, User_Request* request);
	//Arbitration across nonempty SQs: RR pointer, or lowest-SQPL-first with RR among
	//equals (ufs-design.md section 4). Returns UFS_NO_ELIGIBLE_SQ when nothing to fetch.
	stream_id_type arbitrate_next_submission_queue();
	//The fetch pump: issues UTRD fetches, one arbitration decision at a time, while
	//the device-wide MAC cap and the per-queue fetch window allow
	void fetch_pending_requests();
	UFS_Completion_Queue_Context* find_completion_queue(uint16_t cq_id);
	//Deliver a completion to the host (Response UPIU + CQ entry) or park it on the
	//blocked list when the CQ ring is full. Frees the device-side request on deliver
	void deliver_or_block(UFS_Completion_Queue_Context* cq_context, User_Request* request);
	//Flush the aggregation-held completions of one CQ (ufs-design.md section 6)
	void flush_pending_completions(UFS_Completion_Queue_Context* cq_context);

	uint16_t Queue_fetch_size;
	UFS_Arbitration_Type arbitration;
	uint16_t max_active_commands;   //MCQConfig.MAC analogue: device-wide outstanding cap
	uint16_t total_active_commands;
	stream_id_type rr_arbitration_pointer;
	uint8_t iacth;                  //aggregation count threshold, 0 = disabled
	sim_time_type iatoval;          //aggregation timeout (ns internally), 0 = disabled
	std::vector<UFS_Completion_Queue_Context*> completion_queues;
};

class Request_Fetch_Unit_UFS : public Request_Fetch_Unit_Base
{
public:
	Request_Fetch_Unit_UFS(Host_Interface_Base* host_interface);
	~Request_Fetch_Unit_UFS();
	void Fetch_next_request(stream_id_type stream_id);
	void Fetch_write_data(User_Request* request);
	void Send_read_data(User_Request* request);
	//Writes back the Response UPIU (32 B, UCD region) and the CQ entry (32 B, CQ
	//ring), ufs-design.md section 3 steps 7-8. Implemented Phase B Day 7.
	void Send_completion_entry(User_Request* request);
	void Process_pcie_write_message(uint64_t address, void* payload, unsigned int payload_size);
	void Process_pcie_read_message(uint64_t address, void* payload, unsigned int payload_size);

private:
	//UFS command fetch is two-step (ufs-design.md section 3): DMA #1 reads the
	//32 B UTRD from the SQ ring, DMA #2 reads the 48 B UCD (Command UPIU + PRDT).
	//This differs from NVMe's single inline SQE fetch and is the structural
	//per-command link overhead the simulator models.
	enum class UFS_DMA_Req_Type { UTRD, UCD, WRITE_DATA };
	struct UFS_DMA_Req_Item
	{
		UFS_DMA_Req_Type Type;
		void* object;
	};
	std::list<UFS_DMA_Req_Item*> ufs_dma_list;
};

class Host_Interface_UFS : public Host_Interface_Base
{
	friend class Input_Stream_Manager_UFS;
	friend class Request_Fetch_Unit_UFS;

public:
	Host_Interface_UFS(const sim_object_id_type& id, LHA_type max_logical_sector_address,
					   uint16_t queue_size, uint16_t completion_queue_count,
					   uint16_t max_active_commands, UFS_Arbitration_Type arbitration,
					   uint8_t iacth, sim_time_type iatoval,
					   unsigned int no_of_input_streams, uint16_t queue_fetch_size,
					   unsigned int sectors_per_page, Data_Cache_Manager_Base* cache);
	stream_id_type Create_new_stream(IO_Flow_Priority_Class::Priority priority_class,
									 LHA_type start_logical_sector_address, LHA_type end_logical_sector_address,
									 uint64_t submission_queue_base_address, uint64_t completion_queue_base_address);
	void Start_simulation();
	void Validate_simulation_config();
	void Execute_simulator_event(MQSimEngine::Sim_Event*);
	uint16_t Get_queue_depth();
	uint16_t Get_completion_queue_count();
	uint16_t Get_completion_queue_context_count();//actually-created distinct CQ contexts (should equal Get_completion_queue_count once all streams exist)
	void Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter);

private:
	uint16_t queue_size;               //ring slots per SQ and per CQ (one slot always reserved)
	uint16_t completion_queue_count;   //<= no_of_input_streams. SQ i -> CQ (i mod count)
	uint16_t max_active_commands;
	UFS_Arbitration_Type arbitration;
	uint8_t iacth;
	sim_time_type iatoval;
	unsigned int no_of_input_streams;
};
} // namespace SSD_Components

#endif // !HOST_INTERFACE_UFS_H
