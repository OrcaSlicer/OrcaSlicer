#include "SpoolProfileMap.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"       // PRESET_USER_DIR
#include "libslic3r/PresetBundle.hpp" // DEFAULT_USER_FOLDER_NAME
#include "libslic3r/Utils.hpp"        // data_dir
#include "slic3r/GUI/GUI_App.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <system_error>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {

namespace {

struct Rgb
{
    int red;
    int green;
    int blue;
};

// Trim, upper case and collapse runs of separators to a single space. Everything the printer
// sends goes through this before it is compared to anything: printers are not consistent about
// spelling and the user should not pay for that.
std::string normalize(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    bool pending_space = false;
    for (unsigned char c : value) {
        if (std::isspace(c) || c == '_') {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(static_cast<char>(std::toupper(c)));
    }
    return out;
}

// "PLA-CF" -> "PLA", "PETG HF" -> "PETG", "PLA HIGH SPEED" -> "PLA".
std::string material_family(const std::string& normalized_material)
{
    const size_t cut = normalized_material.find_first_of(" -+/");
    return cut == std::string::npos ? normalized_material : normalized_material.substr(0, cut);
}

// The hex digits of "#RRGGBB", "RRGGBB", "0xRRGGBB" or "RRGGBBAA", empty when anything else is in there.
std::string color_hex_digits(const std::string& color)
{
    std::string hex;
    for (char c : color) {
        if (std::isxdigit(static_cast<unsigned char>(c)))
            hex.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        else if (c != '#' && c != 'x' && c != 'X')
            return std::string();
    }
    if (color.size() > 1 && color[0] == '0' && (color[1] == 'x' || color[1] == 'X'))
        hex.erase(0, 1);
    return hex;
}

// Accepts "#RRGGBB", "RRGGBB", "0xRRGGBB" and "RRGGBBAA". Alpha is ignored.
std::optional<Rgb> parse_rgb(const std::string& color)
{
    const std::string hex = color_hex_digits(color);
    if (hex.size() < 6)
        return std::nullopt;
    const unsigned long value = std::strtoul(hex.substr(0, 6).c_str(), nullptr, 16);
    return Rgb{static_cast<int>((value >> 16) & 0xff), static_cast<int>((value >> 8) & 0xff), static_cast<int>(value & 0xff)};
}

// Squared distance in RGB, or a value larger than any real distance when either side is unknown.
long color_distance(const std::string& a, const std::string& b)
{
    const std::optional<Rgb> first  = parse_rgb(a);
    const std::optional<Rgb> second = parse_rgb(b);
    if (!first || !second)
        return 3 * 255 * 255 + 1;
    const long red = first->red - second->red, green = first->green - second->green, blue = first->blue - second->blue;
    return red * red + green * green + blue * blue;
}

bool is_generic(const SpoolPresetCandidate& candidate)
{
    return normalize(candidate.vendor) == "GENERIC" || normalize(candidate.name).rfind("GENERIC ", 0) == 0;
}

// The parts of a key, in the order spool_mapping_key wrote them.
std::vector<std::string> split_key(const std::string& value)
{
    std::vector<std::string> parts;
    size_t                   start = 0;
    while (true) {
        const size_t cut = value.find('|', start);
        parts.push_back(value.substr(start, cut == std::string::npos ? cut : cut - start));
        if (cut == std::string::npos)
            break;
        start = cut + 1;
    }
    return parts;
}

// Which filament a key is about, ignoring anything else it carries. Two keys with the same
// identity are the same filament, so a link saved when the printer reported less than it does
// today is still found.
std::string key_identity(const std::string& key)
{
    if (key.rfind("name:", 0) != 0)
        return std::string();
    const std::vector<std::string> parts = split_key(key.substr(5));
    // Written before the key held the name the user is shown: vendor, name, material.
    if (parts.size() >= 3)
        return normalize(parts[0] + " " + parts[1]);
    return parts.front();
}

// The key this filament's link is filed under, whatever key it was saved with. Empty when the
// filament has never been linked.
std::string filed_key(const std::map<std::string, SpoolLink>& links, const std::string& key)
{
    if (links.count(key) > 0)
        return key;
    const std::string identity = key_identity(key);
    if (identity.empty())
        return std::string();
    for (const auto& entry : links)
        if (key_identity(entry.first) == identity)
            return entry.first;
    return std::string();
}

const SpoolLink* find_link(const std::map<std::string, SpoolLink>& links, const std::string& key)
{
    const auto entry = links.find(filed_key(links, key));
    return entry == links.end() ? nullptr : &entry->second;
}

bool contains(const std::vector<std::string>& values, const std::string& value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string string_field(const nlohmann::json& object, const char* name)
{
    const auto field = object.find(name);
    return field != object.end() && field->is_string() ? field->get<std::string>() : std::string();
}

bool bool_field(const nlohmann::json& object, const char* name, bool fallback)
{
    const auto field = object.find(name);
    return field != object.end() && field->is_boolean() ? field->get<bool>() : fallback;
}

SpoolMapSettings settings_from_json(const nlohmann::json& json)
{
    SpoolMapSettings settings;
    const auto       saved = json.find("settings");
    if (saved == json.end() || !saved->is_object())
        return settings;
    settings.enabled          = bool_field(*saved, "enabled", settings.enabled);
    settings.ask_only_new     = bool_field(*saved, "ask_only_new", settings.ask_only_new);
    settings.remember_generic = bool_field(*saved, "remember_generic", settings.remember_generic);
    return settings;
}

std::vector<std::string> colors_from_json(const nlohmann::json& saved_link)
{
    std::vector<std::string> colors;
    const auto               saved = saved_link.find("colors");
    if (saved == saved_link.end() || !saved->is_array())
        return colors;
    for (const nlohmann::json& color : *saved)
        if (color.is_string())
            colors.push_back(color.get<std::string>());
    return colors;
}

std::map<std::string, std::string> color_presets_from_json(const nlohmann::json& saved_link)
{
    std::map<std::string, std::string> color_presets;
    const auto                         saved = saved_link.find("color_presets");
    if (saved == saved_link.end() || !saved->is_object())
        return color_presets;
    for (const auto& own : saved->items())
        if (own.value().is_string() && !own.value().get<std::string>().empty())
            color_presets[own.key()] = own.value().get<std::string>();
    return color_presets;
}

std::map<std::string, SpoolLink> links_from_json(const nlohmann::json& json)
{
    std::map<std::string, SpoolLink> links;
    const auto                       saved = json.find("links");
    if (saved == json.end() || !saved->is_object())
        return links;
    for (const auto& entry : saved->items()) {
        if (entry.key().empty() || !entry.value().is_object())
            continue;
        SpoolLink link{entry.key(), string_field(entry.value(), "preset"), string_field(entry.value(), "label"),
                       colors_from_json(entry.value()), color_presets_from_json(entry.value())};
        // A link with no profile is nothing to restore.
        if (!link.preset_name.empty())
            links[link.key] = std::move(link);
    }
    return links;
}

// One printer's list, as files written before the list stopped being per printer kept it. Where
// two printers disagreed about the same spool, the first one read wins.
void add_printer_list(const nlohmann::json& printer_list, std::map<std::string, SpoolLink>& links)
{
    for (const auto& entry : printer_list.items()) {
        if (entry.key().empty() || !entry.value().is_string() || entry.value().get<std::string>().empty())
            continue;
        SpoolLink link;
        link.key         = entry.key();
        link.preset_name = entry.value().get<std::string>();
        links.emplace(entry.key(), std::move(link));
    }
}

void add_printer_lists(const nlohmann::json& json, std::map<std::string, SpoolLink>& links)
{
    const auto printers = json.find("printers");
    if (printers == json.end() || !printers->is_object())
        return;
    for (const auto& printer : printers->items())
        if (printer.value().is_object())
            add_printer_list(printer.value(), links);
}

// Links saved when a spool tag was the key: one row per spool, and one set of rows per printer.
// Re-file them under the name the user was shown, which folds them back into one row per
// filament. A link with no name is left alone: there is nothing to re-file it by.
std::map<std::string, SpoolLink> refiled_by_label(std::map<std::string, SpoolLink> links)
{
    std::map<std::string, SpoolLink> refiled;
    for (auto& entry : links) {
        SpoolLink link = std::move(entry.second);
        if (entry.first.rfind("name:", 0) != 0 && !link.label.empty())
            link.key = "name:" + normalize(link.label) + "|";
        const std::string key = link.key;
        refiled.emplace(key, std::move(link));
    }
    return refiled;
}

nlohmann::json link_to_json(const SpoolLink& link)
{
    nlohmann::json saved;
    saved["preset"] = link.preset_name;
    if (!link.label.empty())
        saved["label"] = link.label;
    if (!link.colors.empty())
        saved["colors"] = link.colors;
    if (!link.color_presets.empty())
        saved["color_presets"] = link.color_presets;
    return saved;
}

// The links belong to whoever is logged in, so they sit beside that account's presets.
std::string store_path()
{
    std::string folder = GUI::wxGetApp().app_config->get("preset_folder");
    if (folder.empty())
        folder = DEFAULT_USER_FOLDER_NAME;
    return (boost::filesystem::path(data_dir()) / PRESET_USER_DIR / folder / "spool_profile_map.json").string();
}

// A missing file is a fresh start. A file that is there but cannot be read gives nullopt: saving
// over it would wipe every link the user has.
std::optional<SpoolStore> read_store(const std::string& path)
{
    boost::system::error_code ec;
    if (!boost::filesystem::exists(path, ec) && !ec)
        return SpoolStore{};

    boost::nowide::ifstream         stream(path);
    const std::string               text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const std::optional<SpoolStore> store = spool_store_from_json(text);
    if (!store)
        BOOST_LOG_TRIVIAL(error) << "SpoolProfileMap: cannot read " << path << ", spool links are not saved until it is fixed or removed";
    return store;
}

} // namespace

std::string spool_mapping_key(const SpoolIdentity& spool)
{
    // The name the user is shown IS the identity of a filament. A spool id belongs to one spool on
    // one printer, so keying on it splits one filament into a row per spool and per machine.
    const std::string display = normalize(spool_display_name(spool));
    if (!display.empty())
        return "name:" + display + "|" + normalize(spool.material);

    // Nothing identifying at all: fall back to the lane, so the user's choice is at least
    // remembered for the slot they put it in.
    return "slot:" + std::to_string(spool.slot_index);
}

bool spool_is_generic(const SpoolIdentity& spool)
{
    return spool.name.empty() && spool.vendor.empty() && spool.uid.empty();
}

std::string spool_display_name(const SpoolIdentity& spool)
{
    const bool has_vendor = !spool.vendor.empty() && normalize(spool.vendor) != normalize(spool.name);
    if (!spool.name.empty())
        return has_vendor ? spool.vendor + " " + spool.name : spool.name;
    if (!spool.material.empty())
        return spool.vendor.empty() ? spool.material : spool.vendor + " " + spool.material;
    return spool.vendor;
}

SpoolIdentity spool_identity_from_key(const std::string& key)
{
    SpoolIdentity spool;

    if (key.rfind("uid:", 0) == 0) {
        spool.uid = key.substr(4);
    } else if (key.rfind("name:", 0) == 0) {
        const std::vector<std::string> parts = split_key(key.substr(5));
        if (parts.size() >= 3) {
            // Written before the key held the name the user is shown.
            spool.vendor   = parts[0];
            spool.name     = parts[1];
            spool.material = parts[2];
        } else {
            spool.name     = parts[0];
            spool.material = parts.size() > 1 ? parts[1] : std::string();
        }
    } else if (key.rfind("type:", 0) == 0) {
        const std::vector<std::string> parts = split_key(key.substr(5));
        spool.vendor   = parts[0];
        spool.material = parts.size() > 1 ? parts[1] : std::string();
    } else if (key.rfind("slot:", 0) == 0) {
        const std::string number = key.substr(5);
        int               slot   = 0;
        if (std::from_chars(number.data(), number.data() + number.size(), slot).ec == std::errc())
            spool.slot_index = slot;
    }
    return spool;
}

std::string spool_normalize_color(const std::string& color)
{
    const std::optional<Rgb> rgb = parse_rgb(color);
    if (!rgb)
        return std::string();
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", rgb->red, rgb->green, rgb->blue);
    return std::string(buffer);
}

std::string spool_normalize_color_alpha(const std::string& color)
{
    const std::string opaque = spool_normalize_color(color);
    const std::string hex    = color_hex_digits(color);
    if (opaque.empty() || hex.size() != 8 || hex.compare(6, 2, "FF") == 0)
        return opaque;
    return opaque + hex.substr(6, 2);
}

namespace {

std::string join_colors(const std::vector<std::string>& colors)
{
    std::string joined;
    for (const std::string& color : colors)
        joined += (joined.empty() ? "" : ",") + color;
    return joined;
}

} // namespace

std::string spool_variant_color(const SpoolIdentity& spool)
{
    return spool.colors.size() < 2 ? spool.color : join_colors(spool.colors);
}

std::vector<std::string> spool_variant_colors(const std::string& variant)
{
    std::vector<std::string> colors;
    size_t                   start = 0;
    while (start <= variant.size()) {
        const size_t      end   = std::min(variant.find(',', start), variant.size());
        const std::string color = spool_normalize_color_alpha(variant.substr(start, end - start));
        if (!color.empty())
            colors.push_back(color);
        start = end + 1;
    }
    return colors;
}

namespace {

std::string normalized_variant(const std::string& variant)
{
    return join_colors(spool_variant_colors(variant));
}

enum class SpoolField { Unknown, Material, Name, Uid, Vendor, FilamentId, Color };

SpoolField spool_field_of(const std::string& key)
{
    static const std::vector<std::pair<SpoolField, std::vector<std::string>>> aliases = {
        {SpoolField::Material, {"MATERIAL", "FILAMENT TYPE"}},
        {SpoolField::Name, {"FILAMENT NAME", "NAME", "PRODUCT", "MODEL"}},
        {SpoolField::Uid, {"SPOOL ID", "SPOOL UID", "UID", "TAG UID", "SN", "SERIAL", "ID"}},
        {SpoolField::Vendor, {"VENDOR", "BRAND", "MANUFACTURER"}},
        {SpoolField::FilamentId, {"FILAMENT ID"}},
        {SpoolField::Color, {"COLOR", "COLOUR", "COLORS", "COLOURS", "COLS"}},
    };
    const std::string normalized = normalize(key);
    for (const auto& [field, names] : aliases)
        if (contains(names, normalized))
            return field;
    return SpoolField::Unknown;
}

// Printers say "NONE", "0000000000000000" or nothing when they have nothing to say.
bool is_reported(const std::string& value)
{
    const std::string normalized = normalize(value);
    return !normalized.empty() && normalized != "NONE" && normalized.find_first_not_of('0') != std::string::npos;
}

std::vector<std::string> colors_from_field(const std::string& value)
{
    std::vector<std::string> colors;
    size_t                   start = 0;
    while (start <= value.size()) {
        const size_t end   = std::min(value.find_first_of(",;| ", start), value.size());
        const std::string color = spool_normalize_color_alpha(value.substr(start, end - start));
        if (!color.empty())
            colors.push_back(color);
        start = end + 1;
    }
    return colors;
}

void fill_unset(std::string& target, const std::string& value)
{
    if (target.empty() && is_reported(value))
        target = value;
}

} // namespace

SpoolIdentity spool_identity_from_fields(int slot_index, bool has_filament, const SpoolFields& fields)
{
    SpoolIdentity spool;
    spool.slot_index   = slot_index;
    spool.has_filament = has_filament;

    for (const auto& [key, value] : fields) {
        switch (spool_field_of(key)) {
        case SpoolField::Material:   fill_unset(spool.material, value); break;
        case SpoolField::Name:       fill_unset(spool.name, value); break;
        case SpoolField::Uid:        fill_unset(spool.uid, value); break;
        case SpoolField::Vendor:     fill_unset(spool.vendor, value); break;
        case SpoolField::FilamentId: fill_unset(spool.filament_id, value); break;
        case SpoolField::Color:
            if (spool.colors.empty())
                spool.colors = colors_from_field(value);
            break;
        case SpoolField::Unknown:    spool.extra.emplace_back(key, value); break;
        }
    }
    if (!spool.colors.empty())
        spool.color = spool_normalize_color(spool.colors.front());
    return spool;
}

int spool_material_rank(const std::string& candidate_material, const std::string& reported_material)
{
    const std::string candidate = normalize(candidate_material);
    const std::string reported  = normalize(reported_material);

    // The printer did not say what it is: do not hide anything from the user.
    if (reported.empty())
        return 1;
    if (candidate == reported)
        return 0;

    const std::string candidate_family = material_family(candidate);
    const std::string reported_family  = material_family(reported);
    if (!reported_family.empty() && candidate_family == reported_family)
        return 1;

    return 2;
}

std::vector<SpoolPresetCandidate> spool_filter_candidates(const std::vector<SpoolPresetCandidate>& all,
                                                          const SpoolIdentity&                     spool,
                                                          bool                                     include_all,
                                                          const std::string&                       always_include)
{
    const std::string spool_vendor = normalize(spool.vendor);
    const std::string spool_name   = normalize(spool.name);

    struct Ranked
    {
        const SpoolPresetCandidate* candidate;
        int                         rank;
        int                         vendor_rank; // 0 = same vendor
        int                         name_rank;   // 0 = the printer's name is in the profile name
        size_t                      order;       // keeps the collection's own order for ties
    };

    std::vector<Ranked> ranked;
    ranked.reserve(all.size());
    for (size_t i = 0; i < all.size(); ++i) {
        const SpoolPresetCandidate& candidate = all[i];
        const bool                  pinned    = !always_include.empty() && candidate.name == always_include;
        int                         rank      = spool_material_rank(candidate.material, spool.material);
        if (pinned)
            rank = -1;
        else if (rank == 2 && !include_all)
            continue;

        const int vendor_rank = (!spool_vendor.empty() && normalize(candidate.vendor) == spool_vendor) ? 0 : 1;
        const int name_rank   = (!spool_name.empty() && normalize(candidate.name).find(spool_name) != std::string::npos) ? 0 : 1;
        ranked.push_back(Ranked{&candidate, rank, vendor_rank, name_rank, i});
    }

    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.rank != b.rank)
            return a.rank < b.rank;
        if (a.name_rank != b.name_rank)
            return a.name_rank < b.name_rank;
        if (a.vendor_rank != b.vendor_rank)
            return a.vendor_rank < b.vendor_rank;
        return a.order < b.order;
    });

    std::vector<SpoolPresetCandidate> out;
    out.reserve(ranked.size());
    for (const Ranked& r : ranked)
        out.push_back(*r.candidate);
    return out;
}

