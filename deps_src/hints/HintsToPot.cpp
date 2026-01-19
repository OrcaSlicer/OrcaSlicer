#include <iostream>
#include <vector>
#include <string>
#include <boost/filesystem.hpp>
#include <boost/dll.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/algorithm/string/predicate.hpp>

bool write_to_pot(boost::filesystem::path path, const std::vector<std::pair<std::string, std::string>>& data)
{
	boost::nowide::ofstream file(path.string(), std::ios_base::app);
	for (const auto& element : data)
	{
		//Example of .pot element 
		//#: src/slic3r/GUI/GUI_App.cpp:1647 src/slic3r/GUI/wxExtensions.cpp:687
		//msgctxt "Mode"
		//msgid "Advanced"
		//msgstr ""
		file << "\n#: resources/data/hints.ini: ["<< element.first << "]\nmsgid \"" << element.second << "\"\nmsgstr \"\"\n";
	}
	file.close();
	return true;
}
bool read_hints_ini(boost::filesystem::path path, std::vector<std::pair<std::string, std::string>>& pot_elements)
{
	namespace pt = boost::property_tree;
	pt::ptree tree;
	boost::nowide::ifstream ifs(path.string());
	try {
		pt::read_ini(ifs, tree);
	}
	catch (const boost::property_tree::ini_parser::ini_parser_error& err) {
		std::cout << err.what() << std::endl;
		return false;
	}
	for (const auto& section : tree) {
		if (boost::starts_with(section.first, "hint:")) {
			for (const auto& data : section.second) {
				if (data.first == "text")
				{
					pot_elements.emplace_back(section.first, data.second.data());
					break;
				}
			}
		}
	}
	return true;
}

int main(int argc, char* argv[])
{
	std::vector<std::pair<std::string, std::string>> data;
	boost::filesystem::path path_to_ini;
	boost::filesystem::path path_to_pot;
	if (argc != 3)
	{
		std::cout << "HINTS_TO_POT FAILED: WRONG NUM OF ARGS" << std::endl;
		return -1;
	}
	try {
		path_to_ini = boost::filesystem::canonical(boost::filesystem::path(argv[1])).parent_path() / "resources" / "data" / "hints.ini";
		path_to_pot = boost::filesystem::canonical(boost::filesystem::path(argv[2])).parent_path() / "i18n" /"OrcaSlicer.pot";
	} catch (std::exception&) {
		std::cout << "HINTS_TO_POT FAILED: BOOST CANNONICAL" << std::endl;
		return -1;
	}
	
	if (!boost::filesystem::exists(path_to_ini)){
		std::cout << "HINTS_TO_POT FAILED: PATH TO INI DOES NOT EXISTS" << std::endl;
		std::cout << path_to_ini.string() << std::endl;
		return -1;
	}
	if (!read_hints_ini(std::move(path_to_ini), data)) {
		std::cout << "HINTS_TO_POT FAILED TO READ HINTS INI" << std::endl;
		return -1;
	}
	if (!write_to_pot(std::move(path_to_pot), data)) {
		std::cout << "HINTS_TO_POT FAILED TO WRITE POT FILE" << std::endl;
		return -1;
	}
	std::cout << "HINTS_TO_POT SUCCESS" << std::endl;
    return 0;
}
