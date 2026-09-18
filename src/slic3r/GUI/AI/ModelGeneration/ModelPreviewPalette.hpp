#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Slic3r::GUI::PreviewPalette {
using Color = std::array<float, 3>;
inline constexpr size_t max_preview_colors = 32;
inline size_t initial_trial_color_count(size_t project_filaments, size_t model_suggestion)
{
    return std::clamp(project_filaments ? project_filaments : model_suggestion, size_t(1), max_preview_colors);
}
struct ColorTrialMapping {
    bool enabled {false};
    // Source centers determine group membership; editable target colors do
    // not change that assignment. Values are normalized sRGB.
    std::vector<Color> mapping_colors;
    std::vector<Color> target_colors;
};
inline constexpr float lightness_weight = .35f;
// Oklab matrices: https://bottosson.github.io/posts/oklab/ (public domain).
inline Color to_lab(Color rgb)
{
    for (auto& c : rgb) c = c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
    const float l = std::cbrt(.4122214708f*rgb[0] + .5363325363f*rgb[1] + .0514459929f*rgb[2]);
    const float m = std::cbrt(.2119034982f*rgb[0] + .6806995451f*rgb[1] + .1073969566f*rgb[2]);
    const float s = std::cbrt(.0883024619f*rgb[0] + .2817188376f*rgb[1] + .6299787005f*rgb[2]);
    return {.2104542553f*l + .793617785f*m - .0040720468f*s,
            1.9779984951f*l - 2.428592205f*m + .4505937099f*s,
            .0259040371f*l + .7827717662f*m - .808675766f*s};
}
inline Color to_rgb(Color lab)
{
    const float l = std::pow(lab[0] + .3963377774f*lab[1] + .2158037573f*lab[2], 3.f);
    const float m = std::pow(lab[0] - .1055613458f*lab[1] - .0638541728f*lab[2], 3.f);
    const float s = std::pow(lab[0] - .0894841775f*lab[1] - 1.291485548f*lab[2], 3.f);
    Color rgb {4.0767416621f*l - 3.3077115913f*m + .2309699292f*s,
               -1.2684380046f*l + 2.6097574011f*m - .3413193965f*s,
               -.0041960863f*l - .7034186147f*m + 1.707614701f*s};
    for (auto& c : rgb) c = std::clamp(c <= .0031308f ? 12.92f*c : 1.055f*std::pow(c, 1.f/2.4f) - .055f, 0.f, 1.f);
    return rgb;
}
inline float distance(Color a, Color b)
{
    float d = 0;
    // Small preview palettes should retain chromatic differences before spending
    // several slots on baked illumination. Keep this metric in sync with GLSL.
    for (int c = 0; c < 3; ++c) {
        const float delta = (a[c] - b[c]) * (c == 0 ? lightness_weight : 1.f);
        d += delta * delta;
    }
    return d;
}

// Same nearest-center metric and first-on-tie rule as the fragment shader.
// An empty palette returns its size, allowing callers to retain source RGB.
inline size_t nearest_lab_index(Color lab, const std::vector<Color>& centers)
{
    size_t nearest = centers.size();
    float best = 100.f;
    for (size_t i = 0; i < centers.size(); ++i) {
        const float d = distance(lab, centers[i]);
        if (d < best) { best = d; nearest = i; }
    }
    return nearest;
}

// Preview only. Area weights avoid dependence on tessellation density. A bounded
// RGB histogram keeps background preparation inexpensive; source colors stay intact.
class Histogram {
    struct Bin { double weight = 0; std::array<double, 3> sum {}; };
    struct Sample { Color lab; double weight; };
    std::vector<Bin> bins {32768};

    std::vector<Sample> samples() const
    {
        std::vector<Sample> result;
        for (const auto& bin : bins) if (bin.weight > 0)
            result.push_back({to_lab({float(bin.sum[0]/bin.weight), float(bin.sum[1]/bin.weight), float(bin.sum[2]/bin.weight)}), bin.weight});
        return result;
    }

