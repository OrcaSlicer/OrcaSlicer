#ifndef FLUSH_VOL_PREDICTOR_HPP
#define FLUSH_VOL_PREDICTOR_HPP

#include<cstdint>
#include<unordered_map>
#include<string>
#include<vector>

namespace FlushPredict
{
    struct RGBColor
    {
        unsigned char r{ 0 };
        unsigned char g{ 0 };
        unsigned char b{ 0 };
        RGBColor(unsigned char r_,unsigned char g_,unsigned char b_) :r(r_),g(g_),b(b_){}
        RGBColor() = default;
    };

    struct LABColor
    {
        double l{ 0 };
        double a{ 0 };
        double b{ 0 };
        LABColor() = default;
        LABColor(double l_,double a_,double b_):l(l_),a(a_),b(b_){}
    };
    // transfer colour in RGB space to LAB space
    LABColor RGB2LAB(const RGBColor& color);
    // calculate DeltaE2000
    float calc_color_distance(const LABColor& lab1, const LABColor& lab2);
    float calc_color_distance(const RGBColor& rgb1, const RGBColor& rgb2);
    // check if DeltaE is within the threshold. We consider colors within the threshold to be the same
    bool is_similar_color(const RGBColor& from, const RGBColor& to, float distance_threshold = 5.0);

}

class FlushVolPredictor;

class GenericFlushPredictor
{
    using RGB = FlushPredict::RGBColor;
public:
    explicit GenericFlushPredictor(const int dataset_value);
    bool predict(const RGB& from, const RGB& to, float& flush);
    int get_min_flush_volume();
private:
    FlushVolPredictor* predictor{ nullptr };
};



#endif