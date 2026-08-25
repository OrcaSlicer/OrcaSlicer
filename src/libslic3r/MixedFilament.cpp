#include "MixedFilament.hpp"
#include "filament_mixer.h"
#include "libslic3r.h"

#include <algorithm>
#include <atomic>
#include <boost/log/trivial.hpp>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace Slic3r {

// Upper bound on total filaments (physical + virtual mixed). Mirrors the GUI-side
// MAXIMUM_FILAMENT_NUMBER constant; kept here so libslic3r has no GUI dependency.
static constexpr size_t MAXIMUM_FILAMENT_NUMBER = 64;

namespace {

// Runtime state for mixed filament auto-generation feature.
// This is synchronized with the "auto_generate_gradients" config setting.
// Initial value is false, but will be overridden by AppConfig during application startup.
// See: GUI_App::init_app_config() which loads the actual config value.
std::atomic_bool s_mixed_filament_auto_generate_enabled { false };

} // namespace

static uint64_t canonical_pair_key(unsigned int a, unsigned int b)
{
    const unsigned int lo = std::min(a, b);
    const unsigned int hi = std::max(a, b);
    return (uint64_t(lo) << 32) | uint64_t(hi);
}

// ---------------------------------------------------------------------------
// Colour helpers (internal)
// ---------------------------------------------------------------------------

struct RGB {
    int r = 0, g = 0, b = 0;
};

struct RGBf {
    float r = 0.f, g = 0.f, b = 0.f;
};

[[maybe_unused]] static float clamp01(float v)
{
    return std::max(0.f, std::min(1.f, v));
}

[[maybe_unused]] static RGBf to_rgbf(const RGB &c)
{
    return {
        clamp01(static_cast<float>(c.r) / 255.f),
        clamp01(static_cast<float>(c.g) / 255.f),
        clamp01(static_cast<float>(c.b) / 255.f)
    };
}

[[maybe_unused]] static RGB to_rgb8(const RGBf &c)
{
    auto to_u8 = [](float v) -> int {
        return std::clamp(static_cast<int>(std::round(clamp01(v) * 255.f)), 0, 255);
    };
    return { to_u8(c.r), to_u8(c.g), to_u8(c.b) };
}


// Convert RGB to an artist-pigment style RYB space.
// This is an approximation, but it gives expected pair mixes:
// Red + Blue -> Purple, Blue + Yellow -> Green, Red + Yellow -> Orange.

// Legacy RYB conversion helpers kept for reference.
// Active code paths use FilamentMixer.
[[maybe_unused]] static RGBf rgb_to_ryb(RGBf in)
{
    float r = clamp01(in.r);
    float g = clamp01(in.g);
    float b = clamp01(in.b);

    const float white = std::min({ r, g, b });
    r -= white;
    g -= white;
    b -= white;

    const float max_g = std::max({ r, g, b });

    float y = std::min(r, g);
    r -= y;
    g -= y;

    if (b > 0.f && g > 0.f) {
        b *= 0.5f;
        g *= 0.5f;
    }

    y += g;
    b += g;

    const float max_y = std::max({ r, y, b });
    if (max_y > 1e-6f) {
        const float n = max_g / max_y;
        r *= n;
        y *= n;
        b *= n;
    }

    r += white;
    y += white;
    b += white;
    return { clamp01(r), clamp01(y), clamp01(b) };
}

[[maybe_unused]] static RGBf ryb_to_rgb(RGBf in)
{
    float r = clamp01(in.r);
    float y = clamp01(in.g);
    float b = clamp01(in.b);

    const float white = std::min({ r, y, b });
    r -= white;
    y -= white;
    b -= white;

    const float max_y = std::max({ r, y, b });

    float g = std::min(y, b);
    y -= g;
    b -= g;

    if (b > 0.f && g > 0.f) {
        b *= 2.f;
        g *= 2.f;
    }

    r += y;
    g += y;

    const float max_g = std::max({ r, g, b });
    if (max_g > 1e-6f) {
        const float n = max_y / max_g;
        r *= n;
        g *= n;
        b *= n;
    }

    r += white;
    g += white;
    b += white;
    return { clamp01(r), clamp01(g), clamp01(b) };
}

// Parse "#RRGGBB" to RGB.  Returns black on failure.
static RGB parse_hex_color(const std::string &hex)
{
    RGB c;
    if (hex.size() >= 7 && hex[0] == '#') {
        try {
            c.r = std::stoi(hex.substr(1, 2), nullptr, 16);
            c.g = std::stoi(hex.substr(3, 2), nullptr, 16);
            c.b = std::stoi(hex.substr(5, 2), nullptr, 16);
        } catch (...) {
            c = {};
        }
    }
    return c;
}

static std::string rgb_to_hex(const RGB &c)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return std::string(buf);
}

[[maybe_unused]] static std::string blend_color_ryb_legacy(const RGB &rgb_a,
                                                           const RGB &rgb_b,
                                                           int        ratio_a,
                                                           int        ratio_b)
{
    const int safe_a = std::max(0, ratio_a);
    const int safe_b = std::max(0, ratio_b);
    const float total = static_cast<float>(safe_a + safe_b);
    const float wa    = (total > 0.f) ? static_cast<float>(safe_a) / total : 0.5f;
    const float wb    = 1.f - wa;

    const RGBf color_a = to_rgbf(rgb_a);
    const RGBf color_b = to_rgbf(rgb_b);
    const RGBf ryb_a = rgb_to_ryb(color_a);
    const RGBf ryb_b = rgb_to_ryb(color_b);

    RGBf ryb_out;
    ryb_out.r = wa * ryb_a.r + wb * ryb_b.r;
    ryb_out.g = wa * ryb_a.g + wb * ryb_b.g;
    ryb_out.b = wa * ryb_a.b + wb * ryb_b.b;

    RGBf rgb_out = ryb_to_rgb(ryb_out);
    const float v_out = std::max({ rgb_out.r, rgb_out.g, rgb_out.b });
    const float v_tgt = wa * std::max({ color_a.r, color_a.g, color_a.b }) +
                        wb * std::max({ color_b.r, color_b.g, color_b.b });
    if (v_out > 1e-6f && v_tgt > 0.f) {
        const float scale = v_tgt / v_out;
        rgb_out.r = clamp01(rgb_out.r * scale);
        rgb_out.g = clamp01(rgb_out.g * scale);
        rgb_out.b = clamp01(rgb_out.b * scale);
    }

    return rgb_to_hex(to_rgb8(rgb_out));
}

static int clamp_int(int v, int lo, int hi)
{
    return std::max(lo, std::min(hi, v));
}

static int normalize_distribution_mode_without_pointillism(int distribution_mode, const std::string &gradient_component_ids);

static float clamp_surface_offset(float v)
{
    return std::clamp(v, -2.f, 2.f);
}

float MixedFilamentManager::canonical_signed_bias_value(float component_a_surface_offset, float component_b_surface_offset)
{
    const float offset_a = clamp_surface_offset(component_a_surface_offset);
    const float offset_b = clamp_surface_offset(component_b_surface_offset);

    if (std::abs(offset_b) > EPSILON)
        return offset_b;
    if (std::abs(offset_a) > EPSILON)
        return (offset_a >= 0.f) ? -std::abs(offset_a) : std::abs(offset_a);
    return 0.f;
}

std::string MixedFilamentManager::format_surface_offset_token(float value)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4) << clamp_surface_offset(value);
    std::string out = ss.str();
    while (!out.empty() && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    if (out == "-0")
        out = "0";
    return out.empty() ? std::string("0") : out;
}

static int safe_ratio_from_height(float h, float unit)
{
    if (unit <= 1e-6f)
        return 1;
    return std::max(0, int(std::lround(h / unit)));
}

static void compute_gradient_heights(const MixedFilament &mf, float lower_bound, float upper_bound, float &h_a, float &h_b)
{
    const int   mix_b = clamp_int(mf.mix_b_percent, 0, 100);
    const float pct_b = float(mix_b) / 100.f;
    const float pct_a = 1.f - pct_b;
    const float lo    = std::max(0.01f, lower_bound);
    const float hi    = std::max(lo, upper_bound);

    h_a = lo + pct_a * (hi - lo);
    h_b = lo + pct_b * (hi - lo);
}

void MixedFilamentManager::normalize_ratio_pair(int &a, int &b)
{
    a = std::max(0, a);
    b = std::max(0, b);
    if (a == 0 && b == 0) {
        a = 1;
        return;
    }
    if (a > 0 && b > 0) {
        const int g = std::gcd(a, b);
        if (g > 1) {
            a /= g;
            b /= g;
        }
    }
}

static void compute_gradient_ratios(MixedFilament &mf, int gradient_mode, float lower_bound, float upper_bound)
{
    if (gradient_mode == 1) {
        // Height-weighted mode:
        // map blend to [lower, upper], then convert relative heights to an integer cadence.
        float h_a = 0.f;
        float h_b = 0.f;
        compute_gradient_heights(mf, lower_bound, upper_bound, h_a, h_b);
        // Use lower-bound as quantization unit so this mode differs clearly from layer-cycle mode.
        const float unit = std::max(0.01f, std::min(h_a, h_b));
        mf.ratio_a = std::max(1, safe_ratio_from_height(h_a, unit));
        mf.ratio_b = std::max(1, safe_ratio_from_height(h_b, unit));
    } else {
        // Layer-cycle mode:
        // derive a gradual integer cadence directly from the blend ratio
        // by fixing the minority side to one layer and scaling the majority.
        const int mix_b = clamp_int(mf.mix_b_percent, 0, 100);
        if (mix_b <= 0) {
            mf.ratio_a = 1;
            mf.ratio_b = 0;
        } else if (mix_b >= 100) {
            mf.ratio_a = 0;
            mf.ratio_b = 1;
        } else {
            const int pct_b = mix_b;
            const int pct_a = 100 - pct_b;
            const bool b_is_major = pct_b >= pct_a;
            const int major_pct = b_is_major ? pct_b : pct_a;
            const int minor_pct = b_is_major ? pct_a : pct_b;
            const int major_layers = std::max(1, int(std::lround(double(major_pct) / double(std::max(1, minor_pct)))));
            mf.ratio_a = b_is_major ? 1 : major_layers;
            mf.ratio_b = b_is_major ? major_layers : 1;
        }
    }

    MixedFilamentManager::normalize_ratio_pair(mf.ratio_a, mf.ratio_b);
}

int MixedFilamentManager::safe_mod(int x, int m)
{
    if (m <= 0)
        return 0;
    int r = x % m;
    return (r < 0) ? (r + m) : r;
}

static int dithering_phase_step(int cycle)
{
    if (cycle <= 1)
        return 0;
    int step = cycle / 2 + 1;
    while (std::gcd(step, cycle) != 1)
        ++step;
    return step % cycle;
}