std::string spool_choose_preset(const std::vector<SpoolPresetCandidate>& candidates,
                                const SpoolIdentity&                     spool,
                                const std::string&                       remembered)
{
    if (candidates.empty())
        return std::string();

    // What the user already decided for this spool beats anything we can guess.
    if (!remembered.empty()) {
        const auto it = std::find_if(candidates.begin(), candidates.end(),
                                     [&remembered](const SpoolPresetCandidate& c) { return c.name == remembered; });
        if (it != candidates.end())
            return it->name;
    }

    // The id is the printer's own statement of which profile fits. Generic profiles share ids with
    // everything of their material, so they only count when nothing more specific says the same.
    if (!spool.filament_id.empty()) {
        const auto it = std::find_if(candidates.begin(), candidates.end(), [&spool](const SpoolPresetCandidate& c) {
            return c.filament_id == spool.filament_id && !is_generic(c);
        });
        if (it != candidates.end())
            return it->name;
    }

    const std::string spool_name     = normalize(spool.name);
    const std::string spool_vendor   = normalize(spool.vendor);
    const std::string spool_material = normalize(spool.material);

    const SpoolPresetCandidate* best_named   = nullptr;
    const SpoolPresetCandidate* best_vendor  = nullptr;
    const SpoolPresetCandidate* best_generic = nullptr;
    const SpoolPresetCandidate* best_exact   = nullptr;
    const SpoolPresetCandidate* best_family  = nullptr;
    long                        best_vendor_distance = 0;

    for (const SpoolPresetCandidate& candidate : candidates) {
        const int rank = spool_material_rank(candidate.material, spool_material);
        if (rank > 1)
            continue;

        if (!spool_name.empty() && best_named == nullptr && normalize(candidate.name).find(spool_name) != std::string::npos)
            best_named = &candidate;

        if (rank == 0) {
            if (!spool_vendor.empty() && normalize(candidate.vendor) == spool_vendor) {
                // Several profiles from the same maker in the same material: the colour the
                // printer reported is the only thing left to tell them apart.
                const long distance = color_distance(candidate.color, spool.color);
                if (best_vendor == nullptr || distance < best_vendor_distance) {
                    best_vendor          = &candidate;
                    best_vendor_distance = distance;
                }
            }
            if (best_generic == nullptr && is_generic(candidate))
                best_generic = &candidate;
            if (best_exact == nullptr)
                best_exact = &candidate;
        } else if (best_family == nullptr) {
            best_family = &candidate;
        }
    }

    for (const SpoolPresetCandidate* best : {best_named, best_vendor, best_generic, best_exact, best_family})
        if (best != nullptr)
            return best->name;
    return std::string();
}

