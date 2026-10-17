#include <math.h>
#include <vector>
#include <set>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
#include "GC_and_WL_Unit_Page_Level.h"
#include "Flash_Block_Manager.h"
#include "FTL.h"
#include "../exec/Device_Parameter_Set.h"

namespace SSD_Components
{

	GC_and_WL_Unit_Page_Level::GC_and_WL_Unit_Page_Level(const sim_object_id_type& id,
		Address_Mapping_Unit_Base* address_mapping_unit, Flash_Block_Manager_Base* block_manager, TSU_Base* tsu, NVM_PHY_ONFI* flash_controller, 
		GC_Block_Selection_Policy_Type block_selection_policy, double gc_threshold, bool preemptible_gc_enabled, double gc_hard_threshold,
		unsigned int ChannelCount, unsigned int chip_no_per_channel, unsigned int die_no_per_chip, unsigned int plane_no_per_die,
		unsigned int block_no_per_plane, unsigned int Page_no_per_block, unsigned int sectors_per_page, 
		bool use_copyback, double rho, unsigned int max_ongoing_gc_reqs_per_plane, bool dynamic_wearleveling_enabled, bool static_wearleveling_enabled, unsigned int static_wearleveling_threshold, int seed)
		: GC_and_WL_Unit_Base(id, address_mapping_unit, block_manager, tsu, flash_controller, block_selection_policy, gc_threshold, preemptible_gc_enabled, gc_hard_threshold,
		ChannelCount, chip_no_per_channel, die_no_per_chip, plane_no_per_die, block_no_per_plane, Page_no_per_block, sectors_per_page, use_copyback, rho, max_ongoing_gc_reqs_per_plane, 
			dynamic_wearleveling_enabled, static_wearleveling_enabled, static_wearleveling_threshold, seed)
	{
		rga_set_size = (unsigned int)log2(block_no_per_plane);
	}
	
	bool GC_and_WL_Unit_Page_Level::GC_is_in_urgent_mode(const NVM::FlashMemory::Flash_Chip* chip)
	{
		if (!preemptible_gc_enabled) {
			return true;
		}

		NVM::FlashMemory::Physical_Page_Address addr;
		addr.ChannelID = chip->ChannelID; addr.ChipID = chip->ChipID;
		for (unsigned int die_id = 0; die_id < die_no_per_chip; die_id++) {
			for (unsigned int plane_id = 0; plane_id < plane_no_per_die; plane_id++) {
				addr.DieID = die_id; addr.PlaneID = plane_id;
				if (block_manager->Get_pool_size(addr) < block_pool_gc_hard_threshold)
					return true;
			}
		}

		return false;
	}

