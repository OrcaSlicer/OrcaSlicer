#pragma once

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <clocale>
#include <initializer_list>
#include <string>
#include <vector>

namespace filament_test {

// Switches LC_CTYPE to the first available single-byte Latin locale for the scope, so a check that must not depend on the
// locale sees letters above 0x7F. Stays in the current locale where none is available.
struct ScopedLatinCtype
{
    std::string previous;
    ScopedLatinCtype()
    {
        if (const char* current = std::setlocale(LC_CTYPE, nullptr))
            previous = current;
        for (const char* name : {"tr_TR.ISO8859-9", "tr_TR.ISO-8859-9", "tr_TR.iso88599", "en_US.ISO8859-1", "en_US.ISO-8859-1",
                                 "en_US.iso88591", ".1254", ".1252"})
            if (std::setlocale(LC_CTYPE, name))
                break;
    }
    ~ScopedLatinCtype()
    {
        if (!previous.empty())
            std::setlocale(LC_CTYPE, previous.c_str());
    }
};

// A standalone filament collection, built the same way PresetBundle builds its own.
struct FilamentTestCollection : public Slic3r::PresetCollection
{
    FilamentTestCollection()
        : Slic3r::PresetCollection(Slic3r::Preset::TYPE_FILAMENT,
                                   Slic3r::Preset::filament_options(),
                                   static_cast<const Slic3r::PrintRegionConfig&>(Slic3r::FullPrintConfig::defaults()))
    {}
};

struct FilamentSpec
{
    const char* name;
    const char* filament_id;
    const char* filament_type;
    const char* filament_vendor;
    bool is_system;
};

inline const std::vector<FilamentSpec> ecc2_filaments{{"Elegoo PLA @ECC2", "OFvgE0Zh", "PLA", "Elegoo", true},
                                                      {"Elegoo PLA+ @ECC2", "OFBUIlZ7", "PLA", "Elegoo", true},
                                                      {"Elegoo Rapid PLA+ @ECC2", "OFkhhUS0", "PLA", "Elegoo", true},
                                                      {"Elegoo PLA Silk @ECC2", "SILK", "PLA", "Elegoo", true},
                                                      {"Elegoo PLA PRO @ECC2", "PRO", "PLA", "Elegoo", true},
                                                      {"Elegoo PETG @ECC2", "PETG1", "PETG", "Elegoo", true},
                                                      {"Generic PLA @System", "GENPLA", "PLA", "Generic", true}};

// Stamp the matcher's fields in a second pass: load_preset() keeps the presets sorted, so a
// reference taken while loading can be left dangling by a later load.
inline void populate(Slic3r::PresetCollection& filaments, const std::vector<FilamentSpec>& specs)
{
    for (const FilamentSpec& spec : specs) {
        Slic3r::DynamicPrintConfig config(filaments.default_preset().config);
        config.option<Slic3r::ConfigOptionStrings>("filament_type", true)->values   = {spec.filament_type};
        config.option<Slic3r::ConfigOptionStrings>("filament_vendor", true)->values = {spec.filament_vendor};
        filaments.load_preset(std::string(), spec.name, config, /*select=*/false);
    }
    for (Slic3r::Preset& preset : filaments) {
        const auto spec = std::find_if(specs.begin(), specs.end(), [&](const FilamentSpec& s) { return preset.name == s.name; });
        if (spec == specs.end())
            continue;
        preset.filament_id   = spec->filament_id;
        preset.is_system     = spec->is_system;
        preset.is_default    = false;
        preset.is_visible    = true;
        preset.is_compatible = true;
    }
}

} // namespace filament_test
