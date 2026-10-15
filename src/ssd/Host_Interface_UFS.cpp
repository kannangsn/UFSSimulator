#include "Host_Interface_UFS.h"
#include "../sim/Engine.h"

//Phase A Day 2 skeleton: all method bodies are stubs. Implementation lands in
//Phase B (plan.md section 7): Day 4 queue structures, Day 5 doorbells/arbitration,
//Day 6 command fetch, Day 7 completion path + interrupt aggregation.

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
		PRINT_ERROR("Input_Stream_Manager_UFS::Create_new_stream is not implemented yet (Phase B Day 4)")
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
		//Validation invariants per ufs-design.md section 8, enforced in Phase B Day 8
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

	void Host_Interface_UFS::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter)
	{
	}
}
