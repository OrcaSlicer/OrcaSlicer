// MixedFilamentColorMatchDialog.cpp
// Extracted verbatim from FullSpectrum Plater.cpp ranges:
//   2416-2638  (parse/blend/hex helpers)
//   2639-2868  (candidate builders, expand weights, summarize, presets)
//   2872-3086  (color_delta_e00, build_color_match_sequence, blend_sequence,
//               build_best_color_match_recipe)
//   3782-4288  (MixedFilamentColorMatchDialog)
//   5805-5819  (compute_color_match_recipe_display_color)
//   5622-5681  (build_mixed_filament_display_context)
//   6937-6950  (prompt_best_color_match_recipe)

#include "MixedFilamentColorMatchDialog.hpp"
#include "MixedFilamentColorMapPanel.hpp"

#include "libslic3r/filament_mixer.h"
#include "libslic3r/MixedFilament.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "../Utils/ColorSpaceConvert.hpp"

#include <wx/bitmap.h>
#include <wx/bmpbuttn.h>
#include <wx/clrpicker.h>
#include <wx/dcmemory.h>
#include <wx/event.h>
#include <wx/gauge.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <wx/wrapsizer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace Slic3r { namespace GUI {

// ===========================================================================
// Anonymous-namespace helpers — all verbatim from FullSpectrum Plater.cpp.
// Only the two free functions (color_delta_e00 and
// compute_color_match_recipe_display_color) are exposed in the header.
// ===========================================================================
namespace {

// ---------------------------------------------------------------------------
// Color parsing / blending — Plater.cpp:2416-2485
// ---------------------------------------------------------------------------

wxColour parse_mixed_color(const std::string &value)
{
    wxColour color(value);
    if (!color.IsOk())
        color = wxColour("#26A69A");
    return color;
}

wxColour blend_pair_filament_mixer(const wxColour &left, const wxColour &right, float t)
{
    const wxColour safe_left  = left.IsOk()  ? left  : wxColour("#26A69A");
    const wxColour safe_right = right.IsOk() ? right : wxColour("#26A69A");

    unsigned char out_r = static_cast<unsigned char>(safe_left.Red());
    unsigned char out_g = static_cast<unsigned char>(safe_left.Green());
    unsigned char out_b = static_cast<unsigned char>(safe_left.Blue());
    ::Slic3r::filament_mixer_lerp(static_cast<unsigned char>(safe_left.Red()),
                                  static_cast<unsigned char>(safe_left.Green()),
                                  static_cast<unsigned char>(safe_left.Blue()),
                                  static_cast<unsigned char>(safe_right.Red()),
                                  static_cast<unsigned char>(safe_right.Green()),
                                  static_cast<unsigned char>(safe_right.Blue()),
                                  std::clamp(t, 0.f, 1.f),
                                  &out_r, &out_g, &out_b);
    return wxColour(out_r, out_g, out_b);
}

wxColour blend_multi_filament_mixer(const std::vector<wxColour> &colors, const std::vector<double> &weights)
{
    if (colors.empty() || weights.empty())
        return wxColour("#26A69A");

    unsigned char out_r = 0;
    unsigned char out_g = 0;
    unsigned char out_b = 0;
    double accumulated_weight = 0.0;
    bool has_color = false;

    for (size_t i = 0; i < colors.size() && i < weights.size(); ++i) {
        const double weight = std::max(0.0, weights[i]);
        if (weight <= 0.0)
            continue;

        const wxColour safe = colors[i].IsOk() ? colors[i] : wxColour("#26A69A");
        const unsigned char r = static_cast<unsigned char>(safe.Red());
        const unsigned char g = static_cast<unsigned char>(safe.Green());
        const unsigned char b = static_cast<unsigned char>(safe.Blue());

        if (!has_color) {
            out_r = r;
            out_g = g;
            out_b = b;
            accumulated_weight = weight;
            has_color = true;
            continue;
        }

        const double new_total = accumulated_weight + weight;
        if (new_total <= 0.0)
            continue;
        const float t = float(weight / new_total);
        ::Slic3r::filament_mixer_lerp(out_r, out_g, out_b, r, g, b, t, &out_r, &out_g, &out_b);
        accumulated_weight = new_total;
    }

    if (!has_color)
        return wxColour("#26A69A");

    return wxColour(out_r, out_g, out_b);
}

// ---------------------------------------------------------------------------
// Hex string helpers — Plater.cpp:2487-2516
// ---------------------------------------------------------------------------

wxString normalize_color_match_hex(const wxString &value)
{
    wxString normalized = value;
    normalized.Trim(true);
    normalized.Trim(false);
    normalized.MakeUpper();
    if (!normalized.empty() && normalized[0] != '#')
        normalized.Prepend("#");
    return normalized;
}

bool try_parse_color_match_hex(const wxString &value, wxColour &color_out)
{
    const wxString normalized = normalize_color_match_hex(value);
    if (normalized.length() != 7)
        return false;

    for (size_t idx = 1; idx < normalized.length(); ++idx) {
        const unsigned char ch = static_cast<unsigned char>(normalized[idx]);
        if (!std::isxdigit(ch))
            return false;
    }

    wxColour parsed(normalized);
    if (!parsed.IsOk())
        return false;

    color_out = parsed;
    return true;
}

// ---------------------------------------------------------------------------
// Gradient id / weight decoders — Plater.cpp:2518-2600
// ---------------------------------------------------------------------------

std::vector<unsigned int> decode_color_match_gradient_ids(const std::string &value)
{
    std::vector<unsigned int> ids;
    bool seen[10] = { false };
    for (const char ch : value) {
        if (ch < '1' || ch > '9')
            continue;
        const unsigned int id = unsigned(ch - '0');
        if (seen[id])
            continue;
        seen[id] = true;
        ids.emplace_back(id);
    }
    return ids;
}

std::vector<int> decode_color_match_gradient_weights(const std::string &value, size_t expected_components)
{
    std::vector<int> weights;
    if (value.empty() || expected_components == 0)
        return weights;

    std::string token;
    for (const char ch : value) {
        if (ch >= '0' && ch <= '9') {
            token.push_back(ch);
            continue;
        }
        if (!token.empty()) {
            weights.emplace_back(std::max(0, std::atoi(token.c_str())));
            token.clear();
        }
    }
    if (!token.empty())
        weights.emplace_back(std::max(0, std::atoi(token.c_str())));
    if (weights.size() != expected_components)
        weights.clear();
    return weights;
}

std::vector<int> normalize_color_match_weights(const std::vector<int> &weights, size_t count)
{
    std::vector<int> out = weights;
    if (out.size() != count)
        out.assign(count, count > 0 ? int(100 / int(count)) : 0);

    int sum = 0;
    for (int &value : out) {
        value = std::max(0, value);
        sum += value;
    }
    if (sum <= 0 && count > 0) {
        out.assign(count, 0);
        out[0] = 100;
        return out;
    }

    std::vector<double> remainders(count, 0.0);
    int assigned = 0;
    for (size_t idx = 0; idx < count; ++idx) {
        const double exact = 100.0 * double(out[idx]) / double(sum);
        out[idx]           = int(std::floor(exact));
        remainders[idx]    = exact - double(out[idx]);
        assigned          += out[idx];
    }

    int missing = std::max(0, 100 - assigned);
    while (missing > 0) {
        size_t best_idx       = 0;
        double best_remainder = -1.0;
        for (size_t idx = 0; idx < remainders.size(); ++idx) {
            if (remainders[idx] > best_remainder) {
                best_remainder = remainders[idx];
                best_idx       = idx;
            }
        }
        ++out[best_idx];
        remainders[best_idx] = 0.0;
        --missing;
    }

    return out;
}

// ---------------------------------------------------------------------------
// Weight range checker — Plater.cpp:2605-2620
// ---------------------------------------------------------------------------

bool color_match_weights_within_range(const std::vector<int> &weights, int min_component_percent)
{
    if (min_component_percent <= 0)
        return true;

    const int min_allowed = std::clamp(min_component_percent, 0, 50);
    int active_components = 0;
    for (const int weight : weights) {
        if (weight <= 0)
            continue;
        ++active_components;
        if (weight < min_allowed)
            return false;
    }
    return active_components >= 2;
}

// ---------------------------------------------------------------------------
// Sequence builder — Plater.cpp:2881-2934
// ---------------------------------------------------------------------------

std::vector<unsigned int> build_color_match_sequence(const std::vector<unsigned int> &ids,
                                                     const std::vector<int>          &weights)
{
    if (ids.empty() || ids.size() != weights.size())
        return {};

    constexpr int k_max_cycle = 48;

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          counts;
    filtered_ids.reserve(ids.size());
    counts.reserve(weights.size());
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        const int weight = std::max(0, weights[idx]);
        if (weight <= 0)
            continue;
        filtered_ids.emplace_back(ids[idx]);
        counts.emplace_back(std::max(1, int(std::round((double(weight) / 100.0) * k_max_cycle))));
    }