	void GC_and_WL_Unit_Page_Level::Check_gc_required(const unsigned int free_block_pool_size, const NVM::FlashMemory::Physical_Page_Address& plane_address)
	{
		if (free_block_pool_size < block_pool_gc_threshold) {
			flash_block_ID_type gc_candidate_block_id = block_manager->Get_coldest_block_id(plane_address);
			PlaneBookKeepingType* pbke = block_manager->Get_plane_bookkeeping_entry(plane_address);

			if (pbke->Ongoing_erase_operations.size() >= max_ongoing_gc_reqs_per_plane) {
				return;
			}

			switch (block_selection_policy) {
				case SSD_Components::GC_Block_Selection_Policy_Type::GREEDY:
					gc_candidate_block_id = Select_Block_GREEDY(pbke);
					break;
				case SSD_Components::GC_Block_Selection_Policy_Type::RGA:
					gc_candidate_block_id = Select_Block_RGA(pbke);
					break;
				case SSD_Components::GC_Block_Selection_Policy_Type::RANDOM:
					gc_candidate_block_id = Select_Block_RANDOM(pbke);
					if (gc_candidate_block_id == block_no_per_plane) {
						return;
					}
					break;
				case SSD_Components::GC_Block_Selection_Policy_Type::RANDOM_P:
					gc_candidate_block_id = Select_Block_RANDOM_P(pbke);
					if (gc_candidate_block_id == block_no_per_plane) {
						return;
					}
					break;
				case SSD_Components::GC_Block_Selection_Policy_Type::RANDOM_PP:
					gc_candidate_block_id = Select_Block_RANDOM_PP(pbke);
					if (gc_candidate_block_id == block_no_per_plane) {
						return;
					}
					break;
				case SSD_Components::GC_Block_Selection_Policy_Type::FIFO:
					gc_candidate_block_id = Select_Block_FIFO(pbke);
					if (gc_candidate_block_id == block_no_per_plane) {
						return;
					}
					break;
				case SSD_Components::GC_Block_Selection_Policy_Type::EXTERNAL:
				{
					int action = Query_external_controller(pbke, free_block_pool_size);
					switch (action) {
						case 0: gc_candidate_block_id = Select_Block_GREEDY(pbke); break;
						case 1: gc_candidate_block_id = Select_Block_RGA(pbke); break;
						case 2: gc_candidate_block_id = Select_Block_RANDOM(pbke); break;
						case 3: gc_candidate_block_id = Select_Block_RANDOM_P(pbke); break;
						case 4: gc_candidate_block_id = Select_Block_FIFO(pbke); break;
						default:
							PRINT_ERROR("External GC controller returned an action index outside 0..4")
					}
					//GREEDY and RGA always return a candidate by construction.
					//RANDOM, RANDOM_P, and FIFO can come back empty, same as
					//in their own case above: skip this GC opportunity rather
					//than erasing an unvalidated block.
					if (gc_candidate_block_id == block_no_per_plane) {
						return;
					}
					break;
				}
				default:
					break;
			}

			//This should never happen, but we check it here for safty
			if (pbke->Ongoing_erase_operations.find(gc_candidate_block_id) != pbke->Ongoing_erase_operations.end()) {
				return;
			}
			
			NVM::FlashMemory::Physical_Page_Address gc_candidate_address(plane_address);
			gc_candidate_address.BlockID = gc_candidate_block_id;
			Block_Pool_Slot_Type* block = &pbke->Blocks[gc_candidate_block_id];

			//No invalid page to erase
			if (block->Current_page_write_index == 0 || block->Invalid_page_count == 0) {
				return;
			}
			
			//Run the state machine to protect against race condition
			block_manager->GC_WL_started(gc_candidate_address);
			pbke->Ongoing_erase_operations.insert(gc_candidate_block_id);
			address_mapping_unit->Set_barrier_for_accessing_physical_block(gc_candidate_address);//Lock the block, so no user request can intervene while the GC is progressing
			
			//If there are ongoing requests targeting the candidate block, the gc execution should be postponed
			if (block_manager->Can_execute_gc_wl(gc_candidate_address)) {
				Stats::Total_gc_executions++;
				tsu->Prepare_for_transaction_submit();

				NVM_Transaction_Flash_ER* gc_erase_tr = new NVM_Transaction_Flash_ER(Transaction_Source_Type::GC_WL, pbke->Blocks[gc_candidate_block_id].Stream_id, gc_candidate_address);
				//If there are some valid pages in block, then prepare flash transactions for page movement
				if (block->Current_page_write_index - block->Invalid_page_count > 0) {
					NVM_Transaction_Flash_RD* gc_read = NULL;
					NVM_Transaction_Flash_WR* gc_write = NULL;
					for (flash_page_ID_type pageID = 0; pageID < block->Current_page_write_index; pageID++) {
						if (block_manager->Is_page_valid(block, pageID)) {
							Stats::Total_page_movements_for_gc++;
							gc_candidate_address.PageID = pageID;
							if (use_copyback) {
								gc_write = new NVM_Transaction_Flash_WR(Transaction_Source_Type::GC_WL, block->Stream_id, sector_no_per_page * SECTOR_SIZE_IN_BYTE,
									NO_LPA, address_mapping_unit->Convert_address_to_ppa(gc_candidate_address), NULL, 0, NULL, 0, INVALID_TIME_STAMP);
								gc_write->ExecutionMode = WriteExecutionModeType::COPYBACK;
								tsu->Submit_transaction(gc_write);
							} else {
								gc_read = new NVM_Transaction_Flash_RD(Transaction_Source_Type::GC_WL, block->Stream_id, sector_no_per_page * SECTOR_SIZE_IN_BYTE,
									NO_LPA, address_mapping_unit->Convert_address_to_ppa(gc_candidate_address), gc_candidate_address, NULL, 0, NULL, 0, INVALID_TIME_STAMP);
								gc_write = new NVM_Transaction_Flash_WR(Transaction_Source_Type::GC_WL, block->Stream_id, sector_no_per_page * SECTOR_SIZE_IN_BYTE,
									NO_LPA, NO_PPA, gc_candidate_address, NULL, 0, gc_read, 0, INVALID_TIME_STAMP);
								gc_write->ExecutionMode = WriteExecutionModeType::SIMPLE;
								gc_write->RelatedErase = gc_erase_tr;
								gc_read->RelatedWrite = gc_write;
								tsu->Submit_transaction(gc_read);//Only the read transaction would be submitted. The Write transaction is submitted when the read transaction is finished and the LPA of the target page is determined
							}
							gc_erase_tr->Page_movement_activities.push_back(gc_write);
						}
					}
				}
				block->Erase_transaction = gc_erase_tr;
				tsu->Submit_transaction(gc_erase_tr);

				tsu->Schedule();
			}
		}
	}

