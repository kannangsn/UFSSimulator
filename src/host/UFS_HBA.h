#ifndef UFS_HBA_H
#define UFS_HBA_H

#include <stdint.h>
#include <vector>
#include <set>
#include <list>
#include <queue>
#include <unordered_map>
#include "../sim/Sim_Defs.h"
#include "../sim/Sim_Object.h"
#include "Host_IO_Request.h"
#include "IO_Flow_Base.h"
#include "PCIe_Root_Complex.h"

//Host-side UFS adapter (plan.md section 5.2): owns all SQ/CQ rings and routes
//IO flows through them, generalizing SATA_HBA's single-NCQ pattern to up to 32
//MCQ rings. IO_Flow_Base's per-flow queue-pair model cannot represent MCQ's
//queue-count/flow-count decoupling or N:1 SQ->CQ mapping, so queue topology
//is owned here instead. See ufs-design.md for ring layout and command flow.

namespace Host_Components
{
#define UFS_SQ_FULL(SQ) ((SQ)->Submission_queue_tail < (SQ)->Submission_queue_size - 1 ? (SQ)->Submission_queue_tail + 1 == (SQ)->Submission_queue_head : (SQ)->Submission_queue_head == 0)
#define UFS_UPDATE_SQ_TAIL(SQ) (SQ)->Submission_queue_tail++;\
					if ((SQ)->Submission_queue_tail == (SQ)->Submission_queue_size)\
						(SQ)->Submission_queue_tail = 0;

	//One submission-queue ring (ufs-design.md sections 1.2, 2.4). Task tags are a
	//per-SQ 8-bit pool (spec: 1-byte Task Tag), max 256 outstanding per queue,
	//enforced at allocation.
	struct UFS_SQ_Ring
	{
		uint16_t SQ_id;
		uint8_t SQPL;//flat 8-level priority, 0 = highest, from the owning flow's priority class
		uint16_t CQ_id;//which CQ receives this SQ's completions (SQ i -> CQ (i mod cq_count))
		uint16_t Submission_queue_head;
		uint16_t Submission_queue_tail;
		uint16_t Submission_queue_size;
		uint64_t Submission_tail_register_address_on_device;//UFS_SQ_TAIL_DOORBELL(SQ_id)
		uint64_t Submission_queue_memory_base_address;      //UFS_SQ_MEMORY(SQ_id)
		uint64_t UCD_area_memory_base_address;              //UFS_UCD_MEMORY(SQ_id, 0)
		std::set<uint8_t> available_task_tags;
		std::unordered_map<uint16_t, Host_IO_Request*> queue;//in-flight requests keyed by task tag (completion lookup, Day 7). The authoritative owner of in-flight pointers
		std::vector<Host_IO_Request*> request_queue_in_memory;//ring-slot view of the same requests, addressed by the device's UTRD/UCD reads. Slots are overwritten on ring wrap
		std::list<Host_IO_Request*> waiting_requests_for_submission;//ring full or no free tag
	};

	struct UFS_CQ_Ring
	{
		uint16_t CQ_id;
		uint16_t Completion_queue_head;
		uint16_t Completion_queue_tail;
		uint16_t Completion_queue_size;
		uint64_t Completion_head_register_address_on_device;//UFS_CQ_HEAD_DOORBELL(CQ_id)
		uint64_t Completion_queue_memory_base_address;      //UFS_CQ_MEMORY(CQ_id)
	};

	enum class UFS_HBA_Sim_Events { SUBMIT_IO_REQUEST, CONSUME_IO_REQUEST };

	class IO_Flow_Base;
	class PCIe_Root_Complex;
	class UFS_HBA : public MQSimEngine::Sim_Object
	{
	public:
		UFS_HBA(sim_object_id_type id, uint16_t sq_count, uint16_t cq_count, uint16_t queue_size,
			sim_time_type hba_processing_delay, PCIe_Root_Complex* pcie_root_complex, std::vector<Host_Components::IO_Flow_Base*>* IO_flows);
		~UFS_HBA();
		void Start_simulation();
		void Validate_simulation_config();
		void Execute_simulator_event(MQSimEngine::Sim_Event*);
		void Submit_io_request(Host_IO_Request* request);//flow -> SQ (flow id = SQ id), ufs-design.md section 3 steps 1-2
		void UFS_consume_io_request(UFS_Completion_Queue_Entry* cqe);//SQID+Task Tag -> request, section 3 steps 9-10
		//Device DMA-read callbacks (two-step fetch, ufs-design.md section 3 steps 3-4)
		UTP_Transfer_Request_Descriptor* Read_utrd(uint64_t address);
		UFS_Command_UPIU* Read_ucd(uint64_t address);
		const UFS_SQ_Ring* Get_sq_ring_info(uint16_t sq_id);
		const UFS_CQ_Ring* Get_cq_ring_info(uint16_t cq_id);
		void Set_io_flows(std::vector<Host_Components::IO_Flow_Base*>* IO_flows);
		void Set_root_complex(PCIe_Root_Complex* pcie_root_complex);
	private:
		uint16_t sq_count;
		uint16_t cq_count;
		uint16_t queue_size;
		sim_time_type hba_processing_delay;//UFS_Processing_Delay: UTP/UniPro stack latency [ASSUMPTION, ufs-design.md section 5]
		PCIe_Root_Complex* pcie_root_complex;
		std::vector<Host_Components::IO_Flow_Base*>* IO_flows;
		std::vector<UFS_SQ_Ring*> sq_rings;
		std::vector<UFS_CQ_Ring*> cq_rings;
		//enqueue into the ring + ring the SQ tail doorbell. False if ring full / no tag
		bool enqueue_into_sq(UFS_SQ_Ring* ring, Host_IO_Request* request);
		void Update_and_submit_cq_head(uint16_t cq_id);
		std::queue<UFS_Completion_Queue_Entry*> consume_requests;
		std::queue<Host_IO_Request*> host_requests;
	};
}

#endif // !UFS_HBA_H
