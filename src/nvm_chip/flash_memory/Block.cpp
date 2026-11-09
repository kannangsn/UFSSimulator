#include "Block.h"

namespace NVM
{
	namespace FlashMemory
	{
		Block::Block(unsigned int PagesNoPerBlock, flash_block_ID_type BlockID, Flash_Technology_Type Technology)
		{
			ID = BlockID;
			this->Technology = Technology;
			Pages = new Page[PagesNoPerBlock];
		}

		Block::~Block()
		{
		delete[] Pages;
		}
	}
}