static bool use_component_b_advanced_dither(int layer_index, int ratio_a, int ratio_b)
{
    ratio_a = std::max(0, ratio_a);
    ratio_b = std::max(0, ratio_b);

    const int cycle = ratio_a + ratio_b;
    if (cycle <= 0 || ratio_b <= 0)
        return false;
    if (ratio_a <= 0)
        return true;

    // Base ordered pattern: as evenly distributed as possible for ratio_b/cycle.
    const int pos = MixedFilamentManager::safe_mod(layer_index, cycle);
    const int cycle_idx = (layer_index - pos) / cycle;

    // Rotate each cycle to avoid visible long-period vertical striping.
    const int phase = MixedFilamentManager::safe_mod(cycle_idx * dithering_phase_step(cycle), cycle);
    const int p = MixedFilamentManager::safe_mod(pos + phase, cycle);

    const int b_before = (p * ratio_b) / cycle;
    const int b_after  = ((p + 1) * ratio_b) / cycle;
    return b_after > b_before;
}

static bool parse_row_definition(const std::string &row,
                                 unsigned int      &a,
                                 unsigned int      &b,
                                 uint64_t          &stable_id,
                                 bool              &enabled,
                                 bool              &custom,
                                 bool              &origin_auto,
                                 int               &mix_b_percent,
                                 bool              &pointillism_all_filaments,
                                 std::string       &gradient_component_ids,
                                 std::string       &gradient_component_weights,
                                 std::string       &manual_pattern,
                                 int               &distribution_mode,
                                 float             &component_a_surface_offset,
                                 float             &component_b_surface_offset,
                                 bool              &deleted,
                                 bool              &gradient_enabled,
                                 float             &gradient_start,
                                 float             &gradient_end,
                                 int               &cm_mode)
{
    auto trim_copy = [](const std::string &s) {
        size_t lo = 0;
        size_t hi = s.size();
        while (lo < hi && std::isspace(static_cast<unsigned char>(s[lo])))
            ++lo;
        while (hi > lo && std::isspace(static_cast<unsigned char>(s[hi - 1])))
            --hi;
        return s.substr(lo, hi - lo);
    };

    auto parse_int_token = [&trim_copy](const std::string &tok, int &out) {
        const std::string t = trim_copy(tok);
        if (t.empty())
            return false;
        try {
            size_t consumed = 0;
            int v = std::stoi(t, &consumed);
            if (consumed != t.size())
                return false;
            out = v;
            return true;
        } catch (...) {
            return false;
        }
    };

    auto parse_uint64_token = [&trim_copy](const std::string &tok, uint64_t &out) {
        const std::string t = trim_copy(tok);
        if (t.empty())
            return false;
        try {
            size_t consumed = 0;
            const unsigned long long v = std::stoull(t, &consumed);
            if (consumed != t.size())
                return false;
            out = uint64_t(v);
            return true;
        } catch (...) {
            return false;
        }
    };

    auto parse_float_token = [&trim_copy](const std::string &tok, float &out) {
        const std::string t = trim_copy(tok);
        if (t.empty())
            return false;
        try {
            size_t consumed = 0;
            const float v = std::stof(t, &consumed);
            if (consumed != t.size())
                return false;
            out = v;
            return true;
        } catch (...) {
            return false;
        }
    };

    std::vector<std::string> tokens;
    std::stringstream ss(row);
    std::string token;
    while (std::getline(ss, token, ','))
        tokens.emplace_back(trim_copy(token));

    if (tokens.size() < 4)
        return false;

    int values[5] = { 0, 0, 1, 1, 50 };
    if (tokens.size() == 4) {
        // Legacy: a,b,enabled,mix
        if (!parse_int_token(tokens[0], values[0]) ||
            !parse_int_token(tokens[1], values[1]) ||
            !parse_int_token(tokens[2], values[2]) ||
            !parse_int_token(tokens[3], values[4]))
            return false;
    } else {
        // Current: a,b,enabled,custom,mix[,pointillism_all[,pattern]]
        for (size_t i = 0; i < 5; ++i)
            if (!parse_int_token(tokens[i], values[i]))
                return false;
    }

    if (values[0] <= 0 || values[1] <= 0)
        return false;

    a = unsigned(values[0]);
    b = unsigned(values[1]);
    stable_id = 0;
    enabled = (values[2] != 0);
    custom = (tokens.size() == 4) ? true : (values[3] != 0);
    origin_auto = !custom;
    mix_b_percent = clamp_int(values[4], 0, 100);
    pointillism_all_filaments = false;
    gradient_component_ids.clear();
    gradient_component_weights.clear();
    manual_pattern.clear();
    distribution_mode = int(MixedFilament::Simple);
    component_a_surface_offset = 0.f;
    component_b_surface_offset = 0.f;
    deleted = false;
    gradient_enabled = false;
    gradient_start = MixedFilament::k_default_gradient_dominant;
    gradient_end   = MixedFilament::k_default_gradient_minority;
    cm_mode        = -1;

    size_t token_idx = 5;
    if (tokens.size() >= 6) {
        // Backward compatibility:
        // - old: token[5] is pointillism flag ("0"/"1")
        // - old: token[5] is pattern ("12", "1212", ...)
        // - new: token[5] may be metadata token ("g..." / "m...")
        const std::string &legacy = tokens[5];
        if (legacy == "0" || legacy == "1") {
            pointillism_all_filaments = (legacy == "1");
            token_idx = 6;
        } else if (legacy.empty() || legacy[0] == 'g' || legacy[0] == 'G' ||
                   legacy[0] == 'm' || legacy[0] == 'M' ||
                   legacy[0] == 'r' || legacy[0] == 'R') {
            token_idx = 5;
        } else {
            manual_pattern = legacy;
            token_idx = 6;
        }
    }

    std::vector<std::string> pattern_tokens;
    pattern_tokens.reserve(tokens.size() > token_idx ? tokens.size() - token_idx : 1);
    if (!manual_pattern.empty())
        pattern_tokens.push_back(manual_pattern);
    for (size_t i = token_idx; i < tokens.size(); ++i) {
        const std::string &tok = tokens[i];
        if (tok.empty())
            continue;
        if (tok[0] == 'g' || tok[0] == 'G') {
            gradient_component_ids = tok.substr(1);
            continue;
        }
        if (tok[0] == 'w' || tok[0] == 'W') {
            gradient_component_weights = tok.substr(1);
            continue;
        }
        if (tok[0] == 'm' || tok[0] == 'M') {
            int parsed_mode = distribution_mode;
            if (parse_int_token(tok.substr(1), parsed_mode))
                distribution_mode = clamp_int(parsed_mode, int(MixedFilament::LayerCycle), int(MixedFilament::Simple));
            continue;
        }
        if (tok[0] == 'z' || tok[0] == 'Z')
            continue; // ORCA: legacy Local-Z sublayer token (subdivision removed) — consumed and ignored for project compatibility
        if ((tok[0] == 'x' || tok[0] == 'X') && tok.size() >= 3) {
            const char component = char(std::tolower(static_cast<unsigned char>(tok[1])));
            if (component == 'a' || component == 'b') {
                float parsed_offset = component == 'a' ? component_a_surface_offset : component_b_surface_offset;
                if (parse_float_token(tok.substr(2), parsed_offset)) {
                    if (component == 'a')
                        component_a_surface_offset = clamp_surface_offset(parsed_offset);
                    else
                        component_b_surface_offset = clamp_surface_offset(parsed_offset);
                }
                continue;
            }
        }
        if (tok[0] == 'd' || tok[0] == 'D') {
            int parsed_deleted = deleted ? 1 : 0;
            if (parse_int_token(tok.substr(1), parsed_deleted))
                deleted = parsed_deleted != 0;
            continue;
        }
        if (tok[0] == 'o' || tok[0] == 'O') {
            int parsed_origin_auto = origin_auto ? 1 : 0;
            if (parse_int_token(tok.substr(1), parsed_origin_auto))
                origin_auto = parsed_origin_auto != 0;
            continue;
        }
        if (tok[0] == 'u' || tok[0] == 'U') {
            uint64_t parsed_stable_id = stable_id;
            if (parse_uint64_token(tok.substr(1), parsed_stable_id))
                stable_id = parsed_stable_id;
            continue;
        }
        if ((tok[0] == 'c' || tok[0] == 'C') && tok.size() >= 3 && (tok[1] == 'm' || tok[1] == 'M')) {
            int v = cm_mode;
            if (parse_int_token(tok.substr(2), v))
                cm_mode = std::clamp(v, -1, 3);
            continue;
        }
        if (tok[0] == 'r' || tok[0] == 'R') {
            const std::string body = tok.substr(1);
            size_t s1 = body.find('/');
            size_t s2 = (s1 == std::string::npos) ? std::string::npos : body.find('/', s1 + 1);
            if (s1 != std::string::npos && s2 != std::string::npos) {
                int   parsed_flag  = gradient_enabled ? 1 : 0;
                float parsed_start = gradient_start;
                float parsed_end   = gradient_end;
                if (parse_int_token(body.substr(0, s1), parsed_flag) &&
                    parse_float_token(body.substr(s1 + 1, s2 - s1 - 1), parsed_start) &&
                    parse_float_token(body.substr(s2 + 1), parsed_end)) {
                    gradient_enabled = parsed_flag != 0;
                    if (parsed_start > 0.f && parsed_start < 1.f) gradient_start = parsed_start;
                    if (parsed_end   > 0.f && parsed_end   < 1.f) gradient_end   = parsed_end;
                }
            }
            continue;
        }
        pattern_tokens.push_back(tok);
    }

    if (!pattern_tokens.empty()) {
        std::ostringstream joined_pattern;
        for (size_t i = 0; i < pattern_tokens.size(); ++i) {
            if (i != 0)
                joined_pattern << ',';
            joined_pattern << pattern_tokens[i];
        }
        manual_pattern = joined_pattern.str();
    }

    pointillism_all_filaments = false;
    distribution_mode = normalize_distribution_mode_without_pointillism(distribution_mode, gradient_component_ids);
    
    // Validate gradient parameters if gradient is enabled
    if (gradient_enabled) {
        // Ensure start and end are in valid range (0.01 to 0.99)
        gradient_start = std::clamp(gradient_start, 0.01f, 0.99f);
        gradient_end   = std::clamp(gradient_end,   0.01f, 0.99f);
        
        // Ensure start and end are not too close (need meaningful gradient)
        if (std::abs(gradient_start - gradient_end) < MixedFilament::k_min_gradient_difference) {
            // Gradient range too small, disable gradient mode
            gradient_enabled = false;
        }
    }
    
    return true;
}