std::string spool_remembered_preset(const std::map<std::string, SpoolLink>& links,
                                    const std::string&                      key,
                                    const std::string&                      color)
{
    const SpoolLink* link = find_link(links, key);
    if (link == nullptr)
        return std::string();

    // This one colour was put on a profile of its own; every other colour uses the filament's.
    const std::string wanted = normalized_variant(color);
    if (wanted.empty())
        return link->preset_name;
    const auto own = link->color_presets.find(wanted);
    return own != link->color_presets.end() && !own->second.empty() ? own->second : link->preset_name;
}

bool spool_is_new(const std::map<std::string, SpoolLink>& links, const std::string& key, const std::string& color)
{
    const SpoolLink* link = find_link(links, key);
    if (link == nullptr)
        return true;

    // The printer did not say what colour it is, so there is nothing here the user has not
    // already answered for.
    const std::string wanted = normalized_variant(color);
    return !wanted.empty() && !contains(link->colors, wanted);
}

void spool_remember(std::map<std::string, SpoolLink>& links,
                    const std::string&                key,
                    const std::string&                preset_name,
                    const std::string&                label,
                    const std::string&                color)
{
    if (key.empty())
        return;

    // Everything already filed for this filament, under whatever key it was saved with, becomes
    // this one link: one filament is one row, never one row per spool or per printer.
    SpoolLink         link;
    const std::string identity = key_identity(key);
    for (auto it = links.begin(); it != links.end();) {
        if (it->first != key && (identity.empty() || key_identity(it->first) != identity)) {
            ++it;
            continue;
        }
        if (link.label.empty())
            link.label = it->second.label;
        for (const std::string& seen : it->second.colors)
            if (!contains(link.colors, seen))
                link.colors.push_back(seen);
        // A colour on a profile of its own keeps it when the filament's own profile changes.
        link.color_presets.insert(it->second.color_presets.begin(), it->second.color_presets.end());
        it = links.erase(it);
    }

    if (preset_name.empty())
        return;

    link.key         = key;
    link.preset_name = preset_name;
    // A printer that says nothing this time must not wipe the name it gave last time.
    if (!label.empty())
        link.label = label;
    const std::string seen = normalized_variant(color);
    if (!seen.empty() && !contains(link.colors, seen))
        link.colors.push_back(seen);
    // A colour that now wants the very profile the whole filament uses is not a variation of it.
    for (auto own = link.color_presets.begin(); own != link.color_presets.end();)
        own = own->second == preset_name ? link.color_presets.erase(own) : std::next(own);
    links[key] = std::move(link);
}