	flash_block_ID_type GC_and_WL_Unit_Page_Level::Select_Block_GREEDY(PlaneBookKeepingType* pbke)
	{
		//Find the set of blocks with maximum number of invalid pages and no free pages
		flash_block_ID_type gc_candidate_block_id = 0;
		if (pbke->Ongoing_erase_operations.find(0) != pbke->Ongoing_erase_operations.end()) {
			gc_candidate_block_id++;
		}
		for (flash_block_ID_type block_id = 1; block_id < block_no_per_plane; block_id++) {
			if (pbke->Blocks[block_id].Invalid_page_count > pbke->Blocks[gc_candidate_block_id].Invalid_page_count
				&& pbke->Blocks[block_id].Current_page_write_index == pages_no_per_block
				&& is_safe_gc_wl_candidate(pbke, block_id)) {
				gc_candidate_block_id = block_id;
			}
		}
		return gc_candidate_block_id;
	}

	flash_block_ID_type GC_and_WL_Unit_Page_Level::Select_Block_RGA(PlaneBookKeepingType* pbke)
	{
		std::set<flash_block_ID_type> random_set;
		while (random_set.size() < rga_set_size) {
			flash_block_ID_type block_id = random_generator.Uniform_uint(0, block_no_per_plane - 1);
			if (pbke->Ongoing_erase_operations.find(block_id) == pbke->Ongoing_erase_operations.end()
				&& is_safe_gc_wl_candidate(pbke, block_id)) {
				random_set.insert(block_id);
			}
		}
		flash_block_ID_type gc_candidate_block_id = *random_set.begin();
		for (auto &block_id : random_set) {
			if (pbke->Blocks[block_id].Invalid_page_count > pbke->Blocks[gc_candidate_block_id].Invalid_page_count
				&& pbke->Blocks[block_id].Current_page_write_index == pages_no_per_block) {
				gc_candidate_block_id = block_id;
			}
		}
		return gc_candidate_block_id;
	}

	flash_block_ID_type GC_and_WL_Unit_Page_Level::Select_Block_RANDOM(PlaneBookKeepingType* pbke)
	{
		flash_block_ID_type gc_candidate_block_id = random_generator.Uniform_uint(0, block_no_per_plane - 1);
		unsigned int repeat = 0;

		//A write frontier block should not be selected for garbage collection
		while (!is_safe_gc_wl_candidate(pbke, gc_candidate_block_id) && repeat++ < block_no_per_plane) {
			gc_candidate_block_id = random_generator.Uniform_uint(0, block_no_per_plane - 1);
		}
		//No safe candidate was found within the retry budget. block_no_per_plane
		//is not a valid block id, the caller treats it as "none found".
		if (!is_safe_gc_wl_candidate(pbke, gc_candidate_block_id)) {
			return block_no_per_plane;
		}
		return gc_candidate_block_id;
	}

	flash_block_ID_type GC_and_WL_Unit_Page_Level::Select_Block_RANDOM_P(PlaneBookKeepingType* pbke)
	{
		flash_block_ID_type gc_candidate_block_id = random_generator.Uniform_uint(0, block_no_per_plane - 1);
		unsigned int repeat = 0;

		//A write frontier block or a block with free pages should not be selected for garbage collection
		while ((pbke->Blocks[gc_candidate_block_id].Current_page_write_index < pages_no_per_block || !is_safe_gc_wl_candidate(pbke, gc_candidate_block_id))
			&& repeat++ < block_no_per_plane) {
			gc_candidate_block_id = random_generator.Uniform_uint(0, block_no_per_plane - 1);
		}
		//No safe candidate was found within the retry budget, see Select_Block_RANDOM.
		if (pbke->Blocks[gc_candidate_block_id].Current_page_write_index < pages_no_per_block || !is_safe_gc_wl_candidate(pbke, gc_candidate_block_id)) {
			return block_no_per_plane;
		}
		return gc_candidate_block_id;
	}