    if (filtered_ids.empty())
        return {};

    int cycle = std::accumulate(counts.begin(), counts.end(), 0);
    while (cycle > k_max_cycle) {
        auto it = std::max_element(counts.begin(), counts.end());
        if (it == counts.end() || *it <= 1)
            break;
        --(*it);
        --cycle;
    }

    if (cycle <= 0)
        return {};

    std::vector<unsigned int> sequence;
    sequence.reserve(size_t(cycle));
    std::vector<int> emitted(counts.size(), 0);
    for (int pos = 0; pos < cycle; ++pos) {
        size_t best_idx   = 0;
        double best_score = -1e9;
        for (size_t idx = 0; idx < counts.size(); ++idx) {
            const double target = double((pos + 1) * counts[idx]) / double(std::max(1, cycle));
            const double score  = target - double(emitted[idx]);
            if (score > best_score) {
                best_score = score;
                best_idx   = idx;
            }
        }
        ++emitted[best_idx];
        sequence.emplace_back(filtered_ids[best_idx]);
    }

    return sequence;
}

wxColour blend_sequence_filament_mixer(const std::vector<wxColour> &palette,
                                       const std::vector<unsigned int> &sequence)
{
    if (palette.empty() || sequence.empty())
        return wxColour("#26A69A");

    std::vector<int> counts(palette.size() + 1, 0);
    for (const unsigned int filament_id : sequence) {
        if (filament_id == 0 || filament_id > palette.size())
            continue;
        ++counts[filament_id];
    }

    std::vector<wxColour> colors;
    std::vector<double>   weights;
    colors.reserve(palette.size());
    weights.reserve(palette.size());
    for (size_t filament_id = 1; filament_id <= palette.size(); ++filament_id) {
        if (counts[filament_id] <= 0)
            continue;
        colors.emplace_back(palette[filament_id - 1]);
        weights.emplace_back(double(counts[filament_id]));
    }

    return blend_multi_filament_mixer(colors, weights);
}

// ---------------------------------------------------------------------------
// Candidate builders — Plater.cpp:2639-2724
// ---------------------------------------------------------------------------

MixedColorMatchRecipeResult build_pair_color_match_candidate(const std::vector<wxColour> &palette,
                                                             unsigned int                  component_a,
                                                             unsigned int                  component_b,
                                                             int                           mix_b_percent,
                                                             int                           min_component_percent = 0)
{
    MixedColorMatchRecipeResult candidate;
    if (component_a == 0 || component_b == 0 || component_a == component_b)
        return candidate;
    if (component_a > palette.size() || component_b > palette.size())
        return candidate;
    if (!color_match_weights_within_range({ 100 - std::clamp(mix_b_percent, 0, 100), std::clamp(mix_b_percent, 0, 100) }, min_component_percent))
        return candidate;

    candidate.valid         = true;
    candidate.component_a   = component_a;
    candidate.component_b   = component_b;
    candidate.mix_b_percent = std::clamp(mix_b_percent, 0, 100);
    candidate.preview_color = blend_pair_filament_mixer(palette[component_a - 1], palette[component_b - 1],
                                                        float(candidate.mix_b_percent) / 100.f);
    return candidate;
}

