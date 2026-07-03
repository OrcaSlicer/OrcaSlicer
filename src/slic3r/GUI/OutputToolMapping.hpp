#ifndef slic3r_GUI_OutputToolMapping_hpp_
#define slic3r_GUI_OutputToolMapping_hpp_

#include <map>
#include <string>
#include <boost/filesystem/path.hpp>

class wxWindow;

namespace Slic3r {
namespace GUI {

class Plater;

bool show_output_tool_mapping_dialog(wxWindow* parent, Plater* plater, int plate_idx, std::map<int, int>& output_tool_mapping);
bool has_non_identity_tool_mapping(const std::map<int, int>& output_tool_mapping);
bool remap_gcode_file_tools(const boost::filesystem::path& input_path,
                            const boost::filesystem::path& output_path,
                            const std::map<int, int>& output_tool_mapping,
                            std::string* error = nullptr);

} // namespace GUI
} // namespace Slic3r

#endif
