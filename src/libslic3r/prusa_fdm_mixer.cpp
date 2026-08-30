/*
 * prusa_fdm_mixer.cpp — Implementation of the prusa-fdm-mixer color mixing model.
 *
 * Copyright (c) Prusa Research s.r.o.
 * MIT License — see LICENSE.
 */

#include "prusa_fdm_mixer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace prusa_fdm_mixer {

namespace {

constexpr double PI = 3.14159265358979323846;

/* ----------------------------------------------------------------------------
 * sRGB <-> linear RGB
 * -------------------------------------------------------------------------- */

inline double srgb_to_linear(double c) {
    c = c / 255.0;
    if (c <= 0.04045) return c / 12.92;
    return std::pow((c + 0.055) / 1.055, 2.4);
}

inline double linear_to_srgb(double c) {
    c = std::clamp(c, 0.0, 1.0);
    double v;
    if (c <= 0.0031308) v = 12.92 * c;
    else v = 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return v * 255.0;
}

/* ----------------------------------------------------------------------------
 * sRGB <-> XYZ (D65)
 * -------------------------------------------------------------------------- */

struct XYZ { double x, y, z; };

inline XYZ rgb_to_xyz(const RGB& rgb) {
    const double r = srgb_to_linear(rgb.r);
    const double g = srgb_to_linear(rgb.g);
    const double b = srgb_to_linear(rgb.b);
    return XYZ{
        (r * 0.4124564 + g * 0.3575761 + b * 0.1804375) * 100.0,
        (r * 0.2126729 + g * 0.7151522 + b * 0.0721750) * 100.0,
        (r * 0.0193339 + g * 0.1191920 + b * 0.9503041) * 100.0,
    };
}

inline RGB xyz_to_rgb(const XYZ& xyz) {
    const double x = xyz.x / 100.0;
    const double y = xyz.y / 100.0;
    const double z = xyz.z / 100.0;
    const double r = linear_to_srgb(x *  3.2404542 + y * -1.5371385 + z * -0.4985314);
    const double g = linear_to_srgb(x * -0.9692660 + y *  1.8760108 + z *  0.0415560);
    const double b = linear_to_srgb(x *  0.0556434 + y * -0.2040259 + z *  1.0572252);
    auto clamp_round = [](double v) -> std::uint8_t {
        const long iv = std::lround(v);
        return static_cast<std::uint8_t>(std::clamp<long>(iv, 0, 255));
    };
    return RGB{ clamp_round(r), clamp_round(g), clamp_round(b) };
}

/* ----------------------------------------------------------------------------
 * XYZ <-> CIELAB (D65)
 * -------------------------------------------------------------------------- */

constexpr double XN = 95.047;
constexpr double YN = 100.0;
constexpr double ZN = 108.883;

inline double lab_f(double t) {
    return (t > 0.008856) ? std::cbrt(t) : (7.787 * t + 16.0 / 116.0);
}

inline double lab_f_inv(double t) {
    return (t > 0.206893) ? (t * t * t) : ((t - 16.0 / 116.0) / 7.787);
}

inline LAB xyz_to_lab(const XYZ& xyz) {
    const double fx = lab_f(xyz.x / XN);
    const double fy = lab_f(xyz.y / YN);
    const double fz = lab_f(xyz.z / ZN);
    return LAB{ 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz) };
}

inline XYZ lab_to_xyz(const LAB& lab) {
    const double fy = (lab.L + 16.0) / 116.0;
    const double fx = lab.a / 500.0 + fy;
    const double fz = fy - lab.b / 200.0;
    return XYZ{ XN * lab_f_inv(fx), YN * lab_f_inv(fy), ZN * lab_f_inv(fz) };
}

} // anonymous namespace

/* ============================================================================
 * Public color-space helpers
 * ============================================================================ */

RGB hex_to_rgb(const std::string& hex) {
    std::string s = hex;
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 6) {
        throw std::invalid_argument("prusa_fdm_mixer::hex_to_rgb: expected 6 hex digits, got: " + hex);
    }
    auto parse_byte = [&](size_t off) -> std::uint8_t {
        unsigned int v = 0;
        for (size_t i = 0; i < 2; ++i) {
            char c = s[off + i];
            unsigned int d;
            if (c >= '0' && c <= '9') d = static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') d = 10u + static_cast<unsigned int>(c - 'a');
            else if (c >= 'A' && c <= 'F') d = 10u + static_cast<unsigned int>(c - 'A');
            else throw std::invalid_argument("prusa_fdm_mixer::hex_to_rgb: invalid hex char in: " + hex);
            v = (v << 4) | d;
        }
        return static_cast<std::uint8_t>(v);
    };
    return RGB{ parse_byte(0), parse_byte(2), parse_byte(4) };
}

std::string rgb_to_hex(const RGB& rgb) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                  static_cast<unsigned int>(rgb.r),
                  static_cast<unsigned int>(rgb.g),
                  static_cast<unsigned int>(rgb.b));
    return std::string(buf);
}

