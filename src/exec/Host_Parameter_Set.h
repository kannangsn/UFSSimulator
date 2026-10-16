#ifndef HOST_PARAMETER_SEt_H
#define HOST_PARAMETER_SEt_H

#include <vector>
#include "Parameter_Set_Base.h"
#include "IO_Flow_Parameter_Set.h"

class Host_Parameter_Set : public Parameter_Set_Base
{
public:
	static double PCIe_Lane_Bandwidth;//uint is GB/s
	static unsigned int PCIe_Lane_Count;
	static sim_time_type SATA_Processing_Delay;//The overall hardware and software processing delay to send/receive a SATA message in nanoseconds
	static sim_time_type UFS_Processing_Delay;//UFS_HBA UTP/UniPro stack latency in nanoseconds [ASSUMPTION: no spec value exists, chosen by analogy with SATA_Processing_Delay. See ufs-design.md section 5]
	//UFS link parameters (ufs-design.md section 5). Ignored unless HostInterface_Type is UFS
	static unsigned int UFS_Gear;//HS-Gear 1..6. 5 = UFS 4.1 mandatory max, 6 = UFS 5.0 (exact per-lane rates from JESD220G/H Table 6.7)
	static char UFS_Rate_Series;//'A' or 'B' (Rate-B = fref 19.2/38.4 MHz column)
	static unsigned int UFS_Lane_Count;//1..2 lanes per direction (spec max is 2, unchanged through UFS 5.0)
	static double UFS_Link_Efficiency;//effective-throughput factor on the raw PHY rate [ASSUMPTION: 0.80 for 8b10b line coding. No spec-stated figure exists]
	static bool Enable_ResponseTime_Logging;
	static sim_time_type ResponseTime_Logging_Period_Length;
	static std::vector<IO_Flow_Parameter_Set*> IO_Flow_Definitions;
	static std::string Input_file_path;//This parameter is not serialized. This is used to inform the Host_System class about the input file path.

	void XML_serialize(Utils::XmlWriter& xmlwriter);
	void XML_deserialize(rapidxml::xml_node<> *node);
};

#endif // !HOST_PARAMETER_SEt_H
