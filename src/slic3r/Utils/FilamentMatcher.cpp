#include "FilamentMatcher.hpp"
#include "libslic3r/Preset.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <cctype>
#include <limits>
#include <vector>

namespace Slic3r {
namespace FilamentMatcher {

// Sanitize filament vendor or filament name for use in preset IDs.
// "Acme Inc" -> "Acme_Inc", "PLA Plus" -> "PLA_Plus", "PLA-AERO" -> "PLA_AERO"
std::string sanitize_for_id(const std::string& name)
{
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            result.push_back(c);
        else if (!result.empty() && result.back() != '_')
            result.push_back('_');
    }
    if (!result.empty() && result.back() == '_')
        result.pop_back();
    return result;
}

const Preset* find_visible_preset(const PresetCollection& filaments, const std::string& filament_id)
{
    for (const auto& p : filaments.get_presets()) {
        if (p.is_visible && p.is_compatible
            && boost::iequals(p.filament_id, filament_id))
            return &p;
    }
    return nullptr;
}

static std::string trim_and_upper(const std::string& input)
{
    std::string result = input;
    boost::trim(result);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

// Split a name into uppercase alphanumeric tokens, dropping any " @printer"
// suffix.  Splitting on non-alphanumerics is what lets a reported "PLA" line up
// with a preset named "PLA+", and what makes word order irrelevant, so
// "PLA Matte" matches both "eSun PLA Matte" and "eSun Matte PLA".
//   "eSun Matte PLA Black @Qidi X-Plus 4" -> [ESUN, MATTE, PLA, BLACK]
//   "PLA_Matte"                           -> [PLA, MATTE]
static std::vector<std::string> name_tokens_of(const std::string& name)
{
    const size_t      at   = name.find(" @");
    const std::string base = (at == std::string::npos) ? name : name.substr(0, at);

    std::vector<std::string> tokens;
    std::string              cur;
    for (char c : base) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            cur.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        else if (!cur.empty()) {
            tokens.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty())
        tokens.push_back(cur);
    return tokens;
}

// Does `preset_name` contain every token of the reported product name?  Extra
// tokens in the preset (vendor, colour, nozzle) are fine -- this asks only that
// nothing the printer reported is missing.
static bool name_covers(const std::string& preset_name, const std::vector<std::string>& wanted)
{
    const std::vector<std::string> have = name_tokens_of(preset_name);
    for (const auto& w : wanted)
        if (std::find(have.begin(), have.end(), w) == have.end())
            return false;
    return true;
}

std::string map_type_to_generic_id(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

    // PLA variants
    if (upper == "PLA")           return "OGFL99";
    if (upper == "PLA-CF")        return "OGFL98";
    if (upper == "PLA SILK" || upper == "PLA-SILK") return "OGFL96";
    if (upper == "PLA HIGH SPEED" || upper == "PLA-HS" || upper == "PLA HS") return "OGFL95";

    // ABS/ASA variants
    if (upper == "ABS")           return "OGFB99";
    if (upper == "ASA")           return "OGFB98";

    // PETG/PET variants
    if (upper == "PETG" || upper == "PET") return "OGFG99";
    if (upper == "PCTG")          return "OGFG97";

    // PA/Nylon variants
    if (upper == "PA" || upper == "NYLON") return "OGFN99";
    if (upper == "PA-CF")         return "OGFN98";
    if (upper == "PPA" || upper == "PPA-CF") return "OGFN97";
    if (upper == "PPA-GF")        return "OGFN96";

    // PC variants
    if (upper == "PC")            return "OGFC99";

    // PP/PE variants
    if (upper == "PE")            return "OGFP99";
    if (upper == "PP")            return "OGFP97";

    // Support materials
    if (upper == "PVA")           return "OGFS99";
    if (upper == "HIPS")          return "OGFS98";
    if (upper == "BVOH")          return "OGFS97";

    // TPU variants
    if (upper == "TPU")           return "OGFU99";

    // Other materials
    if (upper == "EVA")           return "OGFR99";
    if (upper == "PHA")           return "OGFR98";
    if (upper == "COPE")          return "OGFLC99";
    if (upper == "SBS")           return "OFLSBS99";

    return UNKNOWN_FILAMENT_ID;
}

std::string resolve_filament_type(const PresetCollection& filaments, const std::string& reported)
{
    std::vector<std::string> want = name_tokens_of(reported);
    if (want.empty())
        return {};

    // The one synonym the profiles do not spell out for themselves.
    for (auto& t : want)
        if (t == "NYLON")
            t = "PA";

    std::string best;
    size_t      best_tokens = 0;
    size_t      best_len    = 0;

    for (const auto& p : filaments.get_presets()) {
        if (!(p.is_visible && p.is_compatible))
            continue;
        const std::string type = p.config.opt_string("filament_type", 0u);
        if (type.empty() || type == best)
            continue;

        const std::vector<std::string> have = name_tokens_of(type);
        if (have.empty())
            continue;
        bool qualifies = true;
        for (const auto& t : have)
            if (std::find(want.begin(), want.end(), t) == want.end()) {
                qualifies = false;
                break;
            }
        if (!qualifies)
            continue;

        // Most specific wins: more tokens, then longer text, then name order so
        // the result never depends on collection order.
        const bool better = have.size() != best_tokens ? have.size() > best_tokens
                          : type.size() != best_len    ? type.size() > best_len
                                                       : (best.empty() || type < best);
        if (better) {
            best        = type;
            best_tokens = have.size();
            best_len    = type.size();
        }
    }
    return best;
}

// Parse an "RRGGBB" or "RRGGBBAA" hex color (optional leading '#') into a
// 24-bit 0xRRGGBB value, ignoring any trailing alpha.  Returns false when the
// string does not start with at least six hex digits.
static bool parse_rgb_hex(const std::string& color, unsigned int& rgb_out)
{
    const size_t start = (!color.empty() && color.front() == '#') ? 1 : 0;
    if (color.size() < start + 6)
        return false;
    const std::string rgb = color.substr(start, 6);
    if (rgb.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        return false;
    rgb_out = static_cast<unsigned int>(std::stoul(rgb, nullptr, 16));
    return true;
}

// Squared Euclidean distance between two 0xRRGGBB colors.
static unsigned int rgb_distance(unsigned int a, unsigned int b)
{
    const int dr = static_cast<int>(a & 0xff)         - static_cast<int>(b & 0xff);
    const int dg = static_cast<int>((a >> 8) & 0xff)  - static_cast<int>((b >> 8) & 0xff);
    const int db = static_cast<int>((a >> 16) & 0xff) - static_cast<int>((b >> 16) & 0xff);
    return static_cast<unsigned int>(dr * dr + dg * dg + db * db);
}

// id closest-color match: among visible, compatible presets whose filament_id is
// `group` + "_" + a 6-digit hex color, return the id nearest (RGB) to target_rgb.
// The bare `group` (no color suffix) belongs to the exact-id matches, not here.
//
// The strict 6-hex suffix is what makes this name-safe: group "..._PLA_Plus"
// matches "..._PLA_Plus_FF0000" but never "..._PLA_Plus_Matte_FF0000" or a bare
// "..._PLA".  There is deliberately no distance floor -- the vendor+filament
// group is the scope limit and color only breaks ties, so a single authored
// color still applies to every color of that vendor+filament.
static const Preset* id_closest_color(const PresetCollection& filaments,
                                      const std::string&      group,
                                      unsigned int            target_rgb)
{
    const Preset* best          = nullptr;
    unsigned int  best_distance = std::numeric_limits<unsigned int>::max();

    for (const auto& p : filaments.get_presets()) {
        if (!(p.is_visible && p.is_compatible))
            continue;
        const std::string& id = p.filament_id;
        if (id.size() != group.size() + 7 || id[group.size()] != '_'
            || !boost::iequals(id.substr(0, group.size()), group))
            continue;
        unsigned int cand_rgb = 0;
        if (!parse_rgb_hex(id.substr(group.size() + 1), cand_rgb)) // 6-char suffix
            continue;
        const unsigned int distance = rgb_distance(target_rgb, cand_rgb);
        if (distance < best_distance) {
            best_distance = distance;
            best          = &p;
        }
    }
    return best;
}

// config-field match: among visible, compatible presets whose config
// filament_type equals `type` (and, when vendor_norm is non-empty, whose
// normalized filament_vendor equals it case-insensitively), return a preset.
// When have_color, the nearest default_filament_colour to target_rgb wins;
// otherwise the first match is returned.  Empty vendor_norm means "any vendor".
//
// The vendor is normalized with sanitize_for_id() on both sides and compared
// case-insensitively, so "SUNLU" matches a preset's "Sunlu", and a separator
// difference matches once both sides collapse to the same token ("Acme Inc" and
// "Acme-Inc" both become "Acme_Inc").  It does not bridge a separator that only
// one side has: a reported "eSUN" will not match a preset's "e-Sun", because
// only the latter gains an underscore.
//
// Derived presets are eligible, not just bases.  This is the path a GUI user
// has -- they save a preset from a vendor profile, which makes it derived, and
// set filament_vendor/default_filament_colour on it.  Restricting the match to
// bases excluded exactly those presets while admitting the hand-authored
// user-root ones, so whether a preset could be matched came down to whether its
// JSON happened to carry an "inherits" key.
// When `name_tokens` is non-empty, only presets whose name carries every token
// of the reported product qualify.  This is the signal that separates two
// profiles of the same vendor and type -- the printer says "PLA Matte", and
// nothing in filament_vendor/filament_type/default_filament_colour can tell an
// "eSun PLA Matte" profile from an "eSun PLA+" one.
static const Preset* config_match(const PresetCollection&         filaments,
                                  const std::string&              vendor_norm,
                                  const std::string&              type,
                                  const std::vector<std::string>& name_tokens,
                                  bool                            have_color,
                                  unsigned int                    target_rgb)
{
    const Preset* best          = nullptr;
    unsigned int  best_distance = std::numeric_limits<unsigned int>::max();

    for (const auto& p : filaments.get_presets()) {
        if (!(p.is_visible && p.is_compatible))
            continue;
        if (p.config.opt_string("filament_type", 0u) != type)
            continue;
        if (!vendor_norm.empty()
            && !boost::iequals(sanitize_for_id(p.config.opt_string("filament_vendor", 0u)), vendor_norm))
            continue;
        if (!name_tokens.empty() && !name_covers(p.name, name_tokens))
            continue;
        if (!have_color)
            return &p; // first match

        // Presets store color as "#RRGGBB".  A preset that declares no usable
        // color sits this level out: it has no color evidence to offer, and
        // scoring it as some default would let it compete on a color it never
        // claimed.  It stays eligible on the colorless levels below.
        unsigned int p_rgb = 0;
        if (!parse_rgb_hex(p.config.opt_string("default_filament_colour", 0u), p_rgb))
            continue;
        const unsigned int distance = rgb_distance(target_rgb, p_rgb);
        if (distance < best_distance) {
            best_distance = distance;
            best          = &p;
        }
    }
    return best;
}

// Resolve the best filament_id for `input` by walking the V/F/C/P cascade
// documented in FilamentMatcher.hpp, from most to least specific.
FilamentMatchResult resolve(const PresetCollection* filaments, const FilamentMatchInput& input)
{
    const bool have_prefix = !input.prefix.empty();
    const bool have_name   = !input.filament_name.empty();

    // C: parse the reported color once (used by every closest-color match).
    unsigned int target_rgb = 0;
    const bool   have_color = parse_rgb_hex(input.color, target_rgb);

    BOOST_LOG_TRIVIAL(info) << "FilamentMatcher::resolve: prefix=\"" << input.prefix
                            << "\" vendor=\"" << input.vendor_name << "\" name=\""
                            << input.filament_name << "\" type=\"" << input.tray_type
                            << "\" color=\"" << input.color << "\"";

    // V[]: vendor identities, most specific first.
    std::vector<std::string> vendors;
    if (!input.vendor_name.empty()) vendors.push_back(input.vendor_name);
    if (input.vendor_type >= 0)     vendors.push_back(std::to_string(input.vendor_type));

    // F[] for the vendor-bearing levels (1 & 2).  Per the name-safety rule, the
    // coarse base type is excluded here whenever a specific filament name exists,
    // so e.g. "PLA Plus" never collapses onto a bare "PLA" profile.
    std::vector<std::string> fil_vf;
    if (have_name)               fil_vf.push_back(input.filament_name);
    if (input.filament_idx > 0)  fil_vf.push_back(std::to_string(input.filament_idx));
    if (!have_name && !input.tray_type.empty()) fil_vf.push_back(input.tray_type);

    // F[] for the vendor-less levels (3 & 4): all filament identities, base type
    // included.
    std::vector<std::string> fil_f = fil_vf;
    if (have_name && !input.tray_type.empty()) fil_f.push_back(input.tray_type);

    // Config-field matching uses only the string forms and never a prefix.
    //
    // The vendor+type variants run even when a filament_name is present.  They
    // are coarser than an id match -- they can only key F on the base type --
    // but they already sit below every id match in the cascade, so a reported
    // name gets its chance first.  Skipping them outright whenever a name
    // existed switched vendor matching off for every printer that reports one,
    // which handed those slots to the vendor-less level 3 instead: a strictly
    // worse answer, since it drops the vendor rather than the name.
    const bool cfg_vf = filaments && !input.vendor_name.empty() && !input.tray_type.empty();
    const bool cfg_f  = filaments && !input.tray_type.empty();

    // Tokens of the reported product name, used to narrow the config rungs.
    // Empty when the printer reports no name, which leaves those rungs exactly
    // as they were for vendor+type-only printers such as Snapmaker.
    const std::vector<std::string> name_tokens = have_name ? name_tokens_of(input.filament_name)
                                                           : std::vector<std::string>{};
    const bool                     have_names  = !name_tokens.empty();

    auto matched = [](const Preset* p, const char* label) {
        BOOST_LOG_TRIVIAL(info) << "  -> matched at " << label << ": \"" << p->filament_id
                                << "\" (preset \"" << p->name << "\")";
        return FilamentMatchResult{p->filament_id, p->name};
    };
    auto matched_id = [](const std::string& id, const char* label) {
        BOOST_LOG_TRIVIAL(info) << "  -> matched at " << label << ": \"" << id << "\"";
        return FilamentMatchResult{id, std::string()};
    };

    if (filaments) {
        // ---- Level 1: V + F + closest color ----
        if (have_color) {
            for (const auto& v : vendors)
                for (const auto& f : fil_vf) {
                    if (have_prefix) {
                        const Preset* p = id_closest_color(*filaments, input.prefix + "_" + v + "_" + f, target_rgb);
                        if (p) return matched(p, "1a id");
                    }
                    const Preset* p = id_closest_color(*filaments, v + "_" + f, target_rgb);
                    if (p) return matched(p, "1b id");
                }
        }
        // Config-field rungs for the vendor+type scope, most specific first.
        //
        // The product name outranks the color.  A preset that matches what the
        // printer said the filament *is* beats one that merely matches what
        // color it is: keying these rungs on color alone let an "eSun PLA+
        // Black" profile (#000000) win an "eSun PLA Matte" spool reported as
        // 060606, because it sits nearer than the matte profile's #202020.  No
        // amount of color precision fixes that -- color was never an identity.
        if (cfg_vf) {
            if (have_names && have_color) {
                const Preset* p = config_match(*filaments, input.vendor_name, input.tray_type, name_tokens, true, target_rgb);
                if (p) return matched(p, "1c cfg V+T+name+color");
            }
            if (have_names) {
                const Preset* p = config_match(*filaments, input.vendor_name, input.tray_type, name_tokens, false, 0);
                if (p) return matched(p, "1d cfg V+T+name");
            }
        }

        // ---- Level 2: V + F ----
        for (const auto& v : vendors)
            for (const auto& f : fil_vf) {
                if (have_prefix) {
                    const Preset* p = find_visible_preset(*filaments, input.prefix + "_" + v + "_" + f);
                    if (p) return matched(p, "2a id");
                }
                const Preset* p = find_visible_preset(*filaments, v + "_" + f);
                if (p) return matched(p, "2b id");
            }
        // The color-only rungs wait until every identity-bearing rung above has
        // had its turn -- an authored filament_id states what a preset *is*, so
        // it outranks a match that only knows the vendor, the type and a nearby
        // color.  Identity first, then color, in that order throughout.
        if (cfg_vf) {
            if (have_color) {
                const Preset* p = config_match(*filaments, input.vendor_name, input.tray_type, {}, true, target_rgb);
                if (p) return matched(p, "2c cfg V+T+color");
            }
            const Preset* p = config_match(*filaments, input.vendor_name, input.tray_type, {}, false, 0);
            if (p) return matched(p, "2d cfg V+T");
        }

        // ---- Level 3: F + closest color ----
        if (have_color) {
            for (const auto& f : fil_f) {
                if (have_prefix) {
                    const Preset* p = id_closest_color(*filaments, input.prefix + "_" + f, target_rgb);
                    if (p) return matched(p, "3a id");
                }
                const Preset* p = id_closest_color(*filaments, f, target_rgb);
                if (p) return matched(p, "3b id");
            }
        }
        // Same rung order as level 1, with the vendor dropped.
        if (cfg_f) {
            if (have_names && have_color) {
                const Preset* p = config_match(*filaments, "", input.tray_type, name_tokens, true, target_rgb);
                if (p) return matched(p, "3c cfg T+name+color");
            }
            if (have_names) {
                const Preset* p = config_match(*filaments, "", input.tray_type, name_tokens, false, 0);
                if (p) return matched(p, "3d cfg T+name");
            }
        }

        // ---- Level 4: F ----
        for (const auto& f : fil_f) {
            if (have_prefix) {
                const Preset* p = find_visible_preset(*filaments, input.prefix + "_" + f);
                if (p) return matched(p, "4a id");
            }
            const Preset* p = find_visible_preset(*filaments, f);
            if (p) return matched(p, "4b id");
        }
        if (cfg_f) {
            // Color-only rung, after the vendor-less id rungs above.
            if (have_color) {
                const Preset* p = config_match(*filaments, "", input.tray_type, {}, true, target_rgb);
                if (p) return matched(p, "4c cfg T+color");
            }
            // filament_id_by_type() answers with a type's generic id rather than
            // one particular preset, so this level names no preset.
            std::string id = filaments->filament_id_by_type(input.tray_type);
            if (!id.empty() && id != UNKNOWN_FILAMENT_ID) return matched_id(id, "4d cfg T");
        }
    }
    // No preset bundle (early init): fall back to the constructed numeric id when
    // the printer supplied numeric indices, preserving the previous offline path.
    else if (have_prefix && input.vendor_type >= 0 && input.filament_idx > 0) {
        return matched_id(input.prefix + "_" + std::to_string(input.vendor_type) + "_"
                          + std::to_string(input.filament_idx), "no-bundle numeric");
    }

    // ---- Level 5: static generic ID mapping (built-in codes) ----
    if (!input.tray_type.empty()) {
        std::string id = map_type_to_generic_id(input.tray_type);
        BOOST_LOG_TRIVIAL(info) << "  Level 5: map_type_to_generic_id(\"" << input.tray_type
                                << "\") -> \"" << id << "\"";
        return FilamentMatchResult{id, std::string()};
    }

    BOOST_LOG_TRIVIAL(info) << "  -> no match, returning UNKNOWN";
    return FilamentMatchResult{UNKNOWN_FILAMENT_ID, std::string()};
}

} // namespace FilamentMatcher
} // namespace Slic3r