static std::vector<std::string> split_manual_pattern_groups(const std::string &pattern)
{
    std::vector<std::string> groups;
    if (pattern.empty())
        return groups;

    std::string current;
    for (const char c : pattern) {
        if (c == ',') {
            if (!current.empty()) {
                groups.emplace_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty())
        groups.emplace_back(std::move(current));
    return groups;
}

static std::string flatten_manual_pattern_groups(const std::string &pattern)
{
    std::string flattened;
    flattened.reserve(pattern.size());
    for (const char c : pattern)
        if (c != ',')
            flattened.push_back(c);
    return flattened;
}

// Basic tokenization of a single group (no comma) into token strings.
// - No '/' in group: legacy mode, each '1'-'9' char is one token.
// - Has '/' in group: split by '/', each segment is one token.
static std::vector<std::string> tokenize_pattern_group(const std::string &group)
{
    std::vector<std::string> tokens;
    if (group.empty())
        return tokens;

    for (size_t i = 0; i < group.size(); ++i) {
        char c = group[i];
        if (c >= '1' && c <= '9') {
            tokens.emplace_back(1, c);
        } else if (c == '[') {
            size_t j = i + 1;
            while (j < group.size() && group[j] >= '0' && group[j] <= '9')
                ++j;
            if (j > i + 1 && j < group.size() && group[j] == ']') {
                tokens.emplace_back(group.substr(i + 1, j - i - 1));
                i = j;
            }
        }
    }
    return tokens;
}

static bool mixed_filament_references_exceed_physical(const std::string &gradient_component_ids,
        const std::string &manual_pattern, size_t n)
{
    if (!gradient_component_ids.empty()) {
        auto ids = MixedFilamentManager::decode_gradient_component_ids(gradient_component_ids, 0);
        for (unsigned int id : ids) {
            if (id > n)
                return true;
        }
    }

    if (!manual_pattern.empty()) {
        const std::string norm = MixedFilamentManager::normalize_manual_pattern(manual_pattern);
        if (!norm.empty()) {
            auto groups = MixedFilamentManager::split_pattern_groups(norm);
            for (const auto &group : groups) {
                auto tokens = tokenize_pattern_group(group);
                for (const auto &token : tokens) {
                    if (token == "1" || token == "2")
                        continue;
                    char *end = nullptr;
                    unsigned long val = std::strtoul(token.c_str(), &end, 10);
                    if (*end == '\0' && val > n)
                        return true;
                }
            }
        }
    }

    return false;
}

std::vector<std::string> MixedFilamentManager::split_pattern_group_to_tokens(const std::string &group, size_t /*num_physical*/)
{
    return tokenize_pattern_group(group);
}

unsigned int MixedFilamentManager::physical_filament_from_token(const std::string &token, const MixedFilament &mf, size_t num_physical)
{
    // Cycle-mode invariant: component_a≡1, component_b≡2 always
    // (enforced by UI and MixedFilamentDialog::MODE_CYCLE).
    // Under this invariant the symbolic tokens "1"/"2" are identity
    // mappings — no ambiguity with direct physical IDs 1 and 2.
    if (token == "1")
        return (mf.component_a >= 1 && mf.component_a <= num_physical) ? mf.component_a : 0;
    if (token == "2")
        return (mf.component_b >= 1 && mf.component_b <= num_physical) ? mf.component_b : 0;

    char *end = nullptr;
    errno = 0;
    unsigned long id = std::strtoul(token.c_str(), &end, 10);
    if (errno != ERANGE && *end == '\0' && id >= 1 && id <= num_physical)
        return unsigned(id);

    return 0;
}

std::vector<std::string> MixedFilamentManager::split_pattern_groups(const std::string &pattern)
{
    std::vector<std::string> groups;
    std::string current;
    for (char c : pattern) {
        if (c == ',') {
            if (!current.empty()) {
                groups.emplace_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty())
        groups.emplace_back(std::move(current));
    return groups;
}

static int mix_percent_from_normalized_pattern(const std::string &pattern)
{
    const std::vector<std::string> groups = split_manual_pattern_groups(pattern);
    if (groups.empty())
        return 50;

    double blend_b = 0.0;
    for (const std::string &group : groups) {
        if (group.empty())
            continue;
        const std::vector<std::string> tokens = tokenize_pattern_group(group);
        if (tokens.empty())
            continue;
        const int count_b = int(std::count(tokens.begin(), tokens.end(), "2"));
        blend_b += double(count_b) / double(tokens.size());
    }
    return clamp_int(int(std::lround(100.0 * blend_b / double(groups.size()))), 0, 100);
}

std::string MixedFilamentManager::normalize_gradient_component_ids(const std::string &components)
{
    // Decode (no validation cap during normalization), then re-encode to canonical form.
    auto ids = decode_gradient_component_ids(components, kMaxPhysicalFilaments);
    return encode_gradient_component_ids(ids);
}

std::string MixedFilamentManager::encode_gradient_component_ids(const std::vector<unsigned int> &ids)
{
    bool extended = false;
    for (unsigned int id : ids)
        if (id > 9) { extended = true; break; }

    // Single extended ID: use leading '/' to disambiguate from legacy format
    if (extended && ids.size() == 1)
        return "/" + std::to_string(ids[0]);

    std::string out;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0 && extended) out.push_back('/');
        if (extended)
            out.append(std::to_string(ids[i]));
        else
            out.push_back(char('0' + ids[i]));
    }
    return out;
}

std::vector<unsigned int> MixedFilamentManager::decode_gradient_component_ids(const std::string &components,
                                                                               size_t             num_physical)
{
    std::vector<unsigned int> ids;
    if (components.empty())
        return ids;

    const bool validate = (num_physical > 0);
    std::unordered_set<unsigned int> seen;
    ids.reserve(components.size());

    // Extended format: /-separated decimal IDs
    if (components.find('/') != std::string::npos) {
        std::string token;
        for (const char c : components) {
            if (c == '/') {
                if (!token.empty()) {
                    const unsigned int id = unsigned(std::strtoul(token.c_str(), nullptr, 10));
                    if (id >= 1 && (!validate || id <= num_physical) && seen.insert(id).second)
                        ids.emplace_back(id);
                    token.clear();
                }
            } else {
                token.push_back(c);
            }
        }
        if (!token.empty()) {
            const unsigned int id = unsigned(std::strtoul(token.c_str(), nullptr, 10));
            if (id >= 1 && (!validate || id <= num_physical) && seen.insert(id).second)
                ids.emplace_back(id);
        }
    } else {
        // Legacy format: concatenated single-digit chars
        bool seen_legacy[10] = { false };
        for (const char c : components) {
            if (c < '1' || c > '9')
                continue;
            const unsigned int id = unsigned(c - '0');
            if (id == 0 || (validate && id > num_physical) || seen_legacy[id])
                continue;
            seen_legacy[id] = true;
            ids.emplace_back(id);
        }
    }
    return ids;
}

void MixedFilamentManager::expand_virtual_extruder_ids(std::vector<int> &ids, size_t num_physical) const
{
    if (num_physical == 0)
        return;
    std::vector<int> expanded;
    expanded.reserve(ids.size() * 2);
    for (int id : ids) {
        if (id > static_cast<int>(num_physical)) {
            const MixedFilament *mf = mixed_filament_from_id(
                static_cast<unsigned int>(id), num_physical);
            if (mf != nullptr && mf->enabled) {
                expanded.push_back(static_cast<int>(mf->component_a));
                expanded.push_back(static_cast<int>(mf->component_b));
                auto gradient_ids = decode_gradient_component_ids(
                    mf->gradient_component_ids, num_physical);
                for (unsigned int gid : gradient_ids)
                    expanded.push_back(static_cast<int>(gid));
            } else {
                expanded.push_back(id);
            }
        } else {
            expanded.push_back(id);
        }
    }
    ids = std::move(expanded);
}

static int normalize_distribution_mode_without_pointillism(int distribution_mode, const std::string &gradient_component_ids)
{
    const int clamped_mode = clamp_int(distribution_mode, int(MixedFilament::LayerCycle), int(MixedFilament::Simple));
    if (clamped_mode != int(MixedFilament::SameLayerPointillisme))
        return clamped_mode;

    const size_t gradient_count = MixedFilamentManager::decode_gradient_component_ids(gradient_component_ids, 0).size();
    return gradient_count >= 3 ? int(MixedFilament::LayerCycle) : int(MixedFilament::Simple);
}

static void disable_pointillism_mode(MixedFilament &mf)
{
    mf.pointillism_all_filaments = false;
    mf.distribution_mode = normalize_distribution_mode_without_pointillism(mf.distribution_mode, mf.gradient_component_ids);
}

static std::vector<int> parse_gradient_weight_tokens(const std::string &weights)
{
    std::vector<int> out;
    std::string token;
    for (const char c : weights) {
        if (c >= '0' && c <= '9') {
            token.push_back(c);
            continue;
        }
        if (!token.empty()) {
            out.emplace_back(std::max(0, std::atoi(token.c_str())));
            token.clear();
        }
    }
    if (!token.empty())
        out.emplace_back(std::max(0, std::atoi(token.c_str())));
    return out;
}

static std::vector<int> normalize_weight_vector_to_percent(const std::vector<int> &weights)
{
    std::vector<int> out(weights.size(), 0);
    if (weights.empty())
        return out;
    int sum = 0;
    for (const int w : weights)
        sum += std::max(0, w);
    if (sum <= 0)
        return out;

    std::vector<double> remainders(weights.size(), 0.);
    int assigned = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        const double exact = 100.0 * double(std::max(0, weights[i])) / double(sum);
        out[i] = int(std::floor(exact));
        remainders[i] = exact - double(out[i]);
        assigned += out[i];
    }
    int missing = std::max(0, 100 - assigned);
    while (missing > 0) {
        size_t best_idx = 0;
        double best_rem = -1.0;
        for (size_t i = 0; i < remainders.size(); ++i) {
            if (weights[i] <= 0)
                continue;
            if (remainders[i] > best_rem) {
                best_rem = remainders[i];
                best_idx = i;
            }
        }
        ++out[best_idx];
        remainders[best_idx] = 0.0;
        --missing;
    }
    return out;
}

static std::string normalize_gradient_component_weights(const std::string &weights, size_t expected_components)
{
    if (expected_components == 0)
        return std::string();
    std::vector<int> parsed = parse_gradient_weight_tokens(weights);
    if (parsed.size() != expected_components)
        return std::string();
    std::vector<int> normalized = normalize_weight_vector_to_percent(parsed);
    int sum = 0;
    for (const int v : normalized)
        sum += v;
    if (sum <= 0)
        return std::string();

    std::ostringstream ss;
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (i > 0)
            ss << '/';
        ss << normalized[i];
    }
    return ss.str();
}

static std::vector<int> decode_gradient_component_weights(const std::string &weights, size_t expected_components)
{
    if (expected_components == 0)
        return {};
    std::vector<int> parsed = parse_gradient_weight_tokens(weights);
    if (parsed.size() != expected_components)
        return {};
    std::vector<int> normalized = normalize_weight_vector_to_percent(parsed);
    int sum = 0;
    for (const int v : normalized)
        sum += v;
    return (sum > 0) ? normalized : std::vector<int>();
}