LAB rgb_to_lab(const RGB& rgb) {
    return xyz_to_lab(rgb_to_xyz(rgb));
}

RGB lab_to_rgb(const LAB& lab) {
    return xyz_to_rgb(lab_to_xyz(lab));
}

/* ============================================================================
 * prusa-fdm-mixer model
 * ============================================================================ */

namespace {

// Empirically fit constants — see companion repo for derivation.
constexpr double YN_EXPONENT          = 3.0;
constexpr double L_SLOPE              = -0.0477;
constexpr double L_INTERCEPT          = -2.112;
constexpr double L_KINK_THRESHOLD     = 15.0;
constexpr double L_KINK_SLOPE         = -0.060;
constexpr double C_SLOPE              =  0.2780;
constexpr double C_INTERCEPT          = -15.580;
constexpr double PEAK_STRENGTH        =  1.375;
constexpr double HUE_PEAK_DEG         = 10.38;
constexpr double HUE_CENTER_DEG       = 210.0;
constexpr double HUE_HALF_WIDTH_DEG   = 30.0;

LAB predict_lab(const std::vector<Part>& parts) {
    if (parts.empty()) {
        throw std::invalid_argument("prusa_fdm_mixer::mix: empty parts list");
    }

    // ---- Gradient safety: if any single part has ratio >= ~1, return it directly. ----
    // Without this guard, the constant lightness correction (-2.112 * cs) would
    // perturb the predicted color even at the endpoints of a gradient.
    for (const Part& p : parts) {
        if (p.ratio >= 0.9999) {
            return rgb_to_lab(hex_to_rgb(p.hex));
        }
    }

    // ---- Step 1: Yule-Nielsen base prediction ----
    double r_acc = 0.0, g_acc = 0.0, b_acc = 0.0;
    std::vector<double> component_Ls;
    component_Ls.reserve(parts.size());

    const double inv_n = 1.0 / YN_EXPONENT;
    for (const Part& p : parts) {
        const RGB rgb = hex_to_rgb(p.hex);
        const double r_lin = srgb_to_linear(rgb.r);
        const double g_lin = srgb_to_linear(rgb.g);
        const double b_lin = srgb_to_linear(rgb.b);
        r_acc += std::pow(r_lin, inv_n) * p.ratio;
        g_acc += std::pow(g_lin, inv_n) * p.ratio;
        b_acc += std::pow(b_lin, inv_n) * p.ratio;
        component_Ls.push_back(rgb_to_lab(rgb).L);
    }

    auto pow_clamped = [](double v, double exp) {
        return std::pow(std::max(0.0, v), exp);
    };
    const double yn_r = linear_to_srgb(pow_clamped(r_acc, YN_EXPONENT));
    const double yn_g = linear_to_srgb(pow_clamped(g_acc, YN_EXPONENT));
    const double yn_b = linear_to_srgb(pow_clamped(b_acc, YN_EXPONENT));

    auto clamp_round = [](double v) -> std::uint8_t {
        const long iv = std::lround(v);
        return static_cast<std::uint8_t>(std::clamp<long>(iv, 0, 255));
    };
    const RGB yn_rgb{ clamp_round(yn_r), clamp_round(yn_g), clamp_round(yn_b) };
    const LAB base = rgb_to_lab(yn_rgb);

    // ---- Bell-curve weight ----
    // For N components: w = N^N * prod(ratios)
    //   = 1 when ratios are equal (peak correction zone)
    //   = 0 at any endpoint (single-component, no correction needed)
    double w = 1.0;
    for (const Part& p : parts) w *= p.ratio;
    w *= std::pow(static_cast<double>(parts.size()),
                  static_cast<double>(parts.size()));
    w = std::clamp(w, 0.0, 1.0);
    const double cs = w * PEAK_STRENGTH;

    // ---- Step 2: piecewise lightness correction ----
    const double L_min = *std::min_element(component_Ls.begin(), component_Ls.end());
    const double L_max = *std::max_element(component_Ls.begin(), component_Ls.end());
    const double L_gap = L_max - L_min;

    double L_corr = L_SLOPE * L_gap + L_INTERCEPT;
    if (L_gap > L_KINK_THRESHOLD) {
        L_corr += L_KINK_SLOPE * (L_gap - L_KINK_THRESHOLD);
    }
    L_corr *= cs;
    const double L_new = base.L + L_corr;

    // ---- Step 3: chroma correction ----
    double a_out = base.a;
    double b_out = base.b;
    const double pred_C = std::hypot(base.a, base.b);
    if (pred_C >= 0.01) {
        const double target_dC = (C_SLOPE * L_new + C_INTERCEPT) * cs;
        const double new_C = std::max(0.0, pred_C + target_dC);
        const double scale = new_C / pred_C;
        a_out = base.a * scale;
        b_out = base.b * scale;
    }

    // ---- Step 4: cyan-band hue rotation ----
    const double new_C_final = std::hypot(a_out, b_out);
    if (new_C_final >= 1.0) {
        double pred_h = std::atan2(b_out, a_out) * 180.0 / PI;
        if (pred_h < 0.0) pred_h += 360.0;

        double h_corr = 0.0;
        if (pred_h >= HUE_CENTER_DEG - HUE_HALF_WIDTH_DEG &&
            pred_h <  HUE_CENTER_DEG + HUE_HALF_WIDTH_DEG) {
            const double dist = std::abs(pred_h - HUE_CENTER_DEG);
            const double falloff = std::max(0.0, 1.0 - dist / HUE_HALF_WIDTH_DEG);
            h_corr = HUE_PEAK_DEG * falloff * w;
        }
        if (h_corr != 0.0) {
            double new_h = std::fmod(pred_h + h_corr, 360.0);
            if (new_h < 0.0) new_h += 360.0;
            const double new_h_rad = new_h * PI / 180.0;
            a_out = new_C_final * std::cos(new_h_rad);
            b_out = new_C_final * std::sin(new_h_rad);
        }
    }

    return LAB{ L_new, a_out, b_out };
}

} // anonymous namespace

