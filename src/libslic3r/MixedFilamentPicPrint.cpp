#include "MixedFilamentPicPrint.hpp"

#include "MixedFilamentMatch.hpp"
#include "Model.hpp"
#include "TriangleSelector.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

int clamp_int(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

int picprint_pixel_x(double u, int w)
{
    if (w <= 1)
        return 0;
    u = std::clamp(u, 0.0, 1.0);
    return clamp_int(int(u * double(w - 1)), 0, w - 1);
}

int picprint_pixel_y(double v, int h)
{
    if (h <= 1)
        return 0;
    v = std::clamp(v, 0.0, 1.0);
    return clamp_int(int((1.0 - v) * double(h - 1)), 0, h - 1);
}

void downsample_rgb(const std::uint8_t *rgb, int w, int h,
                    std::vector<std::uint8_t> &out, int &nw, int &nh)
{
    const int long_edge = std::max(w, h);
    nw = w;
    nh = h;
    if (long_edge > 256) {
        nw = std::max(1, (w * 256) / long_edge);
        nh = std::max(1, (h * 256) / long_edge);
    }
    out.resize(size_t(nw) * size_t(nh) * 3);
    for (int y = 0; y < nh; ++y) {
        const int sy = (y * h) / nh;
        for (int x = 0; x < nw; ++x) {
            const int sx = (x * w) / nw;
            const size_t si = (size_t(sy) * size_t(w) + size_t(sx)) * 3;
            const size_t di = (size_t(y) * size_t(nw) + size_t(x)) * 3;
            out[di]     = rgb[si];
            out[di + 1] = rgb[si + 1];
            out[di + 2] = rgb[si + 2];
        }
    }
}

struct RgbPixel
{
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    int          index = 0;
};

int channel_of(const RgbPixel &p, int ch)
{
    if (ch == 0)
        return p.r;
    if (ch == 1)
        return p.g;
    return p.b;
}

struct ColorBox
{
    size_t begin = 0;
    size_t end   = 0;
    int    minc[3] = {0, 0, 0};
    int    maxc[3] = {0, 0, 0};
};

void refresh_box(ColorBox &box, const std::vector<RgbPixel> &pixels)
{
    box.minc[0] = box.minc[1] = box.minc[2] = 255;
    box.maxc[0] = box.maxc[1] = box.maxc[2] = 0;
    for (size_t i = box.begin; i < box.end; ++i) {
        const RgbPixel &p = pixels[i];
        box.minc[0] = std::min(box.minc[0], int(p.r));
        box.maxc[0] = std::max(box.maxc[0], int(p.r));
        box.minc[1] = std::min(box.minc[1], int(p.g));
        box.maxc[1] = std::max(box.maxc[1], int(p.g));
        box.minc[2] = std::min(box.minc[2], int(p.b));
        box.maxc[2] = std::max(box.maxc[2], int(p.b));
    }
}

int box_longest_channel(const ColorBox &box)
{
    int best_ch = 0;
    int best_r  = box.maxc[0] - box.minc[0];
    for (int ch = 1; ch < 3; ++ch) {
        const int r = box.maxc[ch] - box.minc[ch];
        if (r > best_r) {
            best_r  = r;
            best_ch = ch;
        }
    }
    return best_ch;
}

int box_range(const ColorBox &box)
{
    int best = 0;
    for (int ch = 0; ch < 3; ++ch)
        best = std::max(best, box.maxc[ch] - box.minc[ch]);
    return best;
}

// Deterministic median-cut. cluster_of[pixel] is 0-based cluster index.
void median_cut(const std::vector<std::uint8_t> &rgb, int w, int h, size_t max_clusters,
                std::vector<int> &cluster_of, std::vector<ColorRGB> &centroids)
{
    const size_t n = size_t(w) * size_t(h);
    cluster_of.assign(n, 0);
    centroids.clear();
    if (n == 0 || max_clusters == 0)
        return;

    std::vector<RgbPixel> pixels(n);
    for (size_t i = 0; i < n; ++i) {
        pixels[i].r     = rgb[i * 3];
        pixels[i].g     = rgb[i * 3 + 1];
        pixels[i].b     = rgb[i * 3 + 2];
        pixels[i].index = int(i);
    }

    std::vector<ColorBox> boxes;
    boxes.reserve(max_clusters);
    ColorBox root;
    root.begin = 0;
    root.end   = n;
    refresh_box(root, pixels);
    boxes.push_back(root);

    while (boxes.size() < max_clusters) {
        size_t best_i = 0;
        int    best_r = -1;
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].end - boxes[i].begin < 2)
                continue;
            const int r = box_range(boxes[i]);
            if (r > best_r) {
                best_r = r;
                best_i = i;
            }
        }
        if (best_r <= 0)
            break;

        ColorBox &src = boxes[best_i];
        const int ch  = box_longest_channel(src);
        std::sort(pixels.begin() + std::ptrdiff_t(src.begin), pixels.begin() + std::ptrdiff_t(src.end),
                  [ch](const RgbPixel &a, const RgbPixel &b) {
                      const int ca = channel_of(a, ch);
                      const int cb = channel_of(b, ch);
                      if (ca != cb)
                          return ca < cb;
                      return a.index < b.index;
                  });

        size_t mid = src.begin + (src.end - src.begin) / 2;
        if (mid == src.begin)
            mid = src.begin + 1;
        if (mid >= src.end)
            break;

        ColorBox left  = src;
        ColorBox right = src;
        left.end    = mid;
        right.begin = mid;
        refresh_box(left, pixels);
        refresh_box(right, pixels);
        src = left;
        boxes.push_back(right);
    }

    centroids.resize(boxes.size());
    for (size_t b = 0; b < boxes.size(); ++b) {
        std::uint64_t sr = 0, sg = 0, sb = 0;
        const size_t  cnt = boxes[b].end - boxes[b].begin;
        for (size_t i = boxes[b].begin; i < boxes[b].end; ++i) {
            sr += pixels[i].r;
            sg += pixels[i].g;
            sb += pixels[i].b;
            cluster_of[size_t(pixels[i].index)] = int(b);
        }
        if (cnt == 0) {
            centroids[b] = ColorRGB::BLACK();
            continue;
        }
        const auto avg = [cnt](std::uint64_t s) -> unsigned char {
            return static_cast<unsigned char>((s + cnt / 2) / cnt);
        };
        centroids[b] = ColorRGB(avg(sr), avg(sg), avg(sb));
    }
}