void spool_remember_color(std::map<std::string, SpoolLink>& links,
                          const std::string&                key,
                          const std::string&                color,
                          const std::string&                preset_name)
{
    const std::string wanted = normalized_variant(color);
    const auto        entry  = links.find(filed_key(links, key));
    // A colour belongs to a filament: with no link for the filament there is nothing to hang it on.
    if (entry == links.end() || wanted.empty())
        return;

    SpoolLink& link = entry->second;
    if (preset_name.empty() || preset_name == link.preset_name)
        link.color_presets.erase(wanted);
    else
        link.color_presets[wanted] = preset_name;
    if (!contains(link.colors, wanted))
        link.colors.push_back(wanted);
}

void spool_remember_spool(std::map<std::string, SpoolLink>& links,
                          const std::string&                key,
                          const std::string&                preset_name,
                          const std::string&                label,
                          const std::string&                color)
{
    const SpoolLink*  link        = find_link(links, key);
    const std::string own_profile = link == nullptr ? std::string() : link->preset_name;

    if (own_profile.empty() || own_profile == preset_name) {
        spool_remember(links, key, preset_name, label, color);
        // Answered with the filament's own profile: this colour is not a variation of it any more.
        spool_remember_color(links, key, color, std::string());
        return;
    }
    // The filament keeps the profile it already had, and this colour of it gets the new one.
    spool_remember(links, key, own_profile, label, color);
    spool_remember_color(links, key, color, preset_name);
}

