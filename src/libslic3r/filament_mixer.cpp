#include "filament_mixer.h"

#include <algorithm>
#include <cmath>

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

} // namespace Slic3r
