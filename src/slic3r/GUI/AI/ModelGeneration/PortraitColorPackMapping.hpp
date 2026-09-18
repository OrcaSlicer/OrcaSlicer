#pragma once
#include "ModelPreviewPalette.hpp"

namespace Slic3r::GUI::PreviewPalette {
// The portrait card is an intentional recoloring preset. Match source groups
// to its roles before replacing colors; nearest-to-pastel quantization can turn
// all skin into lip pink and merge dark clothing with hair. This uses color
// features only, not face/garment segmentation. Groups may share a material;
// a role absent from the source does not have to consume a color group.
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
    // Baked skin shadows can be as dark as red accents. Give hue/chroma more
    // influence when inferring a role, while retaining enough lightness to
    // distinguish dark, mid and light neutrals. This is separate from the
    // nearest-source-center metric: group membership must remain unchanged.
    constexpr float role_lightness_weight = .65f;
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
                const float delta = (lab[c] - anchor[c]) * (c == 0 ? role_lightness_weight : 1.f);
                costs[i][role] += delta * delta;
            }
            // Neutral gray must stay in the neutral family even if a skin or
            // clothing anchor is closer in brightness. Exact material choices
            // below still override inference, including a tinted neutral card.
            if (std::hypot(lab[1], lab[2]) < .015f && role != 1 && role != 2 && role != 5)
                costs[i][role] = 1000.f;
            if (existing < card.size()) costs[i][role] = role == existing ? 0.f : 1000.f;
        }
    }
    ColorTrialMapping result;
    result.enabled = true;
    result.mapping_colors = source;
    for (size_t i = 0; i < source.size(); ++i) {
        const size_t role = std::min_element(costs[i].begin(), costs[i].end()) - costs[i].begin();
        result.target_colors.push_back(card[role]);
    }
    return result;
}
} // namespace Slic3r::GUI::PreviewPalette