std::vector<SpoolLink> spool_sorted_links(const std::map<std::string, SpoolLink>& links)
{
    std::vector<SpoolLink> out;
    out.reserve(links.size());
    for (const auto& entry : links)
        out.push_back(entry.second);
    std::sort(out.begin(), out.end(), [](const SpoolLink& a, const SpoolLink& b) {
        if (a.label.empty() != b.label.empty())
            return b.label.empty();
        return a.label.empty() ? a.key < b.key : normalize(a.label) < normalize(b.label);
    });
    return out;
}

std::optional<SpoolStore> spool_store_from_json(const std::string& json_text)
{
    const nlohmann::json json = nlohmann::json::parse(json_text, nullptr, false, true);
    if (json.is_discarded() || !json.is_object())
        return std::nullopt;

    std::map<std::string, SpoolLink> links = links_from_json(json);
    add_printer_lists(json, links);
    return SpoolStore{refiled_by_label(std::move(links)), settings_from_json(json)};
}

std::string spool_store_to_json(const SpoolStore& store)
{
    nlohmann::json json;
    json["version"]                  = 2;
    json["settings"]["enabled"]          = store.settings.enabled;
    json["settings"]["ask_only_new"]     = store.settings.ask_only_new;
    json["settings"]["remember_generic"] = store.settings.remember_generic;
    json["links"]                    = nlohmann::json::object();
    for (const auto& entry : store.links)
        json["links"][entry.first] = link_to_json(entry.second);
    // A name the printer sent in a broken encoding must not stop every link from being saved.
    return json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

SpoolProfileMap& SpoolProfileMap::get()
{
    static SpoolProfileMap instance;
    return instance;
}

void SpoolProfileMap::publish(const std::string& dev_id, std::vector<SpoolIdentity> spools)
{
    if (dev_id.empty())
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_published[dev_id] = std::move(spools);
}

std::vector<SpoolIdentity> SpoolProfileMap::spools(const std::string& dev_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto                  it = m_published.find(dev_id);
    return it == m_published.end() ? std::vector<SpoolIdentity>{} : it->second;
}

std::string SpoolProfileMap::remembered(const std::string& key, const std::string& color)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return spool_remembered_preset(store().links, key, color);
}

