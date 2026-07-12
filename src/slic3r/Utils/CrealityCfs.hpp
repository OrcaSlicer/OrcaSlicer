#ifndef __CREALITY_CFS_HPP__
#define __CREALITY_CFS_HPP__

#include <string>
#include <vector>

namespace Slic3r {

class PresetCollection;

// Neutral descriptor of one loaded CFS slot, decoupled from the Moonraker agent's
// internal AmsTrayData so the providers can live in their own translation unit and
// be reused/tested without pulling in the agent.
struct CfsTray
{
    int         slot_index   = 0;     // box * 4 + slot (0-based)
    bool        has_filament = true;
    std::string tray_type;            // base type, e.g. "PLA"
    std::string tray_color;           // "RRGGBB" (6 hex digits, no '#')
    std::string tray_info_idx;        // resolved filament preset id ("" if unknown)
    int         remain       = -1;    // remaining filament percentage (0..100; -1 = unknown)
};

// Reads CFS state from a Creality printer. Each Creality product line exposes the
// CFS through a different transport, so there is one provider per line behind this
// common interface. fetch() returns true only when the transport actually found a
// CFS with loaded slots, letting the caller fall through to the next provider.
class ICrealityCfsProvider
{
public:
    virtual ~ICrealityCfsProvider() = default;

    // Fill `trays` with the loaded slots and `box_count` with the number of CFS
    // units detected. Return false (and leave outputs empty) when this transport
    // is not the right one for the connected printer.
    virtual bool        fetch(std::vector<CfsTray>& trays, int& box_count) = 0;
    virtual const char* name() const = 0;
};

// K1-series (e.g. K1C): the CFS is the Klipper `box` object, read over Moonraker
// (/printer/objects/query?box). Port 80 on these boards is Creality's own control
// API, so the provider targets Moonraker's ports (7125 direct, 4409/4408 proxied).
class CrealityCfsK1Provider final : public ICrealityCfsProvider
{
public:
    CrealityCfsK1Provider(std::string dev_ip, std::string api_key, std::string base_url = {});
    bool        fetch(std::vector<CfsTray>& trays, int& box_count) override;
    const char* name() const override { return "K1/box"; }

private:
    std::string m_dev_ip;
    std::string m_api_key;
    std::string m_base_url;
};

// K2-platform (K2 / K2 Plus / K2 Pro / SPARKX i7): the CFS is exposed as the
// proprietary port-9999 WebSocket `boxsInfo` document.
class CrealityCfsK2Provider final : public ICrealityCfsProvider
{
public:
    CrealityCfsK2Provider(std::string dev_ip, std::string api_key);
    bool        fetch(std::vector<CfsTray>& trays, int& box_count) override;
    const char* name() const override { return "K2/boxsInfo"; }

private:
    std::string m_dev_ip;
    std::string m_api_key;
};

// ---- Shared CFS helpers (used by the providers; exposed for reuse/tests) -------

// Strip subtype suffixes ("PLA Silk", "ABS Pro", "PLA+") down to the base type
// (PLA/PETG/ABS/...) so preset lookups by type succeed.
std::string creality_normalize_filament_type(const std::string& filament_type);

// Resolve a CFS filamentId (5- or 6-digit; the last 5 digits are the catalogue id)
// to vendor/name/type via the embedded Creality material catalogue. Returns false
// when the code is unknown.
bool creality_cfs_lookup_material(const std::string& code,
                                  std::string&       vendor,
                                  std::string&       name,
                                  std::string&       type);

// Score visible & compatible filament presets against the spool metadata and
// return the best-matching filament_id, falling back to a generic-by-type preset.
std::string creality_match_filament_preset(const PresetCollection& filaments,
                                           const std::string&      vendor,
                                           const std::string&      brand_name,
                                           const std::string&      base_type);

} // namespace Slic3r

#endif // __CREALITY_CFS_HPP__
