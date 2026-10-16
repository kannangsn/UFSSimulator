//UFS test: the completion path (Response UPIU + CQ entry -> HBA consume ->
//tag/slot recycling -> CQ head doorbell round trip) and IACTH/IATOVAL interrupt
//aggregation, through the real event engine (originally the Phase B Day 7
//harness). Completions are injected by calling Handle_serviced_request directly,
//standing in for the role the cache/FTL backend plays in a full simulation.
#define private public
#define protected public
#include <iostream>
#include <cassert>
#include "../ssd/Host_Interface_UFS.h"
#include "../ssd/Host_Interface_Defs.h"
#include "Host_Defs.h"
#include "UFS_HBA.h"
#include "PCIe_Link.h"
#include "PCIe_Switch.h"
#include "PCIe_Root_Complex.h"
#include "../sim/Engine.h"
#undef private
#undef protected

using namespace SSD_Components;
using namespace Host_Components;

struct Rig
{
	Host_Interface_UFS* hi;
	PCIe_Link* link;
	PCIe_Root_Complex* rc;
	PCIe_Switch* sw;
	UFS_HBA* hba;
	Input_Stream_Manager_UFS* mgr;

	Rig(const char* tag, uint8_t iacth, sim_time_type iatoval)
	{
		std::string t(tag);
		hi = new Host_Interface_UFS("HI" + t, (LHA_type)1 << 22, 64, 1, 32,
			UFS_Arbitration_Type::ROUND_ROBIN, iacth, iatoval, 1, 512, 8, NULL);
		hi->Create_new_stream(IO_Flow_Priority_Class::HIGH, 0, ((LHA_type)1 << 22) - 1,
			UFS_SQ_MEMORY(0), UFS_CQ_MEMORY(0));
		link = new PCIe_Link("Link" + t, NULL, NULL);
		rc = new PCIe_Root_Complex(link, HostInterface_Types::UFS, NULL, NULL);
		link->Set_root_complex(rc);
		sw = new PCIe_Switch(link, hi);
		link->Set_pcie_switch(sw);
		hi->Attach_to_device(sw);
		hba = new UFS_HBA("HBA" + t, 1, 1, 64, 400, rc, NULL);
		rc->Set_ufs_hba(hba);
		Simulator->AddObject(link);
		Simulator->AddObject(hba);
		mgr = (Input_Stream_Manager_UFS*)hi->input_stream_manager;
	}

	Host_IO_Request* submit_read(LHA_type lba, unsigned int count)
	{
		Host_IO_Request* r = new Host_IO_Request;
		r->Arrival_time = Simulator->Time();
		r->Start_LBA = lba;
		r->LBA_count = count;
		r->Type = Host_IO_Request_Type::READ;
		r->Source_flow_id = 0;
		hba->Submit_io_request(r);
		return r;
	}

	Input_Stream_UFS* stream() { return (Input_Stream_UFS*)mgr->input_streams[0]; }
	UFS_Completion_Queue_Context* cq() { return mgr->completion_queues[0]; }
	UFS_SQ_Ring* ring() { return hba->sq_rings[0]; }

	//The cache/FTL backend consumes a request's flash transactions before firing
	//the serviced signal. This harness has no backend, so emulate that first
	void complete(User_Request* dev_request)
	{
		for (auto &tr : dev_request->Transaction_list) {
			delete tr;
		}
		dev_request->Transaction_list.clear();
		mgr->Handle_serviced_request(dev_request);
	}
};

int main()
{
	//--- Test A: aggregation disabled -> immediate full round trip ---
	{
		Rig rig("A", 0, 0);
		Host_IO_Request* rd = rig.submit_read(1000, 16);
		rig.submit_read(2000, 8);
		Simulator->Start_simulation();
		assert(rig.stream()->Waiting_user_requests.size() == 2);

		User_Request* dev_rd = rig.stream()->Waiting_user_requests.front();
		rig.complete(dev_rd);//dev_rd is freed on delivery
		Simulator->Start_simulation();

		assert(rig.stream()->Waiting_user_requests.size() == 1);
		assert(rig.stream()->On_the_fly_requests == 1);
		assert(rig.mgr->total_active_commands == 1);
		assert(rig.cq()->Completion_tail == 1);
		assert(rig.cq()->Completion_head == 1);//HBA's CQ head doorbell round-tripped
		assert(rig.ring()->queue.size() == 1);//tag 0 recycled
		assert(rig.ring()->available_task_tags.size() == 255);
		assert(rig.ring()->Submission_queue_head == 1);//slot recycled at completion
		delete rd;//host side: no IO flow attached in this rig, so the harness cleans up
		std::cout << "PASS: immediate completion round trip (CQE -> HBA -> tag/slot freed -> CQ head doorbell)\n";
	}

	//--- Test B: count-based aggregation (IACTH = 2) ---
	{
		Rig rig("B", 2, 0);
		Host_IO_Request* h1 = rig.submit_read(100, 8);
		Host_IO_Request* h2 = rig.submit_read(200, 8);
		rig.submit_read(300, 8);
		Simulator->Start_simulation();
		assert(rig.stream()->Waiting_user_requests.size() == 3);

		User_Request* d1 = rig.stream()->Waiting_user_requests.front();
		rig.complete(d1);
		Simulator->Start_simulation();
		assert(rig.cq()->Pending_completions.size() == 1);//held, below threshold
		assert(rig.ring()->queue.size() == 3);//nothing reached the HBA yet

		User_Request* d2 = rig.stream()->Waiting_user_requests.front();
		rig.complete(d2);//threshold hit -> flush both
		Simulator->Start_simulation();
		assert(rig.cq()->Pending_completions.size() == 0);
		assert(rig.ring()->queue.size() == 1);//two completions consumed together
		assert(rig.cq()->Completion_head == 2 && rig.cq()->Completion_tail == 2);
		delete h1;
		delete h2;
		std::cout << "PASS: IACTH=2 count aggregation (1 held, flush of 2 on threshold)\n";
	}

	//--- Test C: timeout-based aggregation (IATOVAL = 5 us) ---
	{
		Rig rig("C", 0, 5000);
		Host_IO_Request* h1 = rig.submit_read(100, 8);
		rig.submit_read(200, 8);
		Simulator->Start_simulation();

		sim_time_type t_before = Simulator->Time();
		User_Request* d1 = rig.stream()->Waiting_user_requests.front();
		rig.complete(d1);//held. IATOVAL timer armed
		assert(rig.cq()->Pending_completions.size() == 1);
		Simulator->Start_simulation();//runs until the timer fires and the flush completes

		assert(rig.cq()->Pending_completions.size() == 0);
		assert(rig.ring()->queue.size() == 1);//delivered after the timeout
		assert(Simulator->Time() >= t_before + 5000);
		delete h1;
		std::cout << "PASS: IATOVAL timeout aggregation (held, flushed by the 5 us timer, t="
				  << Simulator->Time() - t_before << " ns after hold)\n";
	}

	return 0;
}
