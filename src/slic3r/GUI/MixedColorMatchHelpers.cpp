#include "MixedColorMatchHelpers.hpp"
#include "MixedGradientSelector.hpp"
#include <unordered_set>
#include <ColorSpaceConvert.hpp>
#include "MixedFilamentColorMapPanel.hpp"
#include "GUI_App.hpp"
#include "libslic3r/PresetBundle.hpp"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <queue>
#include <sstream>
#include <mutex>
#include <boost/log/trivial.hpp>
#include "nlohmann/json.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r { namespace GUI {
wxColour parse_mixed_color(const std::string& value)
{
    wxColour color(value);
    if (!color.IsOk())
        color = wxColour("#26A69A");
    return color;
}

wxString normalize_color_match_hex(const wxString& value)
{
    wxString normalized = value;
    normalized.Trim(true);
    normalized.Trim(false);
    normalized.MakeUpper();
    if (!normalized.empty() && normalized[0] != '#')
        normalized.Prepend("#");
    return normalized;
}

bool try_parse_color_match_hex(const wxString& value, wxColour& color_out)
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

std::vector<int> normalize_color_match_weights(const std::vector<int>& weights, size_t count)
{
    std::vector<int> out = weights;
    if (out.size() != count)
        out.assign(count, count > 0 ? int(100 / count) : 0);

    int sum = 0;
    for (int& value : out) {
        value = std::max(0, value);
        sum += value;
    }
    if (sum <= 0 && count > 0) {
        out.assign(count, 0);
        out[0] = 100;
        return out;
    }

    std::vector<double> remainders(count, 0.0);
    int                 assigned = 0;
    for (size_t idx = 0; idx < count; ++idx) {
        const double exact = 100.0 * double(out[idx]) / double(sum);
        out[idx]           = int(std::floor(exact));
        remainders[idx]    = exact - double(out[idx]);
        assigned += out[idx];
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

std::vector<int> expand_color_match_recipe_weights(const MixedColorMatchRecipeResult& recipe, size_t num_physical)
{
    std::vector<int> weights(num_physical, 0);
    if (!recipe.valid || num_physical == 0)
        return weights;

    if (!recipe.gradient_component_ids.empty()) {
        const std::vector<unsigned int> ids = MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids);
        const std::vector<int>          raw_weights =
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

std::string summarize_color_match_recipe(const MixedColorMatchRecipeResult& recipe)
{
    if (!recipe.valid)
        return {};

    std::vector<unsigned int> ids;
    std::vector<int>          weights;
    if (!recipe.gradient_component_ids.empty()) {
        ids     = MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids);
        weights = normalize_color_match_weights(decode_color_match_gradient_weights(recipe.gradient_component_weights, ids.size()),
                                                ids.size());
    } else {
        ids     = {recipe.component_a, recipe.component_b};
        weights = {std::max(0, 100 - std::clamp(recipe.mix_b_percent, 0, 100)), std::max(0, std::clamp(recipe.mix_b_percent, 0, 100))};
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

wxBitmap make_color_match_swatch_bitmap(const wxColour& color, const wxSize& size)
{
    wxBitmap   bmp(size.GetWidth(), size.GetHeight());
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(wxColour(255, 255, 255)));
    dc.Clear();
    dc.SetPen(wxPen(wxColour(120, 120, 120), 1));
    dc.SetBrush(wxBrush(color.IsOk() ? color : wxColour("#26A69A")));
    dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

static std::vector<std::vector<bool>> build_compatibility_matrix(size_t n)
{
    std::vector<std::vector<bool>> m(n, std::vector<bool>(n, false));
    for (size_t i = 0; i < n; ++i) {
        m[i][i] = true;
        for (size_t j = i + 1; j < n; ++j) {
            bool ok = is_filament_compatible(std::vector<unsigned int>{(unsigned int)i, (unsigned int)j});
            m[i][j] = m[j][i] = ok;
        }
    }
    return m;
}

std::vector<MixedColorMatchRecipeResult> build_color_match_presets(const std::vector<std::string>& physical_colors,
                                                                   int                             min_component_percent)
{
    std::vector<MixedColorMatchRecipeResult> presets;
    if (physical_colors.size() < 2)
        return presets;

    std::vector<wxColour> palette;
    palette.reserve(physical_colors.size());
    for (const std::string& hex : physical_colors)
        palette.emplace_back(parse_mixed_color(hex));

    constexpr size_t                k_max_presets = 9999;  // effectively unlimited
    std::unordered_set<std::string> seen_colors;
    auto                            add_candidate = [&presets, &seen_colors](MixedColorMatchRecipeResult candidate) {
        if (!candidate.valid)
            return;
        const std::string color_key = normalize_color_match_hex(candidate.preview_color.GetAsString(wxC2S_HTML_SYNTAX)).ToStdString();
        if (color_key.empty() || !seen_colors.insert(color_key).second)
            return;
        presets.emplace_back(std::move(candidate));
    };

    // Only generate 50:50 ratio for two-color combinations
    auto compat = build_compatibility_matrix(palette.size());
    for (size_t left_idx = 0; left_idx < palette.size() && presets.size() < k_max_presets; ++left_idx) {
        for (size_t right_idx = left_idx + 1; right_idx < palette.size() && presets.size() < k_max_presets; ++right_idx) {
            if (!compat[left_idx][right_idx]) continue;
            add_candidate(build_pair_color_match_candidate(palette, unsigned(left_idx + 1), unsigned(right_idx + 1), 50,
                                                           min_component_percent));
        }
    }

    const size_t           triple_limit         = std::min<size_t>(palette.size(), 6);
    const std::vector<int> equal_triple_weights = normalize_color_match_weights({1, 1, 1}, 3);
    for (size_t first_idx = 0; first_idx + 2 < triple_limit && presets.size() < k_max_presets; ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 1 < triple_limit && presets.size() < k_max_presets; ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx < triple_limit && presets.size() < k_max_presets; ++third_idx) {
                if (!compat[first_idx][second_idx] || !compat[second_idx][third_idx] || !compat[first_idx][third_idx]) continue;
                const std::vector<unsigned int> ids = {unsigned(first_idx + 1), unsigned(second_idx + 1), unsigned(third_idx + 1)};
                add_candidate(build_multi_color_match_candidate(palette, ids, equal_triple_weights, min_component_percent));
                for (size_t dominant_idx = 0; dominant_idx < ids.size() && presets.size() < k_max_presets; ++dominant_idx) {
                    std::vector<int> dominant_weights(ids.size(), 25);
                    dominant_weights[dominant_idx] = 50;
                    add_candidate(build_multi_color_match_candidate(palette, ids, dominant_weights, min_component_percent));
                }
            }
        }
    }

#if 0 // four-color presets: disabled
    const size_t quad_limit = std::min<size_t>(palette.size(), 5);
    for (size_t first_idx = 0; first_idx + 3 < quad_limit && presets.size() < k_max_presets; ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 2 < quad_limit && presets.size() < k_max_presets; ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx + 1 < quad_limit && presets.size() < k_max_presets; ++third_idx) {
                for (size_t fourth_idx = third_idx + 1; fourth_idx < quad_limit && presets.size() < k_max_presets; ++fourth_idx) {
                    add_candidate(build_multi_color_match_candidate(palette,
                                                                    {unsigned(first_idx + 1), unsigned(second_idx + 1),
                                                                     unsigned(third_idx + 1), unsigned(fourth_idx + 1)},
                                                                    {25, 25, 25, 25}, min_component_percent));
                }
            }
        }
    }
#endif

    return presets;
}

