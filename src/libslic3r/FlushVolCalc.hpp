#ifndef slic3r_FlushVolCalc_hpp_
#define slic3r_FlushVolCalc_hpp_

#include "libslic3r.h"
#include "FlushVolPredictor.hpp"
#include "PrintConfig.hpp"


namespace Slic3r {

extern const int g_min_flush_volume_from_support;
extern const int g_flush_volume_to_support;
extern const int g_max_flush_volume;

class FlushVolCalculator
{
public:
    FlushVolCalculator(int min, int max, int flush_dataset, float multiplier = 1.0f);
    ~FlushVolCalculator()
    {
    }

    int calc_flush_vol(unsigned char src_a, unsigned char src_r, unsigned char src_g, unsigned char src_b,
        unsigned char dst_a, unsigned char dst_r, unsigned char dst_g, unsigned char dst_b);

    int calc_flush_vol_rgb(unsigned char src_r,unsigned char src_g,unsigned char src_b,
        unsigned char dst_r, unsigned char dst_g, unsigned char dst_b);

    bool get_flush_vol_from_data(unsigned char src_r, unsigned char src_g, unsigned char src_b,
        unsigned char dst_r, unsigned char dst_g, unsigned char dst_b, float& flush);

private:
    int m_min_flush_vol;
    int m_max_flush_vol;
    float m_multiplier;
    int m_flush_dataset;
};


}

#endif