RGB mix_rgb(const std::vector<Part>& parts) {
    return lab_to_rgb(predict_lab(parts));
}

std::string mix(const std::vector<Part>& parts) {
    return rgb_to_hex(mix_rgb(parts));
}

/* ============================================================================
 * ΔE2000
 * ============================================================================ */

double delta_e_2000(const LAB& lab1, const LAB& lab2) {
    const double L1 = lab1.L, a1 = lab1.a, b1 = lab1.b;
    const double L2 = lab2.L, a2 = lab2.a, b2 = lab2.b;

    const double avg_L = (L1 + L2) / 2.0;
    const double C1 = std::hypot(a1, b1);
    const double C2 = std::hypot(a2, b2);
    const double avg_C = (C1 + C2) / 2.0;
    const double avg_C7 = std::pow(avg_C, 7.0);
    const double pow25_7 = std::pow(25.0, 7.0);
    const double G = 0.5 * (1.0 - std::sqrt(avg_C7 / (avg_C7 + pow25_7)));

    const double a1p = a1 * (1.0 + G);
    const double a2p = a2 * (1.0 + G);
    const double C1p = std::hypot(a1p, b1);
    const double C2p = std::hypot(a2p, b2);
    const double avg_Cp = (C1p + C2p) / 2.0;

    auto deg = [](double x) {
        double d = x * 180.0 / PI;
        if (d < 0.0) d += 360.0;
        return d;
    };
    const double h1p = deg(std::atan2(b1, a1p));
    const double h2p = deg(std::atan2(b2, a2p));

    double dHp_diff = h2p - h1p;
    double dhp = 0.0;
    if (C1p * C2p != 0.0) {
        if (std::abs(dHp_diff) <= 180.0) dhp = dHp_diff;
        else if (dHp_diff > 180.0) dhp = dHp_diff - 360.0;
        else dhp = dHp_diff + 360.0;
    }

    double avg_Hp;
    if (C1p * C2p == 0.0) avg_Hp = h1p + h2p;
    else if (std::abs(h1p - h2p) <= 180.0) avg_Hp = (h1p + h2p) / 2.0;
    else if (h1p + h2p < 360.0) avg_Hp = (h1p + h2p + 360.0) / 2.0;
    else avg_Hp = (h1p + h2p - 360.0) / 2.0;

    const double T = 1.0
        - 0.17 * std::cos((avg_Hp - 30.0) * PI / 180.0)
        + 0.24 * std::cos((2.0 * avg_Hp) * PI / 180.0)
        + 0.32 * std::cos((3.0 * avg_Hp + 6.0) * PI / 180.0)
        - 0.20 * std::cos((4.0 * avg_Hp - 63.0) * PI / 180.0);

    const double dLp = L2 - L1;
    const double dCp = C2p - C1p;
    const double dHpFinal = 2.0 * std::sqrt(C1p * C2p) * std::sin(dhp * PI / 360.0);

    const double SL = 1.0 + (0.015 * std::pow(avg_L - 50.0, 2.0))
                              / std::sqrt(20.0 + std::pow(avg_L - 50.0, 2.0));
    const double SC = 1.0 + 0.045 * avg_Cp;
    const double SH = 1.0 + 0.015 * avg_Cp * T;

    const double dTheta = 30.0 * std::exp(-std::pow((avg_Hp - 275.0) / 25.0, 2.0));
    const double avg_Cp7 = std::pow(avg_Cp, 7.0);
    const double RC = 2.0 * std::sqrt(avg_Cp7 / (avg_Cp7 + pow25_7));
    const double RT = -RC * std::sin(2.0 * dTheta * PI / 180.0);

    return std::sqrt(
        std::pow(dLp / SL, 2.0) +
        std::pow(dCp / SC, 2.0) +
        std::pow(dHpFinal / SH, 2.0) +
        RT * (dCp / SC) * (dHpFinal / SH)
    );
}

} // namespace prusa_fdm_mixer