// ---- BlendLUT ----

BlendLUT::BlendLUT(size_t n) : m_n(n)
{
    if (n < 2) {
        m_n = 0;
        return;
    }
    // Store only b >= a entries; m_pair[a][b - a] for b in [a, n)
    m_pair.resize(n);
    for (size_t a = 0; a < n; ++a) {
        m_pair[a].resize(n - a);
        for (size_t b = a; b < n; ++b)
            m_pair[a][b - a].resize(101);
    }
}

// ---- CIELAB conversion & helpers ----

CIELab sRGB_to_CIELab(const wxColour& c)
{
    double r = c.Red()   / 255.0;
    double g = c.Green() / 255.0;
    double b = c.Blue()  / 255.0;
    float lab[3];
    RGB2Lab(float(r), float(g), float(b), &lab[0], &lab[1], &lab[2]);
    return { double(lab[0]), double(lab[1]), double(lab[2]) };
}

double delta_e_lab(const CIELab& a, const CIELab& b)
{
    return double(DeltaE00(float(a.L), float(a.a), float(a.b),
                           float(b.L), float(b.a), float(b.b)));
}

BlendLUT build_blend_lut(const std::vector<wxColour>& palette)
{
    const size_t n = palette.size();
    BlendLUT lut(n);
    if (lut.empty()) return lut;

    for (size_t a = 0; a < n; ++a) {
        for (size_t b = a; b < n; ++b) {
            for (int pct = 0; pct <= 100; ++pct) {
                wxColour blended = blend_pair_filament_mixer(palette[a], palette[b], float(pct) / 100.f);
                lut.m_pair[a][b - a][pct] = sRGB_to_CIELab(blended);
            }
        }
    }
    return lut;
}

CIELab blend_weighted_lab_accurate(const std::vector<wxColour>& palette,
                                    const std::vector<unsigned int>& ids,
                                    const std::vector<int>& weights)
{
    if (ids.size() != weights.size() || ids.empty())
        return { 50.0, 0.0, 0.0 };

    // Sort by filament ID ascending — matches blend_display_color_from_sequence
    // which iterates IDs from 1..n. Sequential lerp order matters.
    std::vector<std::pair<unsigned int, int>> sorted;
    sorted.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
        sorted.emplace_back(ids[i], weights[i]);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<wxColour> colors;
    std::vector<double>   dweights;
    colors.reserve(sorted.size());
    dweights.reserve(sorted.size());
    for (const auto& [id, w] : sorted) {
        if (id == 0 || id > palette.size()) continue;
        colors.push_back(palette[id - 1]);
        dweights.push_back(double(std::max(0, w)));
    }

    wxColour blended = blend_multi_filament_mixer(colors, dweights);
    return sRGB_to_CIELab(blended);
}

// ---- ΔE2000 ----

double color_delta_e00(const wxColour& lhs, const wxColour& rhs)
{
    float lhs_l = 0.f, lhs_a = 0.f, lhs_b = 0.f;
    float rhs_l = 0.f, rhs_a = 0.f, rhs_b = 0.f;
    RGB2Lab(float(lhs.Red()) / 255.f, float(lhs.Green()) / 255.f, float(lhs.Blue()) / 255.f, &lhs_l, &lhs_a, &lhs_b);
    RGB2Lab(float(rhs.Red()) / 255.f, float(rhs.Green()) / 255.f, float(rhs.Blue()) / 255.f, &rhs_l, &rhs_a, &rhs_b);
    return double(DeltaE00(lhs_l, lhs_a, lhs_b, rhs_l, rhs_a, rhs_b));
}