MixedColorMatchRecipeResult build_multi_color_match_candidate(const std::vector<wxColour>     &palette,
                                                              const std::vector<unsigned int> &ids,
                                                              const std::vector<int>          &weights,
                                                              int                              min_component_percent = 0)
{
    MixedColorMatchRecipeResult candidate;
    if (ids.size() < 3 || ids.size() != weights.size())
        return candidate;
    if (!color_match_weights_within_range(weights, min_component_percent))
        return candidate;

    std::vector<std::pair<int, unsigned int>> weighted_ids;
    weighted_ids.reserve(ids.size());
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        if (ids[idx] == 0 || ids[idx] > palette.size() || ids[idx] > 9)
            return candidate;
        if (weights[idx] <= 0)
            continue;
        weighted_ids.emplace_back(weights[idx], ids[idx]);
    }
    if (weighted_ids.size() < 3)
        return candidate;

    std::sort(weighted_ids.begin(), weighted_ids.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.first != rhs.first)
            return lhs.first > rhs.first;
        return lhs.second < rhs.second;
    });

    std::vector<unsigned int> ordered_ids;
    std::vector<int>          ordered_weights;
    ordered_ids.reserve(weighted_ids.size());
    ordered_weights.reserve(weighted_ids.size());
    for (const auto &[weight, filament_id] : weighted_ids) {
        ordered_ids.emplace_back(filament_id);
        ordered_weights.emplace_back(weight);
    }

    const std::vector<unsigned int> sequence = build_color_match_sequence(ordered_ids, ordered_weights);
    if (sequence.empty())
        return candidate;

    candidate.valid       = true;
    candidate.component_a = ordered_ids[0];
    candidate.component_b = ordered_ids[1];
    const int pair_weight_total = ordered_weights[0] + ordered_weights[1];
    candidate.mix_b_percent = pair_weight_total > 0 ?
        std::clamp(int(std::lround(100.0 * double(ordered_weights[1]) / double(pair_weight_total))), 0, 100) :
        50;
    for (const unsigned int filament_id : ordered_ids)
        candidate.gradient_component_ids.push_back(char('0' + filament_id));
    {
        std::ostringstream weights_ss;
        for (size_t weight_idx = 0; weight_idx < ordered_weights.size(); ++weight_idx) {
            if (weight_idx > 0)
                weights_ss << '/';
            weights_ss << ordered_weights[weight_idx];
        }
        candidate.gradient_component_weights = weights_ss.str();
    }
    candidate.preview_color = blend_sequence_filament_mixer(palette, sequence);
    return candidate;
}

// ---------------------------------------------------------------------------
// expand / summarize / presets — Plater.cpp:2726-2870
// ---------------------------------------------------------------------------

std::vector<int> expand_color_match_recipe_weights(const MixedColorMatchRecipeResult &recipe, size_t num_physical)
{
    std::vector<int> weights(num_physical, 0);
    if (!recipe.valid || num_physical == 0)
        return weights;

    if (!recipe.gradient_component_ids.empty()) {
        const std::vector<unsigned int> ids = decode_color_match_gradient_ids(recipe.gradient_component_ids);
        const std::vector<int> raw_weights =
            normalize_color_match_weights(decode_color_match_gradient_weights(recipe.gradient_component_weights, ids.size()), ids.size());
        if (ids.size() != raw_weights.size())
            return weights;
        for (size_t idx = 0; idx < ids.size(); ++idx) {
            if (ids[idx] >= 1 && ids[idx] <= num_physical)
                weights[ids[idx] - 1] = raw_weights[idx];
        }
        return weights;
    }

    if (recipe.component_a >= 1 && recipe.component_a <= num_physical)
        weights[recipe.component_a - 1] = std::max(0, 100 - std::clamp(recipe.mix_b_percent, 0, 100));
    if (recipe.component_b >= 1 && recipe.component_b <= num_physical)
        weights[recipe.component_b - 1] = std::max(0, std::clamp(recipe.mix_b_percent, 0, 100));
    return weights;
}

std::string summarize_color_match_recipe(const MixedColorMatchRecipeResult &recipe)
{
    if (!recipe.valid)
        return {};

    std::vector<unsigned int> ids;
    std::vector<int>          weights;
    if (!recipe.gradient_component_ids.empty()) {
        ids     = decode_color_match_gradient_ids(recipe.gradient_component_ids);
        weights = normalize_color_match_weights(
            decode_color_match_gradient_weights(recipe.gradient_component_weights, ids.size()), ids.size());
    } else {
        ids     = { recipe.component_a, recipe.component_b };
        weights = { std::max(0, 100 - std::clamp(recipe.mix_b_percent, 0, 100)),
                    std::max(0, std::clamp(recipe.mix_b_percent, 0, 100)) };
    }
    if (ids.empty() || ids.size() != weights.size())
        return {};

    std::ostringstream out;
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        if (idx > 0)
            out << '/';
        out << 'F' << ids[idx];
    }
    out << ' ';
    for (size_t idx = 0; idx < weights.size(); ++idx) {
        if (idx > 0)
            out << '/';
        out << weights[idx] << '%';
    }
    return out.str();
}

wxBitmap make_color_match_swatch_bitmap(const wxColour &color, const wxSize &size)
{
    wxBitmap bmp(size.GetWidth(), size.GetHeight());
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(wxColour(255, 255, 255)));
    dc.Clear();
    dc.SetPen(wxPen(wxColour(120, 120, 120), 1));
    dc.SetBrush(wxBrush(color.IsOk() ? color : wxColour("#26A69A")));
    dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

std::vector<MixedColorMatchRecipeResult> build_color_match_presets(const std::vector<std::string> &physical_colors,
                                                                   int                             min_component_percent = 0)
{
    std::vector<MixedColorMatchRecipeResult> presets;
    if (physical_colors.size() < 2)
        return presets;

    std::vector<wxColour> palette;
    palette.reserve(physical_colors.size());
    for (const std::string &hex : physical_colors)
        palette.emplace_back(parse_mixed_color(hex));

    constexpr size_t k_max_presets = 48;
    std::unordered_set<std::string> seen_colors;
    auto add_candidate = [&presets, &seen_colors](MixedColorMatchRecipeResult candidate) {
        if (!candidate.valid)
            return;
        const std::string color_key = normalize_color_match_hex(candidate.preview_color.GetAsString(wxC2S_HTML_SYNTAX)).ToStdString();
        if (color_key.empty() || !seen_colors.insert(color_key).second)
            return;
        presets.emplace_back(std::move(candidate));
    };

    constexpr int pair_ratios[] = { 25, 50, 75 };
    for (size_t left_idx = 0; left_idx < palette.size() && presets.size() < k_max_presets; ++left_idx) {
        for (size_t right_idx = left_idx + 1; right_idx < palette.size() && presets.size() < k_max_presets; ++right_idx) {
            for (const int mix_b_percent : pair_ratios) {
                add_candidate(build_pair_color_match_candidate(palette, unsigned(left_idx + 1), unsigned(right_idx + 1),
                                                               mix_b_percent, min_component_percent));
                if (presets.size() >= k_max_presets)
                    break;
            }
        }
    }

    const size_t triple_limit = std::min<size_t>(palette.size(), 6);
    const std::vector<int> equal_triple_weights = normalize_color_match_weights({ 1, 1, 1 }, 3);
    for (size_t first_idx = 0; first_idx + 2 < triple_limit && presets.size() < k_max_presets; ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 1 < triple_limit && presets.size() < k_max_presets; ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx < triple_limit && presets.size() < k_max_presets; ++third_idx) {
                const std::vector<unsigned int> ids = {
                    unsigned(first_idx + 1),
                    unsigned(second_idx + 1),
                    unsigned(third_idx + 1)
                };
                add_candidate(build_multi_color_match_candidate(palette, ids, equal_triple_weights, min_component_percent));
                for (size_t dominant_idx = 0; dominant_idx < ids.size() && presets.size() < k_max_presets; ++dominant_idx) {
                    std::vector<int> dominant_weights(ids.size(), 25);
                    dominant_weights[dominant_idx] = 50;
                    add_candidate(build_multi_color_match_candidate(palette, ids, dominant_weights, min_component_percent));
                }
            }
        }
    }

    const size_t quad_limit = std::min<size_t>(palette.size(), 5);
    for (size_t first_idx = 0; first_idx + 3 < quad_limit && presets.size() < k_max_presets; ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 2 < quad_limit && presets.size() < k_max_presets; ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx + 1 < quad_limit && presets.size() < k_max_presets; ++third_idx) {
                for (size_t fourth_idx = third_idx + 1; fourth_idx < quad_limit && presets.size() < k_max_presets; ++fourth_idx) {
                    add_candidate(build_multi_color_match_candidate(
                        palette,
                        { unsigned(first_idx + 1), unsigned(second_idx + 1), unsigned(third_idx + 1), unsigned(fourth_idx + 1) },
                        { 25, 25, 25, 25 },
                        min_component_percent));
                }
            }
        }
    }

    return presets;
}

} // end of anonymous namespace — build_mixed_filament_display_context must be at namespace scope

