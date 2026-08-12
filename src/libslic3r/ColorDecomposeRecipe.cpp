#include "ColorDecomposeRecipe.hpp"

#include "FilamentMixer.hpp"
#include "Utils.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <utility>

namespace Slic3r {
namespace {

struct LabColor {
    double l{0.0};
    double a{0.0};
    double b{0.0};
};

struct StandardRecipeEntry {
    ColorDecomposeRecipeMode mode{ColorDecomposeRecipeMode::CMYW};
    std::string material;
    std::string source;
    std::vector<std::string> component_keys;
    std::vector<std::string> component_hexes;
    std::vector<int> ratios;
    std::string measured_hex;
    LabColor measured_lab;
};

static double srgb_to_linear(double v)
{
    v /= 255.0;
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

static double xyz_to_lab_component(double v)
{
    constexpr double eps = 216.0 / 24389.0;
    constexpr double kappa = 24389.0 / 27.0;
    return v > eps ? std::cbrt(v) : (kappa * v + 16.0) / 116.0;
}

static LabColor rgb_to_lab(const ColorDecomposeRgb& rgb)
{
    const double r = srgb_to_linear(rgb.r);
    const double g = srgb_to_linear(rgb.g);
    const double b = srgb_to_linear(rgb.b);

    const double x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047;
    const double y = (0.2126729 * r + 0.7151522 * g + 0.0721750 * b);
    const double z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883;

    const double fx = xyz_to_lab_component(x);
    const double fy = xyz_to_lab_component(y);
    const double fz = xyz_to_lab_component(z);

    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

static double delta_e76(const LabColor& a, const LabColor& b)
{
    return std::sqrt(std::pow(a.l - b.l, 2.0) + std::pow(a.a - b.a, 2.0) + std::pow(a.b - b.b, 2.0));
}

static bool material_matches(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty())
        return false;
    return a == b || a == b + " Basic" || b == a + " Basic";
}

static std::vector<std::vector<int>> ratio_grid(size_t n)
{
    std::vector<std::vector<int>> out;
    if (n == 2) {
        for (int a = 20; a <= 80; a += 5)
            out.push_back({a, 100 - a});
    } else if (n == 3) {
        for (int a = 20; a <= 60; a += 5)
            for (int b = 20; b <= 80 - a; b += 5) {
                const int c = 100 - a - b;
                if (c >= 20)
                    out.push_back({a, b, c});
            }
    }
    return out;
}

static ColorDecomposeRecipeMode parse_mode(const std::string& s)
{
    if (s == "RYBW" || s == "RGBY")
        return ColorDecomposeRecipeMode::RYBW;
    return ColorDecomposeRecipeMode::CMYW;
}

static std::vector<StandardRecipeEntry> load_standard_entries()
{
    std::vector<StandardRecipeEntry> entries;
    const std::string path = resources_dir() + "/filament_mixing/standard_color_recipes.json";
    std::ifstream ifs(path);
    if (!ifs)
        return entries;

    nlohmann::json root = nlohmann::json::parse(ifs, nullptr, false);
    if (root.is_discarded() || !root.contains("entries") || !root["entries"].is_array())
        return entries;

    for (const auto& item : root["entries"]) {
        if (!item.is_object())
            continue;
        StandardRecipeEntry entry;
        entry.mode = parse_mode(item.value("mode", "CMYW"));
        entry.material = item.value("material", "");
        entry.source = item.value("source", "");
        entry.measured_hex = item.value("measured_rgb", "");

        if (item.contains("components") && item["components"].is_array()) {
            for (const auto& comp : item["components"]) {
                if (comp.is_object()) {
                    entry.component_keys.push_back(comp.value("key", ""));
                    entry.component_hexes.push_back(comp.value("rgb", ""));
                }
            }
        }
        if (item.contains("ratios") && item["ratios"].is_array()) {
            for (const auto& ratio : item["ratios"]) {
                if (ratio.is_number_integer())
                    entry.ratios.push_back(ratio.get<int>());
            }
        }
        if (item.contains("measured_lab") && item["measured_lab"].is_array() && item["measured_lab"].size() >= 3) {
            entry.measured_lab = {
                item["measured_lab"][0].get<double>(),
                item["measured_lab"][1].get<double>(),
                item["measured_lab"][2].get<double>()
            };
        } else {
            ColorDecomposeRgb measured_rgb;
            if (!color_decompose_hex_to_rgb(entry.measured_hex, measured_rgb))
                continue;
            entry.measured_lab = rgb_to_lab(measured_rgb);
        }

        if (entry.component_hexes.size() >= 2 && entry.component_hexes.size() == entry.ratios.size() &&
            !entry.measured_hex.empty())
            entries.push_back(std::move(entry));
    }
    return entries;
}

static const std::vector<StandardRecipeEntry>& standard_entries()
{
    static const std::vector<StandardRecipeEntry> entries = load_standard_entries();
    return entries;
}

static void evaluate_candidate(const ColorDecomposeRgb& target,
                               const std::vector<std::string>& hexes,
                               const std::vector<int>& ratios,
                               const std::vector<unsigned int>& indices,
                               ColorDecomposeRecipeMode mode,
                               double& best_score,
                               ColorDecomposeRecipeResult& best)
{
    const std::string mixed = blend_color_multi(hexes, ratios);
    ColorDecomposeRgb mixed_rgb;
    if (!color_decompose_hex_to_rgb(mixed, mixed_rgb))
        return;

    const double score = delta_e76(rgb_to_lab(target), rgb_to_lab(mixed_rgb));
    if (score >= best_score)
        return;

    best_score = score;
    best.valid = true;
    best.mode = mode;
    best.matched_color_hex = mixed;
    best.components.clear();
    for (size_t i = 0; i < hexes.size(); ++i) {
        ColorDecomposeRecipeComponent comp;
        comp.color_hex = hexes[i];
        comp.ratio = ratios[i];
        comp.filament_index = i < indices.size() ? indices[i] : 0;
        best.components.push_back(comp);
    }
}

} // namespace

std::string color_decompose_rgb_to_hex(const ColorDecomposeRgb& rgb)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", rgb.r, rgb.g, rgb.b);
    return std::string(buf);
}

bool color_decompose_hex_to_rgb(const std::string& hex, ColorDecomposeRgb& out)
{
    if (hex.size() < 7 || hex[0] != '#')
        return false;
    unsigned r = 0, g = 0, b = 0;
    if (std::sscanf(hex.c_str(), "#%02x%02x%02x", &r, &g, &b) != 3)
        return false;
    out = {static_cast<unsigned char>(r), static_cast<unsigned char>(g), static_cast<unsigned char>(b)};
    return true;
}

ColorDecomposeRecipeResult recommend_from_physical_filaments(
    const ColorDecomposeRgb& target,
    const std::vector<ColorDecomposePhysicalFilament>& physical_filaments,
    const std::string& preferred_material_type)
{
    std::vector<ColorDecomposePhysicalFilament> candidates;
    for (const auto& filament : physical_filaments) {
        if (filament.is_mixed)
            continue;
        ColorDecomposeRgb ignored;
        if (!color_decompose_hex_to_rgb(filament.color_hex, ignored))
            continue;
        if (preferred_material_type.empty() || material_matches(filament.type, preferred_material_type))
            candidates.push_back(filament);
    }
    if (candidates.size() < 2)
        candidates = physical_filaments;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const auto& filament) {
        if (filament.is_mixed)
            return true;
        ColorDecomposeRgb ignored;
        return !color_decompose_hex_to_rgb(filament.color_hex, ignored);
    }), candidates.end());

    constexpr size_t kMaxCandidates = 8;
    if (candidates.size() > kMaxCandidates) {
        const LabColor target_lab = rgb_to_lab(target);
        std::sort(candidates.begin(), candidates.end(),
            [&target_lab](const ColorDecomposePhysicalFilament& a, const ColorDecomposePhysicalFilament& b) {
                ColorDecomposeRgb rgb_a, rgb_b;
                color_decompose_hex_to_rgb(a.color_hex, rgb_a);
                color_decompose_hex_to_rgb(b.color_hex, rgb_b);
                return delta_e76(target_lab, rgb_to_lab(rgb_a))
                     < delta_e76(target_lab, rgb_to_lab(rgb_b));
            });
        candidates.resize(kMaxCandidates);
    }

    ColorDecomposeRecipeResult best;
    double best_score = std::numeric_limits<double>::max();

    for (size_t i = 0; i < candidates.size(); ++i) {
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            const std::vector<std::string> hexes = {candidates[i].color_hex, candidates[j].color_hex};
            const std::vector<unsigned int> indices = {candidates[i].filament_index, candidates[j].filament_index};
            for (const auto& ratios : ratio_grid(2))
                evaluate_candidate(target, hexes, ratios, indices, ColorDecomposeRecipeMode::MaterialList, best_score, best);

            for (size_t k = j + 1; k < candidates.size(); ++k) {
                const std::vector<std::string> hexes3 = {candidates[i].color_hex, candidates[j].color_hex, candidates[k].color_hex};
                const std::vector<unsigned int> indices3 = {candidates[i].filament_index, candidates[j].filament_index, candidates[k].filament_index};
                for (const auto& ratios : ratio_grid(3))
                    evaluate_candidate(target, hexes3, ratios, indices3, ColorDecomposeRecipeMode::MaterialList, best_score, best);
            }
        }
    }

    return best;
}