MixedColorMatchRecipeResult build_best_color_match_recipe(const std::vector<std::string>& physical_colors,
                                                          const wxColour&                 target_color,
                                                          int                             min_component_percent)
{
    MixedColorMatchRecipeResult best;
    if (!target_color.IsOk() || physical_colors.size() < 2)
        return best;

    // ---- Step 1: build palette & pre-convert to Lab ----
    const size_t n = physical_colors.size();
    std::vector<wxColour> palette;
    std::vector<CIELab>   palette_lab;
    palette.reserve(n);
    palette_lab.reserve(n);
    for (const std::string& hex : physical_colors) {
        wxColour c = parse_mixed_color(hex);
        palette.emplace_back(c);
        palette_lab.emplace_back(sRGB_to_CIELab(c));
    }
    const CIELab target_lab = sRGB_to_CIELab(target_color);

    const int  loop_min_weight      = std::max(1, std::clamp(min_component_percent, 0, 50));
    const auto compat                = build_compatibility_matrix(n);

    // Helper: encode filament IDs as gradient_component_ids string.
    // Legacy format (all IDs ≤ 9): concatenated single chars, e.g. "123".
    // Extended format (any ID > 9): '/' separated decimals, e.g. "1/12/3".
    // Single-ID extended format uses leading '/' to disambiguate from legacy: "/12".
    auto encode_gradient_ids = [](const std::vector<unsigned int>& ids) -> std::string {
        return MixedFilamentManager::encode_gradient_component_ids(ids);
    };

    auto encode_gradient_weights = [](const std::vector<int>& weights) -> std::string {
        std::ostringstream ss;
        for (size_t i = 0; i < weights.size(); ++i) {
            if (i > 0) ss << '/';
            ss << weights[i];
        }
        return ss.str();
    };

    // ---- Step 2: build pair Blend LUT (polynomial mixing → Lab) ----
    const BlendLUT lut = build_blend_lut(palette);
    if (lut.empty()) return best;

    // ---- helper: update best from a pair candidate ----
    auto update_best_pair = [&](unsigned int a, unsigned int b, int pct, double de) {
        if (!best.valid || de + 1e-6 < best.delta_e) {
            best.valid         = true;
            best.component_a   = a;
            best.component_b   = b;
            best.mix_b_percent = pct;
            best.preview_color = blend_pair_filament_mixer(palette[a - 1], palette[b - 1], float(pct) / 100.f);
            best.delta_e       = de;
            best.gradient_component_ids.clear();
            best.gradient_component_weights.clear();
            best.manual_pattern.clear();
        }
    };

    // ---- Step 3: pair coarse scan (step=5%) ----
    constexpr int k_coarse_step = 5;
    constexpr int k_top_coarse  = 30;

    // max-heap of (ΔE, a, b, percent) — keeps top-k LOWEST ΔE, worst at top
    using HeapEntry = std::tuple<double, unsigned int, unsigned int, int>;
    auto cmp = [](const HeapEntry& x, const HeapEntry& y) { return std::get<0>(x) < std::get<0>(y); };
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)> heap(cmp);

    for (size_t a = 0; a < n; ++a) {
        for (size_t b = a + 1; b < n; ++b) {
            if (!compat[a][b]) continue;
            for (int pct = loop_min_weight; pct <= 100 - loop_min_weight; pct += k_coarse_step) {
                const CIELab& blended_lab = lut.get(a, b, pct);
                double de = delta_e_lab(target_lab, blended_lab);
                update_best_pair(unsigned(a + 1), unsigned(b + 1), pct, de);
                if (heap.size() < k_top_coarse) {
                    heap.emplace(de, unsigned(a + 1), unsigned(b + 1), pct);
                } else if (de < std::get<0>(heap.top())) {
                    heap.pop();
                    heap.emplace(de, unsigned(a + 1), unsigned(b + 1), pct);
                }
            }
        }
    }

    // ---- Step 4: pair fine search (step=1%, top-N from coarse) ----
    while (!heap.empty()) {
        auto [de, a, b, coarse_pct] = heap.top();
        heap.pop();
        int fine_min = std::max(loop_min_weight, coarse_pct - k_coarse_step + 1);
        int fine_max = std::min(100 - loop_min_weight, coarse_pct + k_coarse_step - 1);
        for (int pct = fine_min; pct <= fine_max; ++pct) {
            if ((pct - loop_min_weight) % k_coarse_step == 0) continue; // already evaluated in coarse
            const CIELab& blended_lab = lut.get(a - 1, b - 1, pct);
            update_best_pair(a, b, pct, delta_e_lab(target_lab, blended_lab));
        }
    }

    // ---- save best pair (before triple search may overwrite) ----
    MixedColorMatchRecipeResult best_pair = best;

    // ---- Step 5: early termination ----
    if (best_pair.valid && best_pair.delta_e <= 0.5)
        return best_pair;

    // ---- Step 6: adaptive candidate pool (top-N by single-color ΔE) ----
    std::vector<std::pair<double, unsigned int>> ranked_ids;
    ranked_ids.reserve(n);
    for (size_t idx = 0; idx < n; ++idx)
        ranked_ids.emplace_back(delta_e_lab(target_lab, palette_lab[idx]), unsigned(idx + 1));
    std::sort(ranked_ids.begin(), ranked_ids.end(), [](const auto& x, const auto& y) {
        if (x.first != y.first) return x.first < y.first;
        return x.second < y.second;
    });

    const size_t pool_size = std::min<size_t>(n, 8);
    std::vector<unsigned int> candidate_pool;
    candidate_pool.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i)
        candidate_pool.emplace_back(ranked_ids[i].second);

    if (candidate_pool.size() < 3)
        return best;

    std::sort(candidate_pool.begin(), candidate_pool.end());

    // ---- Step 7: triple layered search ----
    constexpr int k_triple_coarse_step = 10;
    constexpr int k_top_triple        = 20;

    struct TripleEntry {
        double       de;
        unsigned int a, b, c;
        int          wa, wb;
        bool operator<(const TripleEntry& o) const { return de < o.de; }
    };
    std::priority_queue<TripleEntry> triple_heap;

    // Coarse (step=10%)
    for (size_t fi = 0; fi + 2 < candidate_pool.size(); ++fi) {
        for (size_t fj = fi + 1; fj + 1 < candidate_pool.size(); ++fj) {
            for (size_t fk = fj + 1; fk < candidate_pool.size(); ++fk) {
                unsigned int a = candidate_pool[fi], b = candidate_pool[fj], c = candidate_pool[fk];
                if (!compat[a - 1][b - 1] || !compat[b - 1][c - 1] || !compat[a - 1][c - 1]) continue;

                for (int wa = loop_min_weight; wa <= 100 - 2 * loop_min_weight; wa += k_triple_coarse_step) {
                    for (int wb = loop_min_weight; wa + wb <= 100 - loop_min_weight; wb += k_triple_coarse_step) {
                        int wc = 100 - wa - wb;
                        if (wc < loop_min_weight) continue;
                        CIELab blended = blend_weighted_lab_accurate(palette, {a, b, c}, {wa, wb, wc});
                        double  de      = delta_e_lab(target_lab, blended);
                        // Update best triple
                        if (!best.valid || de + 1e-6 < best.delta_e) {
                            best.valid     = true;
                            best.component_a = a;
                            best.component_b = b;
                            best.mix_b_percent = wa + wb > 0 ? int(std::lround(100.0 * double(wb) / double(wa + wb))) : 50;
                            best.gradient_component_ids     = encode_gradient_ids({a, b, c});
                            best.gradient_component_weights = encode_gradient_weights({wa, wb, wc});
                            best.preview_color = blend_multi_filament_mixer(
                                {palette[a - 1], palette[b - 1], palette[c - 1]},
                                {double(wa), double(wb), double(wc)});
                            best.delta_e = de;
                            best.manual_pattern.clear();
                        }
                        if (triple_heap.size() < k_top_triple) {
                            triple_heap.push({de, a, b, c, wa, wb});
                        } else if (de < triple_heap.top().de) {
                            triple_heap.pop();
                            triple_heap.push({de, a, b, c, wa, wb});
                        }
                    }
                }
            }
        }
    }

    // Fine (step=1%, refine ±5 window around coarse center)
    while (!triple_heap.empty()) {
        TripleEntry te = triple_heap.top();
        triple_heap.pop();
        int wa_min = std::max(loop_min_weight, te.wa - k_triple_coarse_step + 1);
        int wa_max = std::min(100 - 2 * loop_min_weight, te.wa + k_triple_coarse_step - 1);
        for (int wa = wa_min; wa <= wa_max; ++wa) {
            if ((wa - loop_min_weight) % k_triple_coarse_step == 0) continue;
            int wb_min = std::max(loop_min_weight, te.wb - k_triple_coarse_step + 1);
            int wb_max = std::min(100 - wa - loop_min_weight, te.wb + k_triple_coarse_step - 1);
            for (int wb = wb_min; wb <= wb_max; ++wb) {
                if ((wb - loop_min_weight) % k_triple_coarse_step == 0) continue;
                int wc = 100 - wa - wb;
                if (wc < loop_min_weight) continue;
                CIELab blended = blend_weighted_lab_accurate(palette, {te.a, te.b, te.c}, {wa, wb, wc});
                double  de2    = delta_e_lab(target_lab, blended);
                if (!best.valid || de2 + 1e-6 < best.delta_e) {
                    best.valid     = true;
                    best.component_a = te.a;
                    best.component_b = te.b;
                    best.mix_b_percent = wa + wb > 0 ? int(std::lround(100.0 * double(wb) / double(wa + wb))) : 50;
                    best.gradient_component_ids     = encode_gradient_ids({te.a, te.b, te.c});
                    best.gradient_component_weights = encode_gradient_weights({wa, wb, wc});
                    best.preview_color = blend_multi_filament_mixer(
                        {palette[te.a - 1], palette[te.b - 1], palette[te.c - 1]},
                        {double(wa), double(wb), double(wc)});
                    best.delta_e = de2;
                    best.manual_pattern.clear();
                }
            }
        }
    }

    // ---- final normalization: re-evaluate ΔE with consistent color_delta_e00 ----
    // Pair and triple search may use different evaluation paths (LUT vs on-the-fly
    // blend_multi_filament_mixer); re-evaluate both via the same pipeline for a fair
    // comparison, then prefer the simpler (pair) recipe when ΔE gain is negligible.
    if (best.valid)
        best.delta_e = color_delta_e00(target_color, best.preview_color);
    if (best_pair.valid) {
        best_pair.delta_e = color_delta_e00(target_color, best_pair.preview_color);
        // Pick the true winner under unified evaluation
        if (!best.valid || best_pair.delta_e + 1e-6 < best.delta_e)
            best = std::move(best_pair);
        // Prefer simpler recipe when multi-color ΔE advantage is imperceptible
        else if (!best.gradient_component_ids.empty() &&
                 best_pair.delta_e <= best.delta_e + 0.5)
            best = std::move(best_pair);
    }

    return best;
}

