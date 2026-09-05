#include "MixedFilamentOfd.hpp"
#include "MixedFilamentCookbook.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

#include <boost/nowide/fstream.hpp>

namespace Slic3r {

namespace {

std::string trim_copy(const std::string &s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

std::string ascii_lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ci_contains(const std::string &hay, const std::string &needle)
{
    if (needle.empty())
        return true;
    const std::string h = ascii_lower(hay);
    const std::string n = ascii_lower(needle);
    return h.find(n) != std::string::npos;
}

std::string normalize_hex(const std::string &raw)
{
    std::string s = trim_copy(raw);
    if (s.empty())
        return {};
    if (s[0] == '#')
        s.erase(s.begin());
    if (s.size() > 6)
        s = s.substr(0, 6);
    if (s.size() != 6)
        return {};
    for (char &c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return {};
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return std::string("#") + s;
}

std::string json_string_field(const nlohmann::json &j, const char *key)
{
    if (!j.is_object() || !j.contains(key))
        return {};
    const nlohmann::json &v = j[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_object() && v.contains("name") && v["name"].is_string())
        return v["name"].get<std::string>();
    return {};
}

bool json_bool_field(const nlohmann::json &j, const char *key)
{
    if (!j.is_object() || !j.contains(key))
        return false;
    const nlohmann::json &v = j[key];
    if (v.is_boolean())
        return v.get<bool>();
    if (v.is_number_integer())
        return v.get<int>() != 0;
    if (v.is_string()) {
        const std::string s = ascii_lower(v.get<std::string>());
        return s == "true" || s == "1" || s == "yes";
    }
    return false;
}

std::vector<std::string> parse_color_hexes(const nlohmann::json &j)
{
    std::vector<std::string> out;
    if (!j.is_object() || !j.contains("color_hex"))
        return out;
    const nlohmann::json &hx = j["color_hex"];
    auto push = [&](const nlohmann::json &item) {
        if (!item.is_string())
            return;
        const std::string n = normalize_hex(item.get<std::string>());
        if (!n.empty())
            out.push_back(n);
    };
    if (hx.is_string())
        push(hx);
    else if (hx.is_array()) {
        for (const nlohmann::json &item : hx)
            push(item);
    }
    return out;
}

bool parse_variant_object(const nlohmann::json &j, SpectrumOfdVariant &out)
{
    if (!j.is_object())
        return false;
    SpectrumOfdVariant v;
    v.color_hexes = parse_color_hexes(j);
    if (v.color_hexes.empty())
        return false;

    v.brand = json_string_field(j, "brand");
    if (v.brand.empty())
        v.brand = json_string_field(j, "brand_name");

    v.filament = json_string_field(j, "filament");
    if (v.filament.empty())
        v.filament = json_string_field(j, "filament_name");

    v.variant = json_string_field(j, "variant");
    if (v.variant.empty())
        v.variant = json_string_field(j, "name");

    v.material = json_string_field(j, "material");

    if (j.contains("traits") && j["traits"].is_object()) {
        v.translucent = json_bool_field(j["traits"], "translucent");
        v.transparent = json_bool_field(j["traits"], "transparent");
    }
    if (!v.translucent)
        v.translucent = json_bool_field(j, "translucent");
    if (!v.transparent)
        v.transparent = json_bool_field(j, "transparent");

    out = std::move(v);
    return true;
}

void append_from_json(const nlohmann::json &j, std::vector<SpectrumOfdVariant> &out)
{
    if (j.is_array()) {
        for (const nlohmann::json &item : j) {
            SpectrumOfdVariant v;
            if (parse_variant_object(item, v))
                out.push_back(std::move(v));
        }
        return;
    }
    if (!j.is_object())
        return;
    if (j.contains("variants") && j["variants"].is_array()) {
        append_from_json(j["variants"], out);
        return;
    }
    SpectrumOfdVariant v;
    if (parse_variant_object(j, v))
        out.push_back(std::move(v));
}

std::string read_file_or_empty(const std::string &path)
{
    if (path.empty())
        return {};
    boost::nowide::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

} // namespace

std::vector<SpectrumOfdVariant> spectrum_ofd_parse(const std::string &text)
{
    std::vector<SpectrumOfdVariant> out;
    const std::string trimmed = trim_copy(text);
    if (trimmed.empty())
        return out;

    try {
        nlohmann::json j = nlohmann::json::parse(trimmed, nullptr, false);
        if (!j.is_discarded() && (j.is_object() || j.is_array())) {
            append_from_json(j, out);
            return out;
        }
    } catch (...) {
        out.clear();
    }

    std::istringstream iss(text);
    std::string        line;
    while (std::getline(iss, line)) {
        const std::string t = trim_copy(line);
        if (t.empty())
            continue;
        try {
            nlohmann::json j = nlohmann::json::parse(t, nullptr, false);
            if (j.is_discarded())
                continue;
            append_from_json(j, out);
        } catch (...) {
            continue;
        }
    }
    return out;
}

std::vector<SpectrumOfdVariant> spectrum_ofd_load_catalog(
    const std::string &seed_json_path,
    const std::string &user_ndjson_path)
{
    std::vector<SpectrumOfdVariant> out = spectrum_ofd_parse(read_file_or_empty(seed_json_path));
    if (!user_ndjson_path.empty()) {
        const auto extra = spectrum_ofd_parse(read_file_or_empty(user_ndjson_path));
        out.insert(out.end(), extra.begin(), extra.end());
    }
    return out;
}

std::vector<SpectrumOfdVariant> spectrum_ofd_lookup(
    const std::vector<SpectrumOfdVariant> &catalog,
    const std::string                     &brand_filter,
    const std::string                     &name_substring)
{
    const std::string brand  = trim_copy(brand_filter);
    const std::string needle = trim_copy(name_substring);
    std::vector<SpectrumOfdVariant> out;
    for (const SpectrumOfdVariant &v : catalog) {
        if (!brand.empty() && !ci_contains(v.brand, brand))
            continue;
        if (!needle.empty()) {
            const bool hit_name = ci_contains(v.filament, needle) || ci_contains(v.variant, needle) ||
                                  ci_contains(v.material, needle);
            const bool hit_brand = brand.empty() && ci_contains(v.brand, needle);
            if (!hit_name && !hit_brand)
                continue;
        }
        out.push_back(v);
    }
    return out;
}

std::string spectrum_ofd_slot_hex(const SpectrumOfdVariant &v)
{
    return v.color_hexes.empty() ? std::string() : v.color_hexes.front();
}

bool spectrum_ofd_stamp_slot(
    std::vector<std::string>       &filament_colour,
    std::vector<std::string>       &filament_multi_colour,
    std::vector<std::string>       &filament_colour_type,
    std::vector<char>              &override_flags,
    size_t                          slot,
    const std::vector<std::string> &hexes,
    bool                            force)
{
    if (hexes.empty())
        return false;
    std::vector<std::string> norm;
    norm.reserve(hexes.size());
    for (const std::string &h : hexes) {
        const std::string n = normalize_hex(h);
        if (!n.empty())
            norm.push_back(n);
    }
    if (norm.empty())
        return false;

    const size_t need = slot + 1;
    if (filament_colour.size() < need)
        filament_colour.resize(need);
    if (filament_multi_colour.size() < need)
        filament_multi_colour.resize(need);
    if (filament_colour_type.size() < need)
        filament_colour_type.resize(need);
    if (override_flags.size() < need)
        override_flags.resize(need, 0);

    if (override_flags[slot] && !force)
        return false;

    filament_colour[slot] = norm.front();
    std::string joined    = norm.front();
    for (size_t i = 1; i < norm.size(); ++i) {
        joined += ' ';
        joined += norm[i];
    }
    filament_multi_colour[slot] = joined;
    filament_colour_type[slot]  = "1";
    if (force)
        override_flags[slot] = 0;
    return true;
}

SpectrumMixSeedMode spectrum_mix_seed_mode_from_string(const std::string &s)
{
    const std::string v = ascii_lower(trim_copy(s));
    if (v == "always")
        return SpectrumMixSeedMode::Always;
    if (v == "never")
        return SpectrumMixSeedMode::Never;
    return SpectrumMixSeedMode::Ask;
}

SpectrumMixSeedDecision spectrum_ofd_mix_seed_decision(
    const std::vector<MixedFilament> &rows,
    const std::vector<std::string>   &slot_hexes,
    SpectrumMixSeedMode               mode,
    bool                              already_prompted)
{
    if (mode == SpectrumMixSeedMode::Never)
        return SpectrumMixSeedDecision::Skip;
    if (already_prompted)
        return SpectrumMixSeedDecision::Skip;
    if (slot_hexes.empty())
        return SpectrumMixSeedDecision::Skip;
    for (const std::string &hex : slot_hexes) {
        if (normalize_hex(hex).empty())
            return SpectrumMixSeedDecision::Skip;
    }
    for (const MixedFilament &mf : rows) {
        if (mf.enabled)
            return SpectrumMixSeedDecision::Skip;
    }
    if (mode == SpectrumMixSeedMode::Always)
        return SpectrumMixSeedDecision::Append;
    return SpectrumMixSeedDecision::Prompt;
}

std::vector<MixedFilament> spectrum_ofd_mix_seed_apply(
    const std::vector<MixedFilament> &existing,
    size_t                            num_physical,
    bool                              user_yes,
    SpectrumMixSeedMode               mode)
{
    if (mode == SpectrumMixSeedMode::Never)
        return existing;
    if (!user_yes && mode != SpectrumMixSeedMode::Always)
        return existing;
    const MixCookbookAppend r = spectrum_cookbook_append(existing, num_physical);
    std::vector<MixedFilament> out = existing;
    out.insert(out.end(), r.added.begin(), r.added.end());
    return out;
}

std::string spectrum_ofd_serialize_mix_rows(const std::vector<MixedFilament> &rows)
{
    std::string joined;
    for (const MixedFilament &mf : rows) {
        if (!joined.empty())
            joined += ';';
        joined += std::to_string(mf.component_a) + "," + std::to_string(mf.component_b) + "," +
                  (mf.enabled ? "1" : "0") + "," + std::to_string(mf.ratio_a) + "," +
                  std::to_string(mf.ratio_b);
        const std::string pattern = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
        if (!pattern.empty())
            joined += "," + pattern;
        if (std::abs(mf.component_a_surface_offset) > 1e-6f)
            joined += ",xa" + std::to_string(mf.component_a_surface_offset);
        if (std::abs(mf.component_b_surface_offset) > 1e-6f)
            joined += ",xb" + std::to_string(mf.component_b_surface_offset);
        if (mf.component_c != 0)
            joined += ",c" + std::to_string(mf.component_c) + ",rc" + std::to_string(mf.ratio_c);
        if (mf.gradient_enabled)
            joined += ",g";
        if (mf.perimeter_modulation)
            joined += ",p";
    }
    MixedFilamentManager parsed;
    parsed.load_definitions(joined, true);
    return parsed.serialize_definitions();
}

} // namespace Slic3r