// ---------------------------------------------------------------------------
// build_mixed_filament_display_context — Plater.cpp:5622-5681
// (Exposed: declared in MixedFilamentColorMatchDialog.hpp)
// ---------------------------------------------------------------------------

MixedFilamentDisplayContext build_mixed_filament_display_context(const std::vector<std::string> &physical_colors)
{
    MixedFilamentDisplayContext context;
    context.num_physical = physical_colors.size();
    context.physical_colors = physical_colors;
    context.nozzle_diameters.assign(context.num_physical, 0.4);

    auto *preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return context;

    DynamicPrintConfig *print_cfg = &preset_bundle->prints.get_edited_preset().config;
    if (const ConfigOptionFloats *opt = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")) {
        const size_t opt_count = opt->values.size();
        if (opt_count > 0) {
            for (size_t i = 0; i < context.num_physical; ++i)
                context.nozzle_diameters[i] = std::max(0.05, opt->get_at(unsigned(std::min(i, opt_count - 1))));
        }
    }

    auto get_mixed_bool = [preset_bundle, print_cfg](const std::string &key, bool fallback) {
        if (const ConfigOptionBool *opt = preset_bundle->project_config.option<ConfigOptionBool>(key))
            return opt->value;
        if (const ConfigOptionInt *opt = preset_bundle->project_config.option<ConfigOptionInt>(key))
            return opt->value != 0;
        if (print_cfg != nullptr) {
            if (const ConfigOptionBool *opt = print_cfg->option<ConfigOptionBool>(key))
                return opt->value;
            if (const ConfigOptionInt *opt = print_cfg->option<ConfigOptionInt>(key))
                return opt->value != 0;
        }
        return fallback;
    };
    auto get_mixed_float = [preset_bundle, print_cfg](const std::string &key, float fallback) {
        if (preset_bundle->project_config.has(key))
            return float(preset_bundle->project_config.opt_float(key));
        if (print_cfg != nullptr && print_cfg->has(key))
            return float(print_cfg->opt_float(key));
        return fallback;
    };

    context.preview_settings.mixed_lower_bound = std::max(0.01, double(get_mixed_float("mixed_filament_height_lower_bound", 0.04f)));
    context.preview_settings.mixed_upper_bound = std::max(context.preview_settings.mixed_lower_bound,
                                                          double(get_mixed_float("mixed_filament_height_upper_bound", 0.16f)));
    context.preview_settings.preferred_a_height = std::max(0.0, double(get_mixed_float("mixed_color_layer_height_a", 0.f)));
    context.preview_settings.preferred_b_height = std::max(0.0, double(get_mixed_float("mixed_color_layer_height_b", 0.f)));
    context.preview_settings.nominal_layer_height = 0.2;
    if (print_cfg != nullptr && print_cfg->has("layer_height"))
        context.preview_settings.nominal_layer_height = std::max(0.01, print_cfg->opt_float("layer_height"));
    if (print_cfg != nullptr && print_cfg->has("wall_loops"))
        context.preview_settings.wall_loops = std::max<size_t>(1, size_t(std::max(1, print_cfg->opt_int("wall_loops"))));
    context.preview_settings.local_z_mode = get_mixed_bool("dithering_local_z_mode", false);
    context.preview_settings.local_z_direct_multicolor =
        get_mixed_bool("dithering_local_z_direct_multicolor", false) &&
        context.preview_settings.preferred_a_height <= EPSILON &&
        context.preview_settings.preferred_b_height <= EPSILON;
    context.component_bias_enabled = get_mixed_bool("mixed_filament_component_bias_enabled", false);

    return context;
}

