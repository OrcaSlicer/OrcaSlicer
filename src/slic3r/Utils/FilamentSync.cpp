#include "FilamentSync.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::FilamentSync {

// Protocol tokens are folded and split in ASCII only: the <cctype> functions follow the C locale, which can fold the
// same letter differently (Turkish I) or rewrite bytes of UTF-8 text.
static char ascii_upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }
static char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
static bool is_ascii_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

std::optional<std::string> parse_hex_rgb(const std::string& color)
{
    if (color.size() != 7 || color.front() != '#' || color.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
        return std::nullopt;

    std::string rgba = color.substr(1);
    for (char& digit : rgba)
        digit = ascii_upper(digit);
    return rgba + "FF";
}

static std::string hex_mask(std::uint32_t mask)
{
    char text[9];
    std::snprintf(text, sizeof(text), "%X", static_cast<unsigned>(mask));
    return text;
}

nlohmann::json build_ams_print_json(const std::vector<std::array<AmsSlot, 4>>& units, const std::string& unit_name)
{
    using nlohmann::json;

    if (units.size() > max_ams_units)
        throw std::invalid_argument("An AMS document holds at most 7 units");

    json ams_units                = json::array();
    std::uint32_t tray_exist_bits = 0;
    for (std::size_t unit_id = 0; unit_id < units.size(); ++unit_id) {
        const auto& slots = units[unit_id];
        json unit         = {{"id", std::to_string(unit_id)}, {"info", "0002"}, {"tray", json::array()}};
        if (!unit_name.empty())
            unit["display_name"] = units.size() == 1 ? unit_name : unit_name + " " + std::to_string(unit_id + 1);
        auto& trays = unit["tray"];
        for (std::size_t slot_id = 0; slot_id < slots.size(); ++slot_id) {
            const AmsSlot& slot   = slots[slot_id];
            json tray             = {{"id", std::to_string(slot_id)}, {"tag_uid", "0000000000000000"}};
            tray["tray_info_idx"] = slot.present ? slot.filament_id : "";
            tray["tray_type"]     = slot.present ? slot.type : "";
            tray["tray_color"]    = slot.present && !slot.color_rgba.empty() ? slot.color_rgba : "00000000";
            if (slot.present) {
                tray_exist_bits |= std::uint32_t(1) << (unit_id * slots.size() + slot_id);
                tray["cols"]  = json::array({tray["tray_color"]});
                tray["ctype"] = 1;
                if (slot.nozzle_temp_max > 0)
                    tray["nozzle_temp_max"] = std::to_string(slot.nozzle_temp_max);
            }
            trays.push_back(std::move(tray));
        }
        ams_units.push_back(std::move(unit));
    }
    json doc;
    auto& ams              = doc["ams"];
    ams["ams"]             = std::move(ams_units);
    ams["ams_exist_bits"]  = hex_mask((std::uint32_t(1) << units.size()) - 1);
    ams["tray_exist_bits"] = hex_mask(tray_exist_bits);
    return doc;
}

static std::string to_lower(std::string text)
{
    for (char& c : text)
        c = ascii_lower(c);
    return text;
}

using Tokens = std::set<std::string>;

// Lower case words of the name before any "@<scope>", without the dropped ones. "+" stays part of a
// word, so "PLA+" and "PLA" are different products.
static Tokens name_tokens(const std::string& name, const Tokens& dropped)
{
    const std::string text = to_lower(name.substr(0, name.find('@')));
    Tokens tokens;
    for (auto it = text.begin(); it != text.end();) {
        const auto end = std::find_if(it, text.end(), is_ascii_space);
        if (std::string word(it, end); !word.empty() && dropped.count(word) == 0)
            tokens.insert(std::move(word));
        it = end == text.end() ? end : end + 1;
    }
    return tokens;
}

// Words a preset or product name repeats from its vendor, plus "generic".
static Tokens vendor_words(const std::string& vendor)
{
    Tokens words = name_tokens(vendor, {});
    words.insert("generic");
    return words;
}

std::string match_filament_preset(const PresetCollection& filaments, const SpoolIdentity& spool)
{
    if (spool.type.empty())
        return {};

    const std::string type   = to_lower(spool.type);
    const std::string vendor = to_lower(spool.vendor);
    // The product, then the vendor's base product.
    const Tokens wanted[] = {name_tokens(spool.product, vendor_words(spool.vendor)), {type}};

    // On equal names a system preset wins over a user copy, then the lexicographically first name.
    const Preset* best[] = {nullptr, nullptr};
    auto better          = [](const Preset& p, const Preset* other) {
        return other == nullptr || std::make_pair(!p.is_system, p.name) < std::make_pair(!other->is_system, other->name);
    };
    for (const Preset& preset : filaments) {
        if (!preset.is_visible || !preset.is_compatible || preset.filament_id.empty() || to_lower(preset.config.get_filament_type()) != type)
            continue;
        // Without a configured vendor the profile vendor also qualifies; the name drops just the words of the configured vendor.
        const std::string filament_vendor = preset.config.get_filament_vendor();
        const bool vendor_ok = to_lower(filament_vendor) == vendor ||
                               (filament_vendor.empty() && preset.vendor != nullptr && to_lower(preset.vendor->name) == vendor);
        if (!vendor_ok)
            continue;
        const Tokens tokens = name_tokens(preset.name, vendor_words(filament_vendor));
        for (std::size_t i = 0; i < std::size(wanted); ++i)
            if (tokens == wanted[i] && better(preset, best[i]))
                best[i] = &preset;
    }
    for (const Preset* preset : best)
        if (preset != nullptr)
            return preset->filament_id;
    return filaments.filament_id_by_type(spool.type);
}

} // namespace Slic3r::FilamentSync