MixedFilamentDisplayContext build_mixed_filament_display_context(const std::vector<std::string>& physical_colors)
{
    MixedFilamentDisplayContext context;
    context.num_physical    = physical_colors.size();
    context.physical_colors = physical_colors;
    context.nozzle_diameters.assign(context.num_physical, 0.4);

    auto* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return context;

    DynamicPrintConfig* print_cfg = &preset_bundle->prints.get_edited_preset().config;
    if (const ConfigOptionFloats* opt = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")) {
        const size_t opt_count = opt->values.size();
        if (opt_count > 0) {
            for (size_t i = 0; i < context.num_physical; ++i)
                context.nozzle_diameters[i] = std::max(0.05, opt->get_at(unsigned(std::min(i, opt_count - 1))));
        }
    }

    auto get_mixed_bool = [preset_bundle, print_cfg](const std::string& key, bool fallback) {
        if (const ConfigOptionBool* opt = preset_bundle->project_config.option<ConfigOptionBool>(key))
            return opt->value;
        if (const ConfigOptionInt* opt = preset_bundle->project_config.option<ConfigOptionInt>(key))
            return opt->value != 0;
        if (print_cfg != nullptr) {
            if (const ConfigOptionBool* opt = print_cfg->option<ConfigOptionBool>(key))
                return opt->value;
            if (const ConfigOptionInt* opt = print_cfg->option<ConfigOptionInt>(key))
                return opt->value != 0;
        }
        return fallback;
    };
    auto get_mixed_float = [preset_bundle, print_cfg](const std::string& key, float fallback) {
        if (preset_bundle->project_config.has(key))
            return float(preset_bundle->project_config.opt_float(key));
        if (print_cfg != nullptr && print_cfg->has(key))
            return float(print_cfg->opt_float(key));
        return fallback;
    };

    context.preview_settings.mixed_lower_bound    = std::max(0.01, double(get_mixed_float("mixed_filament_height_lower_bound", 0.04f)));
    context.preview_settings.mixed_upper_bound    = std::max(context.preview_settings.mixed_lower_bound,
                                                             double(get_mixed_float("mixed_filament_height_upper_bound", 0.16f)));
    context.preview_settings.preferred_a_height   = std::max(0.0, double(get_mixed_float("mixed_color_layer_height_a", 0.f)));
    context.preview_settings.preferred_b_height   = std::max(0.0, double(get_mixed_float("mixed_color_layer_height_b", 0.f)));
    context.preview_settings.nominal_layer_height = 0.2;
    if (print_cfg != nullptr && print_cfg->has("layer_height"))
        context.preview_settings.nominal_layer_height = std::max(0.01, print_cfg->opt_float("layer_height"));
    if (print_cfg != nullptr && print_cfg->has("wall_loops"))
        context.preview_settings.wall_loops = std::max<size_t>(1, size_t(std::max(1, print_cfg->opt_int("wall_loops"))));
    context.component_bias_enabled = get_mixed_bool("mixed_filament_component_bias_enabled", false);

    return context;
}