namespace { // reopen anonymous namespace

// ---------------------------------------------------------------------------
// build_best_color_match_recipe — Plater.cpp:2962-3086
// ---------------------------------------------------------------------------

MixedColorMatchRecipeResult build_best_color_match_recipe(const std::vector<std::string> &physical_colors,
                                                          const wxColour                 &target_color,
                                                          int                             min_component_percent = 0)
{
    MixedColorMatchRecipeResult best;
    if (!target_color.IsOk() || physical_colors.size() < 2)
        return best;

    std::vector<wxColour> palette;
    palette.reserve(physical_colors.size());
    for (const std::string &hex : physical_colors)
        palette.emplace_back(parse_mixed_color(hex));

    auto consider_candidate = [&best, &target_color](MixedColorMatchRecipeResult candidate) {
        if (!candidate.valid)
            return;
        candidate.delta_e = color_delta_e00(target_color, candidate.preview_color);
        if (!best.valid || candidate.delta_e + 1e-6 < best.delta_e)
            best = std::move(candidate);
    };

    const int loop_min_weight      = std::max(1, std::clamp(min_component_percent, 0, 50));
    const int loop_max_pair_weight = 100 - loop_min_weight;

    for (size_t left_idx = 0; left_idx < palette.size(); ++left_idx) {
        for (size_t right_idx = left_idx + 1; right_idx < palette.size(); ++right_idx) {
            for (int mix_b_percent = loop_min_weight; mix_b_percent <= loop_max_pair_weight; ++mix_b_percent)
                consider_candidate(build_pair_color_match_candidate(palette, unsigned(left_idx + 1), unsigned(right_idx + 1),
                                                                    mix_b_percent, min_component_percent));
        }
    }

    std::vector<std::pair<double, unsigned int>> ranked_ids;
    ranked_ids.reserve(palette.size());
    for (size_t idx = 0; idx < palette.size(); ++idx)
        ranked_ids.emplace_back(color_delta_e00(target_color, palette[idx]), unsigned(idx + 1));
    std::sort(ranked_ids.begin(), ranked_ids.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.first != rhs.first)
            return lhs.first < rhs.first;
        return lhs.second < rhs.second;
    });

    std::vector<unsigned int> candidate_pool;
    candidate_pool.reserve(std::min<size_t>(palette.size(), 12));
    auto push_unique_id = [&candidate_pool](unsigned int filament_id) {
        if (filament_id == 0 || filament_id > 9)
            return;
        if (std::find(candidate_pool.begin(), candidate_pool.end(), filament_id) == candidate_pool.end())
            candidate_pool.emplace_back(filament_id);
    };

    const size_t general_pool_limit = std::min<size_t>(ranked_ids.size(), 8);
    for (size_t idx = 0; idx < general_pool_limit; ++idx)
        push_unique_id(ranked_ids[idx].second);

    size_t direct_token_count = 0;
    for (const auto &[distance, filament_id] : ranked_ids) {
        (void) distance;
        if (filament_id < 3 || filament_id > 9)
            continue;
        push_unique_id(filament_id);
        if (++direct_token_count >= 4)
            break;
    }

    if (candidate_pool.size() < 3)
        return best;

    std::vector<unsigned int> triple_pool = candidate_pool;
    std::sort(triple_pool.begin(), triple_pool.end());
    for (size_t first_idx = 0; first_idx + 2 < triple_pool.size(); ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 1 < triple_pool.size(); ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx < triple_pool.size(); ++third_idx) {
                const std::vector<unsigned int> ids = {
                    triple_pool[first_idx],
                    triple_pool[second_idx],
                    triple_pool[third_idx]
                };
                if (std::any_of(ids.begin(), ids.end(), [](unsigned int filament_id) { return filament_id == 0 || filament_id > 9; }))
                    continue;

                for (int weight_a = loop_min_weight; weight_a <= 100 - 2 * loop_min_weight; ++weight_a) {
                    for (int weight_b = loop_min_weight; weight_a + weight_b <= 100 - loop_min_weight; ++weight_b) {
                        const int weight_c = 100 - weight_a - weight_b;
                        consider_candidate(build_multi_color_match_candidate(palette, ids, { weight_a, weight_b, weight_c },
                                                                            min_component_percent));
                    }
                }
            }
        }
    }

    if (candidate_pool.size() < 4)
        return best;

    std::vector<unsigned int> quad_pool(candidate_pool.begin(),
                                        candidate_pool.begin() + std::min<size_t>(candidate_pool.size(), 6));
    std::sort(quad_pool.begin(), quad_pool.end());
    for (size_t first_idx = 0; first_idx + 3 < quad_pool.size(); ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 2 < quad_pool.size(); ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx + 1 < quad_pool.size(); ++third_idx) {
                for (size_t fourth_idx = third_idx + 1; fourth_idx < quad_pool.size(); ++fourth_idx) {
                    const std::vector<unsigned int> ids = {
                        quad_pool[first_idx],
                        quad_pool[second_idx],
                        quad_pool[third_idx],
                        quad_pool[fourth_idx]
                    };

                    for (int weight_a = loop_min_weight; weight_a <= 100 - 3 * loop_min_weight; ++weight_a) {
                        for (int weight_b = loop_min_weight; weight_a + weight_b <= 100 - 2 * loop_min_weight; ++weight_b) {
                            for (int weight_c = loop_min_weight; weight_a + weight_b + weight_c <= 100 - loop_min_weight; ++weight_c) {
                                const int weight_d = 100 - weight_a - weight_b - weight_c;
                                consider_candidate(build_multi_color_match_candidate(
                                    palette, ids, { weight_a, weight_b, weight_c, weight_d }, min_component_percent));
                            }
                        }
                    }
                }
            }
        }
    }

    return best;
}

} // anonymous namespace

// ===========================================================================
// Exposed free functions
// ===========================================================================

double color_delta_e00(const wxColour &lhs, const wxColour &rhs)
{
    float lhs_l = 0.f, lhs_a = 0.f, lhs_b = 0.f;
    float rhs_l = 0.f, rhs_a = 0.f, rhs_b = 0.f;
    RGB2Lab(float(lhs.Red()), float(lhs.Green()), float(lhs.Blue()), &lhs_l, &lhs_a, &lhs_b);
    RGB2Lab(float(rhs.Red()), float(rhs.Green()), float(rhs.Blue()), &rhs_l, &rhs_a, &rhs_b);
    return double(DeltaE00(lhs_l, lhs_a, lhs_b, rhs_l, rhs_a, rhs_b));
}

// Plater.cpp:5805-5819
wxColour compute_color_match_recipe_display_color(const MixedColorMatchRecipeResult &recipe,
                                                  const MixedFilamentDisplayContext &context)
{
    if (!recipe.valid)
        return recipe.preview_color.IsOk() ? recipe.preview_color : wxColour("#26A69A");

    MixedFilament entry;
    entry.component_a                = recipe.component_a;
    entry.component_b                = recipe.component_b;
    entry.mix_b_percent              = recipe.mix_b_percent;
    entry.manual_pattern             = recipe.manual_pattern;
    entry.gradient_component_ids     = recipe.gradient_component_ids;
    entry.gradient_component_weights = recipe.gradient_component_weights;
    entry.distribution_mode = recipe.gradient_component_ids.empty() ?
        int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);

    // parse_mixed_color is in anon namespace — use the equivalent inline logic here.
    const std::string hex = compute_mixed_filament_display_color(entry, context);
    wxColour color(hex);
    if (!color.IsOk())
        color = wxColour("#26A69A");
    return color;
}

// ===========================================================================
// MixedFilamentColorMatchDialog — verbatim from Plater.cpp:3782-4288
// ===========================================================================

