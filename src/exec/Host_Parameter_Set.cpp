#include <algorithm>
#include "Host_Parameter_Set.h"


double Host_Parameter_Set::PCIe_Lane_Bandwidth = 0.4;//uint is GB/s
unsigned int Host_Parameter_Set::PCIe_Lane_Count = 4;
sim_time_type Host_Parameter_Set::SATA_Processing_Delay;//The overall hardware and software processing delay to send/receive a SATA message in nanoseconds
sim_time_type Host_Parameter_Set::UFS_Processing_Delay = 400;//nanoseconds [ASSUMPTION, ufs-design.md section 5]
UFS_Version_Type Host_Parameter_Set::UFS_Version = UFS_Version_Type::UFS_4_1;
unsigned int Host_Parameter_Set::UFS_Gear = 0;//0 = auto: the version's mandatory maximum gear
char Host_Parameter_Set::UFS_Rate_Series = 'B';
unsigned int Host_Parameter_Set::UFS_Lane_Count = 2;
double Host_Parameter_Set::UFS_Link_Efficiency = 0.80;//[ASSUMPTION: 8b10b]
bool Host_Parameter_Set::Enable_ResponseTime_Logging = false;
sim_time_type Host_Parameter_Set::ResponseTime_Logging_Period_Length = 400000;//nanoseconds
std::string Host_Parameter_Set::Input_file_path;
std::vector<IO_Flow_Parameter_Set*> Host_Parameter_Set::IO_Flow_Definitions;

void Host_Parameter_Set::XML_serialize(Utils::XmlWriter& xmlwriter)
{
	std::string tmp;
	tmp = "Host_Parameter_Set";
	xmlwriter.Write_open_tag(tmp);

	std::string attr = "PCIe_Lane_Bandwidth";
	std::string val = std::to_string(PCIe_Lane_Bandwidth);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "PCIe_Lane_Count";
	val = std::to_string(PCIe_Lane_Count);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "SATA_Processing_Delay";
	val = std::to_string(SATA_Processing_Delay);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "UFS_Processing_Delay";
	val = std::to_string(UFS_Processing_Delay);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "UFS_Version";
	val = (UFS_Version == UFS_Version_Type::UFS_5_0 ? "5.0" : "4.1");
	xmlwriter.Write_attribute_string(attr, val);

	attr = "UFS_Gear";
	val = std::to_string(UFS_Gear);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "UFS_Rate_Series";
	val = std::string(1, UFS_Rate_Series);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "UFS_Lane_Count";
	val = std::to_string(UFS_Lane_Count);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "UFS_Link_Efficiency";
	val = std::to_string(UFS_Link_Efficiency);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Enable_ResponseTime_Logging";
	val = (Enable_ResponseTime_Logging ? "true" : "false");
	xmlwriter.Write_attribute_string(attr, val);

	attr = "ResponseTime_Logging_Period_Length";
	val = std::to_string(ResponseTime_Logging_Period_Length);
	xmlwriter.Write_attribute_string(attr, val);

	xmlwriter.Write_close_tag();
}

void Host_Parameter_Set::XML_deserialize(rapidxml::xml_node<> *node)
{
	try {
		for (auto param = node->first_node(); param; param = param->next_sibling()) {
			if (strcmp(param->name(), "PCIe_Lane_Bandwidth") == 0) {
				std::string val = param->value();
				PCIe_Lane_Bandwidth = std::stod(val);
			} else if (strcmp(param->name(), "PCIe_Lane_Count") == 0) {
				std::string val = param->value();
				PCIe_Lane_Count = std::stoul(val);
			} else if (strcmp(param->name(), "SATA_Processing_Delay") == 0) {
				std::string val = param->value();
				SATA_Processing_Delay = std::stoul(val);
			} else if (strcmp(param->name(), "UFS_Processing_Delay") == 0) {
				std::string val = param->value();
				UFS_Processing_Delay = std::stoul(val);
			} else if (strcmp(param->name(), "UFS_Version") == 0) {
				std::string val = param->value();
				if (val.compare("4.1") == 0) {
					UFS_Version = UFS_Version_Type::UFS_4_1;
				} else if (val.compare("5.0") == 0) {
					UFS_Version = UFS_Version_Type::UFS_5_0;
				} else {
					PRINT_ERROR("UFS_Version must be 4.1 or 5.0")
				}
			} else if (strcmp(param->name(), "UFS_Gear") == 0) {
				std::string val = param->value();
				UFS_Gear = std::stoul(val);
				if (UFS_Gear > 6) {
					PRINT_ERROR("UFS_Gear must be between 0 (auto) and 6; the version compatibility check happens at link construction")
				}
			} else if (strcmp(param->name(), "UFS_Rate_Series") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (val.compare("A") == 0) {
					UFS_Rate_Series = 'A';
				} else if (val.compare("B") == 0) {
					UFS_Rate_Series = 'B';
				} else {
					PRINT_ERROR("UFS_Rate_Series must be A or B")
				}
			} else if (strcmp(param->name(), "UFS_Lane_Count") == 0) {
				std::string val = param->value();
				UFS_Lane_Count = std::stoul(val);
				if (UFS_Lane_Count < 1 || UFS_Lane_Count > 2) {
					PRINT_ERROR("UFS_Lane_Count must be 1 or 2 (the spec maximum is 2 lanes per direction)")
				}
			} else if (strcmp(param->name(), "UFS_Link_Efficiency") == 0) {
				std::string val = param->value();
				UFS_Link_Efficiency = std::stod(val);
				if (UFS_Link_Efficiency <= 0.0 || UFS_Link_Efficiency > 1.0) {
					PRINT_ERROR("UFS_Link_Efficiency must be in (0, 1]")
				}
			} else if (strcmp(param->name(), "Enable_ResponseTime_Logging") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				Enable_ResponseTime_Logging = (val.compare("FALSE") == 0 ? false : true);
			} else if (strcmp(param->name(), "ResponseTime_Logging_Period_Length") == 0) {
				std::string val = param->value();
				ResponseTime_Logging_Period_Length = std::stoul(val);
			}
		}
	} catch (...) {
		PRINT_ERROR("Error in the Host_Parameter_Set!")
	}
}
