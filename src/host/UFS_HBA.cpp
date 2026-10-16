#include <cstring>
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
			//sq->queue owns the in-flight pointers. request_queue_in_memory holds
			//duplicates of a subset of them (ring-slot view), so it is not walked
			for (auto &req : sq->queue) {
				if (req.second) {
					delete req.second;
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

	bool UFS_HBA::enqueue_into_sq(UFS_SQ_Ring* ring, Host_IO_Request* request)
	{
		if (UFS_SQ_FULL(ring) || ring->available_task_tags.size() == 0) {
			return false;
		}
		uint8_t task_tag = *ring->available_task_tags.begin();
		if (ring->queue[task_tag] != NULL) {
			PRINT_ERROR("Unexpected situation in UFS_HBA! Overwriting an unhandled I/O request in the queue!")
		}
		request->IO_queue_info = task_tag;
		ring->queue[task_tag] = request;
		ring->available_task_tags.erase(ring->available_task_tags.begin());
		ring->request_queue_in_memory[ring->Submission_queue_tail] = request;
		UFS_UPDATE_SQ_TAIL(ring);
		request->Enqueue_time = Simulator->Time();
		pcie_root_complex->Write_to_device(ring->Submission_tail_register_address_on_device, ring->Submission_queue_tail);
		return true;
	}

	void UFS_HBA::Execute_simulator_event(MQSimEngine::Sim_Event* event)
	{
		switch ((UFS_HBA_Sim_Events)event->Type)
		{
			case UFS_HBA_Sim_Events::SUBMIT_IO_REQUEST:
			{
				Host_IO_Request* request = host_requests.front();
				host_requests.pop();

				if (request->Source_flow_id >= sq_count) {
					PRINT_ERROR("UFS_HBA: I/O request submitted from a flow with no submission queue!")
				}
				UFS_SQ_Ring* ring = sq_rings[request->Source_flow_id];
				if (!enqueue_into_sq(ring, request)) {
					ring->waiting_requests_for_submission.push_back(request);
				}

				if (host_requests.size() > 0) {
					Simulator->Register_sim_event(Simulator->Time() + hba_processing_delay, this, NULL, static_cast<int>(UFS_HBA_Sim_Events::SUBMIT_IO_REQUEST));
				}
				break;
			}
			case UFS_HBA_Sim_Events::CONSUME_IO_REQUEST:
				//completion consumption lands in Phase B Day 7
				PRINT_ERROR("UFS_HBA: completion consumption is not implemented yet (plan.md section 7, Phase B Day 7)")
				break;
		}
	}

	void UFS_HBA::Submit_io_request(Host_IO_Request* request)
	{
		host_requests.push(request);
		if (host_requests.size() == 1) {
			Simulator->Register_sim_event(Simulator->Time() + hba_processing_delay, this, NULL, static_cast<int>(UFS_HBA_Sim_Events::SUBMIT_IO_REQUEST));
		}
	}

	void UFS_HBA::UFS_consume_io_request(UFS_Completion_Queue_Entry* cqe)
	{
		PRINT_ERROR("UFS_HBA::UFS_consume_io_request is not implemented yet (Phase B Day 7)")
	}

	UTP_Transfer_Request_Descriptor* UFS_HBA::Read_utrd(uint64_t address)
	{
		//Device DMA read of an SQ ring slot (two-step fetch, step 1). The UTRD's
		//interpreted content is the UCD pointer. The rest is carried for fidelity.
		uint16_t sq_id = UFS_MEMORY_QUEUE_ID(address);
		if (sq_id >= sq_count) {
			PRINT_ERROR("UFS_HBA: UTRD read from an unknown submission queue!")
		}
		UFS_SQ_Ring* ring = sq_rings[sq_id];
		uint16_t slot = (uint16_t)((address - ring->Submission_queue_memory_base_address) / sizeof(UTP_Transfer_Request_Descriptor));
		if (slot >= ring->Submission_queue_size || ring->request_queue_in_memory[slot] == NULL) {
			PRINT_ERROR("UFS_HBA: UTRD read from an empty or out-of-range ring slot!")
		}

		uint64_t ucd_address = ring->UCD_area_memory_base_address + (uint64_t)slot * 0x100;
		UTP_Transfer_Request_Descriptor* utrd = new UTP_Transfer_Request_Descriptor;
		utrd->DW0_Header = 0x10000000;//Command Type = 01h (SCSI), interrupt/crypto bits clear
		utrd->DW1_DUN_Lower = 0;
		utrd->DW2_OCS = 0x0F;//invalid-by-default. In MCQ mode the real OCS is in the CQ entry
		utrd->DW3_DUN_Upper = 0;
		utrd->DW4_UCD_Base_Lower = (uint32_t)ucd_address;
		utrd->DW5_UCD_Base_Upper = (uint32_t)(ucd_address >> 32);
		utrd->DW6_Response_UPIU = (16 << 16) | 8; //response at UCD+0x40: offset 16 DWords, length 8 DWords
		utrd->DW7_PRDT = (24 << 16) | 1;          //PRDT at UCD+0x60: offset 24 DWords, 1 entry
		return utrd;
	}

	UFS_Command_UPIU* UFS_HBA::Read_ucd(uint64_t address)
	{
		//Device DMA read of the UCD (two-step fetch, step 2): Command UPIU + PRDT.
		//The returned UPIU pointer becomes User_Request::IO_command_info on the
		//device side and is freed there via DELETE_REQUEST_UFS.
		uint16_t sq_id = UFS_MEMORY_QUEUE_ID(address);
		if (sq_id >= sq_count) {
			PRINT_ERROR("UFS_HBA: UCD read from an unknown submission queue!")
		}
		UFS_SQ_Ring* ring = sq_rings[sq_id];
		uint16_t slot = (uint16_t)((address - ring->UCD_area_memory_base_address) / 0x100);
		if (slot >= ring->Submission_queue_size || ring->request_queue_in_memory[slot] == NULL) {
			PRINT_ERROR("UFS_HBA: UCD read from an empty or out-of-range ring slot!")
		}
		Host_IO_Request* request = ring->request_queue_in_memory[slot];

		UFS_Command_UPIU* upiu = new UFS_Command_UPIU;
		memset(upiu, 0, sizeof(UFS_Command_UPIU));
		upiu->Transaction_Type = 0x01;//COMMAND UPIU
		upiu->Flags = (request->Type == Host_IO_Request_Type::READ) ? 0x40 : 0x20;//R / W flag bits
		upiu->LUN = (uint8_t)sq_id;//flow = LU (ufs-design.md section 5)
		upiu->Task_Tag = (uint8_t)request->IO_queue_info;
		upiu->Expected_Data_Transfer_Length = request->LBA_count * SECTOR_SIZE_IN_BYTE;
		//CDB: READ(16)/WRITE(16). 8-byte big-endian LBA, 4-byte big-endian length
		upiu->CDB[0] = (request->Type == Host_IO_Request_Type::READ) ? UFS_READ_OPCODE : UFS_WRITE_OPCODE;
		for (int i = 0; i < 8; i++) {
			upiu->CDB[2 + i] = (uint8_t)(request->Start_LBA >> (8 * (7 - i)));
		}
		for (int i = 0; i < 4; i++) {
			upiu->CDB[10 + i] = (uint8_t)(request->LBA_count >> (8 * (3 - i)));
		}
		return upiu;
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
