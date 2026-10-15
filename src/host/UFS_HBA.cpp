#include "UFS_HBA.h"
#include "../ssd/Host_Interface_Defs.h"
#include "../sim/Engine.h"

//Phase A Day 2 skeleton: all method bodies are stubs. Implementation lands in
//Phase B (plan.md section 7): Day 4 ring construction, Day 5 doorbells, Day 6
//submission path, Day 7 completion consumption.

namespace Host_Components
{
	UFS_HBA::UFS_HBA(sim_object_id_type id, uint16_t sq_count, uint16_t cq_count, uint16_t queue_size,
		sim_time_type hba_processing_delay, PCIe_Root_Complex* pcie_root_complex, std::vector<Host_Components::IO_Flow_Base*>* IO_flows) :
		MQSimEngine::Sim_Object(id), sq_count(sq_count), cq_count(cq_count), queue_size(queue_size),
		hba_processing_delay(hba_processing_delay), pcie_root_complex(pcie_root_complex), IO_flows(IO_flows)
	{
		//Ring construction (sq_count SQ rings, cq_count CQ rings, address map from
		//ufs-design.md section 1) is implemented in Phase B Day 4
	}

	UFS_HBA::~UFS_HBA()
	{
		for (auto &sq : sq_rings) {
			delete sq;
		}
		for (auto &cq : cq_rings) {
			delete cq;
		}
	}

	void UFS_HBA::Start_simulation()
	{
	}

	void UFS_HBA::Validate_simulation_config()
	{
	}

	void UFS_HBA::Execute_simulator_event(MQSimEngine::Sim_Event* event)
	{
		PRINT_ERROR("UFS_HBA::Execute_simulator_event is not implemented yet (Phase B Day 6)")
	}

	void UFS_HBA::Submit_io_request(Host_IO_Request* request)
	{
		PRINT_ERROR("UFS_HBA::Submit_io_request is not implemented yet (Phase B Day 6)")
	}

	void UFS_HBA::UFS_consume_io_request(UFS_Completion_Queue_Entry* cqe)
	{
		PRINT_ERROR("UFS_HBA::UFS_consume_io_request is not implemented yet (Phase B Day 7)")
	}

	UTP_Transfer_Request_Descriptor* UFS_HBA::Read_utrd(uint64_t address)
	{
		PRINT_ERROR("UFS_HBA::Read_utrd is not implemented yet (Phase B Day 6)")
	}

	UFS_Command_UPIU* UFS_HBA::Read_ucd(uint64_t address)
	{
		PRINT_ERROR("UFS_HBA::Read_ucd is not implemented yet (Phase B Day 6)")
	}

	const UFS_SQ_Ring* UFS_HBA::Get_sq_ring_info(uint16_t sq_id)
	{
		return sq_rings[sq_id];
	}

	const UFS_CQ_Ring* UFS_HBA::Get_cq_ring_info(uint16_t cq_id)
	{
		return cq_rings[cq_id];
	}

	void UFS_HBA::Update_and_submit_cq_head(uint16_t cq_id)
	{
		PRINT_ERROR("UFS_HBA::Update_and_submit_cq_head is not implemented yet (Phase B Day 7)")
	}

	void UFS_HBA::Set_io_flows(std::vector<Host_Components::IO_Flow_Base*>* IO_flows)
	{
		this->IO_flows = IO_flows;
	}

	void UFS_HBA::Set_root_complex(PCIe_Root_Complex* pcie_root_complex)
	{
		this->pcie_root_complex = pcie_root_complex;
	}
}