static std::vector<unsigned int> build_weighted_gradient_sequence(const std::vector<unsigned int> &ids,
                                                                  const std::vector<int>          &weights)
{
    if (ids.empty())
        return {};

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          counts;
    filtered_ids.reserve(ids.size());
    counts.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const int w = (i < weights.size()) ? std::max(0, weights[i]) : 0;
        if (w <= 0)
            continue;
        filtered_ids.emplace_back(ids[i]);
        counts.emplace_back(w);
    }
    if (filtered_ids.empty()) {
        filtered_ids = ids;
        counts.assign(ids.size(), 1);
    }

    int g = 0;
    for (const int c : counts)
        g = std::gcd(g, std::max(1, c));
    if (g > 1) {
        for (int &c : counts)
            c = std::max(1, c / g);
    }

    int cycle = std::accumulate(counts.begin(), counts.end(), 0);
    constexpr int k_max_cycle = 48;
    if (cycle > k_max_cycle) {
        const double scale = double(k_max_cycle) / double(cycle);
        for (int &c : counts)
            c = std::max(1, int(std::round(double(c) * scale)));
        cycle = std::accumulate(counts.begin(), counts.end(), 0);
        while (cycle > k_max_cycle) {
            auto it = std::max_element(counts.begin(), counts.end());
            if (it == counts.end() || *it <= 1)
                break;
            --(*it);
            --cycle;
        }
    }
    if (cycle <= 0)
        return {};

    std::vector<unsigned int> sequence;
    sequence.reserve(size_t(cycle));
    std::vector<int> emitted(counts.size(), 0);
    for (int pos = 0; pos < cycle; ++pos) {
        size_t best_idx = 0;
        double best_score = -1e9;
        for (size_t i = 0; i < counts.size(); ++i) {
            const double target = double((pos + 1) * counts[i]) / double(cycle);
            const double score = target - double(emitted[i]);
            if (score > best_score) {
                best_score = score;
                best_idx = i;
            }
        }
        ++emitted[best_idx];
        sequence.emplace_back(filtered_ids[best_idx]);
    }
    return sequence;
}

static unsigned int decode_manual_pattern_preview_token(const std::string &token, unsigned int component_a, unsigned int component_b, size_t num_physical)
{
    if (token == "1")
        return (component_a >= 1 && component_a <= num_physical) ? component_a : 0;
    if (token == "2")
        return (component_b >= 1 && component_b <= num_physical) ? component_b : 0;

    char *end = nullptr;
    errno = 0;
    unsigned long id = std::strtoul(token.c_str(), &end, 10);
    if (errno != ERANGE && *end == '\0' && id >= 1 && id <= num_physical)
        return unsigned(id);

    return 0;
}

static std::vector<unsigned int> build_grouped_manual_pattern_preview_sequence(const std::string &pattern,
                                                                               unsigned int       component_a,
                                                                               unsigned int       component_b,
                                                                               size_t             num_physical,
                                                                               size_t             wall_loops)
{
    std::vector<unsigned int> sequence;
    if (num_physical == 0)
        return sequence;

    const std::string normalized = MixedFilamentManager::normalize_manual_pattern(pattern);
    if (normalized.empty())
        return sequence;

    const std::vector<std::string> groups = split_manual_pattern_groups(normalized);
    if (groups.empty())
        return sequence;

    if (groups.size() == 1) {
        const std::vector<std::string> tokens = MixedFilamentManager::split_pattern_group_to_tokens(groups[0], num_physical);
        sequence.reserve(tokens.size());
        for (const std::string &token : tokens) {
            const unsigned int extruder_id =
                decode_manual_pattern_preview_token(token, component_a, component_b, num_physical);
            if (extruder_id != 0)
                sequence.emplace_back(extruder_id);
        }
        return sequence;
    }

    // Build per-group token vectors for indexed access
    std::vector<std::vector<std::string>> group_tokens;
    group_tokens.reserve(groups.size());
    for (const std::string &group : groups)
        group_tokens.push_back(MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical));

    constexpr size_t k_max_preview_cycle = 48;
    size_t cycle = 1;
    for (const auto &tokens : group_tokens) {
        if (tokens.empty())
            continue;
        cycle = std::lcm(cycle, tokens.size());
        if (cycle >= k_max_preview_cycle) {
            cycle = k_max_preview_cycle;
            break;
        }
    }

    const size_t preview_wall_loops = std::max<size_t>(1, wall_loops == 0 ? groups.size() : wall_loops);
    sequence.reserve(preview_wall_loops * cycle);
    for (size_t layer_idx = 0; layer_idx < cycle; ++layer_idx) {
        for (size_t wall_idx = 0; wall_idx < preview_wall_loops; ++wall_idx) {
            const auto &tokens = group_tokens[std::min(wall_idx, group_tokens.size() - 1)];
            if (tokens.empty())
                continue;
            const std::string &token = tokens[layer_idx % tokens.size()];
            const unsigned int extruder_id =
                decode_manual_pattern_preview_token(token, component_a, component_b, num_physical);
            if (extruder_id != 0)
                sequence.emplace_back(extruder_id);
        }
    }

    return sequence;
}

static std::pair<int, int> effective_pair_preview_ratios(int percent_b)
{
    const int mix_b = std::clamp(percent_b, 0, 100);
    int       ratio_a = 1;
    int       ratio_b = 0;

    if (mix_b >= 100) {
        ratio_a = 0;
        ratio_b = 1;
    } else if (mix_b > 0) {
        const int pct_b       = mix_b;
        const int pct_a       = 100 - pct_b;
        const bool b_is_major = pct_b >= pct_a;
        const int major_pct   = b_is_major ? pct_b : pct_a;
        const int minor_pct   = b_is_major ? pct_a : pct_b;
        const int major_layers =
            std::max(1, int(std::lround(double(major_pct) / double(std::max(1, minor_pct)))));
        ratio_a = b_is_major ? 1 : major_layers;
        ratio_b = b_is_major ? major_layers : 1;
    }

    if (ratio_a > 0 && ratio_b > 0) {
        const int g = std::gcd(ratio_a, ratio_b);
        if (g > 1) {
            ratio_a /= g;
            ratio_b /= g;
        }
    }

    return { std::max(0, ratio_a), std::max(0, ratio_b) };
}

static std::vector<unsigned int> build_effective_pair_preview_sequence(unsigned int component_a,
                                                                       unsigned int component_b,
                                                                       int          percent_b,
                                                                       bool         limit_cycle)
{
    std::vector<unsigned int> sequence;
    if (component_a == 0 || component_b == 0 || component_a == component_b)
        return sequence;

    auto [ratio_a, ratio_b] = effective_pair_preview_ratios(percent_b);
    constexpr int k_max_cycle = 24;
    if (limit_cycle && ratio_a > 0 && ratio_b > 0 && ratio_a + ratio_b > k_max_cycle) {
        const double scale = double(k_max_cycle) / double(ratio_a + ratio_b);
        ratio_a = std::max(1, int(std::round(double(ratio_a) * scale)));
        ratio_b = std::max(1, int(std::round(double(ratio_b) * scale)));
    }
    if (ratio_a == 0 && ratio_b == 0)
        ratio_a = 1;

    const int cycle = std::max(1, ratio_a + ratio_b);
    sequence.reserve(size_t(cycle));
    for (int pos = 0; pos < cycle; ++pos) {
        const int b_before = (pos * ratio_b) / cycle;
        const int b_after  = ((pos + 1) * ratio_b) / cycle;
        sequence.emplace_back((b_after > b_before) ? component_b : component_a);
    }
    return sequence;
}

static std::string blend_display_color_from_sequence(const std::vector<std::string> &colors,
                                                     size_t                           num_physical,
                                                     const std::vector<unsigned int> &sequence,
                                                     const std::string               &fallback)
{
    if (colors.empty() || sequence.empty() || num_physical == 0)
        return fallback;

    std::vector<size_t> counts(num_physical + 1, size_t(0));
    size_t total = 0;
    for (const unsigned int id : sequence) {
        if (id == 0 || id > num_physical)
            continue;
        ++counts[id];
        ++total;
    }
    if (total == 0)
        return fallback;

    std::vector<std::pair<std::string, int>> color_percents;
    color_percents.reserve(num_physical);
    for (size_t id = 1; id <= num_physical; ++id) {
        if (counts[id] == 0 || id > colors.size())
            continue;
        color_percents.emplace_back(colors[id - 1], int(counts[id]));
    }
    if (color_percents.empty())
        return fallback;

    if (color_percents.size() == 1)
        return color_percents.front().first;

    return MixedFilamentManager::blend_color_multi(color_percents);
}

double MixedFilamentManager::mixed_filament_reference_nozzle_mm(unsigned int               component_a,
                                                 unsigned int               component_b,
                                                 const std::vector<double> &nozzle_diameters)
{
    std::vector<double> samples;
    samples.reserve(2);

    auto append_if_valid = [&samples, &nozzle_diameters](unsigned int component_id) {
        if (component_id >= 1 && component_id <= nozzle_diameters.size())
            samples.emplace_back(std::max(0.05, nozzle_diameters[size_t(component_id - 1)]));
    };

    append_if_valid(component_a);
    append_if_valid(component_b);

    if (samples.empty())
        return 0.4;
    return std::accumulate(samples.begin(), samples.end(), 0.0) / double(samples.size());
}

bool mixed_filament_supports_bias_apparent_color(const MixedFilament               &mf,
                                                 const MixedFilamentPreviewSettings &preview_settings,
                                                 bool                                bias_mode_enabled)
{
    if (!bias_mode_enabled)
        return false;
    if (mf.distribution_mode == int(MixedFilament::SameLayerPointillisme))
        return false;
    if (!MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern).empty())
        return false;
    if (MixedFilamentManager::decode_gradient_component_ids(mf.gradient_component_ids, 0).size() >= 3)
        return false;
    return mf.component_a >= 1 && mf.component_b >= 1 && mf.component_a != mf.component_b;
}

std::pair<int, int> mixed_filament_apparent_pair_percentages(const MixedFilament               &mf,
                                                             const MixedFilamentPreviewSettings &preview_settings,
                                                             const std::vector<double>          &nozzle_diameters,
                                                             bool                                bias_mode_enabled)
{
    const int base_b = std::clamp(mf.mix_b_percent, 0, 100);
    if (!mixed_filament_supports_bias_apparent_color(mf, preview_settings, bias_mode_enabled))
        return { 100 - base_b, base_b };

    const double reference_nozzle_mm = MixedFilamentManager::mixed_filament_reference_nozzle_mm(mf.component_a, mf.component_b, nozzle_diameters);
    const int apparent_b = MixedFilamentManager::apparent_mix_b_percent(base_b,
                                                                        mf.component_a_surface_offset,
                                                                        mf.component_b_surface_offset,
                                                                        float(reference_nozzle_mm));
    return { 100 - apparent_b, apparent_b };
}