wxColour compute_color_match_recipe_display_color(const MixedColorMatchRecipeResult& recipe, const MixedFilamentDisplayContext& context)
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
    entry.distribution_mode          = recipe.gradient_component_ids.empty() ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);

    return parse_mixed_color(compute_mixed_filament_display_color(entry, context));
}

std::vector<int> decode_color_match_gradient_weights(const std::string& value, size_t expected_components)
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

MixedColorMatchRecipeResult build_pair_color_match_candidate(
    const std::vector<wxColour>& palette, unsigned int component_a, unsigned int component_b, int mix_b_percent, int min_component_percent)
{
    MixedColorMatchRecipeResult candidate;
    if (component_a == 0 || component_b == 0 || component_a == component_b)
        return candidate;
    if (component_a > palette.size() || component_b > palette.size())
        return candidate;
    if (!color_match_weights_within_range({100 - std::clamp(mix_b_percent, 0, 100), std::clamp(mix_b_percent, 0, 100)},
                                          min_component_percent))
        return candidate;

    candidate.valid         = true;
    candidate.component_a   = component_a;
    candidate.component_b   = component_b;
    candidate.mix_b_percent = std::clamp(mix_b_percent, 0, 100);
    candidate.preview_color = blend_pair_filament_mixer(palette[component_a - 1], palette[component_b - 1],
                                                        float(candidate.mix_b_percent) / 100.f);
    return candidate;
}

MixedColorMatchRecipeResult build_multi_color_match_candidate(const std::vector<wxColour>&     palette,
                                                              const std::vector<unsigned int>& ids,
                                                              const std::vector<int>&          weights,
                                                              int                              min_component_percent)
{
    MixedColorMatchRecipeResult candidate;
    if (ids.size() < 3 || ids.size() != weights.size())
        return candidate;
    if (!color_match_weights_within_range(weights, min_component_percent))
        return candidate;

    std::vector<std::pair<int, unsigned int>> weighted_ids;
    weighted_ids.reserve(ids.size());
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        if (ids[idx] == 0 || ids[idx] > palette.size())
            return candidate;
        if (weights[idx] <= 0)
            continue;
        weighted_ids.emplace_back(weights[idx], ids[idx]);
    }
    if (weighted_ids.size() < 3)
        return candidate;

    std::sort(weighted_ids.begin(), weighted_ids.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first)
            return lhs.first > rhs.first;
        return lhs.second < rhs.second;
    });

    std::vector<unsigned int> ordered_ids;
    std::vector<int>          ordered_weights;
    ordered_ids.reserve(weighted_ids.size());
    ordered_weights.reserve(weighted_ids.size());
    for (const auto& [weight, filament_id] : weighted_ids) {
        ordered_ids.emplace_back(filament_id);
        ordered_weights.emplace_back(weight);
    }

    const std::vector<unsigned int> sequence = build_color_match_sequence(ordered_ids, ordered_weights);
    if (sequence.empty())
        return candidate;

    candidate.valid             = true;
    candidate.component_a       = ordered_ids[0];
    candidate.component_b       = ordered_ids[1];
    const int pair_weight_total = ordered_weights[0] + ordered_weights[1];
    candidate.mix_b_percent     = pair_weight_total > 0 ?
                                      std::clamp(int(std::lround(100.0 * double(ordered_weights[1]) / double(pair_weight_total))), 0, 100) :
                                      50;
    candidate.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ordered_ids);
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

