#ifndef NVME_DEFINITIONS_H
#define NVME_DEFINITIONS_H

#include <cstdint>
#include <string>
#include "Host_Interface_NVMe_Priorities.h"

enum class HostInterface_Types { SATA, NVME, UFS };

#define NVME_FLUSH_OPCODE 0x0000
#define NVME_WRITE_OPCODE 0x0001
#define NVME_READ_OPCODE 0x0002

#define SATA_WRITE_OPCODE 0x0001
#define SATA_READ_OPCODE 0x0002

//SCSI opcodes carried in the UFS Command UPIU's CDB (JESD220G clause 11).
//READ(16)/WRITE(16) rather than the (10) variants: LHA_type is 64-bit and
//READ(10)'s 32-bit LBA field would silently truncate large addresses.
#define UFS_READ_OPCODE 0x88  //SCSI READ(16)
#define UFS_WRITE_OPCODE 0x8A //SCSI WRITE(16)

const uint64_t NCQ_SUBMISSION_REGISTER = 0x1000;
const uint64_t NCQ_COMPLETION_REGISTER = 0x1003;
const uint64_t SUBMISSION_QUEUE_REGISTER_0 = 0x1000;
const uint64_t COMPLETION_QUEUE_REGISTER_0 = 0x1003;
const uint64_t SUBMISSION_QUEUE_REGISTER_1 = 0x1010;
const uint64_t COMPLETION_QUEUE_REGISTER_1 = 0x1013;
const uint64_t SUBMISSION_QUEUE_REGISTER_2 = 0x1020;
const uint64_t COMPLETION_QUEUE_REGISTER_2 = 0x1023;
const uint64_t SUBMISSION_QUEUE_REGISTER_3 = 0x1030;
const uint64_t COMPLETION_QUEUE_REGISTER_3 = 0x1033;
const uint64_t SUBMISSION_QUEUE_REGISTER_4 = 0x1040;
const uint64_t COMPLETION_QUEUE_REGISTER_4 = 0x1043;
const uint64_t SUBMISSION_QUEUE_REGISTER_5 = 0x1050;
const uint64_t COMPLETION_QUEUE_REGISTER_5 = 0x1053;
const uint64_t SUBMISSION_QUEUE_REGISTER_6 = 0x1060;
const uint64_t COMPLETION_QUEUE_REGISTER_6 = 0x1063;
const uint64_t SUBMISSION_QUEUE_REGISTER_7 = 0x1070;
const uint64_t COMPLETION_QUEUE_REGISTER_7 = 0x1073;
const uint64_t SUBMISSION_QUEUE_REGISTER_8 = 0x1080;
const uint64_t COMPLETION_QUEUE_REGISTER_8 = 0x1083;

struct Completion_Queue_Entry
{
	uint32_t Command_specific;
	uint32_t Reserved;
	uint16_t SQ_Head; //SQ Head Pointer, Indicates the current Submission Queue Head pointer for the Submission Queue indicated in the SQ Identifier field
	uint16_t SQ_ID;//SQ Identifier, Indicates the Submission Queue to which the associated command was issued to.
	uint16_t Command_Identifier;//Command Identifier, Indicates the identifier of the command that is being completed
	uint16_t SF_P; //Status Field (SF)+ Phase Tag(P)
				   //SF: Indicates status for the command that is being completed
				   //P:Identifies whether a Completion Queue entry is new
};

struct Submission_Queue_Entry
{
	uint8_t Opcode;//Is it a read or write request
	uint8_t PRP_FUSE;
	uint16_t Command_Identifier;//The id of the command in the I/O submission queue
	uint64_t Namespace_identifier;
	uint64_t Reserved;
	uint64_t Metadata_pointer_1;
	uint64_t PRP_entry_1;
	uint64_t PRP_entry_2;
	uint32_t Command_specific[6];
};

//======================= UFS (MCQ mode, UFSHCI 4.1/5.0) =======================
//See ufs-design.md for the full design. UFS registers are in their own block at
//0x2000, deliberately disjoint from NVMe's 0x1000 block above. Doorbell values are
//slot indices (the spec's DWord-offset encoding and SQDAOy/CQDAOy indirection are
//abstracted away as timing-neutral, ufs-design.md section 1.1).

const uint64_t UFS_DOORBELL_BLOCK_BASE = 0x2000;
const uint64_t UFS_DOORBELL_QUEUE_STRIDE = 0x10;
#define UFS_SQ_TAIL_DOORBELL(Q) (UFS_DOORBELL_BLOCK_BASE + (Q) * UFS_DOORBELL_QUEUE_STRIDE)
#define UFS_CQ_HEAD_DOORBELL(C) (UFS_DOORBELL_BLOCK_BASE + 0x8 + (C) * UFS_DOORBELL_QUEUE_STRIDE)

//MCQConfig.AS analogue (JESD223F 5.9.2): no weighted-round-robin exists in UFS
enum class UFS_Arbitration_Type { ROUND_ROBIN, STRICT_PRIORITY };