	flash_block_ID_type GC_and_WL_Unit_Page_Level::Select_Block_RANDOM_PP(PlaneBookKeepingType* pbke)
	{
		flash_block_ID_type gc_candidate_block_id = random_generator.Uniform_uint(0, block_no_per_plane - 1);
		unsigned int repeat = 0;

		//The selected gc block should have a minimum number of invalid pages
		while ((pbke->Blocks[gc_candidate_block_id].Current_page_write_index < pages_no_per_block
			|| pbke->Blocks[gc_candidate_block_id].Invalid_page_count < random_pp_threshold
			|| !is_safe_gc_wl_candidate(pbke, gc_candidate_block_id))
			&& repeat++ < block_no_per_plane) {
			gc_candidate_block_id = random_generator.Uniform_uint(0, block_no_per_plane - 1);
		}
		//No safe candidate was found within the retry budget, see Select_Block_RANDOM.
		if (pbke->Blocks[gc_candidate_block_id].Current_page_write_index < pages_no_per_block
			|| pbke->Blocks[gc_candidate_block_id].Invalid_page_count < random_pp_threshold
			|| !is_safe_gc_wl_candidate(pbke, gc_candidate_block_id)) {
			return block_no_per_plane;
		}
		return gc_candidate_block_id;
	}

	flash_block_ID_type GC_and_WL_Unit_Page_Level::Select_Block_FIFO(PlaneBookKeepingType* pbke)
	{
		//A GC burst on a small device can pop this queue faster than new
		//block allocations refill it. See Select_Block_RANDOM for the
		//block_no_per_plane "none found" sentinel.
		if (pbke->Block_usage_history.empty()) {
			return block_no_per_plane;
		}
		flash_block_ID_type gc_candidate_block_id = pbke->Block_usage_history.front();
		pbke->Block_usage_history.pop();
		return gc_candidate_block_id;
	}

	void GC_and_WL_Unit_Page_Level::Ensure_external_pipes_open()
	{
		if (external_pipes_opened) {
			return;
		}
		if (Device_Parameter_Set::External_GC_State_Pipe_Path.empty() || Device_Parameter_Set::External_GC_Action_Pipe_Path.empty()) {
			PRINT_ERROR("GC_Block_Selection_Policy=EXTERNAL requires External_GC_State_Pipe_Path and External_GC_Action_Pipe_Path to be set")
		}
		//Both open() calls block until the external controller has opened
		//the matching end of that same pipe. This is the standard named-pipe
		//handshake, not a bug: the controller is expected to have created
		//and opened both pipes before launching MQSim.
		external_state_fd = open(Device_Parameter_Set::External_GC_State_Pipe_Path.c_str(), O_WRONLY);
		external_action_fd = open(Device_Parameter_Set::External_GC_Action_Pipe_Path.c_str(), O_RDONLY);
		if (external_state_fd < 0 || external_action_fd < 0) {
			PRINT_ERROR("Failed to open the external GC controller pipes")
		}
		external_pipes_opened = true;
	}

	int GC_and_WL_Unit_Page_Level::Query_external_controller(PlaneBookKeepingType* pbke, unsigned int free_block_pool_size)
	{
		Ensure_external_pipes_open();

		//One line of state: the free-block-pool size, then one
		//block_id:invalid_page_count:write_index:erase_count group per
		//block in this plane, the same fields the built-in heuristics
		//already read.
		std::ostringstream state;
		state << free_block_pool_size;
		for (flash_block_ID_type block_id = 0; block_id < block_no_per_plane; block_id++) {
			state << " " << block_id << ":" << pbke->Blocks[block_id].Invalid_page_count
				<< ":" << pbke->Blocks[block_id].Current_page_write_index
				<< ":" << pbke->Blocks[block_id].Erase_count;
		}
		state << "\n";
		std::string state_line = state.str();
		//A short write on a pipe is legal and must be retried until every
		//byte is sent, raw write() does not guarantee the whole buffer
		//goes in one call the way std::ostream::operator<< implies it does.
		size_t written = 0;
		while (written < state_line.size()) {
			ssize_t n = write(external_state_fd, state_line.c_str() + written, state_line.size() - written);
			if (n <= 0) {
				PRINT_ERROR("Failed to write GC decision state to the external controller")
			}
			written += (size_t)n;
		}

		//Read one line, byte by byte. Wasteful for a large payload, fine
		//for a single decimal digit followed by a newline.
		std::string line;
		char c;
		while (true) {
			ssize_t n = read(external_action_fd, &c, 1);
			if (n == 0) {
				PRINT_ERROR("The external GC controller closed the action pipe before answering a decision")
			}
			if (n < 0) {
				PRINT_ERROR("Failed to read the external controller's action")
			}
			if (c == '\n') {
				break;
			}
			line += c;
		}
		return std::stoi(line);
	}
}