    static std::vector<Color> select_hues(const std::vector<Sample>& samples, size_t limit)
    {
        // This protects supported color families, not semantic regions. Tiny
        // texture speckles and near-neutral illumination must not consume slots.
        std::array<Bin, 12> hues {}, accents {};
        constexpr float accent_chroma_gap = .03f;
        double total = 0;
        constexpr float pi = 3.14159265358979323846f;
        for (const auto& sample : samples) {
            total += sample.weight;
            // Dark, muted materials can have low absolute chroma despite a
            // clearly visible hue. Keep them when their area is meaningful.
            const float chroma = std::hypot(sample.lab[1], sample.lab[2]);
            if (chroma < .015f) continue;
            float angle = std::atan2(sample.lab[2], sample.lab[1]);
            if (angle < 0) angle += 2*pi;
            const size_t hue_index = std::min(size_t(11), size_t(angle*12/(2*pi)));
            auto& hue = hues[hue_index];
            hue.weight += sample.weight;
            for (int c = 0; c < 3; ++c) hue.sum[c] += sample.lab[c]*sample.weight;
            if (chroma >= .045f) {
                auto& accent = accents[hue_index];
                accent.weight += sample.weight;
                for (int c = 0; c < 3; ++c) accent.sum[c] += sample.lab[c]*sample.weight;
            }
        }
        // Relative high-chroma tails distinguish small accents from an already
        // chromatic dominant material in the same hue bucket.
        std::array<Bin, 12> tails {};
        for (const auto& sample : samples) {
            const float chroma = std::hypot(sample.lab[1], sample.lab[2]);
            if (chroma < .045f) continue;
            float angle = std::atan2(sample.lab[2], sample.lab[1]);
            if (angle < 0) angle += 2*pi;
            const size_t index = std::min(size_t(11), size_t(angle*12/(2*pi)));
            const auto& hue = hues[index];
            if (hue.weight <= 0) continue;
            const float mean_chroma = std::hypot(float(hue.sum[1]/hue.weight), float(hue.sum[2]/hue.weight));
            if (chroma < mean_chroma + accent_chroma_gap) continue;
            auto& tail = tails[index]; tail.weight += sample.weight;
            for (int c = 0; c < 3; ++c) tail.sum[c] += sample.lab[c]*sample.weight;
        }
        std::vector<Sample> candidates;
        for (size_t index = 0; index < hues.size(); ++index) {
            const auto& hue = hues[index];
            if (hue.weight <= 0) continue;
            Color lab {};
            for (int c = 0; c < 3; ++c) lab[c] = float(hue.sum[c]/hue.weight);
            // Strong accents may cover less surface than muted material areas.
            // Both need support; isolated saturated pixels still fail this gate.
            const double minimum_area = std::hypot(lab[1], lab[2]) >= .045f ? .0005 : .0015;
            const bool supported = hue.weight >= total*minimum_area;
            if (supported) candidates.push_back({lab, hue.weight/total});
            // A small saturated accent can share a hue with a much larger
            // muted material. Keep its supported chroma separately instead
            // of averaging it away; brightness alone does not add a candidate.
            const auto add_accent = [&](const Bin& accent) {
                if (accent.weight <= 0 || accent.weight < total*.0005) return false;
                Color accent_lab {};
                for (int c = 0; c < 3; ++c) accent_lab[c] = float(accent.sum[c]/accent.weight);
                if (supported && std::hypot(accent_lab[1], accent_lab[2]) - std::hypot(lab[1], lab[2]) < accent_chroma_gap)
                    return false;
                candidates.push_back({accent_lab, accent.weight/total});
                return true;
            };
            // Retain the former absolute-chroma accent when the relative
            // tail lacks area support or separation from the material mean.
            if (!add_accent(tails[index])) add_accent(accents[index]);
        }
        std::vector<Color> selected;
        while (selected.size() < limit) {
            double best = 0; Color candidate {};
            for (const auto& sample : candidates) {
                float d = sample.lab[1]*sample.lab[1] + sample.lab[2]*sample.lab[2];
                if (!selected.empty()) {
                    // Merge nearby hue/chroma candidates rather than spending
                    // a channel on baked shadows. A supported chroma accent
                    // may still share a hue with a muted material.
                    const float angle = std::atan2(sample.lab[2], sample.lab[1]);
                    if (std::any_of(selected.begin(), selected.end(), [&](const Color& center) {
                        const float difference = std::abs(std::remainder(angle - std::atan2(center[2], center[1]), 2*pi));
                        const float chroma_difference = std::abs(std::hypot(sample.lab[1], sample.lab[2]) - std::hypot(center[1], center[2]));
                        return difference < pi/12 && chroma_difference < accent_chroma_gap;
                    })) continue;
                    d = 100;
                    for (const auto& center : selected) d = std::min(d, distance(sample.lab, center));
                    // Adjacent hue bins for the same muted material should not
                    // spend multiple slots merely on a small brightness change.
                    if (d < .000225f) continue;
                }
                // Sublinear support gives a meaningful minority hue a chance
                // after the dominant hue, without prioritizing isolated pixels.
                const double score = d*std::pow(sample.weight, .25);
                if (score > best) { best = score; candidate = sample.lab; }
            }
            if (best <= 0) break;
            selected.push_back(candidate);
        }
        return selected;
    }
public:
    void add(uint32_t rgb, double area)
    {
        if (!std::isfinite(area) || area <= 0) return;
        const unsigned r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
        auto& bin = bins[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)];
        if (!std::isfinite(bin.weight + area)) return;
        bin.weight += area;
        bin.sum[0] += area*(r/255.); bin.sum[1] += area*(g/255.); bin.sum[2] += area*(b/255.);
    }
    std::vector<Color> hue_candidates(size_t limit = 6) const
    {
        auto result = select_hues(samples(), std::min(size_t(6), limit));
        for (auto& c : result) c = to_rgb(c);
        return result;
    }

    // Locked RGB values remain exact and in input order. Invalid values and
    // exact duplicates are ignored. Automatic protection is intentionally opt-in.
    std::vector<Color> palette(size_t limit = 6, const std::vector<Color>& locked = {}, bool preserve_hues = false) const
    {
        limit = std::min(max_preview_colors, limit);
        if (limit == 0) return {};
        std::vector<Color> locked_rgb, centers;
        for (const auto& color : locked) {
            if (!std::all_of(color.begin(), color.end(), [](float c) { return std::isfinite(c) && c >= 0 && c <= 1; })) continue;
            if (std::find(locked_rgb.begin(), locked_rgb.end(), color) != locked_rgb.end()) continue;
            locked_rgb.push_back(color); centers.push_back(to_lab(color));
            if (centers.size() == limit) return locked_rgb;
        }
        const auto source_samples = samples();
        if (source_samples.empty()) return locked_rgb;
        if (preserve_hues && limit > 2) {
            // Reserve supported neutral tones before allocating hue slots.
            // Otherwise gray cloth between black hair and white highlights
            // can be assigned to a warm skin center by the hue-weighted metric.
            std::vector<Sample> neutral;
            double neutral_area = 0, total_area = 0;
            for (const auto& sample : source_samples) {
                total_area += sample.weight;
                if (std::hypot(sample.lab[1], sample.lab[2]) < .015f) {
                    neutral.push_back(sample); neutral_area += sample.weight;
                }
            }
            std::vector<Color> neutral_centers;
            if (neutral_area >= total_area*.05) {
                std::sort(neutral.begin(), neutral.end(), [](const Sample& a, const Sample& b) { return a.lab[0] < b.lab[0]; });
                for (double quantile : {.05, .95}) {
                    double cumulative = 0;
                    for (const auto& sample : neutral) {
                        cumulative += sample.weight;
                        if (cumulative < neutral_area*quantile) continue;
                        if (neutral_centers.empty() || distance(sample.lab, neutral_centers.front()) >= .000225f)
                            neutral_centers.push_back(sample.lab);
                        break;
                    }
                }
                if (neutral_centers.size() == 2 && limit >= 5) {
                    // Endpoints alone cannot represent a supported middle tone.
                    // Require separation from both area-quantile endpoints and
                    // meaningful area, so isolated shading does not take a slot.
                    constexpr float middle_lightness_gap = .12f;
                    Bin middle;
                    for (const auto& sample : neutral) {
                        if (sample.lab[0] <= neutral_centers[0][0] + middle_lightness_gap ||
                            sample.lab[0] >= neutral_centers[1][0] - middle_lightness_gap) continue;
                        middle.weight += sample.weight;
                        for (int c = 0; c < 3; ++c) middle.sum[c] += sample.lab[c]*sample.weight;
                    }
                    if (middle.weight >= total_area*.005) {
                        Color lab {};
                        for (int c = 0; c < 3; ++c) lab[c] = float(middle.sum[c]/middle.weight);
                        neutral_centers.push_back(lab);
                    }
                }
            }
            // User locks still take precedence. With no supported neutrals,
            // keep two slots free for dominant tones during ordinary filling.
            const size_t neutral_budget = std::max(size_t(2), neutral_centers.size());
            for (const auto& hue : select_hues(source_samples, limit - neutral_budget)) {
                if (centers.size() >= limit - neutral_budget) break;
                if (std::any_of(centers.begin(), centers.end(), [&](const Color& c) { return distance(hue, c) < .000225f; })) continue;
                centers.push_back(hue);
            }
            for (const auto& lab : neutral_centers) {
                if (centers.size() == limit) break;
                if (std::none_of(centers.begin(), centers.end(), [&](const Color& c) { return distance(lab, c) < .000225f; }))
                    centers.push_back(lab);
            }
        }
        const size_t fixed = centers.size();
        if (centers.empty()) centers.push_back(std::max_element(source_samples.begin(), source_samples.end(),
            [](const auto& a, const auto& b) { return a.weight < b.weight; })->lab);
        while (centers.size() < std::min(limit, source_samples.size() + locked_rgb.size())) {
            double best = 0; Color candidate {};
            for (const auto& sample : source_samples) {
                float d = 100;
                for (const auto& center : centers) d = std::min(d, distance(sample.lab, center));
                const double score = d * std::sqrt(sample.weight);
                if (score > best) { best = score; candidate = sample.lab; }
            }
            if (best < 1e-15) break;
            centers.push_back(candidate);
        }
        for (int iteration = 0; iteration < 20; ++iteration) {
            std::vector<Bin> totals(centers.size());
            for (const auto& sample : source_samples) {
                const size_t nearest = nearest_lab_index(sample.lab, centers);
                totals[nearest].weight += sample.weight;
                for (int c = 0; c < 3; ++c) totals[nearest].sum[c] += sample.lab[c]*sample.weight;
            }
            float movement = 0;
            for (size_t j = fixed; j < centers.size(); ++j) if (totals[j].weight > 0) {
                Color next {};
                for (int c = 0; c < 3; ++c) next[c] = float(totals[j].sum[c]/totals[j].weight);
                movement += distance(next, centers[j]); centers[j] = next;
            }
            if (movement < 1e-9f) break;
        }
        std::sort(centers.begin() + locked_rgb.size(), centers.end(), [](const Color& a, const Color& b) { return a[0] < b[0]; });
        for (auto& c : centers) c = to_rgb(c);
        std::copy(locked_rgb.begin(), locked_rgb.end(), centers.begin());
        return centers;
    }
};
} // namespace Slic3r::GUI::PreviewPalette
