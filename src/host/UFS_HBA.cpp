#include "UFS_HBA.h"
#include "Host_Defs.h"
#include "../ssd/Host_Interface_Defs.h"
#include "../sim/Engine.h"

//Remaining stubs land in Phase B (plan.md section 7): Day 5 doorbells, Day 6
//submission path, Day 7 completion consumption.

namespace Host_Components
{
	//Flow priority class -> SQATTRy.SQPL (0 = highest). UNDEFINED maps to the
	//lowest level (ufs-design.md section 4)
	static uint8_t priority_class_to_sqpl(IO_Flow_Priority_Class::Priority priority_class)
	{
		return priority_class == IO_Flow_Priority_Class::UNDEFINED ? 7 : (uint8_t)priority_class;
	}

	UFS_HBA::UFS_HBA(sim_object_id_type id, uint16_t sq_count, uint16_t cq_count, uint16_t queue_size,
		sim_time_type hba_processing_delay, PCIe_Root_Complex* pcie_root_complex, std::vector<Host_Components::IO_Flow_Base*>* IO_flows) :
		MQSimEngine::Sim_Object(id), sq_count(sq_count), cq_count(cq_count), queue_size(queue_size),
		hba_processing_delay(hba_processing_delay), pcie_root_complex(pcie_root_complex), IO_flows(IO_flows)
	{
		//Address map per ufs-design.md section 1. SQPL is stamped from the owning
		//flow's priority class in Start_simulation, since IO_flows may not be
		//populated yet at construction time (same lifecycle as SATA_HBA)
		for (uint16_t q = 0; q < sq_count; q++) {
			UFS_SQ_Ring* sq = new UFS_SQ_Ring;
			sq->SQ_id = q;
			sq->SQPL = 7;
			sq->CQ_id = q % cq_count;
			sq->Submission_queue_head = 0;
			sq->Submission_queue_tail = 0;
			sq->Submission_queue_size = queue_size;
			sq->Submission_tail_register_address_on_device = UFS_SQ_TAIL_DOORBELL(q);
			sq->Submission_queue_memory_base_address = UFS_SQ_MEMORY(q);
			sq->UCD_area_memory_base_address = UFS_UCD_MEMORY(q, 0);
			for (int tag = 0; tag < 256; tag++) {
				sq->available_task_tags.insert((uint8_t)tag);
			}
			sq->request_queue_in_memory.resize(queue_size, NULL);
			sq_rings.push_back(sq);
		}
		for (uint16_t c = 0; c < cq_count; c++) {
			UFS_CQ_Ring* cq = new UFS_CQ_Ring;
			cq->CQ_id = c;
			cq->Completion_queue_head = 0;
			cq->Completion_queue_tail = 0;
			cq->Completion_queue_size = queue_size;
			cq->Completion_head_register_address_on_device = UFS_CQ_HEAD_DOORBELL(c);
			cq->Completion_queue_memory_base_address = UFS_CQ_MEMORY(c);
			cq_rings.push_back(cq);
		}
	}

	UFS_HBA::~UFS_HBA()
	{
		for (auto &sq : sq_rings) {
			for (auto &req : sq->request_queue_in_memory) {
				if (req) {
					delete req;
				}
			}
			for (auto &req : sq->waiting_requests_for_submission) {
				if (req) {
					delete req;
				}
			}
			delete sq;
		}
		for (auto &cq : cq_rings) {
			delete cq;
		}
	}

	void UFS_HBA::Start_simulation()
	{
		if (IO_flows != NULL) {
			for (uint16_t q = 0; q < sq_count && q < (uint16_t)IO_flows->size(); q++) {
				sq_rings[q]->SQPL = priority_class_to_sqpl((*IO_flows)[q]->Priority_class());
			}
		}
	}

	void UFS_HBA::Validate_simulation_config()
	{
		//ufs-design.md section 8 validity conditions that are HBA-local. The config-level
		//checks land with the XML parsing in Phase B Day 8
		if (sq_count < 1 || sq_count > 32) {
			PRINT_ERROR("UFS_HBA: SQ count must be between 1 and 32 (MCQCAP.MAXQ)")
		}
		if (cq_count < 1 || cq_count > sq_count) {
			PRINT_ERROR("UFS_HBA: CQ count must be between 1 and the SQ count")
		}
		if (queue_size < 2) {
			PRINT_ERROR("UFS_HBA: queue size must be at least 2 (one ring slot is always reserved)")
		}
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
