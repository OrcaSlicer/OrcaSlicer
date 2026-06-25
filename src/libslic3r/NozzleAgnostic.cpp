#include "NozzleAgnostic.hpp"

#include <algorithm>
#include <cmath>

#include "PrintConfig.hpp"
#include "Preset.hpp"
#include "PresetBundle.hpp"

namespace Slic3r {
namespace nozzle_agnostic {

// Nozzle diameters within this tolerance (mm) are treated as equal.
static const double NOZZLE_EPSILON = 1e-6;

double to_percent(double abs_mm, double base_nozzle)
{
    return abs_mm / base_nozzle * 100.0;
}

ConfigOptionFloatOrPercent convert_field(const ConfigOptionFloatOrPercent &in, double base_nozzle)
{
    if (in.percent || base_nozzle <= 0.0)
        return in;
    // Round the percentage to 0.1 (one decimal place) so the converted values stay clean/readable
    // (e.g. 112.5%, not 112.53125%). The per-toolhead absolute width is recomputed from this % at
    // slice time, so a 0.1% rounding is a sub-micron width difference -- below printable resolution.
    const double pct = std::round(to_percent(in.value, base_nozzle) * 10.0) / 10.0;
    return ConfigOptionFloatOrPercent(pct, /*percent=*/true);
}

bool is_in_scope(const ConfigOptionDef &def)
{
    return def.type == coFloatOrPercent && def.ratio_over == "nozzle_diameter";
}

std::vector<std::string> scoped_keys(const ConfigDef &def_registry)
{
    std::vector<std::string> keys;
    for (const auto &kv : def_registry.options)
        if (is_in_scope(kv.second))
            keys.push_back(kv.first);
    return keys;
}

bool is_mixed_nozzle(const std::vector<double> &nozzles)
{
    if (nozzles.size() < 2)
        return false;
    const double first = nozzles.front();
    for (const double d : nozzles)
        if (std::abs(d - first) > NOZZLE_EPSILON)
            return true;
    return false;
}

bool has_absolute_scoped_fields(const DynamicPrintConfig &process_cfg)
{
    const ConfigDef *def_registry = process_cfg.def();
    if (def_registry == nullptr)
        return false;
    for (const std::string &key : scoped_keys(*def_registry)) {
        const auto *fop = dynamic_cast<const ConfigOptionFloatOrPercent *>(process_cfg.option(key));
        if (fop != nullptr && !fop->percent)
            return true;
    }
    return false;
}

std::optional<double> resolve_base_nozzle(const Preset &process, const PresetBundle &bundle)
{
    const auto *compatible_printers =
        dynamic_cast<const ConfigOptionStrings *>(process.config.option("compatible_printers"));
    if (compatible_printers == nullptr || compatible_printers->values.empty())
        return std::nullopt;

    std::vector<double> bases;
    for (const std::string &printer_name : compatible_printers->values) {
        const Preset *machine = bundle.printers.find_preset(printer_name);
        if (machine == nullptr)
            continue;
        const auto *nozzle_diameter =
            dynamic_cast<const ConfigOptionFloats *>(machine->config.option("nozzle_diameter"));
        if (nozzle_diameter == nullptr)
            continue;
        for (const double d : nozzle_diameter->values) {
            const bool seen = std::any_of(bases.begin(), bases.end(),
                [d](double b) { return std::abs(b - d) <= NOZZLE_EPSILON; });
            if (!seen)
                bases.push_back(d);
        }
    }

    // Exactly one distinct base -> divisor; zero or several -> refuse.
    return bases.size() == 1 ? std::optional<double>(bases.front()) : std::nullopt;
}

} // namespace nozzle_agnostic
} // namespace Slic3r
