#include "ParameterUtils.hpp"
#include "PrintConfig.hpp"
#include <boost/log/trivial.hpp>
#include <cassert>
#include <vector>
#include <cstddef>
#include <utility>
#include <string>

namespace Slic3r {

std::vector<LayerPrintSequence> get_other_layers_print_sequence(int sequence_nums, const std::vector<int> &sequence)
{
    std::vector<LayerPrintSequence> res;
    if (sequence_nums <= 0 || sequence.empty())
        return res;

    // The two keys are written together by the GUI, but a config edited by hand or
    // overridden on the command line can carry a count that does not divide the
    // values, or groups too short for a layer range and one extruder. Treat such a
    // pair as no custom sequence rather than reading past the groups.
    size_t item_nums = sequence.size() / size_t(sequence_nums);
    if (sequence.size() % size_t(sequence_nums) != 0 || item_nums < 3) {
        BOOST_LOG_TRIVIAL(warning) << "other_layers_print_sequence_nums " << sequence_nums << " does not fit the "
                                   << sequence.size() << " values of other_layers_print_sequence, ignoring the custom sequence";
        return res;
    }

    res.reserve(sequence_nums);

    for (int i = 0; i < sequence_nums; ++i) {
        std::vector<int> item;
        item.assign(sequence.begin() + i * item_nums, sequence.begin() + ((i + 1) * item_nums));

        std::pair<std::pair<int, int>, std::vector<int>> res_item;
        res_item.first.first  = item[0];
        res_item.first.second = item[1];
        res_item.second.assign(item.begin() + 2, item.end());
        res.emplace_back(std::move(res_item));
    }

    return res;
}

void get_other_layers_print_sequence(const std::vector<LayerPrintSequence> &customize_sequences, int &sequence_nums, std::vector<int> &sequence)
{
    sequence_nums = 0;
    sequence.clear();
    if (customize_sequences.empty()) { return; }

    sequence_nums = (int) customize_sequences.size();
    for (const auto &customize_sequence : customize_sequences) {
        sequence.push_back(customize_sequence.first.first);
        sequence.push_back(customize_sequence.first.second);
        sequence.insert(sequence.end(), customize_sequence.second.begin(), customize_sequence.second.end());
    }
}

int get_index_for_extruder_parameter(const DynamicPrintConfig &config, const std::string &opt_key, int cur_extruder_id, ExtruderType extruder_type, NozzleVolumeType nozzle_volume_type)
{
    std::string  id_name, variant_name;
    unsigned int stride = 1;
    if (printer_options_with_variant_1.count(opt_key) > 0) { // printer parameter
        id_name      = "printer_extruder_id";
        variant_name = "printer_extruder_variant";
    } else if (printer_options_with_variant_2.count(opt_key) > 0) {
        id_name      = "printer_extruder_id";
        variant_name = "printer_extruder_variant";
        stride       = 2;
    } else if (filament_options_with_variant.count(opt_key) > 0) {
        // filament don't use id anymore
        // id_name      = "filament_extruder_id";
        variant_name = "filament_extruder_variant";
    } else if (print_options_with_variant.count(opt_key) > 0) {
        id_name      = "print_extruder_id";
        variant_name = "print_extruder_variant";
    } else {
        return 0;
    }

    // variant index
    int variant_index = config.get_index_for_extruder(cur_extruder_id + 1, id_name, extruder_type, nozzle_volume_type, variant_name, stride);
    if (variant_index < 0) {
        assert(false);
        return 0;
    }

    return variant_index;
}

}; // namespace Slic3r
