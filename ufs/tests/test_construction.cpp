//UFS test: UFS_HBA ring construction and device-side stream/CQ-context creation
//(originally the Phase B Day 4 verification harness). Build and run via run_tests.sh.
#include <iostream>
#include <cassert>
#include "UFS_HBA.h"
#include "Host_Defs.h"
#include "../ssd/Host_Interface_UFS.h"
#include "../ssd/Host_Interface_Defs.h"

using namespace Host_Components;
using namespace SSD_Components;

int main()
{
	//--- UFS_HBA: 32 SQs, 8 CQs (N:1 = 4:1), depth 64 ---
	UFS_HBA hba("Test.UFS_HBA", 32, 8, 64, 400, NULL, NULL);
	hba.Validate_simulation_config();

	const UFS_SQ_Ring* sq0 = hba.Get_sq_ring_info(0);
	const UFS_SQ_Ring* sq31 = hba.Get_sq_ring_info(31);
	assert(sq0->Submission_tail_register_address_on_device == 0x2000);
	assert(sq31->Submission_tail_register_address_on_device == 0x2000 + 31 * 0x10);
	assert(sq0->Submission_queue_memory_base_address == 0x0ULL);
	assert(sq31->Submission_queue_memory_base_address == ((uint64_t)31 << 40));
	assert(sq31->UCD_area_memory_base_address == (((uint64_t)31 << 40) | (0x1ULL << 36)));
	assert(sq0->CQ_id == 0 && sq31->CQ_id == 31 % 8);
	assert(sq0->available_task_tags.size() == 256);
	assert(sq0->request_queue_in_memory.size() == 64);
	assert(sq0->Submission_queue_head == 0 && sq0->Submission_queue_tail == 0);

	const UFS_CQ_Ring* cq7 = hba.Get_cq_ring_info(7);
	assert(cq7->Completion_head_register_address_on_device == 0x2008 + 7 * 0x10);
	assert(cq7->Completion_queue_memory_base_address == (((uint64_t)7 << 40) | (0xFULL << 36)));
	assert(cq7->Completion_queue_size == 64);
	std::cout << "PASS: UFS_HBA ring construction (32 SQ, 8 CQ, addresses, tags, CQID map)\n";

	//--- Device side: 4 streams sharing 2 CQs (N:1 = 2:1) ---
	Host_Interface_UFS hi("Test.HostInterface", 1000000,
		/*queue_size*/ 64, /*cq_count*/ 2, /*MAC*/ 32,
		UFS_Arbitration_Type::ROUND_ROBIN, /*iacth*/ 0, /*iatoval*/ 0,
		/*streams*/ 4, /*fetch*/ 512, /*sectors_per_page*/ 8, NULL);

	for (int i = 0; i < 4; i++) {
		stream_id_type sid = hi.Create_new_stream(IO_Flow_Priority_Class::HIGH,
			i * 250000, (i + 1) * 250000 - 1,
			UFS_SQ_MEMORY(i), UFS_CQ_MEMORY(i % 2));
		assert(sid == i);
	}
	hi.Validate_simulation_config();
	assert(hi.Get_queue_depth() == 64);
	assert(hi.Get_completion_queue_count() == 2);
	//4 SQs mapped onto 2 CQs: find-or-create must have collapsed the contexts
	assert(hi.Get_completion_queue_context_count() == 2);

	std::cout << "PASS: Host_Interface_UFS + 4 streams, 2 shared CQ contexts, validation clean\n";
	return 0;
}