bool color_match_weights_within_range(const std::vector<int>& weights, int min_component_percent)
{
    if (min_component_percent <= 0)
        return true;

    const int min_allowed       = std::clamp(min_component_percent, 0, 50);
    int       active_components = 0;
    for (const int weight : weights) {
        if (weight <= 0)
            continue;
        ++active_components;
        if (weight < min_allowed)
            return false;
    }
    return active_components >= 2;
}

std::vector<unsigned int> build_color_match_sequence(const std::vector<unsigned int>& ids, const std::vector<int>& weights)
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

wxColour blend_sequence_filament_mixer(const std::vector<wxColour>& palette, const std::vector<unsigned int>& sequence)
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
// Material compatibility
// ---------------------------------------------------------------------------

enum class FilamentCategory : uint8_t {
    PLA, PETG, TPU, PET, ABS, ASA, PC, PA, SUPPORT,
    UNKNOWN
};

static constexpr const char* k_category_names[] = {
    "PLA", "PETG", "TPU", "PET", "ABS", "ASA", "PC", "PA", "SUPPORT"
};
static constexpr size_t k_category_count = sizeof(k_category_names) / sizeof(k_category_names[0]);
// Matrix dimension covers all valid categories + UNKNOWN sentinel
static constexpr size_t k_compat_dim = (size_t)FilamentCategory::UNKNOWN + 1;

static FilamentCategory filament_category_from_name(const std::string& name)
{
    for (size_t i = 0; i < k_category_count; ++i) {
        if (name == k_category_names[i])
            return static_cast<FilamentCategory>(i);
    }
    return FilamentCategory::UNKNOWN;
}

// 2D compatibility matrix. Dimension is tied to the enum so adding a category
// to FilamentCategory automatically grows the table.
static std::vector<std::vector<bool>> s_compat;
static bool                           s_compat_loaded = false;
static std::mutex                     s_compat_mutex;

static void load_filament_compatibility()
{
    if (s_compat_loaded) return;
    std::lock_guard<std::mutex> lock(s_compat_mutex);
    if (s_compat_loaded) return;

    // Default: each category compatible only with itself
    s_compat.assign(k_compat_dim, std::vector<bool>(k_compat_dim, false));
    for (size_t i = 0; i < k_category_count; ++i)
        s_compat[i][i] = true;

    try {
        // Prefer user data dir (where PresetUpdater deploys updates), fall back to bundled resources
        const boost::filesystem::path user_path = (boost::filesystem::path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR
                                                    / PresetBundle::ORCA_FILAMENT_LIBRARY / "filament"
                                                    / "filament_compatibility.json").make_preferred();
        const boost::filesystem::path rsrc_path = (boost::filesystem::path(Slic3r::resources_dir()) / "profiles"
                                                    / PresetBundle::ORCA_FILAMENT_LIBRARY / "filament"
                                                    / "filament_compatibility.json").make_preferred();
        const std::string path = (boost::filesystem::exists(user_path) ? user_path : rsrc_path).string();

        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            BOOST_LOG_TRIVIAL(error) << "Failed to open filament compatibility config: " << path;
            return;
        }
        nlohmann::json j;
        ifs >> j;

        if (!j.contains("compatibility")) {
            BOOST_LOG_TRIVIAL(error) << "Missing 'compatibility' key in " << path;
            return;
        }

        for (auto& [cat_a_str, partner_list] : j["compatibility"].items()) {
            FilamentCategory cat_a = filament_category_from_name(cat_a_str);
            if (cat_a == FilamentCategory::UNKNOWN) {
                BOOST_LOG_TRIVIAL(warning) << "Unknown category '" << cat_a_str << "' in compatibility config";
                continue;
            }

            if (!partner_list.is_array()) {
                BOOST_LOG_TRIVIAL(warning) << "Expected array for category '" << cat_a_str << "'";
                continue;
            }

            for (auto& cat_b_val : partner_list) {
                const std::string cat_b_str = cat_b_val.get<std::string>();
                FilamentCategory   cat_b     = filament_category_from_name(cat_b_str);
                if (cat_b == FilamentCategory::UNKNOWN) {
                    BOOST_LOG_TRIVIAL(warning) << "Unknown category '" << cat_b_str << "' in compatibility config";
                    continue;
                }
                s_compat[(size_t)cat_a][(size_t)cat_b] = true;
                s_compat[(size_t)cat_b][(size_t)cat_a] = true;
            }
        }
        s_compat_loaded = true;
        BOOST_LOG_TRIVIAL(info) << "Loaded filament compatibility matrix from " << path;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to parse filament compatibility config: " << e.what();
    }
}

static bool is_category_compatible(FilamentCategory a, FilamentCategory b)
{
    return s_compat[(size_t)a][(size_t)b];
}

struct ResolvedFilamentCategory {
    unsigned int     filament_id;
    FilamentCategory category;
};

static FilamentCategory get_filament_category(const std::string& filament_type);