std::vector<ColorRGB> match_slots(const std::vector<ColorRGB> &physicals)
{
    const size_t n = physicals.size() < size_t(4) ? physicals.size() : size_t(4);
    std::vector<ColorRGB> slots;
    slots.reserve(n);
    for (size_t i = 0; i < n; ++i)
        slots.push_back(physicals[i]);
    return slots;
}

ColorRGB predicted_of_row(const std::string &row, const std::vector<ColorRGB> &slots)
{
    MixedFilamentManager mgr;
    mgr.load_definitions(row);
    if (mgr.enabled_count() == 0)
        return ColorRGB::BLACK();
    return predicted_swatch_for_mix(mgr.mixed_filaments().front(), slots, nullptr);
}

// Duplicate of 0048 bake collapse (MixedFilamentPaintBake.cpp). Do not change bake.
void collapse_recipe_rows_like_bake(
    std::vector<std::string>                            &recipe_rows,
    std::unordered_map<std::string, std::vector<size_t>> &recipe_sources,
    const std::vector<ColorRGB>                         &slots,
    size_t                                               mix_base)
{
    while (mix_base + recipe_rows.size() > SPECTRUM_PAINT_ID_PERSIST_CAP && recipe_rows.size() >= 2) {
        size_t best_i = 0;
        size_t best_j = 1;
        float  best_d = 1e30f;
        for (size_t i = 0; i < recipe_rows.size(); ++i) {
            const ColorRGB pi = predicted_of_row(recipe_rows[i], slots);
            for (size_t j = i + 1; j < recipe_rows.size(); ++j) {
                const float d = mixer_delta_e00(pi, predicted_of_row(recipe_rows[j], slots));
                if (d < best_d) {
                    best_d = d;
                    best_i = i;
                    best_j = j;
                }
            }
        }
        size_t keep = best_i;
        size_t drop = best_j;
        if (recipe_sources[recipe_rows[best_j]].size() > recipe_sources[recipe_rows[best_i]].size()) {
            keep = best_j;
            drop = best_i;
        }
        const std::string keep_row  = recipe_rows[keep];
        const std::string drop_row  = recipe_rows[drop];
        auto             &keep_srcs = recipe_sources[keep_row];
        const auto       &drop_srcs = recipe_sources[drop_row];
        keep_srcs.insert(keep_srcs.end(), drop_srcs.begin(), drop_srcs.end());
        recipe_sources.erase(drop_row);
        recipe_rows.erase(recipe_rows.begin() + int(drop));
    }
}

