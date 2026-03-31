#include "filament_mixer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <numeric>

#include "filament_mixer_model.h"

namespace Slic3r {
namespace {

inline float clamp01(float x)
{
    return std::max(0.0f, std::min(1.0f, x));
}

inline float srgb_to_linear(float x)
{
    return (x >= 0.04045f) ? std::pow((x + 0.055f) / 1.055f, 2.4f) : x / 12.92f;
}

inline float linear_to_srgb(float x)
{
    return (x >= 0.0031308f) ? (1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f) : (12.92f * x);
}

inline unsigned char to_u8(float x)
{
    const float clamped = clamp01(x);
    return static_cast<unsigned char>(clamped * 255.0f + 0.5f);
}

inline float to_f01(unsigned char x)
{
    return static_cast<float>(x) / 255.0f;
}

} // namespace

void filament_mixer_lerp(unsigned char r1, unsigned char g1, unsigned char b1,
                         unsigned char r2, unsigned char g2, unsigned char b2,
                         float t,
                         unsigned char* out_r, unsigned char* out_g, unsigned char* out_b)
{
    ::filament_mixer::lerp(r1, g1, b1, r2, g2, b2, t, out_r, out_g, out_b);
}

void filament_mixer_lerp_float(float r1, float g1, float b1,
                               float r2, float g2, float b2,
                               float t,
                               float* out_r, float* out_g, float* out_b)
{
    unsigned char ur = 0, ug = 0, ub = 0;
    filament_mixer_lerp(to_u8(r1), to_u8(g1), to_u8(b1),
                        to_u8(r2), to_u8(g2), to_u8(b2),
                        t, &ur, &ug, &ub);
    *out_r = to_f01(ur);
    *out_g = to_f01(ug);
    *out_b = to_f01(ub);
}

void filament_mixer_lerp_linear_float(float r1, float g1, float b1,
                                      float r2, float g2, float b2,
                                      float t,
                                      float* out_r, float* out_g, float* out_b)
{
    const float sr1 = linear_to_srgb(clamp01(r1));
    const float sg1 = linear_to_srgb(clamp01(g1));
    const float sb1 = linear_to_srgb(clamp01(b1));
    const float sr2 = linear_to_srgb(clamp01(r2));
    const float sg2 = linear_to_srgb(clamp01(g2));
    const float sb2 = linear_to_srgb(clamp01(b2));

    float out_sr = 0.0f, out_sg = 0.0f, out_sb = 0.0f;
    filament_mixer_lerp_float(sr1, sg1, sb1, sr2, sg2, sb2, t, &out_sr, &out_sg, &out_sb);

    *out_r = srgb_to_linear(clamp01(out_sr));
    *out_g = srgb_to_linear(clamp01(out_sg));
    *out_b = srgb_to_linear(clamp01(out_sb));
}

static bool parse_hex(const std::string &hex, unsigned char &r, unsigned char &g, unsigned char &b)
{
    if (hex.size() < 7 || hex[0] != '#') return false;
    unsigned rv = 0, gv = 0, bv = 0;
    if (std::sscanf(hex.c_str(), "#%02x%02x%02x", &rv, &gv, &bv) != 3) return false;
    r = (unsigned char)rv; g = (unsigned char)gv; b = (unsigned char)bv;
    return true;
}

std::string blend_color(const std::string& hex_a, const std::string& hex_b, float ratio_b)
{
    unsigned char r1 = 128, g1 = 128, b1 = 128;
    unsigned char r2 = 128, g2 = 128, b2 = 128;
    parse_hex(hex_a, r1, g1, b1);
    parse_hex(hex_b, r2, g2, b2);

    unsigned char mr = 0, mg = 0, mb = 0;
    filament_mixer_lerp(r1, g1, b1, r2, g2, b2, ratio_b, &mr, &mg, &mb);

    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", mr, mg, mb);
    return std::string(buf);
}

std::vector<unsigned int> parse_mixed_components(const std::string &str)
{
    std::vector<unsigned int> components;
    if (str.empty())
        return components;
    std::istringstream ss(str);
    std::string token;
    while (std::getline(ss, token, ',')) {
        try {
            int val = std::stoi(token);
            if (val >= 1)
                components.push_back(static_cast<unsigned int>(val));
        } catch (...) {}
    }
    return components;
}

std::vector<double> parse_mixed_ratios(const std::string &str, size_t n_components)
{
    std::vector<double> ratios;
    if (!str.empty()) {
        std::istringstream ss(str);
        std::string token;
        while (std::getline(ss, token, ',')) {
            try {
                double val = std::stod(token);
                if (val > 0.0)
                    ratios.push_back(val);
            } catch (...) {}
        }
    }

    if (ratios.size() != n_components || n_components == 0) {
        ratios.assign(n_components, n_components > 0 ? 1.0 / n_components : 0.0);
        return ratios;
    }

    double sum = std::accumulate(ratios.begin(), ratios.end(), 0.0);
    if (sum > 0.0 && std::abs(sum - 1.0) > 1e-6) {
        for (double &r : ratios)
            r /= sum;
    }
    return ratios;
}

bool has_any_mixed_filament(const std::vector<unsigned char> &is_mixed)
{
    for (unsigned char v : is_mixed)
        if (v) return true;
    return false;
}

std::vector<size_t> check_mixed_filament_integrity(
    const std::vector<unsigned char> &is_mixed,
    const std::vector<std::string>   &comp_strs,
    size_t num_physical)
{
    std::vector<size_t> broken;
    for (size_t i = 0; i < is_mixed.size(); ++i) {
        if (!is_mixed[i]) continue;
        if (i >= comp_strs.size() || comp_strs[i].empty()) {
            broken.push_back(i);
            continue;
        }
        auto comps = parse_mixed_components(comp_strs[i]);
        if (comps.size() < 2) {
            broken.push_back(i);
            continue;
        }
        for (unsigned int c : comps) {
            if (c < 1 || c > num_physical) {
                broken.push_back(i);
                break;
            }
        }
    }
    return broken;
}

std::vector<unsigned int> expand_mixed_filaments(
    const std::vector<unsigned int> &extruders_0based,
    const std::vector<unsigned char> &is_mixed,
    const std::vector<std::string>  &comp_strs)
{
    std::vector<unsigned int> result;
    for (unsigned int ext : extruders_0based) {
        if (ext < is_mixed.size() && is_mixed[ext] && ext < comp_strs.size()) {
            auto comps = parse_mixed_components(comp_strs[ext]);
            for (unsigned int c : comps)
                if (c >= 1) result.push_back(c - 1);
        } else {
            result.push_back(ext);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace Slic3r
