#pragma once

#include "MoonrakerPrinterAgent.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class SnapmakerPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit SnapmakerPrinterAgent(std::string log_dir);
    ~SnapmakerPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id) override;

private:
    // Raw, per-slot AMS data -- parsed once per slot and handed to each match strategy
    // below in turn.
    struct SlotInput
    {
        int                       slot_index;
        std::string               vendor;
        std::string               base_type;       // e.g. "PLA" (trimmed/upper-cased, defaults to "PLA")
        std::string               tray_sub_type;    // this tray's normalized sub-type, e.g. "MATTE", "HS"; may be empty
        std::string               display_type;     // combine_filament_type() fallback display string
        std::string               color;            // RRGGBBAA
        int                       bed_temp;
        int                       nozzle_temp;
    };

    // Combine filament_type + filament_sub_type into a unified type string
    static std::string combine_filament_type(const std::string& type, const std::string& sub_type);

    // Match strategies, tried in order from most to least specific. The caller pre-populates
    // *tray's slot_index and has_filament=true; each strategy fills in the remaining fields
    // (tray_type/tray_color/tray_info_idx/bed_temp/nozzle_temp) and returns whether it
    // produced a match. On failure (false), *tray is left untouched for the next strategy.
    //
    // Strategy 1 (most specific): match by vendor + base type + esthetic sub-type + closest
    // color against the installed filament preset library. On a match, the tray's display
    // type is taken from the matched preset's own name (e.g. "Overture Matte PLA @System")
    // rather than a synthesized string, since we now know the real preset.
    bool try_vendor_subtype_color_match(const SlotInput& input, AmsTrayData* tray);

    // Strategy 2: fall back to a plain type-only search of the installed preset library (no
    // vendor/sub-type/color match).
    bool try_type_only_match(const SlotInput& input, AmsTrayData* tray);

    // Last resort: static generic OrcaFilamentLibrary type-to-id mapping. Not a "try" --
    // it needs no preset bundle and always fills in *tray, terminating the strategy chain.
    void apply_generic_type_map(const SlotInput& input, AmsTrayData* tray);
};

} // namespace Slic3r
