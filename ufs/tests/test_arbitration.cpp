//UFS test: doorbell decode, ring-pointer semantics, RR + strict-priority
//arbitration, and the MCQConfig.MAC cap (originally the Phase B Day 5 harness).
//Uses `#define private public` to inspect internal state. Access control is
//compile-time only, object layout is unaffected. A real PCIe_Link+Switch chain is
//attached so Send_read_message_to_host has somewhere to go (messages queue as
//simulator events. The engine never runs here).
#define private public
#define protected public
#include <iostream>
#include <cassert>
#include <vector>
#include "../ssd/Host_Interface_UFS.h"
#include "../ssd/Host_Interface_Defs.h"
#include "Host_Defs.h"
#include "PCIe_Link.h"
#include "PCIe_Switch.h"
#include "PCIe_Message.h"
#undef private
#undef protected

using namespace SSD_Components;
using namespace Host_Components;

static void ring_doorbell(Host_Interface_UFS& hi, uint64_t address, uint16_t value)
{
	PCIe_Message* m = new PCIe_Message;
	m->Type = PCIe_Message_Type::WRITE_REQ;
	m->Destination = PCIe_Destination_Type::DEVICE;
	m->Address = address;
	m->Payload = (void*)(intptr_t)value;
	m->Payload_size = sizeof(value);
	hi.Consume_pcie_message(m);
}

static std::vector<stream_id_type> fetched_stream_order(Host_Interface_UFS& hi)
{
	std::vector<stream_id_type> order;
	Request_Fetch_Unit_UFS* rfu = (Request_Fetch_Unit_UFS*)hi.request_fetch_unit;
	for (auto &item : rfu->ufs_dma_list) {
		order.push_back((stream_id_type)(intptr_t)item->object);
	}
	return order;
}

int main()
{
	//--- Test 1: RR arbitration under a MAC cap of 2 ---
	Host_Interface_UFS hi("T.HI", 1000000, /*qsize*/64, /*cq*/2, /*MAC*/2,
		UFS_Arbitration_Type::ROUND_ROBIN, 0, 0, /*streams*/3, /*fetch*/512, 8, NULL);
	PCIe_Link link("T.Link", NULL, NULL);
	PCIe_Switch sw(&link, &hi);
	hi.Attach_to_device(&sw);
	for (int i = 0; i < 3; i++) {
		hi.Create_new_stream(IO_Flow_Priority_Class::HIGH, i * 1000, (i + 1) * 1000 - 1,
			UFS_SQ_MEMORY(i), UFS_CQ_MEMORY(i % 2));
	}
	Input_Stream_Manager_UFS* mgr = (Input_Stream_Manager_UFS*)hi.input_stream_manager;

	ring_doorbell(hi, UFS_SQ_TAIL_DOORBELL(0), 2);//2 entries in SQ0 -> both fetched, MAC now full
	assert(mgr->total_active_commands == 2);
	ring_doorbell(hi, UFS_SQ_TAIL_DOORBELL(1), 1);//blocked by MAC
	ring_doorbell(hi, UFS_SQ_TAIL_DOORBELL(2), 1);//blocked by MAC
	assert(mgr->total_active_commands == 2);
	{
		std::vector<stream_id_type> order = fetched_stream_order(hi);
		assert(order.size() == 2 && order[0] == 0 && order[1] == 0);
	}
	Input_Stream_UFS* s0 = (Input_Stream_UFS*)mgr->input_streams[0];
	assert(s0->Submission_head == 2 && s0->Submission_tail == 2 && s0->On_the_fly_requests == 2);

	//capacity frees up (what Handle_serviced_request does on completion):
	//RR must now pick SQ1 then SQ2, not SQ0 again
	mgr->total_active_commands--;
	mgr->fetch_pending_requests();
	mgr->total_active_commands--;
	mgr->fetch_pending_requests();
	{
		std::vector<stream_id_type> order = fetched_stream_order(hi);
		assert(order.size() == 4 && order[2] == 1 && order[3] == 2);
	}
	std::cout << "PASS: RR arbitration + MAC cap (fetch order 0,0 | 1,2 after capacity freed)\n";

	//--- Test 2: strict priority (SQPL, lower wins) ---
	Host_Interface_UFS hi2("T.HI2", 1000000, 64, 1, /*MAC*/32,
		UFS_Arbitration_Type::STRICT_PRIORITY, 0, 0, 3, 512, 8, NULL);
	PCIe_Link link2("T.Link2", NULL, NULL);
	PCIe_Switch sw2(&link2, &hi2);
	hi2.Attach_to_device(&sw2);
	hi2.Create_new_stream(IO_Flow_Priority_Class::LOW,    0,    999, UFS_SQ_MEMORY(0), UFS_CQ_MEMORY(0));//SQPL 3
	hi2.Create_new_stream(IO_Flow_Priority_Class::URGENT, 1000, 1999, UFS_SQ_MEMORY(1), UFS_CQ_MEMORY(0));//SQPL 0
	hi2.Create_new_stream(IO_Flow_Priority_Class::MEDIUM, 2000, 2999, UFS_SQ_MEMORY(2), UFS_CQ_MEMORY(0));//SQPL 2
	Input_Stream_Manager_UFS* mgr2 = (Input_Stream_Manager_UFS*)hi2.input_stream_manager;

	//make all three queues pending simultaneously, then run the pump once
	((Input_Stream_UFS*)mgr2->input_streams[0])->Submission_tail = 1;
	((Input_Stream_UFS*)mgr2->input_streams[1])->Submission_tail = 1;
	((Input_Stream_UFS*)mgr2->input_streams[2])->Submission_tail = 1;
	mgr2->fetch_pending_requests();
	{
		std::vector<stream_id_type> order = fetched_stream_order(hi2);
		assert(order.size() == 3);
		assert(order[0] == 1);//URGENT (SQPL 0) first
		assert(order[1] == 2);//MEDIUM (SQPL 2)
		assert(order[2] == 0);//LOW (SQPL 3) last
	}
	std::cout << "PASS: strict-priority arbitration (URGENT, MEDIUM, LOW order)\n";

	//--- Test 3: CQ head doorbell decode ---
	ring_doorbell(hi, UFS_CQ_HEAD_DOORBELL(1), 7);
	assert(mgr->find_completion_queue(1)->Completion_head == 7);
	std::cout << "PASS: CQ head doorbell decode (CQ1 head = 7)\n";

	return 0;
}
