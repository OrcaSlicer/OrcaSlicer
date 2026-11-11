#ifndef slic3r_Parameter_Utils_hpp_
#define slic3r_Parameter_Utils_hpp_

#include <utility>
#include <string>
#include <vector>
#include <map>
#include "PrintConfig.hpp"

namespace Slic3r {
using LayerPrintSequence = std::pair<std::pair<int, int>, std::vector<int>>;
std::vector<LayerPrintSequence> get_other_layers_print_sequence(int sequence_nums, const std::vector<int> &sequence);
void get_other_layers_print_sequence(const std::vector<LayerPrintSequence> &customize_sequences, int &sequence_nums, std::vector<int> &sequence);

extern int get_index_for_extruder_parameter(const DynamicPrintConfig &config, const std::string &opt_key, int cur_extruder_id, ExtruderType extruder_type, NozzleVolumeType nozzle_volume_type);

// Gives each tool of a per-tool process its own values. Options with fewer values than columns are padded with their
// first value, and printer tools without columns get one per variant, copied from tool 1's column of that variant.
// Returns whether anything changed.
bool complete_process_tool_columns(DynamicPrintConfig &process_config, const DynamicPrintConfig &printer_config);
} // namespace Slic3r

#endif // slic3r_Parameter_Utils_hpp_