bool SpoolProfileMap::is_new_spool(const std::string& key, const std::string& color)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return spool_is_new(store().links, key, color);
}

std::vector<SpoolLink> SpoolProfileMap::all_remembered()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return spool_sorted_links(store().links);
}

void SpoolProfileMap::remember(const std::string& key,
                               const std::string& preset_name,
                               const std::string& label,
                               const std::string& color)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    spool_remember(store().links, key, preset_name, label, color);
    save();
}

void SpoolProfileMap::remember_color(const std::string& key, const std::string& color, const std::string& preset_name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    spool_remember_color(store().links, key, color, preset_name);
    save();
}

void SpoolProfileMap::remember_spool(const std::string& key,
                                     const std::string& preset_name,
                                     const std::string& label,
                                     const std::string& color)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    spool_remember_spool(store().links, key, preset_name, label, color);
    save();
}

SpoolMapSettings SpoolProfileMap::settings()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return store().settings;
}

void SpoolProfileMap::set_settings(const SpoolMapSettings& settings)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    store().settings = settings;
    save();
}

SpoolStore& SpoolProfileMap::store()
{
    const std::string path = store_path();
    if (path == m_store_path)
        return m_store;

    const std::optional<SpoolStore> read = read_store(path);
    m_store_path       = path;
    m_store_unreadable = !read;
    m_store            = read.value_or(SpoolStore{});
    return m_store;
}

void SpoolProfileMap::save() const
{
    if (m_store_unreadable) {
        BOOST_LOG_TRIVIAL(error) << "SpoolProfileMap: " << m_store_path << " could not be read, so it is not overwritten";
        return;
    }
    boost::system::error_code ec;
    // A folder that cannot be made shows up as the failed write below.
    boost::filesystem::create_directories(boost::filesystem::path(m_store_path).parent_path(), ec);
    boost::nowide::ofstream stream(m_store_path);
    stream << spool_store_to_json(m_store);
    if (!stream)
        BOOST_LOG_TRIVIAL(error) << "SpoolProfileMap: cannot write " << m_store_path;
}

} // namespace Slic3r
