//UFS test: end-to-end submission path through the REAL simulator engine.
//flow-level Submit_io_request -> HBA ring + doorbell -> device arbitration ->
//UTRD fetch -> UCD fetch -> Command UPIU parse -> User_Request -> segmentation
//into flash transactions (originally the Phase B Day 6 harness). Only the
//cache/FTL backend is absent (the arrival broadcast has no subscribers).
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

int main()
{
	//Device side: 1 stream, depth 64, RR, MAC 32
	Host_Interface_UFS hi("HI", (LHA_type)1 << 22, 64, 1, 32,
		UFS_Arbitration_Type::ROUND_ROBIN, 0, 0, 1, 512, 8, NULL);
	hi.Create_new_stream(IO_Flow_Priority_Class::HIGH, 0, ((LHA_type)1 << 22) - 1,
		UFS_SQ_MEMORY(0), UFS_CQ_MEMORY(0));

	//Host side, connected the same way Host_System does it
	PCIe_Link link("Link", NULL, NULL);
	PCIe_Root_Complex rc(&link, HostInterface_Types::UFS, NULL, NULL);
	link.Set_root_complex(&rc);
	PCIe_Switch sw(&link, &hi);
	link.Set_pcie_switch(&sw);
	hi.Attach_to_device(&sw);
	UFS_HBA hba("HBA", 1, 1, 64, 400, &rc, NULL);
	rc.Set_ufs_hba(&hba);

	//Register only the objects whose events must fire (hi's Setup_triggers would
	//dereference the NULL cache. Its work in this test is all message-driven)
	Simulator->AddObject(&link);
	Simulator->AddObject(&hba);

	//One READ and one WRITE
	Host_IO_Request* rd = new Host_IO_Request;
	rd->Arrival_time = 0;
	rd->Start_LBA = 1000;
	rd->LBA_count = 16;
	rd->Type = Host_IO_Request_Type::READ;
	rd->Source_flow_id = 0;
	hba.Submit_io_request(rd);

	Host_IO_Request* wr = new Host_IO_Request;
	wr->Arrival_time = 0;
	wr->Start_LBA = 5000;
	wr->LBA_count = 24;
	wr->Type = Host_IO_Request_Type::WRITE;
	wr->Source_flow_id = 0;
	hba.Submit_io_request(wr);

	Simulator->Start_simulation();

	//--- Host-side state ---
	UFS_SQ_Ring* ring = hba.sq_rings[0];
	assert(ring->Submission_queue_tail == 2);
	assert(ring->queue.size() == 2);
	assert(ring->available_task_tags.size() == 254);
	assert(rd->IO_queue_info == 0 && wr->IO_queue_info == 1);//task tags in order
	std::cout << "PASS: HBA ring state (tail=2, 2 in-flight, tags 0 and 1 allocated)\n";

	//--- Device-side state ---
	Input_Stream_Manager_UFS* mgr = (Input_Stream_Manager_UFS*)hi.input_stream_manager;
	Input_Stream_UFS* stream = (Input_Stream_UFS*)mgr->input_streams[0];
	assert(stream->Waiting_user_requests.size() == 2);
	assert(mgr->total_active_commands == 2);
	assert(stream->On_the_fly_requests == 2);
	assert(stream->Submission_head == 2 && stream->Submission_head_informed_to_host == 2);
	assert(stream->STAT_number_of_read_requests == 1 && stream->STAT_number_of_write_requests == 1);

	User_Request* dev_rd = stream->Waiting_user_requests.front();
	assert(dev_rd->Type == UserRequestType::READ);
	assert(dev_rd->Start_LBA == 1000);
	assert(dev_rd->SizeInSectors == 16);
	assert(dev_rd->Size_in_byte == 16 * SECTOR_SIZE_IN_BYTE);
	assert(dev_rd->Transaction_list.size() == 2);//16 sectors / 8 per page, aligned
	UFS_Command_UPIU* rd_upiu = (UFS_Command_UPIU*)dev_rd->IO_command_info;
	assert(rd_upiu->Transaction_Type == 0x01 && rd_upiu->LUN == 0 && rd_upiu->Task_Tag == 0);
	assert(rd_upiu->CDB[0] == UFS_READ_OPCODE);

	User_Request* dev_wr = stream->Waiting_user_requests.back();
	assert(dev_wr->Type == UserRequestType::WRITE);
	assert(dev_wr->Start_LBA == 5000);
	assert(dev_wr->SizeInSectors == 24);
	assert(dev_wr->Transaction_list.size() == 3);//24 sectors / 8 per page, aligned
	UFS_Command_UPIU* wr_upiu = (UFS_Command_UPIU*)dev_wr->IO_command_info;
	assert(wr_upiu->Task_Tag == 1 && wr_upiu->CDB[0] == UFS_WRITE_OPCODE);
	std::cout << "PASS: device parsed both commands (LBA/size/type/tag) and segmented them\n";

	//--- Timing sanity: HBA delay + link transfers accumulated simulated time ---
	assert(Simulator->Time() > 400);
	std::cout << "PASS: simulated time advanced through HBA delay + link transfers (t="
			  << Simulator->Time() << " ns)\n";
	return 0;
}