MixedFilamentColorMatchDialog::MixedFilamentColorMatchDialog(wxWindow *parent,
                                                             const std::vector<std::string> &physical_colors,
                                                             const wxColour &initial_color)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Add Color"),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_physical_colors(physical_colors)
{
    m_recipe_timer.SetOwner(this);
    m_loading_timer.SetOwner(this);
    m_display_context = build_mixed_filament_display_context(m_physical_colors);

    m_palette.reserve(m_physical_colors.size());
    for (const std::string &hex : m_physical_colors)
        m_palette.emplace_back(parse_mixed_color(hex));

    const wxColour safe_initial = initial_color.IsOk() ? initial_color :
        (m_palette.size() >= 2 ? blend_pair_filament_mixer(m_palette[0], m_palette[1], 0.5f) : wxColour("#26A69A"));
    std::vector<int> initial_weights(m_palette.size(), 0);
    if (!initial_weights.empty())
        initial_weights[0] = 100;
    if (initial_weights.size() >= 2) {
        initial_weights[0] = 50;
        initial_weights[1] = 50;
    }

    std::vector<unsigned int> filament_ids;
    filament_ids.reserve(m_palette.size());
    for (size_t idx = 0; idx < m_palette.size(); ++idx)
        filament_ids.emplace_back(unsigned(idx + 1));

    SetMinSize(wxSize(FromDIP(430), FromDIP(520)));

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *description = new wxStaticText(
        this, wxID_ANY,
        _L("Pick from the current filament gamut. The dialog previews the closest 2-color, 3-color, or 4-color FilamentMixer recipe before it is added."));
    description->Wrap(FromDIP(390));
    root->Add(description, 0, wxEXPAND | wxALL, FromDIP(12));

    m_color_map = new MixedFilamentColorMapPanel(this, filament_ids, m_palette, initial_weights,
                                                 wxSize(FromDIP(260), FromDIP(260)));
    root->Add(m_color_map, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    auto *hex_row = new wxBoxSizer(wxHORIZONTAL);
    hex_row->Add(new wxStaticText(this, wxID_ANY, _L("Hex")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_hex_input = new wxTextCtrl(this, wxID_ANY, normalize_color_match_hex(safe_initial.GetAsString(wxC2S_HTML_SYNTAX)),
                                 wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_hex_input->SetToolTip(_L("Enter a hex color like #00FF88. The picker will snap to the closest supported FilamentMixer color."));
    hex_row->Add(m_hex_input, 1, wxALIGN_CENTER_VERTICAL);
    hex_row->AddSpacer(FromDIP(8));
    m_classic_picker = new wxColourPickerCtrl(this, wxID_ANY, safe_initial);
    m_classic_picker->SetToolTip(_L("Classic color picker. The result will snap to the closest supported FilamentMixer color."));
    hex_row->Add(m_classic_picker, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(hex_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto *range_row = new wxBoxSizer(wxHORIZONTAL);
    range_row->Add(new wxStaticText(this, wxID_ANY, _L("Range")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_range_slider = new wxSlider(this, wxID_ANY, m_min_component_percent, 0, 50);
    m_range_slider->SetToolTip(_L("Minimum percent for each participating color. Higher values block highly skewed mixes."));
    range_row->Add(m_range_slider, 1, wxALIGN_CENTER_VERTICAL);
    range_row->AddSpacer(FromDIP(8));
    m_range_value = new wxStaticText(this, wxID_ANY, wxEmptyString);
    range_row->Add(m_range_value, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(range_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto *summary_grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(8));
    summary_grid->AddGrowableCol(1, 1);

    summary_grid->Add(new wxStaticText(this, wxID_ANY, _L("Requested")), 0, wxALIGN_CENTER_VERTICAL);
    auto *selected_row = new wxBoxSizer(wxHORIZONTAL);
    m_selected_preview = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(72), FromDIP(24)), wxBORDER_SIMPLE);
    selected_row->Add(m_selected_preview, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_selected_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    selected_row->Add(m_selected_label, 1, wxALIGN_CENTER_VERTICAL);
    summary_grid->Add(selected_row, 1, wxEXPAND);

    summary_grid->Add(new wxStaticText(this, wxID_ANY, _L("Creates")), 0, wxALIGN_CENTER_VERTICAL);
    auto *recipe_row = new wxBoxSizer(wxHORIZONTAL);
    m_recipe_preview = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(72), FromDIP(24)), wxBORDER_SIMPLE);
    recipe_row->Add(m_recipe_preview, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_recipe_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_recipe_label->Wrap(FromDIP(280));
    recipe_row->Add(m_recipe_label, 1, wxALIGN_CENTER_VERTICAL);
    summary_grid->Add(recipe_row, 1, wxEXPAND);

    root->Add(summary_grid, 0, wxEXPAND | wxALL, FromDIP(12));

    m_delta_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    root->Add(m_delta_label, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    m_presets_label = new wxStaticText(this, wxID_ANY, _L("Exact preset mixes"));
    root->Add(m_presets_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    m_presets_host = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(96)),
                                          wxVSCROLL | wxBORDER_SIMPLE);
    m_presets_host->SetScrollRate(FromDIP(6), FromDIP(6));
    m_presets_sizer = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
    m_presets_host->SetSizer(m_presets_sizer);
    root->Add(m_presets_host, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_error_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_error_label->SetForegroundColour(wxColour(196, 67, 63));
    root->Add(m_error_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    if (wxSizer *button_sizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL))
        root->Add(button_sizer, 0, wxEXPAND | wxALL, FromDIP(12));

    m_loading_panel = new wxPanel(this, wxID_ANY);
    m_loading_panel->SetMinSize(wxSize(-1, FromDIP(24)));
    auto *loading_row = new wxBoxSizer(wxHORIZONTAL);
    m_loading_label = new wxStaticText(m_loading_panel, wxID_ANY, " ");
    loading_row->Add(m_loading_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_loading_gauge = new wxGauge(m_loading_panel, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(150), FromDIP(8)),
                                  wxGA_HORIZONTAL | wxGA_SMOOTH);
    m_loading_gauge->SetValue(0);
    m_loading_gauge->Enable(false);
    loading_row->Add(m_loading_gauge, 0, wxALIGN_CENTER_VERTICAL);
    m_loading_panel->SetSizer(loading_row);
    root->Add(m_loading_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    SetSizerAndFit(root);

    m_selected_target  = safe_initial;
    m_requested_target = safe_initial;
    if (m_color_map)
        m_color_map->set_min_component_percent(m_min_component_percent);
    update_range_label();
    rebuild_presets_ui();
    sync_inputs_to_requested();
    update_dialog_state();

    if (m_color_map) {
        m_color_map->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
            if (!m_color_map)
                return;
            request_recipe_match(m_color_map->selected_color(), true, _L("Matching closest supported mix..."));
        });
    }

    if (m_hex_input) {
        m_hex_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) {
            apply_hex_input(true);
        });
        m_hex_input->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &evt) {
            apply_hex_input(false);
            evt.Skip();
        });
    }
    if (m_classic_picker) {
        m_classic_picker->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent &evt) {
            if (m_syncing_inputs)
                return;
            apply_requested_target(evt.GetColour());
        });
    }
    if (m_range_slider) {
        m_range_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
            m_min_component_percent = m_range_slider ? std::clamp(m_range_slider->GetValue(), 0, 50) : m_min_component_percent;
            update_range_label();
            if (m_color_map)
                m_color_map->set_min_component_percent(m_min_component_percent);
            rebuild_presets_ui();
            request_recipe_match(m_requested_target, true, _L("Matching closest supported mix..."));
        });
    }

    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { refresh_selected_recipe(); }, m_recipe_timer.GetId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
        if (m_loading_gauge && m_recipe_loading)
            m_loading_gauge->Pulse();
    }, m_loading_timer.GetId());
    if (wxWindow *ok_button = FindWindow(wxID_OK)) {
        ok_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) {
            if (m_recipe_refresh_pending)
                refresh_selected_recipe();
            if (m_recipe_loading || !m_selected_recipe.valid)
                return;
            evt.Skip();
        });
    }

    CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