std::string compute_mixed_filament_display_color(const MixedFilament &entry, const MixedFilamentDisplayContext &context)
{
    constexpr const char *fallback = "#26A69A";
    if (context.num_physical == 0 || context.physical_colors.empty())
        return fallback;

    if (mixed_filament_supports_bias_apparent_color(entry, context.preview_settings, context.component_bias_enabled) &&
        entry.component_a >= 1 && entry.component_b >= 1 &&
        entry.component_a <= context.num_physical && entry.component_b <= context.num_physical &&
        entry.component_a <= context.physical_colors.size() && entry.component_b <= context.physical_colors.size()) {
        const auto [apparent_pct_a, apparent_pct_b] =
            mixed_filament_apparent_pair_percentages(entry, context.preview_settings, context.nozzle_diameters, context.component_bias_enabled);
        return MixedFilamentManager::blend_color(
            context.physical_colors[entry.component_a - 1],
            context.physical_colors[entry.component_b - 1],
            apparent_pct_a,
            apparent_pct_b);
    }

    const std::string normalized_pattern = MixedFilamentManager::normalize_manual_pattern(entry.manual_pattern);
    if (!normalized_pattern.empty()) {
        const std::vector<unsigned int> sequence = build_grouped_manual_pattern_preview_sequence(
            normalized_pattern, entry.component_a, entry.component_b, context.num_physical, context.preview_settings.wall_loops);
        if (!sequence.empty())
            return blend_display_color_from_sequence(context.physical_colors, context.num_physical, sequence, fallback);
    }

    if (entry.distribution_mode != int(MixedFilament::Simple)) {
        const std::vector<unsigned int> gradient_ids = MixedFilamentManager::decode_gradient_component_ids(entry.gradient_component_ids, context.num_physical);
        if (gradient_ids.size() >= 3) {
            const std::vector<int> gradient_weights =
                decode_gradient_component_weights(entry.gradient_component_weights, gradient_ids.size());
            const std::vector<unsigned int> sequence = build_weighted_gradient_sequence(
                gradient_ids, gradient_weights.empty() ? std::vector<int>(gradient_ids.size(), 1) : gradient_weights);
            if (!sequence.empty())
                return blend_display_color_from_sequence(context.physical_colors, context.num_physical, sequence, fallback);
        }
    }

    const int effective_mix_b = std::clamp(entry.mix_b_percent, 0, 100);
    const bool same_layer_mode = entry.distribution_mode == int(MixedFilament::SameLayerPointillisme);
    const std::vector<unsigned int> pair_sequence =
        build_effective_pair_preview_sequence(entry.component_a, entry.component_b, effective_mix_b, same_layer_mode);
    if (!pair_sequence.empty())
        return blend_display_color_from_sequence(context.physical_colors, context.num_physical, pair_sequence, fallback);

    if (entry.component_a == 0 || entry.component_b == 0 ||
        entry.component_a > context.num_physical || entry.component_b > context.num_physical ||
        entry.component_a > context.physical_colors.size() || entry.component_b > context.physical_colors.size()) {
        return fallback;
    }

    const int mix_b = std::clamp(entry.mix_b_percent, 0, 100);
    return MixedFilamentManager::blend_color(
        context.physical_colors[entry.component_a - 1],
        context.physical_colors[entry.component_b - 1],
        100 - mix_b,
        mix_b);
}

// ---------------------------------------------------------------------------
// MixedFilamentManager
// ---------------------------------------------------------------------------

uint64_t MixedFilamentManager::allocate_stable_id()
{
    const uint64_t stable_id = std::max<uint64_t>(1, m_next_stable_id);
    m_next_stable_id = stable_id + 1;
    return stable_id;
}

uint64_t MixedFilamentManager::normalize_stable_id(uint64_t stable_id)
{
    if (stable_id == 0)
        return allocate_stable_id();
    if (stable_id >= m_next_stable_id)
        m_next_stable_id = stable_id + 1;
    return stable_id;
}

void MixedFilamentManager::set_auto_generate_enabled(bool enabled)
{
    s_mixed_filament_auto_generate_enabled.store(enabled, std::memory_order_relaxed);
}

bool MixedFilamentManager::auto_generate_enabled()
{
    return s_mixed_filament_auto_generate_enabled.load(std::memory_order_relaxed);
}

void MixedFilamentManager::auto_generate(const std::vector<std::string> &filament_colours)
{
    // Keep a copy of the old list so we can preserve user-modified ratios and
    // enabled flags and custom rows.
    std::vector<MixedFilament> old = std::move(m_mixed);
    m_mixed.clear();

    const size_t n = filament_colours.size();

    std::vector<MixedFilament> custom_rows;
    custom_rows.reserve(old.size());
    std::unordered_map<uint64_t, const MixedFilament *> old_auto_rows;
    old_auto_rows.reserve(old.size());
    for (const MixedFilament &prev : old) {
        if (!prev.custom) {
            old_auto_rows.emplace(canonical_pair_key(prev.component_a, prev.component_b), &prev);
            continue;
        }
        if (prev.component_a == 0 || prev.component_b == 0 || prev.component_a > n || prev.component_b > n || prev.component_a == prev.component_b)
            continue;
        MixedFilament custom = prev;
        custom.stable_id = normalize_stable_id(custom.stable_id);
        custom_rows.push_back(std::move(custom));
    }

    if (n < 2 || !auto_generate_enabled()) {
        for (MixedFilament &mf : custom_rows)
            m_mixed.push_back(std::move(mf));
        refresh_display_colors(filament_colours);
        return;
    }

    // Generate all C(N,2) pairwise combinations.
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            MixedFilament mf;
            mf.component_a = static_cast<unsigned int>(i + 1); // 1-based
            mf.component_b = static_cast<unsigned int>(j + 1);
            mf.ratio_a     = 1;
            mf.ratio_b     = 1;
            mf.mix_b_percent = 50;
            mf.enabled     = true;
            mf.deleted     = false;
            mf.custom      = false;
            mf.origin_auto = true;

            const auto it_prev = old_auto_rows.find(canonical_pair_key(mf.component_a, mf.component_b));
            if (it_prev != old_auto_rows.end()) {
                const MixedFilament &prev = *it_prev->second;
                mf.enabled = prev.enabled;
                mf.deleted = prev.deleted;
                mf.stable_id = prev.stable_id;
                if (mf.deleted)
                    mf.enabled = false;
            }
            mf.stable_id = normalize_stable_id(mf.stable_id);
            m_mixed.push_back(mf);
        }
    }

    for (MixedFilament &mf : custom_rows)
        m_mixed.push_back(std::move(mf));

    refresh_display_colors(filament_colours);
}

void MixedFilamentManager::remove_physical_filament(unsigned int deleted_filament_id)
{
    if (deleted_filament_id == 0 || m_mixed.empty())
        return;

    // Check and adjust filaments following resolve() order:
    //   1. manual_pattern (cycle mode tokens)
    //   2. gradient_component_ids
    //   3. component_a / component_b (pair)

    std::vector<MixedFilament> filtered;
    filtered.reserve(m_mixed.size());
    for (MixedFilament mf : m_mixed) {

        // ---- 1. manual_pattern ----
        bool uses_deleted_in_pattern = false;
        const std::string norm = normalize_manual_pattern(mf.manual_pattern);
        if (!norm.empty()) {
            const auto groups = split_pattern_groups(norm);
            for (const std::string &group : groups) {
                const auto tokens = tokenize_pattern_group(group);
                for (const std::string &token : tokens) {
                    if (physical_filament_from_token(token, mf, kMaxPhysicalFilaments) == deleted_filament_id) {
                        uses_deleted_in_pattern = true;
                        break;
                    }
                }
                if (uses_deleted_in_pattern) break;
            }
        }
        if (uses_deleted_in_pattern)
            continue;

        // ---- 2. gradient components ----
        // Only check when there is no manual_pattern; a pattern already resolves
        // every token, so the gradient check would be a false positive at worst.
        if (norm.empty()) {
            bool uses_deleted_in_gradient = false;
            for (unsigned int comp_id : decode_gradient_component_ids(mf.gradient_component_ids, 0)) {
                if (comp_id == deleted_filament_id) {
                    uses_deleted_in_gradient = true;
                    break;
                }
            }
            if (uses_deleted_in_gradient)
                continue;
        }

        // ---- 3. pair components ----
        // Only check when there is no manual_pattern; a pattern already resolves
        // every token through physical_filament_from_token (symbolic "1"/"2" or
        // literal numeric), so the pair check would be redundant at best and a
        // false positive at worst (component_a/b may hold unrelated default values).
        if (norm.empty() && (mf.component_a == deleted_filament_id || mf.component_b == deleted_filament_id))
            continue;

        // ---- Adjust IDs for the surviving mixed filament ----

        // Adjust manual_pattern
        if (!norm.empty()) {
            const auto groups = split_pattern_groups(norm);
            std::string adjusted;
            for (size_t gi = 0; gi < groups.size(); ++gi) {
                if (gi > 0) adjusted += ',';
                const auto tokens = tokenize_pattern_group(groups[gi]);
                for (const std::string &token : tokens) {
                    // All tokens are treated as literal physical-filament IDs
                    // during adjustment. In cycle mode (component_a≡1, component_b≡2)
                    // the "1"/"2" identity mapping means decrementing them produces
                    // the correct result; for non-cycle patterns without "1"/"2",
                    // component_a/b are irrelevant (pair adjustment is guarded by
                    // norm.empty()).
                    char *end = nullptr;
                    errno = 0;
                    unsigned long id = std::strtoul(token.c_str(), &end, 10);
                    if (errno != ERANGE && *end == '\0' && id > deleted_filament_id) {
                        --id;
                        if (id >= 10) {
                            adjusted += '[';
                            adjusted += std::to_string(id);
                            adjusted += ']';
                        } else {
                            adjusted += std::to_string(id);
                        }
                    } else {
                        if (token.size() > 1) {
                            adjusted += '[';
                            adjusted += token;
                            adjusted += ']';
                        } else {
                            adjusted += token;
                        }
                    }
                }
            }
            mf.manual_pattern = adjusted;
        }

        // Adjust pair components (only when no pattern — same rationale as Step 3)
        if (norm.empty()) {
            if (mf.component_a > deleted_filament_id)
                --mf.component_a;
            if (mf.component_b > deleted_filament_id)
                --mf.component_b;
        }

        // Adjust gradient component IDs
        {
            auto decoded = decode_gradient_component_ids(mf.gradient_component_ids, 0);
            if (!norm.empty()) {
                // When manual_pattern is the active resolution source the
                // gradient deletion check was skipped — remove stale IDs
                // that reference the now-deleted physical filament.
                decoded.erase(
                    std::remove(decoded.begin(), decoded.end(), deleted_filament_id),
                    decoded.end());
            }
            for (unsigned int &id : decoded)
                if (id > deleted_filament_id)
                    --id;
            mf.gradient_component_ids = encode_gradient_component_ids(decoded);
        }

        filtered.emplace_back(std::move(mf));
    }
    m_mixed = std::move(filtered);
}

void MixedFilamentManager::add_custom_filament(unsigned int component_a,
                                               unsigned int component_b,
                                               int          mix_b_percent,
                                               const std::vector<std::string> &filament_colours)
{
    const size_t n = filament_colours.size();
    if (n < 2)
        return;
    if (total_filaments(n) >= MAXIMUM_FILAMENT_NUMBER)
        return;

    component_a = std::max<unsigned int>(1, std::min<unsigned int>(component_a, unsigned(n)));
    component_b = std::max<unsigned int>(1, std::min<unsigned int>(component_b, unsigned(n)));
    if (component_a == component_b) {
        component_b = (component_a == 1) ? 2 : 1;
    }

    MixedFilament mf;
    mf.component_a = component_a;
    mf.component_b = component_b;
    mf.stable_id = allocate_stable_id();
    mf.mix_b_percent = clamp_int(mix_b_percent, 0, 100);
    mf.ratio_a = 1;
    mf.ratio_b = 1;
    mf.manual_pattern.clear();
    mf.gradient_component_ids.clear();
    mf.gradient_component_weights.clear();
    mf.pointillism_all_filaments = false;
    mf.distribution_mode = int(MixedFilament::Simple);
    mf.component_a_surface_offset = 0.f;
    mf.component_b_surface_offset = 0.f;
    mf.enabled = true;
    mf.deleted = false;
    mf.custom = true;
    mf.origin_auto = false;
    m_mixed.push_back(std::move(mf));
    refresh_display_colors(filament_colours);
}

