#pragma once
#include "ModelPreviewPalette.hpp"
#include <limits>

namespace Slic3r::GUI::PreviewPalette {
// The portrait card is an intentional recoloring preset. Match source groups
// to its roles before replacing colors; nearest-to-pastel quantization can turn
// all skin into lip pink and merge dark clothing with hair. This uses color
// features only, not face/garment segmentation. At six groups, enumerating the
// 720 assignments is bounded and needs no solver/runtime dependency.
inline ColorTrialMapping portrait_pack_mapping(const std::vector<Color>& source,
                                               const std::vector<Color>& card)
{
    if (source.empty() || source.size() > 6 || card.size() != 6) return {};
    // Source appearance anchors: warm skin, dark neutral, light neutral,
    // red accent, cool clothing, and mid neutral. These are not print colors.
    const std::array<Color, 6> anchors {{
        {.76f, .55f, .41f}, {.12f, .11f, .12f}, {.94f, .94f, .94f},
        {.68f, .26f, .24f}, {.24f, .39f, .38f}, {.40f, .39f, .38f}
    }};
    std::array<std::array<float, 6>, 6> costs {};
    for (size_t i = 0; i < source.size(); ++i) {
        const auto lab = to_lab(source[i]);
        // A local recolor has already expressed a material choice. Preserve an
        // existing card color before inferring roles from appearance anchors.
        // Allow the rounding used by vertex-color OBJ and histogram RGB8 data.
        size_t existing = card.size();
        for (size_t slot = 0; slot < card.size(); ++slot) {
            bool same = true;
            for (size_t c = 0; c < 3; ++c)
                same = same && std::abs(source[i][c] - card[slot][c]) <= 0.5f / 255.f;
            if (same) { existing = slot; break; }
        }
        for (size_t role = 0; role < 6; ++role) {
            const auto anchor = to_lab(anchors[role]);
            for (size_t c = 0; c < 3; ++c) {
                const float delta = lab[c] - anchor[c];
                costs[i][role] += delta * delta;
            }
            if (existing < card.size()) costs[i][role] = role == existing ? 0.f : 1000.f;
        }
    }
    std::array<size_t, 6> order {{0, 1, 2, 3, 4, 5}}, best = order;
    float best_cost = std::numeric_limits<float>::max();
    do {
        float cost = 0;
        for (size_t i = 0; i < source.size(); ++i) cost += costs[i][order[i]];
        if (cost < best_cost) { best_cost = cost; best = order; }
    } while (std::next_permutation(order.begin(), order.end()));
    ColorTrialMapping result;
    result.enabled = true;
    result.mapping_colors = source;
    for (size_t i = 0; i < source.size(); ++i) result.target_colors.push_back(card[best[i]]);
    return result;
}
} // namespace Slic3r::GUI::PreviewPalette