MixedFilamentColorMatchDialog::~MixedFilamentColorMatchDialog()
{
    if (m_recipe_timer.IsRunning())
        m_recipe_timer.Stop();
    if (m_loading_timer.IsRunning())
        m_loading_timer.Stop();
}

void MixedFilamentColorMatchDialog::begin_initial_recipe_load()
{
    request_recipe_match(m_requested_target, false, _L("Calculating closest supported mix..."));
}

void MixedFilamentColorMatchDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    wxUnusedVar(suggested_rect);
    Layout();
    Fit();
    Refresh();
}

void MixedFilamentColorMatchDialog::sync_recipe_preview(MixedColorMatchRecipeResult &recipe,
                                                         const wxColour *requested_target)
{
    if (!recipe.valid)
        return;

    recipe.preview_color = compute_color_match_recipe_display_color(recipe, m_display_context);
    if (requested_target != nullptr && requested_target->IsOk())
        recipe.delta_e = color_delta_e00(*requested_target, recipe.preview_color);
}

void MixedFilamentColorMatchDialog::update_range_label()
{
    if (m_range_value)
        m_range_value->SetLabel(wxString::Format(_L("%d%% min"), m_min_component_percent));
}

void MixedFilamentColorMatchDialog::rebuild_presets_ui()
{
    if (!m_presets_host || !m_presets_sizer || !m_presets_label)
        return;

    m_presets = build_color_match_presets(m_physical_colors, m_min_component_percent);
    for (MixedColorMatchRecipeResult &preset : m_presets)
        sync_recipe_preview(preset);

    m_presets_host->Freeze();
    while (m_presets_sizer->GetItemCount() > 0) {
        wxSizerItem *item   = m_presets_sizer->GetItem(size_t(0));
        wxWindow    *window = item ? item->GetWindow() : nullptr;
        m_presets_sizer->Remove(0);
        if (window)
            window->Destroy();
    }

    for (const MixedColorMatchRecipeResult &preset : m_presets) {
        auto *button = new wxBitmapButton(m_presets_host, wxID_ANY,
                                          make_color_match_swatch_bitmap(preset.preview_color, wxSize(FromDIP(30), FromDIP(20))),
                                          wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        const wxString tooltip = from_u8(summarize_color_match_recipe(preset)) + "\n" +
            normalize_color_match_hex(preset.preview_color.GetAsString(wxC2S_HTML_SYNTAX));
        button->SetToolTip(tooltip);
        button->Bind(wxEVT_BUTTON, [this, preset](wxCommandEvent &) { apply_preset(preset); });
        m_presets_sizer->Add(button, 0, wxALL, FromDIP(2));
    }

    m_presets_host->FitInside();
    const bool show_presets = !m_presets.empty();
    m_presets_label->Show(show_presets);
    m_presets_host->Show(show_presets);
    m_presets_host->Thaw();
}

void MixedFilamentColorMatchDialog::set_recipe_loading(bool loading, const wxString &message)
{
    m_recipe_loading = loading;
    if (!message.empty())
        m_loading_message = message;

    if (m_loading_label)
        m_loading_label->SetLabel(loading ? m_loading_message : wxString(" "));
    if (m_loading_gauge) {
        if (loading) {
            m_loading_gauge->Enable(true);
            m_loading_gauge->Pulse();
            if (!m_loading_timer.IsRunning())
                m_loading_timer.Start(100);
        } else {
            if (m_loading_timer.IsRunning())
                m_loading_timer.Stop();
            m_loading_gauge->SetValue(0);
            m_loading_gauge->Enable(false);
        }
    }
}

void MixedFilamentColorMatchDialog::sync_inputs_to_requested()
{
    m_syncing_inputs = true;
    if (m_hex_input)
        m_hex_input->ChangeValue(normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX)));
    if (m_classic_picker)
        m_classic_picker->SetColour(m_requested_target);
    m_syncing_inputs = false;
}

bool MixedFilamentColorMatchDialog::apply_requested_target(const wxColour &requested_target)
{
    request_recipe_match(requested_target, false, _L("Matching closest supported mix..."));
    return true;
}

bool MixedFilamentColorMatchDialog::apply_hex_input(bool show_invalid_error)
{
    if (!m_hex_input || m_syncing_inputs)
        return false;

    wxColour parsed;
    if (!try_parse_color_match_hex(m_hex_input->GetValue(), parsed)) {
        if (show_invalid_error && m_error_label)
            m_error_label->SetLabel(_L("Use a valid hex color like #00FF88."));
        return false;
    }

    return apply_requested_target(parsed);
}

void MixedFilamentColorMatchDialog::request_recipe_match(const wxColour &requested_target,
                                                          bool debounce,
                                                          const wxString &loading_message)
{
    m_requested_target = requested_target;
    m_selected_target  = requested_target;
    sync_inputs_to_requested();

    ++m_recipe_request_token;
    set_recipe_loading(true, loading_message);

    if (m_recipe_timer.IsRunning())
        m_recipe_timer.Stop();
    m_recipe_refresh_pending = debounce;
    update_dialog_state();

    if (debounce) {
        m_recipe_timer.StartOnce(120);
        return;
    }

    launch_recipe_match(m_recipe_request_token, requested_target);
}