void MixedFilamentManager::clear_custom_entries()
{
    m_mixed.erase(std::remove_if(m_mixed.begin(), m_mixed.end(), [](const MixedFilament &mf) { return mf.custom; }), m_mixed.end());
}

void MixedFilamentManager::cleanup_deleted_entries()
{
    // Remove all deleted entries from memory
    m_mixed.erase(std::remove_if(m_mixed.begin(), m_mixed.end(), [](const MixedFilament &mf) { return mf.deleted; }), m_mixed.end());
}

std::string MixedFilamentManager::normalize_manual_pattern(const std::string &pattern)
{
    if (pattern.empty())
        return {};

    std::string normalized;
    normalized.reserve(pattern.size());
    bool group_has_content = false;

    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c >= '1' && c <= '9') {
            normalized.push_back(c);
            group_has_content = true;
        } else if (c == ',') {
            if (!group_has_content)
                return {};
            normalized.push_back(',');
            group_has_content = false;
        } else if (c == '[') {
            size_t j = i + 1;
            while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9')
                ++j;
            if (j == i + 1 || j >= pattern.size() || pattern[j] != ']')
                return {};

            std::string num_str = pattern.substr(i + 1, j - i - 1);
            if (num_str.size() > 2)
                return {};
            if (num_str.size() > 1 && num_str[0] == '0')
                return {};
            if (num_str == "0")
                return {};

            // Compressing [1]→1 and [2]→2 is safe under the cycle-mode
            // invariant (component_a≡1, component_b≡2) — the symbolic
            // tokens are identity mappings, so no information is lost.
            if (num_str.size() == 1) {
                normalized.push_back(num_str[0]);
            } else {
                normalized.push_back('[');
                normalized.append(num_str);
                normalized.push_back(']');
            }
            group_has_content = true;
            i = j;
        } else if (c == ']' || c == '0') {
            return {};
        } else {
            return {};
        }
    }

    if (!group_has_content)
        return {};

    return normalized;
}

int MixedFilamentManager::mix_percent_from_manual_pattern(const std::string &pattern)
{
    return mix_percent_from_normalized_pattern(normalize_manual_pattern(pattern));
}

void MixedFilamentManager::apply_gradient_settings(int   gradient_mode,
                                                   float lower_bound,
                                                   float upper_bound,
                                                   bool  advanced_dithering)
{
    m_gradient_mode      = (gradient_mode != 0) ? 1 : 0;
    m_height_lower_bound = std::max(0.01f, lower_bound);
    m_height_upper_bound = std::max(m_height_lower_bound, upper_bound);
    m_advanced_dithering = advanced_dithering;

    for (MixedFilament &mf : m_mixed) {
        disable_pointillism_mode(mf);
        if (!mf.custom) {
            mf.ratio_a = 1;
            mf.ratio_b = 1;
            continue;
        }
        compute_gradient_ratios(mf, m_gradient_mode, m_height_lower_bound, m_height_upper_bound);
    }
}

std::vector<int> fill_continuous_layer_range(const std::vector<int> &sorted_layers)
{
    if (sorted_layers.empty()) return {};
    const int first = sorted_layers.front();
    const int last  = sorted_layers.back();
    std::vector<int> result;
    result.reserve(last - first + 1);
    for (int layer = first; layer <= last; ++layer)
        result.push_back(layer);
    return result;
}

std::string MixedFilamentManager::serialize_custom_entries()
{
    std::ostringstream ss;
    bool first = true;
    for (MixedFilament &mf : m_mixed) {
        if (!first)
            ss << ';';
        first = false;
        disable_pointillism_mode(mf);
        mf.stable_id = normalize_stable_id(mf.stable_id);
        const std::string normalized_ids = normalize_gradient_component_ids(mf.gradient_component_ids);
        const std::string normalized_weights = normalize_gradient_component_weights(mf.gradient_component_weights, decode_gradient_component_ids(normalized_ids, 0).size());
        ss << mf.component_a << ','
           << mf.component_b << ','
           << (mf.enabled ? 1 : 0) << ','
           << (mf.custom ? 1 : 0) << ','
           << clamp_int(mf.mix_b_percent, 0, 100) << ','
           << (mf.pointillism_all_filaments ? 1 : 0) << ','
           << 'g' << normalized_ids << ','
           << 'w' << normalized_weights << ','
           << 'm' << clamp_int(mf.distribution_mode, int(MixedFilament::LayerCycle), int(MixedFilament::Simple)) << ','
           << "xa" << format_surface_offset_token(mf.component_a_surface_offset) << ','
           << "xb" << format_surface_offset_token(mf.component_b_surface_offset) << ','
           << 'd' << (mf.deleted ? 1 : 0) << ','
           << 'o' << (mf.origin_auto ? 1 : 0) << ','
           << 'u' << mf.stable_id;
        if (mf.ui_mode >= 0)
            ss << ",cm" << mf.ui_mode;
        if (mf.gradient_enabled) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.4f/%.4f",
                          double(mf.gradient_start), double(mf.gradient_end));
            ss << ",r1/" << buf;
        }
        const std::string normalized_pattern = normalize_manual_pattern(mf.manual_pattern);
        if (!normalized_pattern.empty())
            ss << ',' << normalized_pattern;
    }
    return ss.str();
}

void MixedFilamentManager::load_custom_entries(const std::string &serialized, const std::vector<std::string> &filament_colours)
{
    const size_t n = filament_colours.size();
    if (serialized.empty() || n < 2) {
        BOOST_LOG_TRIVIAL(debug) << "MixedFilamentManager::load_custom_entries skipped"
                                 << ", serialized_empty=" << (serialized.empty() ? 1 : 0)
                                 << ", physical_count=" << n;
        return;
    }

    size_t parsed_rows   = 0;
    size_t loaded_rows   = 0;
    size_t updated_auto  = 0;
    size_t appended_auto = 0;
    size_t skipped_rows  = 0;

    std::vector<const MixedFilament *> auto_rows_in_order;
    auto_rows_in_order.reserve(m_mixed.size());
    std::unordered_map<uint64_t, const MixedFilament *> auto_rows_by_pair;
    auto_rows_by_pair.reserve(m_mixed.size());
    for (const MixedFilament &mf : m_mixed) {
        if (!mf.custom) {
            auto_rows_in_order.push_back(&mf);
            auto_rows_by_pair.emplace(canonical_pair_key(mf.component_a, mf.component_b), &mf);
        }
    }

    std::vector<MixedFilament> rebuilt;
    rebuilt.reserve(m_mixed.size() + 8);
    std::unordered_set<uint64_t> consumed_auto_pairs;
    consumed_auto_pairs.reserve(auto_rows_by_pair.size());
    std::unordered_set<uint64_t> used_stable_ids;
    used_stable_ids.reserve(m_mixed.size() + 8);
    auto dedupe_stable_id = [this, &used_stable_ids](uint64_t stable_id) {
        stable_id = normalize_stable_id(stable_id);
        if (used_stable_ids.insert(stable_id).second)
            return stable_id;
        uint64_t replacement = allocate_stable_id();
        used_stable_ids.insert(replacement);
        return replacement;
    };

    std::stringstream all(serialized);
    std::string row;
    while (std::getline(all, row, ';')) {
        if (row.empty())
            continue;
        ++parsed_rows;
        unsigned int a = 0;
        unsigned int b = 0;
        uint64_t stable_id = 0;
        bool enabled = true;
        bool custom = true;
        bool origin_auto = false;
        int mix = 50;
        bool pointillism_all_filaments = false;
        std::string gradient_component_ids;
        std::string gradient_component_weights;
        std::string manual_pattern;
        int distribution_mode = int(MixedFilament::Simple);
        float component_a_surface_offset = 0.f;
        float component_b_surface_offset = 0.f;
        bool deleted = false;
        bool gradient_enabled = false;
        float gradient_start = 0.8f;
        float gradient_end   = 0.2f;
        int   cm_mode = -1;
        if (!parse_row_definition(row, a, b, stable_id, enabled, custom, origin_auto, mix, pointillism_all_filaments,
                                  gradient_component_ids, gradient_component_weights, manual_pattern, distribution_mode,
                                  component_a_surface_offset, component_b_surface_offset, deleted,
                                  gradient_enabled, gradient_start, gradient_end, cm_mode)) {
            ++skipped_rows;
            BOOST_LOG_TRIVIAL(warning) << "MixedFilamentManager::load_custom_entries invalid row format: " << row;
            continue;
        }
        if (a == 0 || b == 0 || a > n || b > n || a == b) {
            ++skipped_rows;
            BOOST_LOG_TRIVIAL(warning) << "MixedFilamentManager::load_custom_entries row rejected"
                                       << ", row=" << row
                                       << ", a=" << a
                                       << ", b=" << b
                                       << ", physical_count=" << n;
            continue;
        }

        if (mixed_filament_references_exceed_physical(gradient_component_ids, manual_pattern, n)) {
            ++skipped_rows;
            BOOST_LOG_TRIVIAL(warning) << "MixedFilamentManager::load_custom_entries row rejected: "
                                          "gradient/pattern references filament exceeding physical count"
                                       << ", row=" << row
                                       << ", gradient_ids=" << gradient_component_ids
                                       << ", manual_pattern=" << manual_pattern
                                       << ", physical_count=" << n;
            continue;
        }

        if (!custom) {
            const uint64_t key = canonical_pair_key(a, b);
            if (consumed_auto_pairs.count(key) != 0) {
                ++skipped_rows;
                BOOST_LOG_TRIVIAL(warning) << "MixedFilamentManager::load_custom_entries duplicate auto row"
                                           << ", row=" << row
                                           << ", a=" << std::min(a, b)
                                           << ", b=" << std::max(a, b);
                continue;
            }

            auto it_auto = auto_rows_by_pair.find(key);
            if (it_auto == auto_rows_by_pair.end()) {
                ++skipped_rows;
                BOOST_LOG_TRIVIAL(warning) << "MixedFilamentManager::load_custom_entries auto row missing after regenerate"
                                           << ", row=" << row
                                           << ", a=" << std::min(a, b)
                                           << ", b=" << std::max(a, b);
                continue;
            }

            MixedFilament mf = *it_auto->second;
            mf.component_a = std::min(a, b);
            mf.component_b = std::max(a, b);
            mf.stable_id = dedupe_stable_id(stable_id != 0 ? stable_id : mf.stable_id);
            mf.ui_mode   = cm_mode;
            mf.enabled = enabled;
            mf.pointillism_all_filaments = pointillism_all_filaments;
            mf.gradient_component_ids = normalize_gradient_component_ids(gradient_component_ids);
            mf.gradient_component_weights =
                normalize_gradient_component_weights(gradient_component_weights, decode_gradient_component_ids(mf.gradient_component_ids, 0).size());
            mf.manual_pattern = normalize_manual_pattern(manual_pattern);
            mf.distribution_mode = clamp_int(distribution_mode, int(MixedFilament::LayerCycle), int(MixedFilament::Simple));
            mf.component_a_surface_offset = clamp_surface_offset(component_a_surface_offset);
            mf.component_b_surface_offset = clamp_surface_offset(component_b_surface_offset);
            mf.mix_b_percent = mf.manual_pattern.empty() ? mix : mix_percent_from_normalized_pattern(mf.manual_pattern);
            mf.deleted = deleted;
            if (mf.deleted)
                mf.enabled = false;
            mf.custom = false;
            mf.origin_auto = true;
            mf.gradient_enabled = gradient_enabled;
            mf.gradient_start   = gradient_start;
            mf.gradient_end     = gradient_end;
            disable_pointillism_mode(mf);

            rebuilt.push_back(std::move(mf));
            consumed_auto_pairs.insert(key);
            ++updated_auto;
            continue;
        }

        MixedFilament mf;
        mf.component_a = a;
        mf.component_b = b;
        mf.stable_id = dedupe_stable_id(stable_id);
        mf.ui_mode   = cm_mode;
        mf.mix_b_percent = mix;
        mf.ratio_a = 1;
        mf.ratio_b = 1;
        mf.pointillism_all_filaments = pointillism_all_filaments;
        mf.gradient_component_ids = normalize_gradient_component_ids(gradient_component_ids);
        mf.gradient_component_weights =
            normalize_gradient_component_weights(gradient_component_weights, decode_gradient_component_ids(mf.gradient_component_ids, 0).size());
        mf.manual_pattern = normalize_manual_pattern(manual_pattern);
        mf.distribution_mode = clamp_int(distribution_mode, int(MixedFilament::LayerCycle), int(MixedFilament::Simple));
        mf.component_a_surface_offset = clamp_surface_offset(component_a_surface_offset);
        mf.component_b_surface_offset = clamp_surface_offset(component_b_surface_offset);
        if (!mf.manual_pattern.empty())
            mf.mix_b_percent = mix_percent_from_normalized_pattern(mf.manual_pattern);
        mf.enabled = enabled;
        mf.deleted = deleted;
        if (mf.deleted)
            mf.enabled = false;
        mf.custom = custom;
        mf.origin_auto = origin_auto;
        mf.gradient_enabled = gradient_enabled;
        mf.gradient_start   = gradient_start;
        mf.gradient_end     = gradient_end;
        disable_pointillism_mode(mf);
        rebuilt.push_back(std::move(mf));
        ++loaded_rows;
    }

    // Keep any newly generated auto rows that were not present in serialized
    // definitions and append them at the end to preserve existing virtual IDs.
    for (const MixedFilament *auto_mf_ptr : auto_rows_in_order) {
        if (auto_mf_ptr == nullptr)
            continue;
        const uint64_t key = canonical_pair_key(auto_mf_ptr->component_a, auto_mf_ptr->component_b);
        if (consumed_auto_pairs.count(key) != 0)
            continue;
        MixedFilament mf = *auto_mf_ptr;
        const unsigned int lo = std::min(mf.component_a, mf.component_b);
        const unsigned int hi = std::max(mf.component_a, mf.component_b);
        mf.component_a = lo;
        mf.component_b = hi;
        mf.stable_id = dedupe_stable_id(mf.stable_id);
        mf.custom = false;
        mf.origin_auto = true;
        rebuilt.push_back(std::move(mf));
        ++appended_auto;
    }

    m_mixed = std::move(rebuilt);
    refresh_display_colors(filament_colours);
    BOOST_LOG_TRIVIAL(info) << "MixedFilamentManager::load_custom_entries"
                            << ", physical_count=" << n
                            << ", parsed_rows=" << parsed_rows
                            << ", loaded_rows=" << loaded_rows
                            << ", updated_auto_rows=" << updated_auto
                            << ", appended_auto_rows=" << appended_auto
                            << ", skipped_rows=" << skipped_rows
                            << ", mixed_total=" << m_mixed.size();
}