//HS-Gear per-lane raw PHY rates in Mbps, exact spec values (JESD220G Table 6.7,
//confirmed identical in JESD220H which adds HS-GEAR6 for UFS 5.0). The Rate-B
//column is the fref 19.2/38.4 MHz series. The 26.0/52.0 MHz variant differs
//slightly and is not exposed as a knob. Raw PHY rate only. Line-coding
//efficiency is applied separately (UFS_Link_Efficiency [ASSUMPTION], no
//spec-stated effective-throughput figure exists. See ufs-design.md section 5).
inline double UFS_gear_rate_mbps(unsigned int gear, bool rate_series_b)
{
	static const double rate_a_mbps[6] = { 1248.0, 2496.0, 4992.0, 9984.0, 19968.0, 39936.0 };
	static const double rate_b_mbps[6] = { 1459.2, 2918.4, 5836.8, 11673.6, 23347.2, 46694.4 };
	if (gear < 1 || gear > 6) {
		return 0.0;//rejected at config parse. Never reached in a validated run
	}
	return rate_series_b ? rate_b_mbps[gear - 1] : rate_a_mbps[gear - 1];
}

//UTP Transfer Request Descriptor = MCQ SQ entry, 8 DWORDs (JESD223F 6.1.1/6.2.1).
//DW1/DW3 crypto fields are carried for layout fidelity, never interpreted.
struct UTP_Transfer_Request_Descriptor
{
	uint32_t DW0_Header;         //Command Type (31:28), DD (26:25), I (24), CCI (7:0)
	uint32_t DW1_DUN_Lower;
	uint32_t DW2_OCS;            //unused in MCQ mode (OCS is in the CQ entry), holds 0x0F (invalid)
	uint32_t DW3_DUN_Upper;
	uint32_t DW4_UCD_Base_Lower;
	uint32_t DW5_UCD_Base_Upper;
	uint32_t DW6_Response_UPIU;  //offset (31:16), length (15:0), in DWords
	uint32_t DW7_PRDT;           //offset (31:16), length in entries (15:0)
};

//MCQ Completion Queue entry, 8 DWORDs (JESD223F 6.2.2, Figure 14)
struct UFS_Completion_Queue_Entry
{
	uint32_t DW0;                //UCDLBA (31:7) | SQID (4:0)
	uint32_t DW1;                //UCD base upper
	uint32_t DW2;                //Response UPIU offset (31:16) | length (15:0)
	uint32_t DW3;                //PRDT offset (31:16) | length (15:0)
	uint32_t DW4;                //Extended Error Code (15:8) | OCS (7:0)
	uint32_t DW5;                //EXT_IID (23:20) | IID (19:16) | LUN (15:8) | Task Tag (7:0)
	uint32_t DW6_Reserved;
	uint32_t DW7_Reserved;
};

//Command UPIU, 32 bytes fixed (JESD220G Table 10.13): 12-byte basic header +
//Expected Data Transfer Length + 16-byte CDB inline. Only Transaction_Type, LUN,
//Task_Tag, Expected_Data_Transfer_Length and the CDB opcode/LBA/length are ever
//interpreted by the simulator (ufs-design.md section 2.3).
struct UFS_Command_UPIU
{
	uint8_t Transaction_Type;
	uint8_t Flags;
	uint8_t LUN;
	uint8_t Task_Tag;
	uint8_t IID_Command_Set_Type;
	uint8_t Reserved0;
	uint8_t Reserved1;
	uint8_t Reserved2;
	uint8_t Total_EHS_Length;
	uint8_t Reserved3;
	uint16_t Data_Segment_Length;
	uint32_t Expected_Data_Transfer_Length;
	uint8_t CDB[16];
};

//Response UPIU, 32 bytes (JESD220G 10.7.2), written back to the UCD response
//region on completion so its link cost is modeled (ufs-design.md section 3 step 7)
struct UFS_Response_UPIU
{
	uint8_t Transaction_Type;
	uint8_t Flags;
	uint8_t LUN;
	uint8_t Task_Tag;
	uint8_t IID_Command_Set_Type;
	uint8_t Reserved0;
	uint8_t Response;
	uint8_t Status;
	uint8_t Total_EHS_Length;
	uint8_t Device_Information;
	uint16_t Data_Segment_Length;
	uint32_t Residual_Transfer_Count;
	uint8_t Reserved[16];
};

//PRDT entry, 4 DWORDs (JESD223F 6.1), one entry per request with dummy data
//addresses, mirroring NVMe's dummy PRP pointers (ufs-design.md section 2.3)
struct UFS_PRDT_Entry
{
	uint32_t Data_Base_Address;
	uint32_t Data_Base_Address_Upper;
	uint32_t Reserved;
	uint32_t Data_Byte_Count;    //bits 17:0, 0's based, max 256 KB per segment
};

#endif // !NVME_DEFINISIONS_H