void MixedFilamentColorMatchDialog::refresh_selected_recipe()
{
    m_recipe_refresh_pending = false;
    launch_recipe_match(m_recipe_request_token, m_requested_target);
}

void MixedFilamentColorMatchDialog::launch_recipe_match(size_t request_token, const wxColour &requested_target)
{
    const std::vector<std::string> physical_colors    = m_physical_colors;
    const int                      min_component_percent = m_min_component_percent;
    wxWeakRef<wxWindow>            weak_self(this);
    std::thread([weak_self, physical_colors, requested_target, request_token, min_component_percent]() {
        MixedColorMatchRecipeResult recipe = build_best_color_match_recipe(physical_colors, requested_target, min_component_percent);
        wxGetApp().CallAfter([weak_self, requested_target, recipe = std::move(recipe), request_token]() mutable {
            if (!weak_self)
                return;
            auto *self = static_cast<MixedFilamentColorMatchDialog *>(weak_self.get());
            self->handle_recipe_result(request_token, requested_target, std::move(recipe));
        });
    }).detach();
}

void MixedFilamentColorMatchDialog::handle_recipe_result(size_t                      request_token,
                                                          const wxColour             &requested_target,
                                                          MixedColorMatchRecipeResult recipe)
{
    if (request_token != m_recipe_request_token)
        return;

    m_has_recipe_result = true;
    m_selected_recipe   = std::move(recipe);
    sync_recipe_preview(m_selected_recipe, &requested_target);
    set_recipe_loading(false, wxEmptyString);

    if (m_selected_recipe.valid) {
        m_selected_target = m_selected_recipe.preview_color;
        if (m_color_map)
            m_color_map->set_normalized_weights(expand_color_match_recipe_weights(m_selected_recipe, m_palette.size()), false);
        sync_inputs_to_requested();
    } else {
        m_selected_target = requested_target;
    }

    update_dialog_state();
}

void MixedFilamentColorMatchDialog::apply_preset(MixedColorMatchRecipeResult preset)
{
    preset.delta_e = 0.0;
    sync_recipe_preview(preset);
    ++m_recipe_request_token;
    m_requested_target       = preset.preview_color;
    m_selected_target        = preset.preview_color;
    m_selected_recipe        = std::move(preset);
    m_has_recipe_result      = true;
    m_recipe_refresh_pending = false;
    if (m_recipe_timer.IsRunning())
        m_recipe_timer.Stop();
    set_recipe_loading(false, wxEmptyString);
    if (m_color_map)
        m_color_map->set_normalized_weights(expand_color_match_recipe_weights(m_selected_recipe, m_palette.size()), false);
    sync_inputs_to_requested();
    update_dialog_state();
}

void MixedFilamentColorMatchDialog::update_dialog_state()
{
    const wxColour fallback = wxColour("#26A69A");
    if (m_selected_preview) {
        m_selected_preview->SetBackgroundColour(m_requested_target.IsOk() ? m_requested_target : fallback);
        m_selected_preview->Refresh();
    }
    if (m_selected_label)
        m_selected_label->SetLabel(m_requested_target.IsOk() ?
            normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX)) :
            normalize_color_match_hex(fallback.GetAsString(wxC2S_HTML_SYNTAX)));

    const bool valid = m_selected_recipe.valid;
    const wxColour recipe_color = (valid && m_selected_recipe.preview_color.IsOk()) ?
        m_selected_recipe.preview_color :
        (m_requested_target.IsOk() ? m_requested_target : fallback);
    if (m_recipe_preview) {
        m_recipe_preview->SetBackgroundColour(recipe_color);
        m_recipe_preview->Refresh();
    }
    if (m_recipe_label) {
        if (m_recipe_loading) {
            m_recipe_label->SetLabel(m_loading_message);
        } else if (valid) {
            const wxString recipe_summary = from_u8(summarize_color_match_recipe(m_selected_recipe));
            const wxString recipe_hex     = normalize_color_match_hex(recipe_color.GetAsString(wxC2S_HTML_SYNTAX));
            m_recipe_label->SetLabel(recipe_summary + "  " + recipe_hex);
        } else if (m_has_recipe_result) {
            m_recipe_label->SetLabel(_L("No supported 2-color, 3-color, or 4-color recipe found."));
        } else {
            m_recipe_label->SetLabel(wxEmptyString);
        }
    }
    if (m_delta_label) {
        if (m_recipe_loading && m_requested_target.IsOk()) {
            m_delta_label->SetLabel(wxString::Format(_L("Matching %s..."),
                                                     normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX))));
        } else if (valid && m_requested_target.IsOk()) {
            m_delta_label->SetLabel(wxString::Format(_L("Requested %s, closest recipe delta: %.2f"),
                                                     normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX)),
                                                     m_selected_recipe.delta_e));
        } else {
            m_delta_label->SetLabel(wxEmptyString);
        }
    }
    if (m_error_label) {
        if (m_recipe_loading)
            m_error_label->SetLabel(wxEmptyString);
        else if (!valid && m_has_recipe_result)
            m_error_label->SetLabel(_L("Unable to create a color mix from the current physical filament colors within the selected range."));
        else if (m_hex_input && !m_syncing_inputs) {
            wxColour parsed;
            if (!try_parse_color_match_hex(m_hex_input->GetValue(), parsed))
                m_error_label->SetLabel(_L("Use a valid hex color like #00FF88."));
            else
                m_error_label->SetLabel(wxEmptyString);
        } else {
            m_error_label->SetLabel(wxEmptyString);
        }
    }
    if (wxWindow *ok_button = FindWindow(wxID_OK))
        ok_button->Enable(valid && !m_recipe_loading && !m_recipe_refresh_pending);

    Layout();
}

// ===========================================================================
// prompt_best_color_match_recipe — Plater.cpp:6937-6950
// ===========================================================================

MixedColorMatchRecipeResult prompt_best_color_match_recipe(wxWindow *parent,
                                                           const std::vector<std::string> &physical_colors,
                                                           const wxColour &initial_color)
{
    MixedFilamentColorMatchDialog dlg(parent, physical_colors, initial_color);
    dlg.begin_initial_recipe_load();
    if (dlg.ShowModal() != wxID_OK) {
        MixedColorMatchRecipeResult cancelled;
        cancelled.cancelled = true;
        return cancelled;
    }

    return dlg.selected_recipe();
}

} } // namespace Slic3r::GUI