unsigned int MixedFilamentManager::resolve(unsigned int filament_id,
                                           size_t       num_physical,
                                           int          layer_index,
                                           float        layer_print_z,
                                           float        layer_height,
                                           bool         force_height_weighted,
                                           const PrintObject* current_object) const
{
    const int mixed_idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (mixed_idx < 0)
        return filament_id;

    const MixedFilament &mf = m_mixed[size_t(mixed_idx)];

    // Manual pattern takes precedence when provided.
    // Tokens: '1'=>component_a, '2'=>component_b, '3'..'9'=>direct IDs,
    // with '/' delimited multi-digit tokens (e.g. /12/).
    if (!mf.manual_pattern.empty()) {
        const std::string flattened_pattern = flatten_manual_pattern_groups(mf.manual_pattern);
        if (!flattened_pattern.empty()) {
            const std::vector<std::string> tokens = split_pattern_group_to_tokens(flattened_pattern, num_physical);
            if (!tokens.empty()) {
                const int pos = MixedFilamentManager::safe_mod(layer_index, int(tokens.size()));
                const unsigned int resolved = physical_filament_from_token(tokens[size_t(pos)], mf, num_physical);
                if (resolved >= 1 && resolved <= num_physical)
                    return resolved;
            }
        }
        return mf.component_a;
    }

    const bool use_simple_mode = mf.distribution_mode == int(MixedFilament::Simple);
    const std::vector<unsigned int> gradient_ids = decode_gradient_component_ids(mf.gradient_component_ids, num_physical);
    if (!use_simple_mode && gradient_ids.size() >= 3) {
        const std::vector<int> gradient_weights =
            decode_gradient_component_weights(mf.gradient_component_weights, gradient_ids.size());
        const std::vector<unsigned int> gradient_sequence = build_weighted_gradient_sequence(
            gradient_ids, gradient_weights.empty() ? std::vector<int>(gradient_ids.size(), 1) : gradient_weights);
        if (!gradient_sequence.empty()) {
            const size_t pos = size_t(MixedFilamentManager::safe_mod(layer_index, int(gradient_sequence.size())));
            return gradient_sequence[pos];
        }
    }

    // Height-weighted cadence can be forced by the local-Z planner. The
    // regular gradient height mode keeps historical behavior (custom rows).
    const bool use_height_weighted = force_height_weighted || (m_gradient_mode == 1 && mf.custom);
    if (use_height_weighted) {
        float h_a = 0.f;
        float h_b = 0.f;
        compute_gradient_heights(mf, m_height_lower_bound, m_height_upper_bound, h_a, h_b);
        const float cycle_h = std::max(0.01f, h_a + h_b);
        const float z_anchor = (layer_height > 1e-6f)
            ? std::max(0.f, layer_print_z - 0.5f * layer_height)
            : std::max(0.f, layer_print_z);
        float phase = std::fmod(z_anchor, cycle_h);
        if (phase < 0.f)
            phase += cycle_h;
        return (phase < h_a) ? mf.component_a : mf.component_b;
    }

    const int cycle = mf.ratio_a + mf.ratio_b;
    if (cycle <= 0)
        return mf.component_a;

    if (m_gradient_mode == 0 && m_advanced_dithering && mf.custom)
        return use_component_b_advanced_dither(layer_index, mf.ratio_a, mf.ratio_b) ? mf.component_b : mf.component_a;

    const int pos = ((layer_index % cycle) + cycle) % cycle; // safe modulo for negatives
    return (pos < mf.ratio_a) ? mf.component_a : mf.component_b;
}

unsigned int MixedFilamentManager::resolve_perimeter(unsigned int filament_id,
                                                     size_t       num_physical,
                                                     int          layer_index,
                                                     int          perimeter_index,
                                                     float        layer_print_z,
                                                     float        layer_height,
                                                     bool         force_height_weighted,
                                                     const PrintObject* current_object) const
{
    const int mixed_idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (mixed_idx < 0)
        return filament_id;

    const MixedFilament &mf = m_mixed[size_t(mixed_idx)];
    if (!mf.manual_pattern.empty()) {
        const std::vector<std::string> pattern_groups = split_manual_pattern_groups(mf.manual_pattern);
        if (!pattern_groups.empty()) {
            const size_t group_idx = size_t(std::max(0, perimeter_index));
            const std::string &group = pattern_groups[std::min(group_idx, pattern_groups.size() - 1)];
            if (!group.empty()) {
                const std::vector<std::string> tokens = split_pattern_group_to_tokens(group, num_physical);
                if (!tokens.empty()) {
                    const int pos = MixedFilamentManager::safe_mod(layer_index, int(tokens.size()));
                    const unsigned int resolved = physical_filament_from_token(tokens[size_t(pos)], mf, num_physical);
                    if (resolved >= 1 && resolved <= num_physical)
                        return resolved;
                }
            }
        }
    }

    return resolve(filament_id, num_physical, layer_index, layer_print_z, layer_height, force_height_weighted, current_object);
}

unsigned int MixedFilamentManager::effective_painted_region_filament_id(unsigned int filament_id,
                                                                        size_t       num_physical,
                                                                        int          layer_index,
                                                                        float        layer_print_z,
                                                                        float        layer_height,
                                                                        float        layer_height_a,
                                                                        float        layer_height_b,
                                                                        float        base_layer_height) const
{
    const int mixed_idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (mixed_idx < 0)
        return filament_id;

    const MixedFilament &mf = m_mixed[size_t(mixed_idx)];
    if (mf.distribution_mode == int(MixedFilament::SameLayerPointillisme))
        return filament_id;

    const std::string normalized_pattern = normalize_manual_pattern(mf.manual_pattern);
    if (normalized_pattern.find(',') != std::string::npos)
        return filament_id;

    const bool is_custom_mixed = mf.custom;
    if (!is_custom_mixed && (layer_height_a > 0.f || layer_height_b > 0.f)) {
        const float safe_base = std::max<float>(0.01f, base_layer_height);
        const int ratio_a = std::max(1, int(std::lround((layer_height_a > 0.f ? layer_height_a : safe_base) / safe_base)));
        const int ratio_b = std::max(1, int(std::lround((layer_height_b > 0.f ? layer_height_b : safe_base) / safe_base)));
        const int cycle   = ratio_a + ratio_b;

        if (cycle > 0) {
            const int pos = ((layer_index % cycle) + cycle) % cycle;
            return pos < ratio_a ? mf.component_a : mf.component_b;
        }
    }

    return resolve(filament_id, num_physical, layer_index, layer_print_z, layer_height);
}

float MixedFilamentManager::component_surface_offset(unsigned int filament_id,
                                                     size_t       num_physical,
                                                     int          layer_index,
                                                     float        layer_print_z,
                                                     float        layer_height,
                                                     bool         force_height_weighted) const
{
    const MixedFilament *mixed_row = mixed_filament_from_id(filament_id, num_physical);
    if (mixed_row == nullptr)
        return 0.f;

    if (mixed_row->distribution_mode == int(MixedFilament::SameLayerPointillisme))
        return 0.f;

    const std::string normalized_pattern = normalize_manual_pattern(mixed_row->manual_pattern);
    if (normalized_pattern.find(',') != std::string::npos)
        return 0.f;

    const unsigned int resolved = resolve(filament_id,
                                          num_physical,
                                          layer_index,
                                          layer_print_z,
                                          layer_height,
                                          force_height_weighted);
    const float signed_bias = canonical_signed_bias_value(mixed_row->component_a_surface_offset, mixed_row->component_b_surface_offset);
    if (signed_bias > EPSILON && resolved == mixed_row->component_b)
        return signed_bias;
    if (signed_bias < -EPSILON && resolved == mixed_row->component_a)
        return -signed_bias;
    return 0.f;
}