ColorDecomposeRecipeResult lookup_standard_recipe(
    const ColorDecomposeRgb& target,
    ColorDecomposeRecipeMode mode,
    const std::string& preferred_material_type)
{
    const LabColor target_lab = rgb_to_lab(target);
    ColorDecomposeRecipeResult best;
    double best_score = std::numeric_limits<double>::max();

    auto consider = [&](bool require_material_match) {
        for (const StandardRecipeEntry& entry : standard_entries()) {
            if (entry.mode != mode)
                continue;
            if (require_material_match && !material_matches(entry.material, preferred_material_type))
                continue;
            if (!require_material_match && !preferred_material_type.empty() && material_matches(entry.material, preferred_material_type))
                continue;

            const double score = delta_e76(target_lab, entry.measured_lab);
            if (score >= best_score)
                continue;

            best_score = score;
            best.valid = true;
            best.mode = mode;
            best.matched_color_hex = entry.measured_hex;
            best.components.clear();
            for (size_t i = 0; i < entry.component_hexes.size(); ++i) {
                ColorDecomposeRecipeComponent comp;
                comp.color_hex = entry.component_hexes[i];
                comp.base_color = i < entry.component_keys.size() ? entry.component_keys[i] : "";
                comp.ratio = entry.ratios[i];
                comp.filament_index = 0;
                best.components.push_back(comp);
            }
        }
    };

    consider(true);
    if (!best.valid)
        consider(false);
    return best;
}