// Resolves a set of 0-based filament IDs into their (id, category) pairs.
// Skips IDs that cannot be resolved (out of range, missing preset, missing type).
// Caller must have called load_filament_compatibility() first.
static std::vector<ResolvedFilamentCategory> resolve_filament_categories(
    const std::vector<unsigned int>& filament_ids,
    PresetBundle*                    preset_bundle)
{
    std::vector<ResolvedFilamentCategory> result;
    if (!preset_bundle) return result;

    const auto& filament_presets = preset_bundle->filament_presets;
    result.reserve(filament_ids.size());

    for (unsigned int id : filament_ids) {
        if (id >= filament_presets.size()) continue;
        const Preset* preset = preset_bundle->filaments.find_preset(filament_presets[id]);
        if (!preset) continue;
        auto* type_opt = dynamic_cast<const ConfigOptionStrings*>(preset->config.option("filament_type"));
        if (!type_opt || type_opt->values.empty()) continue;

        FilamentCategory cat = get_filament_category(type_opt->values[0]);
        result.push_back({id, cat});
    }
    return result;
}

// Hardcoded filament_type → category. New filament types should be added here.
static const std::unordered_map<std::string, FilamentCategory>& filament_type_category_map()
{
    static const std::unordered_map<std::string, FilamentCategory> m = {
        // PLA family — from classification table
        {"PLA", FilamentCategory::PLA}, {"PLA-CF", FilamentCategory::PLA},
        // ABS
        {"ABS", FilamentCategory::ABS},
        // ASA
        {"ASA", FilamentCategory::ASA},
        // PETG family — from classification table
        {"PETG", FilamentCategory::PETG}, {"PETG-CF", FilamentCategory::PETG}, {"PCTG", FilamentCategory::PETG},
        // TPU
        {"TPU", FilamentCategory::TPU},
        // PET — from compatibility matrix
        {"PET", FilamentCategory::PET},
        // PA
        {"PA", FilamentCategory::PA}, {"PA-CF", FilamentCategory::PA},
        // PC
        {"PC", FilamentCategory::PC},
        // Support materials — from classification table
        {"BVOH", FilamentCategory::SUPPORT}, {"PVA", FilamentCategory::SUPPORT},
    };
    return m;
}

static std::string normalize_filament_type(const std::string& type)
{
    std::string normalized = type;
    size_t start = normalized.find_first_not_of(" \t\r\n");
    size_t end   = normalized.find_last_not_of(" \t\r\n");
    if (start != std::string::npos && end != std::string::npos)
        normalized = normalized.substr(start, end - start + 1);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::toupper);
    return normalized;
}

static FilamentCategory get_filament_category(const std::string& filament_type)
{
    const std::string normalized = normalize_filament_type(filament_type);
    const auto&       m          = filament_type_category_map();
    auto              it         = m.find(normalized);
    if (it != m.end()) return it->second;
    return FilamentCategory::UNKNOWN;
}

bool is_filament_compatible(const std::vector<unsigned int>& filament_ids)
{
    if (filament_ids.size() <= 1) return true;

    load_filament_compatibility();

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) {
        BOOST_LOG_TRIVIAL(error) << "PresetBundle is null in filament compatibility check";
        return false;
    }

    auto resolved = resolve_filament_categories(filament_ids, preset_bundle);

    for (const auto& r : resolved) {
        if (r.category == FilamentCategory::UNKNOWN) {
            BOOST_LOG_TRIVIAL(info) << "Filament type at index " << r.filament_id
                                    << " not in compatibility table, treating as incompatible";
            return false;
        }
    }

    if (resolved.size() <= 1) return true;

    if (std::all_of(resolved.begin() + 1, resolved.end(),
                    [&](const ResolvedFilamentCategory& r) { return r.category == resolved[0].category; }))
        return true;

    for (size_t i = 0; i < resolved.size(); ++i) {
        for (size_t j = i + 1; j < resolved.size(); ++j) {
            if (!is_category_compatible(resolved[i].category, resolved[j].category)) {
                BOOST_LOG_TRIVIAL(info) << "Incompatible filament categories: '"
                                        << k_category_names[(size_t)resolved[i].category]
                                        << "' vs '"
                                        << k_category_names[(size_t)resolved[j].category] << "'";
                return false;
            }
        }
    }

    return true;
}

bool is_filament_compatible(const MixedFilament& mf)
{
    std::vector<unsigned int> fids;

    if (!mf.manual_pattern.empty()) {
        // Cycle / pattern mode: the pattern tokens encode all participating
        // filaments. component_a / component_b are not added separately —
        // in cycle mode they are hardcoded to 1 and 2 and would introduce
        // phantom filaments into the compatibility check.
        const std::string norm = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
        if (norm.empty()) {
            BOOST_LOG_TRIVIAL(warning)
                << "Mixed filament compatibility: manual_pattern '"
                << mf.manual_pattern << "' normalized to empty, malformed pattern";
            return false;
        }

        PresetBundle* preset_bundle = wxGetApp().preset_bundle;
        if (!preset_bundle || preset_bundle->filament_presets.empty()) {
            BOOST_LOG_TRIVIAL(error)
                << "Mixed filament compatibility: PresetBundle is "
                << (preset_bundle ? "empty (no filament presets)" : "null")
                << ", cannot resolve pattern '" << mf.manual_pattern << "'";
            return false;
        }

        const size_t num_physical = preset_bundle->filament_presets.size();
        const auto groups = MixedFilamentManager::split_pattern_groups(norm);
        for (const auto& group : groups) {
            const auto tokens = MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
            for (const auto& token : tokens) {
                const unsigned int eid = MixedFilamentManager::physical_filament_from_token(token, mf, num_physical);
                if (eid >= 1 && eid <= num_physical)
                    fids.push_back(eid - 1);
            }
        }
    } else {
        // ORCA: bound component ids to the physical filaments that currently exist; a printer
        // change can leave a mixed entry referencing filaments beyond the new count.
        PresetBundle* pb = wxGetApp().preset_bundle;
        const unsigned int num_physical = pb ? unsigned(pb->filament_presets.size()) : 0u;
        if (mf.component_a >= 1 && mf.component_a <= num_physical) fids.push_back(mf.component_a - 1);
        if (mf.component_b >= 1 && mf.component_b <= num_physical) fids.push_back(mf.component_b - 1);
        if (!mf.gradient_component_ids.empty()) {
            for (unsigned int fid : MixedFilamentManager::decode_gradient_component_ids(mf.gradient_component_ids))
                if (fid >= 1 && fid <= num_physical) fids.push_back(fid - 1);
        }
    }

    if (fids.empty()) {
        BOOST_LOG_TRIVIAL(warning)
            << "Mixed filament compatibility: no valid filament IDs extracted"
            << " (component_a=" << mf.component_a
            << ", component_b=" << mf.component_b
            << ", manual_pattern='" << mf.manual_pattern
            << "', gradient_component_ids='" << mf.gradient_component_ids << "')"
            << " — treating as incompatible";
        return false;
    }

    return is_filament_compatible(fids);
}

