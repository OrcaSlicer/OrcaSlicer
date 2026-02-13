#ifndef slic3r_Parameter_Utils_hpp_
#define slic3r_Parameter_Utils_hpp_

#include <vector>
#include <map>
#include "PrintConfig.hpp"

namespace Slic3r {
using LayerPrintSequence = std::pair<std::pair<int, int>, std::vector<int>>;
std::vector<LayerPrintSequence> get_other_layers_print_sequence(int sequence_nums, const std::vector<int> &sequence);
void get_other_layers_print_sequence(const std::vector<LayerPrintSequence> &customize_sequences, int &sequence_nums, std::vector<int> &sequence);

extern int get_index_for_extruder_parameter(const DynamicPrintConfig &config, const std::string &opt_key, int cur_extruder_id, ExtruderType extruder_type, NozzleVolumeType nozzle_volume_type);
} // namespace Slic3r

#endif // slic3r_Parameter_Utils_hpp_