std::string lookup_measured_blend_color(const std::vector<std::string>& component_hexes,
                                       const std::vector<int>& ratios)
{
    if (component_hexes.size() < 2 || component_hexes.size() != ratios.size())
        return {};

    auto normalize_hex = [](const std::string& hex) -> std::string {
        ColorDecomposeRgb rgb;
        if (!color_decompose_hex_to_rgb(hex, rgb))
            return {};
        char buf[8];
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", rgb.r, rgb.g, rgb.b);
        return std::string(buf);
    };

    std::vector<std::string> norm_hexes;
    norm_hexes.reserve(component_hexes.size());
    for (const auto& h : component_hexes) {
        std::string n = normalize_hex(h);
        if (n.empty())
            return {};
        norm_hexes.push_back(std::move(n));
    }

    for (const StandardRecipeEntry& entry : standard_entries()) {
        if (entry.source != "measured" && entry.source != "interpolated")
            continue;
        if (entry.component_hexes.size() != norm_hexes.size())
            continue;
        if (entry.ratios != ratios)
            continue;

        bool match = true;
        for (size_t i = 0; i < norm_hexes.size(); ++i) {
            if (normalize_hex(entry.component_hexes[i]) != norm_hexes[i]) {
                match = false;
                break;
            }
        }
        if (match)
            return entry.measured_hex;
    }
    return {};
}

} // namespace Slic3r
