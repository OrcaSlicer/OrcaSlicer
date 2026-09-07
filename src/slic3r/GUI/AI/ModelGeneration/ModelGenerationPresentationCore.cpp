#include "ModelGenerationPresentation.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace Slic3r::GUI::ModelGenerationPresentation {
namespace {

struct LabColor
{
    double l { 0.0 };
    double a { 0.0 };
    double b { 0.0 };
};

LabColor lab_color(const std::string& color)
{
    const auto channel = [&color](size_t offset) {
        const auto digit = [](char value) {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return value - 'a' + 10;
        };
        return static_cast<unsigned char>(digit(color[offset]) * 16 + digit(color[offset + 1]));
    };
    if (!Slic3r::AI::is_rgb_hex_color(color))
        return {};
    const auto linear = [](unsigned char value) {
        const double channel_value = value / 255.0;
        return channel_value <= 0.04045
            ? channel_value / 12.92
            : std::pow((channel_value + 0.055) / 1.055, 2.4);
    };
    const double red = linear(channel(1));
    const double green = linear(channel(3));
    const double blue = linear(channel(5));
    const double x = (0.4124564 * red + 0.3575761 * green + 0.1804375 * blue) / 0.95047;
    const double y = 0.2126729 * red + 0.7151522 * green + 0.0721750 * blue;
    const double z = (0.0193339 * red + 0.1191920 * green + 0.9503041 * blue) / 1.08883;
    const auto transform = [](double value) {
        return value > 0.008856 ? std::cbrt(value) : 7.787 * value + 16.0 / 116.0;
    };
    const double fx = transform(x);
    const double fy = transform(y);
    const double fz = transform(z);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

} // namespace

void synchronize_palette_roles(const std::vector<std::string>& current_palette,
                               AIModelGenerationClient::PaletteRoles& current_roles,
                               const std::vector<std::string>& submitted_palette,
                               const AIModelGenerationClient::PaletteRoles& submitted_roles,
                               const std::vector<std::string>& returned_palette,
                               const AIModelGenerationClient::PaletteRoles& returned_roles)
{
    // Portrait processing may correct skin/garment ownership without changing
    // the colors. Adopt that correction only while the submitted inputs are intact.
    if (!returned_roles.empty() && current_palette == submitted_palette &&
        returned_palette == submitted_palette && current_roles == submitted_roles)
        current_roles = returned_roles;
}

AIModelGenerationClient::PaletteRoles automatic_palette_roles(const std::vector<std::string>& palette)
{
    AIModelGenerationClient::PaletteRoles result;
    if (!Slic3r::AI::is_supported_target_palette_color_count(palette.size()) ||
        !std::all_of(palette.begin(), palette.end(), Slic3r::AI::is_rgb_hex_color) ||
        std::set<std::string>(palette.begin(), palette.end()).size() != palette.size())
        return result;
    std::map<std::string, LabColor> labs;
    for (const std::string& color : palette)
        labs.emplace(color, lab_color(color));
    std::vector<std::string> available = palette;
    const auto take = [&available, &result](const char* role, const auto& better) {
        if (available.empty())
            return;
        const auto selected = std::max_element(available.begin(), available.end(), better);
        result.emplace(role, *selected);
        available.erase(selected);
    };
    if (palette.size() >= 2)
        take("structure", [&labs](const std::string& left, const std::string& right) {
            return labs[left].l > labs[right].l;
        });
    if (palette.size() >= 3)
        take("light", [&labs](const std::string& left, const std::string& right) {
            return labs[left].l < labs[right].l;
        });
    take("primary", [&labs](const std::string& left, const std::string& right) {
        const double left_chroma = std::hypot(labs[left].a, labs[left].b);
        const double right_chroma = std::hypot(labs[right].a, labs[right].b);
        if (left_chroma != right_chroma)
            return left_chroma < right_chroma;
        return std::abs(labs[left].l - 58.0) > std::abs(labs[right].l - 58.0);
    });
    const auto take_by_chroma = [&take, &labs](const char* role) {
        take(role, [&labs](const std::string& left, const std::string& right) {
            return std::hypot(labs[left].a, labs[left].b) < std::hypot(labs[right].a, labs[right].b);
        });
    };
    take_by_chroma("accent");
    take_by_chroma("secondary");
    take_by_chroma("detail");
    return result;
}

bool same_palette_color(const std::string& left, const std::string& right)
{
    return left.size() == right.size() && std::equal(
        left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
            return std::toupper(a) == std::toupper(b);
        });
}

double minimum_palette_distance(const std::vector<std::string>& palette)
{
    double minimum = std::numeric_limits<double>::infinity();
    for (size_t left = 0; left < palette.size(); ++left) {
        const LabColor a = lab_color(palette[left]);
        for (size_t right = left + 1; right < palette.size(); ++right) {
            const LabColor b = lab_color(palette[right]);
            minimum = std::min(minimum, std::sqrt(
                std::pow(a.l - b.l, 2.0) + std::pow(a.a - b.a, 2.0) + std::pow(a.b - b.b, 2.0)));
        }
    }
    return minimum;
}

int remap_progress(int value, int input_start, int input_end, int output_start, int output_end)
{
    value = std::clamp(value, input_start, input_end);
    return output_start + (value - input_start) * (output_end - output_start) / (input_end - input_start);
}

int display_progress(const AIModelGenerationClient::JobStatus& status)
{
    if (status.state == "recommending_palette")
        return remap_progress(status.progress, 5, 10, 3, 10);
    if (status.state == "awaiting_palette_confirmation")
        return 10;
    if (status.state == "preprocessing")
        return remap_progress(status.progress, 5, 15, 10, 25);
    if (status.phase == "preparing_multiview")
        return remap_progress(status.progress, 10, 20, 36, 42);
    if (status.phase == "multiview_retry")
        return 40;
    if (status.state == "awaiting_confirmation")
        return 35;
    if (status.phase == "generating" || status.phase == "texturing")
        return remap_progress(status.progress, 20, 70, 40, 78);
    if (status.phase == "converting")
        return remap_progress(status.progress, 75, 95, 80, 90);
    if (status.phase == "downloading_artifact")
        return 92;
    if (status.phase == "checking_model")
        return remap_progress(status.progress, 96, 99, 93, 97);
    if (status.phase == "checking_visual")
        return 98;
    if (status.state == "ready")
        return 100;
    if (status.state == "failed" && status.progress >= 10)
        return 40;
    return 0;
}

int style_selection(const std::string& style)
{
    if (style == "realistic" || style == "enamel_inlay") return 1;
    return style == "sculpture" || style.empty() ? 0 : 2;
}

int stylized_style_selection(const std::string& style)
{
    if (style == "portrait_sketch") return 0;
    if (style == "low_poly") return 2;
    if (style == "relief") return 3;
    if (style == "ink_relief") return 4;
    if (style == "diorama") return 5;
    if (style == "custom") return 6;
    return 1;
}

std::string selected_style(int family, int stylized)
{
    static constexpr std::array<const char*, 7> variants {
        "portrait_sketch", "cartoon", "low_poly", "relief", "ink_relief", "diorama", "custom"
    };
    if (family == 0) return "sculpture";
    if (family == 1) return "realistic";
    return variants[stylized >= 0 && stylized < static_cast<int>(variants.size()) ? stylized : 1];
}

bool style_uses_printable_colors(const std::string& style)
{
    return style != "sculpture";
}

} // namespace Slic3r::GUI::ModelGenerationPresentation
