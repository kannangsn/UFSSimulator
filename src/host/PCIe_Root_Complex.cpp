#include "PCIe_Root_Complex.h"
#include "UFS_HBA.h"


namespace Host_Components
{
	PCIe_Root_Complex::PCIe_Root_Complex(PCIe_Link* pcie_link, HostInterface_Types SSD_device_type, SATA_HBA* sata_hba, std::vector<Host_Components::IO_Flow_Base*>* IO_flows) :
		pcie_link(pcie_link), SSD_device_type(SSD_device_type), sata_hba(sata_hba), ufs_hba(NULL), IO_flows(IO_flows) {}

	void PCIe_Root_Complex::Set_ufs_hba(UFS_HBA* ufs_hba)
	{
		this->ufs_hba = ufs_hba;
	}

	void PCIe_Root_Complex::Write_to_memory(const uint64_t address, const void* payload)
	{
		//This is a request to write back a read request data into memory (in modern systems the write is done to LLC)
		if (address >= DATA_MEMORY_REGION) {
			//nothing to do
		} else {
			switch (SSD_device_type) {
				case HostInterface_Types::NVME:
				{
					unsigned int flow_id = QUEUE_ID_TO_FLOW_ID(((Completion_Queue_Entry*)payload)->SQ_ID);
					((*IO_flows)[flow_id])->NVMe_consume_io_request((Completion_Queue_Entry*)payload);
					break;
				}
				case HostInterface_Types::SATA:
					sata_hba->SATA_consume_io_request((Completion_Queue_Entry*)payload);
					break;
				case HostInterface_Types::UFS:
					//routes UFS_Completion_Queue_Entry to the UFS_HBA. Implemented in Phase B Day 7.
					PRINT_ERROR("UFS completion consumption is not implemented yet (plan.md section 7, Phase B Day 7)")
					break;
				default:
					PRINT_ERROR("Uknown Host Interface type in PCIe_Root_Complex")
			}
		}
	}

	void PCIe_Root_Complex::Write_to_device(uint64_t address, uint16_t write_value)
	{
		PCIe_Message* pcie_message = new Host_Components::PCIe_Message;
		pcie_message->Type = PCIe_Message_Type::WRITE_REQ;
		pcie_message->Destination = Host_Components::PCIe_Destination_Type::DEVICE;
		pcie_message->Address = address;
		pcie_message->Payload = (void*)(intptr_t)write_value;
		pcie_message->Payload_size = sizeof(write_value);
		pcie_link->Deliver(pcie_message);
	}

	void PCIe_Root_Complex::Read_from_memory(const uint64_t address, const unsigned int read_size)
	{
		PCIe_Message* new_pcie_message = new Host_Components::PCIe_Message;
		new_pcie_message->Type = PCIe_Message_Type::READ_COMP;
		new_pcie_message->Destination = Host_Components::PCIe_Destination_Type::DEVICE;
		new_pcie_message->Address = address;

		//This is a request to read the data of a write request
		if (address >= DATA_MEMORY_REGION) {
			//nothing to do
			new_pcie_message->Payload_size = read_size;
			new_pcie_message->Payload = NULL;//No need to transfer data in the standalone mode of MQSim
		} else {
			switch (SSD_device_type) {
				case HostInterface_Types::NVME:
				{
					uint16_t flow_id = QUEUE_ID_TO_FLOW_ID(uint16_t(address >> NVME_COMP_Q_MEMORY_REGION));
					new_pcie_message->Payload = (*IO_flows)[flow_id]->NVMe_read_sqe(address);
					new_pcie_message->Payload_size = sizeof(Submission_Queue_Entry);
					break;
				}
				case HostInterface_Types::SATA:
					new_pcie_message->Payload = sata_hba->Read_ncq_entry(address);
					new_pcie_message->Payload_size = sizeof(Submission_Queue_Entry);
					break;
				case HostInterface_Types::UFS:
					//two-step fetch (ufs-design.md section 3): the region-kind bits of
					//the address say whether this is the UTRD read (32 B) or the UCD
					//read (Command UPIU + PRDT, 48 B). The UFS payload sizes
					//deliberately differ from NVMe's sizeof(Submission_Queue_Entry)
					switch (UFS_MEMORY_REGION_KIND(address)) {
						case 0x0://SQ ring: UTRD
							new_pcie_message->Payload = ufs_hba->Read_utrd(address);
							new_pcie_message->Payload_size = sizeof(UTP_Transfer_Request_Descriptor);
							break;
						case 0x1://UCD area: Command UPIU + PRDT
							new_pcie_message->Payload = ufs_hba->Read_ucd(address);
							new_pcie_message->Payload_size = sizeof(UFS_Command_UPIU) + sizeof(UFS_PRDT_Entry);
							break;
						default:
							PRINT_ERROR("UFS: device read from an unknown host memory region!")
					}
					break;
				default:
					//this switch previously had no default case, so an unmatched type
					//fell through without setting new_pcie_message->Payload, leaving it
					//uninitialized instead of triggering an explicit error (see plan.md section 6)
					PRINT_ERROR("Unknown Host Interface type in PCIe_Root_Complex")
			}
		}

		pcie_link->Deliver(new_pcie_message);
	}
	
	void PCIe_Root_Complex::Set_io_flows(std::vector<Host_Components::IO_Flow_Base*>* IO_flows)
	{
		this->IO_flows = IO_flows;
	}
}
