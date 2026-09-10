#pragma once

#include "../ModelGeneration/ModelPreviewPalette.hpp"
#include "ModelObjText.hpp"
#include <boost/pending/disjoint_sets.hpp>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r::AI::ColorCleanup {
using Color = GUI::PreviewPalette::Color;
struct VertexColor {
    Color rgb;
    size_t begin, end;
};

// Keep the position spelling, optional alpha, whitespace and comment verbatim.
// OBJ position weights and texture-only colors are deliberately unsupported.
inline VertexColor parse(const std::string& line)
{
    std::array<std::string_view, 9> tokens;
    size_t count = 0, at = 0;
    while (count < tokens.size()) {
        const auto token = ObjText::next(line, at);
        if (token.empty()) break;
        tokens[count++] = token;
    }
    if (count != 7 && count != 8)
        throw std::runtime_error("Color cleanup requires normalized RGB vertex colors (optionally alpha); texture-only or missing colors are unsupported.");
    VertexColor color {};
    color.begin = size_t(tokens[4].data() - line.data());
    color.end = size_t(tokens[6].data() + tokens[6].size() - line.data());
    for (size_t i = 4; i < count; ++i) {
        double channel;
        if (!ObjText::number(tokens[i], channel) || channel < 0 || channel > 1)
            throw std::runtime_error("Color cleanup found invalid vertex RGB or alpha; channels must be between 0 and 1.");
        if (i < 7) color.rgb[i - 4] = float(channel);
    }
    return color;
}

// All decisions use the immutable input. Disjoint sets reuse Boost's connected
// component primitive; processing one speckle cannot trigger a cascade in others.
template<class Position, class Cancel>
std::vector<size_t> replacements(const std::vector<VertexColor>& colors,
    const std::vector<Color>& palette, const std::vector<Position>& vertices,
    const std::vector<std::vector<size_t>>& neighbors, const std::vector<bool>& pinned,
    const std::vector<double>& areas, double strength, size_t& cleaned_regions, Cancel check_cancel)
{
    const size_t count = vertices.size(), none = count;
    std::vector<size_t> replacement(count, none);
    if (strength <= 0) return replacement;
    std::vector<Color> centers;
    for (const Color& color : palette) centers.push_back(GUI::PreviewPalette::to_lab(color));
    std::vector<size_t> labels(count);
    boost::disjoint_sets_with_storage<> patches(count), regions(count);
    for (size_t i = 0; i < count; ++i) {
        if ((i & 4095) == 0) check_cancel();
        labels[i] = GUI::PreviewPalette::nearest_lab_index(GUI::PreviewPalette::to_lab(colors[i].rgb), centers);
    }
    for (size_t i = 0; i < count; ++i) {
        if ((i & 4095) == 0) check_cancel();
        for (size_t j : neighbors[i]) if (j > i) {
            patches.union_set(i, j);
            if (labels[i] == labels[j]) regions.union_set(i, j);
        }
    }
    std::vector<double> patch_area(count, 0), region_area(count, 0);
    std::vector<bool> protected_region(count, false);
    std::vector<std::vector<size_t>> members(count);
    for (size_t i = 0; i < count; ++i) {
        if ((i & 4095) == 0) check_cancel();
        if (areas[i] <= 0) continue;
        const size_t region = regions.find_set(i);
        patch_area[patches.find_set(i)] += areas[i]; region_area[region] += areas[i];
        protected_region[region] = protected_region[region] || pinned[i];
        members[region].push_back(i);
    }
    for (size_t region = 0; region < count; ++region) {
        if ((region & 4095) == 0) check_cancel();
        if (members[region].empty() || protected_region[region]) continue;
        const double limit = patch_area[patches.find_set(members[region].front())] * (.005 + .045 * strength);
        if (region_area[region] <= 0 || region_area[region] > limit) continue;
        std::vector<double> votes(palette.size(), 0);
        double total = 0;
        for (size_t i : members[region]) {
            check_cancel();
            for (size_t j : neighbors[i]) if (regions.find_set(j) != region) {
                const double weight = (vertices[i] - vertices[j]).norm();
                total += weight;
                // A tiny adjacent speckle cannot justify absorbing this region.
                if (region_area[regions.find_set(j)] >= 10 * region_area[region]) votes[labels[j]] += weight;
            }
        }
        const size_t winner = size_t(std::max_element(votes.begin(), votes.end()) - votes.begin());
        if (total <= 0 || votes[winner] < .9 * total) continue;
        // Choose an actual boundary color nearest its weighted average. Copying
        // its original RGB text introduces no new colors, unlike averaging RGB.
        Color mean {};
        double weight_sum = 0;
        for (size_t i : members[region]) for (size_t j : neighbors[i])
            if (labels[j] == winner && region_area[regions.find_set(j)] >= 10 * region_area[region]) {
                const double weight = (vertices[i] - vertices[j]).norm();
                const Color lab = GUI::PreviewPalette::to_lab(colors[j].rgb);
                for (size_t k = 0; k < 3; ++k) mean[k] += float(lab[k] * weight);
                weight_sum += weight;
            }
        for (float& value : mean) value /= float(weight_sum);
        float best = std::numeric_limits<float>::max();
        size_t representative = none;
        for (size_t i : members[region]) for (size_t j : neighbors[i])
            if (labels[j] == winner && region_area[regions.find_set(j)] >= 10 * region_area[region]) {
                const float distance = GUI::PreviewPalette::distance(mean, GUI::PreviewPalette::to_lab(colors[j].rgb));
                if (distance < best || (distance == best && j < representative)) { best = distance; representative = j; }
            }
        if (representative == none) continue;
        for (size_t i : members[region]) replacement[i] = representative;
        ++cleaned_regions;
    }
    return replacement;
}
} // namespace Slic3r::AI::ColorCleanup
