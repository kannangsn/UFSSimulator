#ifndef GC_AND_WL_UNIT_PAGE_LEVEL_H
#define GC_AND_WL_UNIT_PAGE_LEVEL_H

#include "GC_and_WL_Unit_Base.h"
#include "NVM_PHY_ONFI.h"
#include "../utils/RandomGenerator.h"
#include <queue>


namespace SSD_Components
{
	class GC_and_WL_Unit_Page_Level : public GC_and_WL_Unit_Base
	{
	public:
		GC_and_WL_Unit_Page_Level(const sim_object_id_type& id,
			Address_Mapping_Unit_Base* address_mapping_unit, Flash_Block_Manager_Base* block_manager, TSU_Base* tsu, NVM_PHY_ONFI* flash_controller,
			GC_Block_Selection_Policy_Type block_selection_policy, double gc_threshold, bool preemptible_gc_enabled, double gc_hard_threshold,
			unsigned int channel_count, unsigned int chip_no_per_channel, unsigned int die_no_per_chip, unsigned int plane_no_per_die,
			unsigned int block_no_per_plane, unsigned int page_no_per_block, unsigned int sectors_per_page,
			bool use_copyback, double rho, unsigned int max_ongoing_gc_reqs_per_plane = 10,
			bool dynamic_wearleveling_enabled = true, bool static_wearleveling_enabled = true, unsigned int static_wearleveling_threshold = 100, int seed = 432);

		/*This function is used for implementing preemptible GC execution. If for a flash chip the free block
		* pool becomes close to empty, then the GC requests for that flash chip should be prioritized and
		* GC should go on in non-preemptible mode.*/
		bool GC_is_in_urgent_mode(const NVM::FlashMemory::Flash_Chip*);

		void Check_gc_required(const unsigned int free_block_pool_size, const NVM::FlashMemory::Physical_Page_Address& plane_address);
	private:
		NVM_PHY_ONFI * flash_controller;

		//One function per built-in block-selection policy, each returning
		//block_no_per_plane (an otherwise invalid block id) if no safe
		//candidate was found. Check_gc_required's own switch calls these
		//directly for its five non-EXTERNAL cases, and EXTERNAL's dispatch
		//calls whichever one the external controller's action selects, so
		//neither path duplicates the other's selection logic.
		flash_block_ID_type Select_Block_GREEDY(PlaneBookKeepingType* pbke);
		flash_block_ID_type Select_Block_RGA(PlaneBookKeepingType* pbke);
		flash_block_ID_type Select_Block_RANDOM(PlaneBookKeepingType* pbke);
		flash_block_ID_type Select_Block_RANDOM_P(PlaneBookKeepingType* pbke);
		flash_block_ID_type Select_Block_RANDOM_PP(PlaneBookKeepingType* pbke);
		flash_block_ID_type Select_Block_FIFO(PlaneBookKeepingType* pbke);

		//Opens Device_Parameter_Set::External_GC_State_Pipe_Path/
		//External_GC_Action_Pipe_Path on first use, writes the current
		//decision state, blocks for one line back, and returns it parsed
		//as an action index into {GREEDY, RGA, RANDOM, RANDOM_P, FIFO}
		//(RANDOM_PP excluded, see plan.md's known issue with it).
		//Raw POSIX file descriptors, not std::ofstream/ifstream: libc++'s
		//filebuf buffering on a FIFO silently never saw data a peer had
		//already written, confirmed by direct testing. Read/write
		//ourselves instead of trusting that layer on a pipe.
		int Query_external_controller(PlaneBookKeepingType* pbke, unsigned int free_block_pool_size);
		void Ensure_external_pipes_open();
		bool external_pipes_opened = false;
		int external_state_fd = -1;
		int external_action_fd = -1;
	};
}
#endif // !GC_AND_WL_UNIT_PAGE_LEVEL_H