std::optional<std::pair<unsigned int, unsigned int>> find_incompatible_filament_pair(const std::vector<unsigned int>& filament_ids)
{
    if (filament_ids.size() <= 1) return std::nullopt;

    load_filament_compatibility();

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) {
        BOOST_LOG_TRIVIAL(error) << "PresetBundle is null in filament incompatibility search";
        return std::nullopt;
    }

    auto resolved = resolve_filament_categories(filament_ids, preset_bundle);

    // Treat UNKNOWN filament types as incompatible, consistent with is_filament_compatible().
    for (const auto& r : resolved) {
        if (r.category == FilamentCategory::UNKNOWN) {
            BOOST_LOG_TRIVIAL(info) << "Filament type at index " << r.filament_id
                                    << " not in compatibility table, treating as incompatible";
            // Return this UNKNOWN paired with the first other resolved filament.
            for (const auto& other : resolved) {
                if (other.filament_id != r.filament_id)
                    return std::make_pair(r.filament_id + 1, other.filament_id + 1);
            }
        }
    }

    if (resolved.size() <= 1) return std::nullopt;

    if (std::all_of(resolved.begin() + 1, resolved.end(),
                    [&](const ResolvedFilamentCategory& r) { return r.category == resolved[0].category; }))
        return std::nullopt;

    for (size_t i = 0; i < resolved.size(); ++i) {
        for (size_t j = i + 1; j < resolved.size(); ++j) {
            if (!is_category_compatible(resolved[i].category, resolved[j].category))
                return std::make_pair(
                    resolved[i].filament_id + 1,
                    resolved[j].filament_id + 1);
        }
    }

    return std::nullopt;
}

CyclePatternParseResult parse_cycle_pattern(const std::string& normalized_pattern, int num_physical)
{
    CyclePatternParseResult result;
    if (normalized_pattern.empty() || num_physical <= 0) return result;

    const auto groups = MixedFilamentManager::split_pattern_groups(normalized_pattern);
    for (const auto& group : groups) {
        const auto tokens = MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
        for (const auto& token : tokens) {
            ++result.total_tokens;
            char* end = nullptr;
            unsigned long id = std::strtoul(token.c_str(), &end, 10);
            if (!end || *end != '\0') {
                if (result.invalid_token.empty()) result.invalid_token = token;
                continue;
            }
            if (id < 1 || id > (unsigned long)num_physical) {
                if (result.invalid_id == 0) result.invalid_id = (unsigned int)id;
                continue;
            }
            result.ids.push_back((unsigned int)id);
        }
    }
    return result;
}

std::string summarize_cycle_pattern_text(const std::string& normalized_pattern,
                                         const MixedFilament& entry,
                                         int num_physical)
{
    if (normalized_pattern.empty() || num_physical <= 0)
        return {};

    const auto groups = MixedFilamentManager::split_pattern_groups(normalized_pattern);
    if (groups.empty())
        return {};

    std::map<unsigned int, int> counts;
    int                         total = 0;
    for (const auto& group : groups) {
        const auto tokens = MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
        for (const auto& token : tokens) {
            unsigned int eid = MixedFilamentManager::physical_filament_from_token(token, entry, num_physical);
            if (eid >= 1 && eid <= (unsigned)num_physical) {
                counts[eid]++;
                total++;
            }
        }
    }

    if (total <= 0 || counts.empty())
        return {};

    std::vector<std::pair<unsigned int, int>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Compute floor percentages; distribute remainder via largest remainders.
    std::vector<int> pcts(sorted.size());
    int              sum_pct = 0;
    for (size_t i = 0; i < sorted.size(); ++i) {
        pcts[i]  = int((static_cast<long long>(sorted[i].second) * 100) / total);
        sum_pct += pcts[i];
    }

    if (sum_pct < 100) {
        // Remainder indexed by original position, value = count * 100 % total
        std::vector<std::pair<size_t, int>> rem;
        rem.reserve(sorted.size());
        for (size_t i = 0; i < sorted.size(); ++i)
            rem.emplace_back(i, int((static_cast<long long>(sorted[i].second) * 100) % total));
        // Sort descending by remainder, then by original index for stability
        std::sort(rem.begin(), rem.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });
        for (int extra = 100 - sum_pct; extra > 0; --extra) {
            pcts[rem.front().first]++;
            rem.erase(rem.begin());
        }
    }

    std::ostringstream out;
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (i > 0)
            out << '+';
        out << 'F' << sorted[i].first << ' ' << pcts[i] << '%';
    }
    return out.str();
}

}} // namespace Slic3r::GUI
