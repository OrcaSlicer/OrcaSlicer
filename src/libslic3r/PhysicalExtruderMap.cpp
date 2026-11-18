#include "libslic3r/PhysicalExtruderMap.hpp"

#include <algorithm>
#include <numeric>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace Slic3r {

ConfigOptionInts effective_physical_extruder_map(const ConfigOptionInts* explicit_pem, int nozzle_count)
{
    // One entry per logical extruder, so a profile has authored a map only when its length
    // matches. The single-element PrintConfig default does not, and is replaced rather than
    // treated as a one-extruder machine.
    if (explicit_pem && nozzle_count > 0 && (int) explicit_pem->values.size() == nozzle_count)
        return *explicit_pem;

    ConfigOptionInts derived;
    derived.values.resize(std::max(nozzle_count, 1));
    std::iota(derived.values.begin(), derived.values.end(), 0);
    return derived;
}

ConfigOptionInts effective_physical_extruder_map(const PresetBundle& pb)
{
    const ConfigOptionInts* explicit_pem = pb.project_config.option<ConfigOptionInts>("physical_extruder_map");
    if (!explicit_pem || explicit_pem->values.size() < 2)
        explicit_pem = pb.printers.get_edited_preset().config.option<ConfigOptionInts>("physical_extruder_map");
    const auto* nozzles = pb.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    return effective_physical_extruder_map(explicit_pem, nozzles ? (int) nozzles->values.size() : 0);
}

} // namespace Slic3r