std::vector<unsigned int> MixedFilamentManager::ordered_perimeter_extruders(unsigned int filament_id,
                                                                            size_t       num_physical,
                                                                            int          layer_index,
                                                                            float        layer_print_z,
                                                                            float        layer_height,
                                                                            bool         force_height_weighted) const
{
    std::vector<unsigned int> ordered;

    const int mixed_idx = mixed_index_from_filament_id(filament_id, num_physical);
    if (mixed_idx < 0) {
        ordered.emplace_back(filament_id);
        return ordered;
    }

    const MixedFilament &mf = m_mixed[size_t(mixed_idx)];
    if (!mf.manual_pattern.empty()) {
        const std::vector<std::string> pattern_groups = split_manual_pattern_groups(mf.manual_pattern);
        if (!pattern_groups.empty()) {
            ordered.reserve(pattern_groups.size());
            for (size_t group_idx = 0; group_idx < pattern_groups.size(); ++group_idx) {
                const unsigned int resolved = resolve_perimeter(filament_id,
                                                                num_physical,
                                                                layer_index,
                                                                int(group_idx),
                                                                layer_print_z,
                                                                layer_height,
                                                                force_height_weighted);
                if (resolved < 1 || resolved > num_physical)
                    continue;
                if (std::find(ordered.begin(), ordered.end(), resolved) == ordered.end())
                    ordered.emplace_back(resolved);
            }
            if (!ordered.empty())
                return ordered;
        }
    }

    ordered.emplace_back(resolve(filament_id, num_physical, layer_index, layer_print_z, layer_height, force_height_weighted));
    return ordered;
}

int MixedFilamentManager::mixed_index_from_filament_id(unsigned int filament_id, size_t num_physical) const
{
    if (filament_id <= num_physical)
        return -1;

    const size_t enabled_virtual_idx = size_t(filament_id - num_physical - 1);
    size_t enabled_seen = 0;
    for (size_t i = 0; i < m_mixed.size(); ++i) {
        if (!m_mixed[i].enabled || m_mixed[i].deleted)
            continue;
        if (enabled_seen == enabled_virtual_idx)
            return int(i);
        ++enabled_seen;
    }
    return -1;
}

const MixedFilament *MixedFilamentManager::mixed_filament_from_id(unsigned int filament_id, size_t num_physical) const
{
    const int idx = mixed_index_from_filament_id(filament_id, num_physical);
    return idx >= 0 ? &m_mixed[size_t(idx)] : nullptr;
}

// Get all mixed filament indices that depend on a specific physical filament
std::vector<size_t> MixedFilamentManager::mixed_filaments_using_physical(unsigned int physical_filament_1based) const
{
    std::vector<size_t> result;
    
    for (size_t j = 0; j < m_mixed.size(); ++j) {
        const MixedFilament& mf = m_mixed[j];
        if (mf.deleted || !mf.enabled) continue;
        
        bool depends_on_physical = false;

        // Check manual_pattern (cycle mode tokens — resolve order #1)
        const std::string norm = normalize_manual_pattern(mf.manual_pattern);
        if (!norm.empty()) {
            const auto groups = split_pattern_groups(norm);
            for (const std::string &group : groups) {
                const auto tokens = tokenize_pattern_group(group);
                for (const std::string &token : tokens) {
                    if (physical_filament_from_token(token, mf, kMaxPhysicalFilaments) == physical_filament_1based) {
                        depends_on_physical = true;
                        break;
                    }
                }
                if (depends_on_physical) break;
            }
        }

        // Check gradient components (resolve order #2)
        if (!depends_on_physical && norm.empty()) {
            for (unsigned int comp_id : decode_gradient_component_ids(mf.gradient_component_ids, 0)) {
                if (comp_id == physical_filament_1based) {
                    depends_on_physical = true;
                    break;
                }
            }
        }

        // Check pair components (resolve order #3)
        if (!depends_on_physical && norm.empty()) {
            if (mf.component_a == physical_filament_1based || mf.component_b == physical_filament_1based) {
                depends_on_physical = true;
            }
        }
        
        if (depends_on_physical) {
            result.push_back(j);
        }
    }
    
    return result;
}

// Blend N colours using weighted pairwise FilamentMixer blending.
std::string MixedFilamentManager::blend_color_multi(
    const std::vector<std::pair<std::string, int>> &color_percents)
{
    if (color_percents.empty())
        return "#000000";
    if (color_percents.size() == 1)
        return color_percents.front().first;

    struct WeightedColor {
        RGB color;
        int pct;
    };
    std::vector<WeightedColor> colors;
    colors.reserve(color_percents.size());

    int total_pct = 0;
    for (const auto &[hex, pct] : color_percents) {
        if (pct <= 0)
            continue;
        colors.push_back({parse_hex_color(hex), pct});
        total_pct += pct;
    }
    if (colors.empty() || total_pct <= 0)
        return "#000000";

    unsigned char r = static_cast<unsigned char>(colors.front().color.r);
    unsigned char g = static_cast<unsigned char>(colors.front().color.g);
    unsigned char b = static_cast<unsigned char>(colors.front().color.b);
    int accumulated_pct = colors.front().pct;

    for (size_t i = 1; i < colors.size(); ++i) {
        const auto &next = colors[i];
        const int new_total = accumulated_pct + next.pct;
        if (new_total <= 0)
            continue;
        const float t = static_cast<float>(next.pct) / static_cast<float>(new_total);
        filament_mixer_lerp(
            r, g, b,
            static_cast<unsigned char>(next.color.r),
            static_cast<unsigned char>(next.color.g),
            static_cast<unsigned char>(next.color.b),
            t, &r, &g, &b);
        accumulated_pct = new_total;
    }

    return rgb_to_hex({int(r), int(g), int(b)});
}

std::string MixedFilamentManager::blend_color(const std::string &color_a,
                                              const std::string &color_b,
                                              int ratio_a, int ratio_b)
{
    const int safe_a = std::max(0, ratio_a);
    const int safe_b = std::max(0, ratio_b);
    const int total  = safe_a + safe_b;
    const float t    = (total > 0) ? (static_cast<float>(safe_b) / static_cast<float>(total)) : 0.5f;

    const RGB rgb_a = parse_hex_color(color_a);
    const RGB rgb_b = parse_hex_color(color_b);

    unsigned char out_r = static_cast<unsigned char>(rgb_a.r);
    unsigned char out_g = static_cast<unsigned char>(rgb_a.g);
    unsigned char out_b = static_cast<unsigned char>(rgb_a.b);
    filament_mixer_lerp(static_cast<unsigned char>(rgb_a.r),
                        static_cast<unsigned char>(rgb_a.g),
                        static_cast<unsigned char>(rgb_a.b),
                        static_cast<unsigned char>(rgb_b.r),
                        static_cast<unsigned char>(rgb_b.g),
                        static_cast<unsigned char>(rgb_b.b),
                        t, &out_r, &out_g, &out_b);

    return rgb_to_hex({int(out_r), int(out_g), int(out_b)});
}

float MixedFilamentManager::max_component_surface_offset_mm(float reference_width_mm)
{
    const float safe_reference = std::max(0.05f, std::abs(reference_width_mm));
    return std::clamp(safe_reference, 0.01f, 0.35f);
}

float MixedFilamentManager::max_pair_bias_mm(float reference_width_mm)
{
    return max_component_surface_offset_mm(reference_width_mm);
}

std::pair<float, float> MixedFilamentManager::surface_offset_pair_from_signed_bias(float bias_mm,
                                                                                    float reference_width_mm)
{
    const float clamped_bias = std::clamp(bias_mm,
                                          -max_pair_bias_mm(reference_width_mm),
                                          max_pair_bias_mm(reference_width_mm));
    if (clamped_bias > EPSILON)
        return std::make_pair(0.f, clamped_bias);
    if (clamped_bias < -EPSILON)
        return std::make_pair(-clamped_bias, 0.f);
    return std::make_pair(0.f, 0.f);
}

float MixedFilamentManager::bias_ui_value_from_surface_offsets(float component_a_surface_offset,
                                                               float component_b_surface_offset,
                                                               float reference_width_mm)
{
    return std::clamp(canonical_signed_bias_value(component_a_surface_offset, component_b_surface_offset),
                      -max_pair_bias_mm(reference_width_mm),
                      max_pair_bias_mm(reference_width_mm));
}

int MixedFilamentManager::apparent_mix_b_percent(int   mix_b_percent,
                                                 float component_a_surface_offset,
                                                 float component_b_surface_offset,
                                                 float reference_width_mm)
{
    const float safe_reference = std::max(0.05f, std::abs(reference_width_mm));
    const float shift_pct = -100.f * std::clamp(canonical_signed_bias_value(component_a_surface_offset, component_b_surface_offset),
                                                -max_pair_bias_mm(reference_width_mm),
                                                max_pair_bias_mm(reference_width_mm)) / safe_reference;
    return clamp_int(int(std::lround(float(clamp_int(mix_b_percent, 0, 100)) + shift_pct)), 0, 100);
}

void MixedFilamentManager::refresh_display_colors(const std::vector<std::string> &filament_colours)
{
    MixedFilamentDisplayContext context = m_display_context;
    context.num_physical = filament_colours.size();
    context.physical_colors = filament_colours;
    if (context.preview_settings.wall_loops == 0)
        context.preview_settings.wall_loops = 1;
    if (context.nozzle_diameters.size() < context.num_physical)
        context.nozzle_diameters.resize(context.num_physical, 0.4);

    for (MixedFilament &mf : m_mixed)
        mf.display_color = compute_mixed_filament_display_color(mf, context);
}

size_t MixedFilamentManager::enabled_count() const
{
    size_t count = 0;
    for (const auto &mf : m_mixed)
        if (mf.enabled && !mf.deleted)
            ++count;
    return count;
}

std::vector<std::string> MixedFilamentManager::display_colors() const
{
    std::vector<std::string> colors;
    for (const auto &mf : m_mixed)
        if (mf.enabled && !mf.deleted)
            colors.push_back(mf.display_color);
    return colors;
}

void MixedFilamentManager::set_display_context(const MixedFilamentDisplayContext &context)
{
    m_display_context = context;
    if (m_display_context.num_physical == 0 || m_display_context.num_physical < m_display_context.physical_colors.size())
        m_display_context.num_physical = m_display_context.physical_colors.size();
    if (m_display_context.preview_settings.wall_loops == 0)
        m_display_context.preview_settings.wall_loops = 1;
    if (m_display_context.nozzle_diameters.size() < m_display_context.num_physical)
        m_display_context.nozzle_diameters.resize(m_display_context.num_physical, 0.4);
    if (!m_display_context.physical_colors.empty())
        refresh_display_colors(m_display_context.physical_colors);
}

} // namespace Slic3r
