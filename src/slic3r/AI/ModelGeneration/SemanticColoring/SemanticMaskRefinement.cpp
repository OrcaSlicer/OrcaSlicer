#include "SemanticMaskRefinement.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>

namespace Slic3r::AI::SemanticColoring {
namespace {

Color oklab(const uint8_t* pixel)
{
    Color rgb {{pixel[0] / 255.f, pixel[1] / 255.f, pixel[2] / 255.f}};
    for (float& channel : rgb)
        channel = channel <= .04045f ? channel / 12.92f : std::pow((channel + .055f) / 1.055f, 2.4f);
    const float l = std::cbrt(.4122214708f*rgb[0] + .5363325363f*rgb[1] + .0514459929f*rgb[2]);
    const float m = std::cbrt(.2119034982f*rgb[0] + .6806995451f*rgb[1] + .1073969566f*rgb[2]);
    const float s = std::cbrt(.0883024619f*rgb[0] + .2817188376f*rgb[1] + .6299787005f*rgb[2]);
    return {{.2104542553f*l + .793617785f*m - .0040720468f*s,
        1.9779984951f*l - 2.428592205f*m + .4505937099f*s,
        .0259040371f*l + .7827717662f*m - .808675766f*s}};
}

float distance(const Color& a, const Color& b)
{
    const float dl = (a[0] - b[0]) * .65f, da = a[1] - b[1], db = a[2] - b[2];
    return dl*dl + da*da + db*db;
}

bool dark_natural_hair(const Color& color)
{
    const float saturation = std::hypot(color[1], color[2]);
    return color[0] < .67f && saturation < .115f &&
        (saturation >= .008f || color[0] < .40f) &&
        color[1] >= -.012f && color[2] >= -.012f &&
        color[1] <= color[2] * 1.15f + .018f;
}

bool candidate(Label label, float confidence, const Color& appearance)
{
    if (label == Label::Hair) return true;
    // A dark brown source may be hair even when body segmentation confidently
    // calls it skin. Ear and neck colors in the fixed portraits are materially
    // lighter, so do not let local color continuity drift from hair into them.
    if (label == Label::BodySkin) return appearance[0] < .55f;
    return confidence < minimum_confidence &&
        (label == Label::Unknown || label == Label::Background ||
         (label == Label::FaceSkin && appearance[0] < .55f) || label == Label::Clothes);
}

} // namespace

bool refine_dark_hair_mask(const RGBImage& image, Prediction& prediction, const Cancel& cancel)
{
    if (!image.valid() || !prediction.valid_for(image)) return true;
    if (cancel && cancel()) return false;
    const size_t count = prediction.labels.size();
    std::vector<Color> appearance(count);
    std::vector<uint8_t> eligible(count, 0), seed(count, 0), seen(count, 0), promote(count, 0);
    for (size_t pixel = 0; pixel < count; ++pixel) {
        if ((pixel & 4095) == 0 && cancel && cancel()) return false;
        appearance[pixel] = oklab(&image.pixels[pixel * 3]);
        if (!dark_natural_hair(appearance[pixel])) continue;
        const Label label = prediction.labels[pixel];
        const float confidence = prediction.confidence[pixel];
        eligible[pixel] = candidate(label, confidence, appearance[pixel]);
        seed[pixel] = label == Label::Hair && confidence >= .80f;
    }

    std::deque<uint32_t> pending;
    std::vector<uint32_t> component;
    for (size_t first = 0; first < count; ++first) {
        if (!eligible[first] || seen[first]) continue;
        if ((first & 4095) == 0 && cancel && cancel()) return false;
        component.clear(); pending.clear();
        pending.push_back(uint32_t(first)); seen[first] = 1;
        size_t seeds = 0;
        while (!pending.empty()) {
            const uint32_t pixel = pending.front(); pending.pop_front();
            component.push_back(pixel); seeds += seed[pixel];
            const int x = int(pixel % image.width), y = int(pixel / image.width);
            const std::array<int64_t, 4> neighbors {{
                x > 0 ? int64_t(pixel) - 1 : -1,
                x + 1 < image.width ? int64_t(pixel) + 1 : -1,
                y > 0 ? int64_t(pixel) - image.width : -1,
                y + 1 < image.height ? int64_t(pixel) + image.width : -1
            }};
            for (int64_t neighbor : neighbors) {
                if (neighbor < 0 || seen[size_t(neighbor)] || !eligible[size_t(neighbor)] ||
                    distance(appearance[pixel], appearance[size_t(neighbor)]) > .0032f) continue;
                seen[size_t(neighbor)] = 1; pending.push_back(uint32_t(neighbor));
            }
        }
        // A few pixels at a dark silhouette are not enough evidence. A large
        // component may tolerate sparse recognizer holes, while a small one
        // still needs eight independent reliable pixels.
        if (seeds >= 8 && (seeds >= component.size() * .005 || seeds >= 32))
            for (uint32_t pixel : component) promote[pixel] = 1;
    }
    if (cancel && cancel()) return false;

    auto labels = prediction.labels;
    auto confidence = prediction.confidence;
    for (size_t pixel = 0; pixel < count; ++pixel) if (promote[pixel]) {
        labels[pixel] = Label::Hair;
        confidence[pixel] = std::max(confidence[pixel], .82f);
    }
    prediction.labels.swap(labels);
    prediction.confidence.swap(confidence);
    return true;
}

} // namespace Slic3r::AI::SemanticColoring