std::string serialize_recipe_rows(const std::vector<std::string> &recipe_rows)
{
    if (recipe_rows.empty())
        return {};
    std::string joined;
    for (size_t i = 0; i < recipe_rows.size(); ++i) {
        if (i)
            joined += ';';
        joined += recipe_rows[i];
    }
    MixedFilamentManager mgr;
    mgr.load_definitions(joined);
    return mgr.serialize_definitions();
}

std::vector<std::string> canonical_recipe_rows(const std::string &defs)
{
    MixedFilamentManager mgr;
    mgr.load_definitions(defs);
    const std::string canon = mgr.serialize_definitions();
    std::vector<std::string> rows;
    if (canon.empty())
        return rows;
    size_t start = 0;
    while (start < canon.size()) {
        const size_t semi = canon.find(';', start);
        if (semi == std::string::npos) {
            rows.push_back(canon.substr(start));
            break;
        }
        rows.push_back(canon.substr(start, semi - start));
        start = semi + 1;
    }
    return rows;
}

} // namespace

SpectrumPicPrintPlan plan_spectrum_picprint(
    const std::uint8_t *rgb, int w, int h,
    const std::vector<ColorRGB> &physicals,
    size_t mix_base,
    size_t max_clusters,
    const std::string &existing_mix_defs)
{
    SpectrumPicPrintPlan plan;
    if (rgb == nullptr || w <= 0 || h <= 0 || physicals.size() < 2) {
        plan.error = "PicPrint needs a non-empty RGB buffer and at least two physical filament colours.";
        return plan;
    }
    if (max_clusters == 0)
        max_clusters = 1;
    if (max_clusters > SPECTRUM_PAINT_ID_PERSIST_CAP)
        max_clusters = SPECTRUM_PAINT_ID_PERSIST_CAP;

    std::vector<std::uint8_t> sampled;
    int nw = 0;
    int nh = 0;
    downsample_rgb(rgb, w, h, sampled, nw, nh);
    plan.width  = nw;
    plan.height = nh;

    std::vector<int>      cluster_of;
    std::vector<ColorRGB> centroids;
    median_cut(sampled, nw, nh, max_clusters, cluster_of, centroids);
    plan.cluster_count = centroids.size();

    const std::vector<ColorRGB> slots = match_slots(physicals);
    const size_t num_match_slots = slots.size();

    std::vector<unsigned> cluster_dest(centroids.size(), 1);
    std::vector<std::string> recipe_rows;
    std::unordered_map<std::string, std::vector<size_t>> recipe_sources;
    recipe_sources.reserve(centroids.size());

    for (size_t c = 0; c < centroids.size(); ++c) {
        const MixMatchResult match = match_printable_mix(centroids[c], slots, nullptr, 4, 70);
        if (!match.valid || (match.kind == MixMatchResult::Kind::Mix && match.recipe_row.empty())) {
            cluster_dest[c] = 1;
            continue;
        }
        if (match.kind == MixMatchResult::Kind::Physical) {
            unsigned pid = match.physical_id;
            if (pid < 1)
                pid = 1;
            if (num_match_slots > 0 && pid > num_match_slots)
                pid = unsigned(num_match_slots);
            cluster_dest[c] = pid;
            continue;
        }
        auto found = recipe_sources.find(match.recipe_row);
        if (found == recipe_sources.end()) {
            recipe_rows.push_back(match.recipe_row);
            recipe_sources.emplace(match.recipe_row, std::vector<size_t>{c});
        } else {
            found->second.push_back(c);
        }
    }

    if (mix_base + recipe_rows.size() > SPECTRUM_PAINT_ID_PERSIST_CAP)
        collapse_recipe_rows_like_bake(recipe_rows, recipe_sources, slots, mix_base);

    std::vector<std::string> merged = canonical_recipe_rows(existing_mix_defs);
    for (const std::string &row : recipe_rows) {
        const std::string canon_row = serialize_recipe_rows({row});
        if (canon_row.empty())
            continue;
        bool found = false;
        for (const std::string &have : merged) {
            if (have == canon_row) {
                found = true;
                break;
            }
        }
        if (!found)
            merged.push_back(canon_row);
    }
    if (mix_base + merged.size() > SPECTRUM_PAINT_ID_PERSIST_CAP) {
        plan.error = "PicPrint mix rows plus existing mixes exceed the paint persist cap (32).";
        return plan;
    }

    std::unordered_map<std::string, unsigned> recipe_to_dest;
    recipe_to_dest.reserve(merged.size());
    for (size_t i = 0; i < merged.size(); ++i)
        recipe_to_dest.emplace(merged[i], unsigned(mix_base + i + 1));

    for (size_t i = 0; i < recipe_rows.size(); ++i) {
        const std::string canon_row = serialize_recipe_rows({recipe_rows[i]});
        const auto dest_it = recipe_to_dest.find(canon_row);
        if (dest_it == recipe_to_dest.end())
            continue;
        const auto src_it = recipe_sources.find(recipe_rows[i]);
        if (src_it == recipe_sources.end())
            continue;
        for (size_t c : src_it->second)
            cluster_dest[c] = dest_it->second;
    }

    plan.mix_count = merged.size();
    plan.mixed_filament_definitions = serialize_recipe_rows(merged);

    const size_t pix = size_t(nw) * size_t(nh);
    plan.dest_id.resize(pix, 1);
    for (size_t i = 0; i < pix; ++i) {
        const int c = cluster_of[i];
        if (c >= 0 && size_t(c) < cluster_dest.size())
            plan.dest_id[i] = cluster_dest[size_t(c)];
    }

    plan.valid = true;
    return plan;
}

