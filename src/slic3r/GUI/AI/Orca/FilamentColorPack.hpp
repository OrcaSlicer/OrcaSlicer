#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

class wxWindow;

namespace Slic3r::GUI {

// A physical-slot color card, not a temperature/flow preset or mixing recipe.
struct FilamentColorPack {
    std::string name;
    std::vector<std::string> colors;
    std::vector<std::string> labels;
    // Optional native physical-spool appearance metadata for saved cards.
    std::vector<std::string> multi_colors, color_types;

    bool valid() const {
        if (name.empty() || colors.empty() || colors.size() > 6 || labels.size() != colors.size() ||
            (!multi_colors.empty() && multi_colors.size() != colors.size()) ||
            (!color_types.empty() && color_types.size() != colors.size())) return false;
        return std::all_of(colors.begin(), colors.end(), [](const std::string& color) {
            return color.size() == 7 && color[0] == '#' &&
                std::all_of(color.begin() + 1, color.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
        });
    }
};

inline FilamentColorPack young_portrait_color_pack() {
    return {u8"人物基础 · 年轻向六色",
        {"#F7E2DA", "#282629", "#F6F7F9", "#EA9A92", "#668CB6", "#958B86"},
        {u8"清透浅嫩肤色", u8"柔炭黑", u8"冷调奶白", u8"蜜桃浅橘红", u8"雾感浅牛仔蓝", u8"柔灰棕"}};
}

std::vector<FilamentColorPack> load_filament_color_packs();
// Explicit project operation. Cancelling the picker leaves the project unchanged.
bool show_filament_color_packs(wxWindow* parent);

} // namespace Slic3r::GUI
