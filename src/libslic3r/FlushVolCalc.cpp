#include <cmath>
#include <assert.h>
#include "slic3r/Utils/ColorSpaceConvert.hpp"
#include "Utils.hpp"
#include "FlushVolCalc.hpp"


namespace Slic3r {

const int g_min_flush_volume_from_support = 700;
const int g_flush_volume_to_support = 230;

const int g_max_flush_volume = 20000; //Orca: increase limit to 20k vs 900 in upstream BBS code.
                                      // Retain a clamp to guard against extreme values entered in the UI

static float to_radians(float degree)
{
    return degree / 180.f * M_PI;
}


static float get_luminance(float r, float g, float b)
{
    return r * 0.3 + g * 0.59 + b * 0.11;
}

static float calc_triangle_3rd_edge(float edge_a, float edge_b, float degree_ab)
{
    return std::sqrt(edge_a * edge_a + edge_b * edge_b - 2 * edge_a * edge_b * std::cos(to_radians(degree_ab)));
}

static float DeltaHS_BBS(float h1, float s1, float v1, float h2, float s2, float v2)
{
    float h1_rad = to_radians(h1);
    float h2_rad = to_radians(h2);

    float dx = std::cos(h1_rad) * s1 * v1 - cos(h2_rad) * s2 * v2;
    float dy = std::sin(h1_rad) * s1 * v1 - sin(h2_rad) * s2 * v2;
    float dxy = std::sqrt(dx * dx + dy * dy);
    return std::min(1.2f, dxy);
}

FlushVolCalculator::FlushVolCalculator(int min, int max, int flush_dataset, float multiplier)
    :m_min_flush_vol(min), m_max_flush_vol(max), m_multiplier(multiplier), m_flush_dataset(flush_dataset)
{
}

bool FlushVolCalculator::get_flush_vol_from_data(unsigned char src_r, unsigned char src_g, unsigned char src_b,
    unsigned char dst_r, unsigned char dst_g, unsigned char dst_b, float& flush)
{
    GenericFlushPredictor pd(m_flush_dataset);
    FlushPredict::RGBColor src(src_r, src_g, src_b);
    FlushPredict::RGBColor dst(dst_r, dst_g, dst_b);

    return pd.predict(src, dst, flush);
}

int FlushVolCalculator::calc_flush_vol_rgb(unsigned char src_r, unsigned char src_g, unsigned char src_b,
    unsigned char dst_r, unsigned char dst_g, unsigned char dst_b)
{
    float flush_volume;
    if(m_flush_dataset == 0 && get_flush_vol_from_data(src_r, src_g, src_b, dst_r, dst_g, dst_b, flush_volume))
        return flush_volume;
    float src_r_f, src_g_f, src_b_f, dst_r_f, dst_g_f, dst_b_f;
    float from_hsv_h, from_hsv_s, from_hsv_v;
    float to_hsv_h, to_hsv_s, to_hsv_v;

    src_r_f = (float)src_r / 255.f;
    src_g_f = (float)src_g / 255.f;
    src_b_f = (float)src_b / 255.f;
    dst_r_f = (float)dst_r / 255.f;
    dst_g_f = (float)dst_g / 255.f;
    dst_b_f = (float)dst_b / 255.f;

    // Calculate color distance in HSV color space
    RGB2HSV(src_r_f, src_g_f, src_b_f, &from_hsv_h, &from_hsv_s, &from_hsv_v);
    RGB2HSV(dst_r_f, dst_g_f, dst_b_f, &to_hsv_h, &to_hsv_s, &to_hsv_v);
    float hs_dist = DeltaHS_BBS(from_hsv_h, from_hsv_s, from_hsv_v, to_hsv_h, to_hsv_s, to_hsv_v);

    // 1. Color difference is more obvious if the dest color has high luminance
    // 2. Color difference is more obvious if the source color has low luminance
    float from_lumi = get_luminance(src_r_f, src_g_f, src_b_f);
    float to_lumi = get_luminance(dst_r_f, dst_g_f, dst_b_f);
    float lumi_flush = 0.f;
    if (to_lumi >= from_lumi) {
        lumi_flush = std::pow(to_lumi - from_lumi, 0.7f) * 560.f;
    }
    else {
        lumi_flush = (from_lumi - to_lumi) * 80.f;

        float inter_hsv_v = 0.67 * to_hsv_v + 0.33 * from_hsv_v;
        hs_dist = std::min(inter_hsv_v, hs_dist);
    }
    float hs_flush = 230.f * hs_dist;

    flush_volume = calc_triangle_3rd_edge(hs_flush, lumi_flush, 120.f);
    flush_volume = std::max(flush_volume, 60.f);

    return flush_volume;
}

int FlushVolCalculator::calc_flush_vol(unsigned char src_a, unsigned char src_r, unsigned char src_g, unsigned char src_b,
    unsigned char dst_a, unsigned char dst_r, unsigned char dst_g, unsigned char dst_b)
{
    // BBS: Transparent materials are treated as white materials
    if (src_a == 0) {
        src_r = src_g = src_b = 255;
    }
    if (dst_a == 0) {
        dst_r = dst_g = dst_b = 255;
    }

    float flush_volume;
    if(m_flush_dataset != 0  && get_flush_vol_from_data(src_r, src_g, src_b, dst_r, dst_g, dst_b, flush_volume))
        return std::min((int)flush_volume, m_max_flush_vol);


    flush_volume = calc_flush_vol_rgb(src_r, src_g, src_b, dst_r, dst_g, dst_b);

    constexpr float dark_color_thres = 180.f/255.f;
    constexpr float light_color_thres = 75.f/255.f;
    bool is_from_dark = get_luminance(src_r, src_g, src_b) > dark_color_thres;
    bool is_to_light = get_luminance(dst_r, dst_g, dst_b) < light_color_thres;
    if (m_flush_dataset != 0 && is_from_dark && is_to_light)
        flush_volume *= 1.3;

    flush_volume += m_min_flush_vol;
    return std::min((int)flush_volume, m_max_flush_vol);
}

}