unsigned spectrum_picprint_sample_facet(const SpectrumPicPrintPlan &plan, double u, double v)
{
    if (!plan.valid || plan.width <= 0 || plan.height <= 0)
        return 0;
    const size_t expect = size_t(plan.width) * size_t(plan.height);
    if (plan.dest_id.size() != expect)
        return 0;
    const int px = picprint_pixel_x(u, plan.width);
    const int py = picprint_pixel_y(v, plan.height);
    return plan.dest_id[size_t(py) * size_t(plan.width) + size_t(px)];
}

bool spectrum_picprint_world_to_uv(const Vec3d &world, const BoundingBoxf3 &xy_bbox, double &u, double &v)
{
    const double dx = xy_bbox.max.x() - xy_bbox.min.x();
    const double dy = xy_bbox.max.y() - xy_bbox.min.y();
    if (dx < 1e-6 || dy < 1e-6)
        return false;
    u = (world.x() - xy_bbox.min.x()) / dx;
    v = (world.y() - xy_bbox.min.y()) / dy;
    return true;
}

std::string spectrum_collapse_mix_recipe_rows(
    std::vector<std::string> recipe_rows,
    const std::vector<ColorRGB> &physicals,
    size_t mix_base)
{
    std::vector<std::string> unique_rows;
    unique_rows.reserve(recipe_rows.size());
    std::unordered_map<std::string, std::vector<size_t>> recipe_sources;
    for (const std::string &row : recipe_rows) {
        if (row.empty())
            continue;
        if (recipe_sources.find(row) != recipe_sources.end())
            continue;
        unique_rows.push_back(row);
        recipe_sources.emplace(row, std::vector<size_t>{unique_rows.size() - 1});
    }
    const std::vector<ColorRGB> slots = match_slots(physicals);
    collapse_recipe_rows_like_bake(unique_rows, recipe_sources, slots, mix_base);
    return serialize_recipe_rows(unique_rows);
}

bool spectrum_picprint_apply_to_volume(ModelVolume &vol,
                                       const Transform3d &world,
                                       const BoundingBoxf3 &xy_bbox,
                                       const SpectrumPicPrintPlan &plan)
{
    if (!plan.valid)
        return false;

    const indexed_triangle_set &its = vol.mesh().its;
    TriangleSelector selector(vol.mesh());
    const int n = int(its.indices.size());
    for (int i = 0; i < n; ++i) {
        const stl_triangle_vertex_indices &tri = its.indices[size_t(i)];
        Vec3d centroid = Vec3d::Zero();
        for (int k = 0; k < 3; ++k)
            centroid += its.vertices[size_t(tri(k))].cast<double>();
        centroid /= 3.0;
        const Vec3d world_pt = world * centroid;
        double u = 0;
        double v = 0;
        if (!spectrum_picprint_world_to_uv(world_pt, xy_bbox, u, v))
            return false;
        unsigned dest = spectrum_picprint_sample_facet(plan, u, v);
        if (dest < 1)
            dest = 1;
        selector.set_facet(i, EnforcerBlockerType(dest));
    }
    vol.mmu_segmentation_facets.set(selector);
    return true;
}

} // namespace Slic3r
