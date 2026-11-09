#include "Plane.h"

namespace NVM
{
	namespace FlashMemory
	{
		Plane::Plane(unsigned int BlocksNoPerPlane, unsigned int PagesNoPerBlock, Flash_Technology_Type BaseTechnology, unsigned int SLCBlockCount) :
			Read_count(0), Progam_count(0), Erase_count(0)
		{
			Healthy_block_no = BlocksNoPerPlane;
			Blocks = new Block*[BlocksNoPerPlane];
			for (unsigned int i = 0; i < BlocksNoPerPlane; i++) {
				//The first SLCBlockCount blocks in the plane are a fixed SLC
				//region, the rest stay at the chip's base technology. Static
				//for now: nothing moves a block between technologies yet,
				//that is the relocation path, not this per-block tag.
				Blocks[i] = new Block(PagesNoPerBlock, i, (i < SLCBlockCount) ? Flash_Technology_Type::SLC : BaseTechnology);
			}
			Allocated_streams = NULL;
		}

		Plane::~Plane()
		{
			for (unsigned int i = 0; i < Healthy_block_no; i++) {
				delete Blocks[i];
			}
			delete[] Blocks;
		}
	}
}