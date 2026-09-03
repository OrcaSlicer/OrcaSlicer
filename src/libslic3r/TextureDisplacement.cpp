#include "TextureDisplacement.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>

#include "MeshBoolean.hpp"
#include "Model.hpp"
#include "PNGReadWrite.hpp"
#include "TriangleSelector.hpp"
#include "TextureBake/TextureBakeMesh.hpp"
#include "TextureBake/TextureBakePipeline.hpp"

namespace Slic3r {

bool DecodedHeightTexture::texel_tap(const Vec2f &uv, bool tile_enabled, TextureTileMethod tile_method,
                                     TexelTap &tap) const
{
    if (empty())
        return false;

    auto repeat01 = [](float x) {
        x = std::fmod(x, 1.f);
        return x < 0.f ? x + 1.f : x;
    };
    // Standard mirrored-repeat: reflect back and forth every other unit, so tile edges always
    // line up with themselves instead of jumping from one edge of the image to the other.
    auto mirrored_repeat01 = [](float x) {
        x = std::fmod(std::abs(x), 2.f);
        return x > 1.f ? 2.f - x : x;
    };

    if (!tile_enabled && (uv.x() < 0.f || uv.x() >= 1.f || uv.y() < 0.f || uv.y() >= 1.f))
        // Outside the single, non-repeating placement entirely: no texture there, not "smeared
        // edge pixel" - clamping the *coordinate* to [0, 1] would otherwise keep returning the
        // border row/column's height forever in every direction, stretching it out to infinity.
        return false;

    float u, v;
    if (!tile_enabled) {
        u = uv.x();
        v = uv.y();
    } else if (tile_method == TextureTileMethod::MirroredRepeat) {
        u = mirrored_repeat01(uv.x());
        v = mirrored_repeat01(uv.y());
    } else {
        u = repeat01(uv.x());
        v = repeat01(uv.y());
    }

    const float fx = u * float(width);
    const float fy = v * float(height);
    const int   x0 = std::clamp(int(std::floor(fx)), 0, width - 1);
    const int   y0 = std::clamp(int(std::floor(fy)), 0, height - 1);
    // Neighbour for bilinear filtering: wrap for tiling methods, clamp at the edge otherwise (a
    // repeating neighbour would incorrectly blend against the opposite edge of the image).
    tap.x0 = x0;
    tap.y0 = y0;
    tap.x1 = tile_enabled ? (x0 + 1) % width  : std::min(x0 + 1, width - 1);
    tap.y1 = tile_enabled ? (y0 + 1) % height : std::min(y0 + 1, height - 1);
    tap.tx = fx - std::floor(fx);
    tap.ty = fy - std::floor(fy);
    return true;
}

float DecodedHeightTexture::sample(const Vec2f &uv, bool tile_enabled, TextureTileMethod tile_method) const
{
    TexelTap tap;
    if (!texel_tap(uv, tile_enabled, tile_method, tap))
        return 0.f;
    const int   x0 = tap.x0, y0 = tap.y0, x1 = tap.x1, y1 = tap.y1;
    const float tx = tap.tx, ty = tap.ty;

    auto at = [this](int x, int y) { return float(pixels[size_t(y) * size_t(width) + size_t(x)]) / 255.f; };
    const float top    = at(x0, y0) * (1.f - tx) + at(x1, y0) * tx;
    const float bottom = at(x0, y1) * (1.f - tx) + at(x1, y1) * tx;
    return top * (1.f - ty) + bottom * ty;
}

Vec3f DecodedHeightTexture::sample_color(const Vec2f &uv, bool tile_enabled, TextureTileMethod tile_method) const
{
    TexelTap tap;
    if (!has_color() || !texel_tap(uv, tile_enabled, tile_method, tap))
        return Vec3f::Zero();

    auto at = [this](int x, int y) {
        const size_t i = (size_t(y) * size_t(width) + size_t(x)) * 3;
        return Vec3f(float(rgb[i]) / 255.f, float(rgb[i + 1]) / 255.f, float(rgb[i + 2]) / 255.f);
    };
    const Vec3f top    = at(tap.x0, tap.y0) * (1.f - tap.tx) + at(tap.x1, tap.y0) * tap.tx;
    const Vec3f bottom = at(tap.x0, tap.y1) * (1.f - tap.tx) + at(tap.x1, tap.y1) * tap.tx;
    return top * (1.f - tap.ty) + bottom * tap.ty;
}

namespace {
// Decoding a PNG (zlib inflate + defilter) is real work, and image_data never changes in place
// once assigned to a layer (a new texture always gets a brand new image_data), so the decoded
// result can be cached for the lifetime of that specific image_data allocation. This matters
// because the GUI's live preview calls decode_height_texture() again on every rebuild (every
// paint stroke / parameter tweak), which would otherwise re-decode the same unchanged bytes over
// and over. Keyed by a weak_ptr (not just the raw pointer) so a freed image_data's address being
// reused by an unrelated later allocation can never alias a stale cache entry: a weak_ptr to a
// destroyed object always fails to lock, forcing a correct re-decode instead of a false hit.
struct DecodedTextureCache
{
    std::mutex mutex;
    std::unordered_map<const void *, std::pair<std::weak_ptr<const std::vector<unsigned char>>, DecodedHeightTexture>> entries;
};
DecodedTextureCache g_decoded_texture_cache;
} // namespace

namespace {
// A few passes of a separable box blur approximate a Gaussian, cheaply. `radius` is in pixels; 0 is
// a no-op. Wraps at the edges so a tiling height map stays seamless after smoothing. Operates on the
// grayscale byte buffer in place.
void smooth_height_pixels(std::vector<uint8_t> &pixels, int width, int height, int radius)
{
    if (radius <= 0 || width <= 0 || height <= 0 || pixels.size() != size_t(width) * size_t(height))
        return;

    const int   window = 2 * radius + 1;
    const float inv     = 1.f / float(window);
    std::vector<uint8_t> tmp(pixels.size());

    for (int pass = 0; pass < 2; ++pass) { // two passes -> smoother than a single box
        // Horizontal.
        for (int y = 0; y < height; ++y) {
            const size_t row = size_t(y) * width;
            for (int x = 0; x < width; ++x) {
                float sum = 0.f;
                for (int k = -radius; k <= radius; ++k) {
                    int sx = x + k;
                    sx = (sx % width + width) % width; // wrap
                    sum += float(pixels[row + size_t(sx)]);
                }
                tmp[row + size_t(x)] = uint8_t(std::lround(sum * inv));
            }
        }
        // Vertical.
        for (int x = 0; x < width; ++x)
            for (int y = 0; y < height; ++y) {
                float sum = 0.f;
                for (int k = -radius; k <= radius; ++k) {
                    int sy = y + k;
                    sy = (sy % height + height) % height; // wrap
                    sum += float(tmp[size_t(sy) * width + size_t(x)]);
                }
                pixels[size_t(y) * width + size_t(x)] = uint8_t(std::lround(sum * inv));
            }
    }
}
} // namespace

DecodedHeightTexture decode_height_texture(const TextureDisplacementLayer &layer)
{
    DecodedHeightTexture result;
    if (layer.empty())
        return result;

    // The raw (unsmoothed) decode is what gets cached, keyed by the image_data allocation - decoding
    // a PNG is the expensive part and never changes for a given image. Smoothing is applied afterwards
    // to a throwaway copy, so moving the smoothing slider never invalidates the decode cache.
    const void *key = layer.image_data.get();
    bool        have_raw = false;
    {
        std::lock_guard<std::mutex> lock(g_decoded_texture_cache.mutex);
        auto it = g_decoded_texture_cache.entries.find(key);
        if (it != g_decoded_texture_cache.entries.end() && it->second.first.lock() == layer.image_data) {
            result   = it->second.second;
            have_raw = true;
        }
    }

    if (!have_raw) {
        const png::ReadBuf rbuf{ layer.image_data->data(), layer.image_data->size() };
        if (!png::is_png(rbuf))
            // PNG only. The GUI converts any other imported format (jpg, bmp, ...) on import, so this
            // code needs no dependency on wxWidgets/libjpeg to read arbitrary user images.
            return result;

        png::ImageGreyscale img;
        if (png::decode_png(rbuf, img) && img.cols > 0 && img.rows > 0) {
            // The shipped library, and anything imported before colour was kept.
            result.width  = int(img.cols);
            result.height = int(img.rows);
            result.pixels = std::move(img.buf);
        } else {
            // A colour source: keep the colour, and take the height from its luminance. The
            // coefficients are wxImage::ConvertToGreyscale()'s, which is what the importer used to
            // apply on the way in - so a texture that used to be flattened to grey at import time
            // displaces identically now that its colour is preserved.
            png::ImageColorscale col;
            if (!png::decode_colored_png(rbuf, col) || col.cols == 0 || col.rows == 0 ||
                col.bytes_per_pixel < 3)
                return result;

            const size_t n   = size_t(col.cols) * size_t(col.rows);
            const size_t bpp = size_t(col.bytes_per_pixel);
            result.width  = int(col.cols);
            result.height = int(col.rows);
            result.pixels.resize(n);
            result.rgb.resize(n * 3);
            for (size_t i = 0; i < n; ++i) {
                const uint8_t r = col.buf[i * bpp], g = col.buf[i * bpp + 1], b = col.buf[i * bpp + 2];
                result.rgb[i * 3]     = r;
                result.rgb[i * 3 + 1] = g;
                result.rgb[i * 3 + 2] = b;
                result.pixels[i] = uint8_t(std::lround(0.299 * r + 0.587 * g + 0.114 * b));
            }
        }

        std::lock_guard<std::mutex> lock(g_decoded_texture_cache.mutex);
        // Opportunistically drop entries for image_data that no longer exists anywhere, so the
        // cache doesn't grow without bound across many add/remove-texture cycles in a long session.
        auto &entries = g_decoded_texture_cache.entries;
        for (auto it = entries.begin(); it != entries.end();)
            it = it->second.first.expired() ? entries.erase(it) : std::next(it);
        entries[key] = { std::weak_ptr<const std::vector<unsigned char>>(layer.image_data), result };
    }

    // Smoothing radius scales with the texture so the effect is resolution-independent; capped so the
    // blur stays affordable on large maps.
    if (layer.smoothing > 0.f) {
        const int max_radius = std::clamp(int(std::lround(0.02f * std::min(result.width, result.height))), 1, 32);
        const int radius     = std::max(1, int(std::lround(layer.smoothing * float(max_radius))));
        smooth_height_pixels(result.pixels, result.width, result.height, radius);
        // Colour gets the same blur, per channel. It is the same knob for the same reason: detail in
        // the image finer than the mesh can carry is noise either way, and low-passing it here is the
        // cheapest place to remove it - one blur of the texture, rather than a fight per triangle.
        if (result.has_color()) {
            const size_t         n = size_t(result.width) * size_t(result.height);
            std::vector<uint8_t> channel(n);
            for (int c = 0; c < 3; ++c) {
                for (size_t i = 0; i < n; ++i)
                    channel[i] = result.rgb[i * 3 + size_t(c)];
                smooth_height_pixels(channel, result.width, result.height, radius);
                for (size_t i = 0; i < n; ++i)
                    result.rgb[i * 3 + size_t(c)] = channel[i];
            }
        }
    }
    return result;
}

Vec2f project_planar(const Vec3f &position, const Vec3f &normal)
{
    // Planar-project onto the two axes orthogonal to the dominant component of `normal`. Called
    // with each vertex's *own* normal (TextureProjectionMethod::Triplanar), this is a standard
    // tri-planar/cube projection; a patch spanning several differently-oriented faces gets each
    // face projected along its own best-fit axis instead of all faces sharing one axis picked
    // from a single averaged normal (which looks correct on one face but visibly distorts on any
    // other face in the same patch - exactly the bug an earlier version of this feature had).
    const Vec3f n = normal.cwiseAbs();
    if (n.x() >= n.y() && n.x() >= n.z())
        return Vec2f(position.y(), position.z());
    if (n.y() >= n.x() && n.y() >= n.z())
        return Vec2f(position.x(), position.z());
    return Vec2f(position.x(), position.y());
}

namespace {
// Wrapped around patch_axis, centered at patch_center. u is the arc length (mm) around the axis at
// this point's own radius, v is the signed distance along the axis - a reasonable approximation
// for roughly cylindrical selections, not an exact fit for arbitrary geometry.
Vec2f project_cylindrical(const Vec3f &position, const Vec3f &patch_center, const Vec3f &patch_axis)
{
    Vec3f up = patch_axis;
    up       = (up.norm() > 1e-8f) ? Vec3f(up.normalized()) : Vec3f::UnitZ();
    const Vec3f arbitrary = (std::abs(up.dot(Vec3f::UnitZ())) < 0.9f) ? Vec3f::UnitZ() : Vec3f::UnitX();
    const Vec3f right     = Vec3f(up.cross(arbitrary).normalized());
    const Vec3f fwd       = Vec3f(right.cross(up).normalized());

    const Vec3f rel        = position - patch_center;
    const float along_axis = rel.dot(up);
    const float x          = rel.dot(right);
    const float y          = rel.dot(fwd);
    const float radius     = std::sqrt(x * x + y * y);
    const float angle      = std::atan2(y, x);

    return Vec2f(angle * radius, along_axis);
}

// Longitude/latitude around patch_center. u/v are scaled by this point's own distance from the
// center so the result is in roughly the same mm-ish units tiling_scale expects, rather than bare
// radians - again an approximation, not an exact geodesic parametrization.
Vec2f project_spherical(const Vec3f &position, const Vec3f &patch_center)
{
    const Vec3f rel    = position - patch_center;
    const float radius = rel.norm();
    if (radius < 1e-8f)
        return Vec2f::Zero();

    const Vec3f dir       = rel / radius;
    const float longitude = std::atan2(dir.y(), dir.x());
    const float latitude  = std::asin(std::clamp(dir.z(), -1.f, 1.f));
    return Vec2f(longitude, latitude) * radius;
}

// CGAL's LSCM parameterizer expects a clean mesh with no isolated (unreferenced) vertices - but
// `patch` here (from TriangleSelector::get_facets_strict()) carries the *entire* mesh's vertex
// array, only its `indices` filtered to the painted triangles. Build a compacted copy referencing
// only the vertices `patch.indices` actually uses, plus a map back to the original vertex index so
// the resulting per-vertex UVs can be looked up by the caller's own (uncompacted) indexing.
indexed_triangle_set compact_patch_with_map(const indexed_triangle_set &patch, std::vector<int> &original_to_compact)
{
    original_to_compact.assign(patch.vertices.size(), -1);
    indexed_triangle_set compact;
    compact.vertices.reserve(patch.vertices.size());
    compact.indices.reserve(patch.indices.size());
    for (const stl_triangle_vertex_indices &tri : patch.indices) {
        stl_triangle_vertex_indices new_tri;
        for (int i = 0; i < 3; ++i) {
            const int vi = tri[i];
            if (original_to_compact[vi] < 0) {
                original_to_compact[vi] = int(compact.vertices.size());
                compact.vertices.push_back(patch.vertices[vi]);
            }
            new_tri[i] = original_to_compact[vi];
        }
        compact.indices.push_back(new_tri);
    }
    return compact;
}

} // namespace

namespace {

// Union-find over triangles, used to grow charts.
struct UnionFind
{
    std::vector<int> parent;
    explicit UnionFind(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    int find(int x)
    {
        while (parent[size_t(x)] != x) {
            parent[size_t(x)] = parent[size_t(parent[size_t(x)])]; // path halving
            x                 = parent[size_t(x)];
        }
        return x;
    }
    void unite(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a != b)
            parent[size_t(b)] = a;
    }
};

uint64_t undirected_edge_key(int a, int b)
{
    if (a > b)
        std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}

Vec3f face_normal(const indexed_triangle_set &mesh, const stl_triangle_vertex_indices &tri)
{
    const Vec3f n   = (mesh.vertices[tri[1]] - mesh.vertices[tri[0]]).cross(mesh.vertices[tri[2]] - mesh.vertices[tri[0]]);
    const float len = n.norm();
    return (len > 1e-12f) ? Vec3f(n / len) : Vec3f::UnitZ();
}

// Groups triangles into charts: two triangles sharing an edge join the same chart only if the angle
// between their normals is below `seam_angle_deg`. Everything sharper than that is a seam, and the
// unwrap gets cut there instead of being forced to flatten across it.
std::vector<int> segment_into_charts(const indexed_triangle_set &mesh, const std::vector<Vec3f> &normals,
                                      float seam_angle_deg, const std::unordered_set<uint64_t> &seam_keys,
                                      int &chart_count)
{
    const int   n_faces       = int(mesh.indices.size());
    const float cos_threshold = std::cos(std::clamp(seam_angle_deg, 0.f, 180.f) * float(M_PI) / 180.f);

    // Each shared edge, with the (up to two) faces on it.
    std::unordered_map<uint64_t, std::pair<int, int>> edge_faces;
    edge_faces.reserve(size_t(n_faces) * 3);
    for (int f = 0; f < n_faces; ++f) {
        const stl_triangle_vertex_indices &tri = mesh.indices[size_t(f)];
        for (int i = 0; i < 3; ++i) {
            const uint64_t key        = undirected_edge_key(tri[i], tri[(i + 1) % 3]);
            const auto [it, inserted] = edge_faces.emplace(key, std::make_pair(f, -1));
            if (!inserted)
                it->second.second = f;
        }
    }

    // The chart-join test compares a *neighbourhood-averaged* normal per face - the face plus its
    // edge-adjacent neighbours (~5 samples) - rather than the single face normal. On a finely
    // tessellated curved surface this stops one noisy triangle from spuriously cutting (or a lone
    // near-flat sliver from wrongly merging) a chart, while a genuine sharp crease, where the whole
    // neighbourhood on each side agrees, still cuts. This is the "use 5 points, not one" refinement.
    std::vector<Vec3f> smoothed(mesh.indices.size());
    for (int f = 0; f < n_faces; ++f) {
        Vec3f                              acc = normals[size_t(f)];
        const stl_triangle_vertex_indices &tri = mesh.indices[size_t(f)];
        for (int i = 0; i < 3; ++i) {
            const auto it = edge_faces.find(undirected_edge_key(tri[i], tri[(i + 1) % 3]));
            if (it == edge_faces.end())
                continue;
            const int nb = (it->second.first == f) ? it->second.second : it->second.first;
            if (nb >= 0)
                acc += normals[size_t(nb)];
        }
        smoothed[size_t(f)] = (acc.norm() > 1e-8f) ? Vec3f(acc.normalized()) : normals[size_t(f)];
    }

    UnionFind uf(mesh.indices.size());
    for (const auto &[key, fp] : edge_faces) {
        if (fp.second < 0)
            continue; // a boundary edge of the patch, nothing on the far side to join
        // A manually/auto marked seam always cuts, whatever the dihedral angle - that is exactly
        // what lets "mark seam" / "cut island" split a chart that is otherwise flat enough to merge.
        if (!seam_keys.empty() && seam_keys.count(key))
            continue;
        if (smoothed[size_t(fp.first)].dot(smoothed[size_t(fp.second)]) >= cos_threshold)
            uf.unite(fp.first, fp.second);
    }

    std::vector<int>             chart_of(mesh.indices.size(), -1);
    std::unordered_map<int, int> root_to_chart;
    chart_count = 0;
    for (int f = 0; f < n_faces; ++f) {
        const auto [it, inserted] = root_to_chart.emplace(uf.find(f), chart_count);
        if (inserted)
            ++chart_count;
        chart_of[size_t(f)] = it->second;
    }
    return chart_of;
}

// Projects a chart onto an orthonormal basis of its own average normal. This is *isometric* for a
// flat chart - lengths and angles come out exactly right - which is why a flat chart never needs a
// solve at all, and why this also serves as the fallback for a chart LSCM cannot handle.
std::vector<Vec2f> project_to_tangent_plane(const indexed_triangle_set &chart, const Vec3f &normal)
{
    const Vec3f seed = (std::abs(normal.z()) < 0.9f) ? Vec3f::UnitZ() : Vec3f::UnitX();
    const Vec3f u    = Vec3f(seed.cross(normal).normalized());
    const Vec3f v    = Vec3f(normal.cross(u).normalized());

    std::vector<Vec2f> uvs(chart.vertices.size());
    for (size_t i = 0; i < chart.vertices.size(); ++i)
        uvs[i] = Vec2f(chart.vertices[i].dot(u), chart.vertices[i].dot(v));
    return uvs;
}

float area_3d(const indexed_triangle_set &mesh)
{
    float area = 0.f;
    for (const stl_triangle_vertex_indices &t : mesh.indices)
        area += 0.5f * (mesh.vertices[t[1]] - mesh.vertices[t[0]]).cross(mesh.vertices[t[2]] - mesh.vertices[t[0]]).norm();
    return area;
}

float area_2d(const std::vector<Vec2f> &uvs, const std::vector<stl_triangle_vertex_indices> &indices)
{
    float area = 0.f;
    for (const stl_triangle_vertex_indices &t : indices) {
        const Vec2f e0 = uvs[size_t(t[1])] - uvs[size_t(t[0])];
        const Vec2f e1 = uvs[size_t(t[2])] - uvs[size_t(t[0])];
        area += 0.5f * std::abs(e0.x() * e1.y() - e0.y() * e1.x());
    }
    return area;
}

// FNV-1a over the patch's geometry plus the seam angle. The unwrap depends on nothing else about a
// layer - not depth, tiling, rotation, offset or even which texture is on it - so keying the cache
// on just this is what lets every one of those sliders be dragged without paying for a re-solve.
uint64_t unwrap_cache_key(const indexed_triangle_set &patch, float seam_angle_deg, float padding_mm,
                          const std::vector<std::pair<int, int>> &seam_edges)
{
    uint64_t   h   = 1469598103934665603ull;
    const auto mix = [&h](const void *data, size_t bytes) {
        const unsigned char *p = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    mix(patch.vertices.data(), patch.vertices.size() * sizeof(Vec3f));
    mix(patch.indices.data(), patch.indices.size() * sizeof(stl_triangle_vertex_indices));
    mix(&seam_angle_deg, sizeof(seam_angle_deg));
    mix(&padding_mm, sizeof(padding_mm));
    mix(seam_edges.data(), seam_edges.size() * sizeof(std::pair<int, int>));
    return h;
}

struct UnwrapCache
{
    std::mutex                                mutex;
    std::unordered_map<uint64_t, PatchUnwrap> entries;
};
UnwrapCache  g_unwrap_cache;
const size_t UNWRAP_CACHE_MAX_ENTRIES = 8;

} // namespace

PatchUnwrap compute_patch_unwrap(const indexed_triangle_set &patch, float seam_angle_deg, float padding_mm,
                                 const std::vector<std::pair<int, int>> &seam_edges)
{
    PatchUnwrap result;
    if (patch.indices.empty())
        return result;

    const uint64_t key = unwrap_cache_key(patch, seam_angle_deg, padding_mm, seam_edges);
    {
        std::lock_guard<std::mutex> lock(g_unwrap_cache.mutex);
        if (auto it = g_unwrap_cache.entries.find(key); it != g_unwrap_cache.entries.end())
            return it->second;
    }

    // CGAL wants a mesh with no unreferenced vertices; `patch` carries the whole mesh's vertex array
    // (see get_facets_strict()), so compact it and keep the map back to the caller's numbering.
    std::vector<int>           patch_to_compact;
    const indexed_triangle_set compact = compact_patch_with_map(patch, patch_to_compact);
    std::vector<int>           compact_to_patch(compact.vertices.size(), -1);
    for (size_t vi = 0; vi < patch_to_compact.size(); ++vi)
        if (patch_to_compact[vi] >= 0)
            compact_to_patch[size_t(patch_to_compact[vi])] = int(vi);

    std::vector<Vec3f> normals(compact.indices.size());
    for (size_t f = 0; f < compact.indices.size(); ++f)
        normals[f] = face_normal(compact, compact.indices[f]);

    // Seam edges arrive in patch (== mesh) vertex space; translate to the compact numbering the
    // segmentation runs in. An edge whose endpoints didn't both survive compaction is simply ignored.
    std::unordered_set<uint64_t> seam_keys;
    seam_keys.reserve(seam_edges.size() * 2);
    for (const auto &[a, b] : seam_edges) {
        if (a < 0 || b < 0 || size_t(a) >= patch_to_compact.size() || size_t(b) >= patch_to_compact.size())
            continue;
        const int ca = patch_to_compact[size_t(a)];
        const int cb = patch_to_compact[size_t(b)];
        if (ca >= 0 && cb >= 0)
            seam_keys.insert(undirected_edge_key(ca, cb));
    }

    // Once the user has marked seams by hand, those seams define the islands: the automatic
    // sharp-angle cutting must not keep splitting faces the user left un-seamed, or islands the user
    // meant to be one piece never merge (the "seam angle overrides it" the user hit). So when any
    // manual seam exists, the angle threshold is dropped to 180 degrees - nothing is cut except the
    // marked seams. With no manual seams, the seam angle behaves exactly as before.
    const float effective_seam_angle = seam_keys.empty() ? seam_angle_deg : 180.f;
    int                    chart_count = 0;
    const std::vector<int> chart_of    = segment_into_charts(compact, normals, effective_seam_angle, seam_keys, chart_count);

    struct FlatChart
    {
        std::vector<Vec2f>                       uvs;
        std::vector<int>                         to_patch; // chart vertex -> patch vertex
        std::vector<stl_triangle_vertex_indices> indices;  // chart-local
        Vec2f                                    min  = Vec2f::Zero();
        Vec2f                                    size = Vec2f::Zero();
    };
    // static_cast, not size_t(...): the latter is a most-vexing-parse and declares a function.
    std::vector<FlatChart> charts(static_cast<size_t>(chart_count));

    for (int c = 0; c < chart_count; ++c) {
        FlatChart &chart = charts[size_t(c)];

        // Build the chart's own sub-mesh. Each chart gets its *own* compact->local vertex map, which
        // is exactly what duplicates a seam vertex: it appears once in each chart that touches it,
        // free to hold a different UV in each.
        indexed_triangle_set chart_mesh;
        std::vector<int>     compact_to_local(compact.vertices.size(), -1);
        Vec3f                normal_sum = Vec3f::Zero();
        for (size_t f = 0; f < compact.indices.size(); ++f) {
            if (chart_of[f] != c)
                continue;
            const stl_triangle_vertex_indices &tri = compact.indices[f];
            // Area-weighted, so a chart's average normal isn't dragged around by slivers.
            normal_sum += (compact.vertices[tri[1]] - compact.vertices[tri[0]]).cross(compact.vertices[tri[2]] - compact.vertices[tri[0]]);

            stl_triangle_vertex_indices local_tri;
            for (int i = 0; i < 3; ++i) {
                const int cv = tri[i];
                if (compact_to_local[size_t(cv)] < 0) {
                    compact_to_local[size_t(cv)] = int(chart_mesh.vertices.size());
                    chart_mesh.vertices.push_back(compact.vertices[size_t(cv)]);
                    chart.to_patch.push_back(compact_to_patch[size_t(cv)]);
                }
                local_tri[i] = compact_to_local[size_t(cv)];
            }
            chart_mesh.indices.push_back(local_tri);
        }
        if (chart_mesh.indices.empty())
            continue;

        const Vec3f chart_normal = (normal_sum.norm() > 1e-12f) ? Vec3f(normal_sum.normalized()) : Vec3f::UnitZ();

        // Is the chart flat? Charts are grown by a *pairwise* angle threshold, so a chart can still
        // curve gradually across many triangles - being merged is not the same as being planar. But
        // when it is planar (a cube face, and after seam-cutting that is the common case), the
        // tangent-plane projection is already the exact answer, and skipping the solve is the single
        // biggest speed-up here.
        bool planar = true;
        for (size_t f = 0; f < compact.indices.size() && planar; ++f)
            if (chart_of[f] == c)
                planar = normals[f].dot(chart_normal) >= 0.9998f; // ~1 degree

        // Measured before chart_mesh.indices is moved out from under it, below - area_3d() iterates
        // those indices, so taking it afterwards silently measures an empty mesh and returns 0.
        const float mesh_area_3d = area_3d(chart_mesh);

        std::optional<std::vector<Vec2f>> uvs;
        if (!planar)
            uvs = MeshBoolean::cgal::parameterize_lscm(chart_mesh);
        // Flat chart, or one LSCM refused (not a topological disk - closed, or holed).
        chart.uvs = uvs ? std::move(*uvs) : project_to_tangent_plane(chart_mesh, chart_normal);
        chart.indices = std::move(chart_mesh.indices);

        // LSCM's output is only defined up to a similarity, so charts come back at arbitrary and
        // mutually inconsistent scales. Rescale each to its true surface area, so `Tile size (mm)`
        // means the same thing on every chart and the texture doesn't change density across a seam.
        const float uv_area = area_2d(chart.uvs, chart.indices);
        if (uv_area > 1e-12f && mesh_area_3d > 1e-12f) {
            const float scale = std::sqrt(mesh_area_3d / uv_area);
            for (Vec2f &uv : chart.uvs)
                uv *= scale;
        }

        Vec2f lo(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
        Vec2f hi(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
        for (const Vec2f &uv : chart.uvs) {
            lo = lo.cwiseMin(uv);
            hi = hi.cwiseMax(uv);
        }
        chart.min  = lo;
        chart.size = hi - lo;
    }

    // Shelf-pack the charts side by side so they don't overlap. Overlap would be harmless for a
    // repeating texture but wrong for a non-tiled (decal) one, and it makes the UV editor unreadable.
    float total_area = 0.f;
    float widest     = 0.f;
    for (const FlatChart &chart : charts) {
        total_area = total_area + chart.size.x() * chart.size.y();
        widest     = std::max(widest, chart.size.x());
    }
    const float shelf_width = std::max(widest, std::sqrt(std::max(total_area, 0.f)) * 1.4f);
    // Negative padding means auto: a small fraction of the packed size, which is scale-independent
    // and so does something sensible for a 5 mm patch and a 500 mm one alike.
    const float margin      = (padding_mm >= 0.f) ? padding_mm : std::max(shelf_width * 0.02f, 1e-4f);

    std::vector<int> order(charts.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), // tallest first: the usual way to keep shelves tight
              [&charts](int a, int b) { return charts[size_t(a)].size.y() > charts[size_t(b)].size.y(); });

    float cursor_x = 0.f, cursor_y = 0.f, row_height = 0.f;
    for (const int c : order) {
        FlatChart &chart = charts[size_t(c)];
        if (chart.uvs.empty())
            continue;
        if (cursor_x > 0.f && cursor_x + chart.size.x() > shelf_width) {
            cursor_x   = 0.f;
            cursor_y  += row_height + margin;
            row_height = 0.f;
        }
        const Vec2f translation = Vec2f(cursor_x, cursor_y) - chart.min;
        for (Vec2f &uv : chart.uvs)
            uv += translation;

        cursor_x  += chart.size.x() + margin;
        row_height = std::max(row_height, chart.size.y());
    }

    // Concatenate the packed charts into the flat, duplicated-vertex form PatchUnwrap describes.
    result.chart_centroid.assign(size_t(chart_count), Vec2f::Zero());
    for (int c = 0; c < chart_count; ++c) {
        FlatChart &chart = charts[size_t(c)];
        const int  base  = int(result.uvs.size());
        result.uvs.insert(result.uvs.end(), chart.uvs.begin(), chart.uvs.end());
        result.source_vertex.insert(result.source_vertex.end(), chart.to_patch.begin(), chart.to_patch.end());
        result.vertex_chart.insert(result.vertex_chart.end(), chart.uvs.size(), c);
        for (const stl_triangle_vertex_indices &tri : chart.indices)
            result.indices.emplace_back(tri[0] + base, tri[1] + base, tri[2] + base);

        if (!chart.uvs.empty()) {
            Vec2f sum = Vec2f::Zero();
            for (const Vec2f &uv : chart.uvs)
                sum += uv;
            result.chart_centroid[size_t(c)] = sum / float(chart.uvs.size());
        }
    }
    result.chart_count = chart_count;

    // Island outlines: an edge used by exactly one triangle. Charts have disjoint vertex sets (seam
    // vertices are duplicated), so an edge along a seam shows up once in each chart and is correctly
    // reported as a boundary of both.
    {
        std::unordered_map<uint64_t, int> edge_use;
        edge_use.reserve(result.indices.size() * 3);
        for (const stl_triangle_vertex_indices &tri : result.indices)
            for (int i = 0; i < 3; ++i)
                ++edge_use[undirected_edge_key(tri[i], tri[(i + 1) % 3])];
        for (const stl_triangle_vertex_indices &tri : result.indices)
            for (int i = 0; i < 3; ++i) {
                const int a = tri[i], b = tri[(i + 1) % 3];
                if (edge_use[undirected_edge_key(a, b)] == 1)
                    result.boundary_edges.emplace_back(a, b);
            }
    }

    {
        std::lock_guard<std::mutex> lock(g_unwrap_cache.mutex);
        // A patch changes on every paint stroke, so this would grow without bound over a session.
        // Nothing here is worth an LRU: the working set is "the patch I am editing right now".
        if (g_unwrap_cache.entries.size() >= UNWRAP_CACHE_MAX_ENTRIES)
            g_unwrap_cache.entries.clear();
        g_unwrap_cache.entries.emplace(key, result);
    }
    return result;
}

Eigen::Matrix<float, 2, 3> island_transform_matrix(int chart, const PatchUnwrap &unwrap, const std::vector<TextureIsland> &islands)
{
    Eigen::Matrix<float, 2, 3> m;
    m << 1.f, 0.f, 0.f,
         0.f, 1.f, 0.f;
    if (chart < 0 || size_t(chart) >= islands.size() || size_t(chart) >= unwrap.chart_centroid.size())
        return m; // no hand placement for this island: leave it where the packing put it

    const TextureIsland &island = islands[size_t(chart)];
    const Vec2f          centre = unwrap.chart_centroid[size_t(chart)];

    const float rad = island.rotation_deg * float(M_PI) / 180.f;
    const float s   = island.scale;
    const float cs  = std::cos(rad) * s;
    const float sn  = std::sin(rad) * s;

    // uv -> centre + R*S*(uv - centre) + offset, with the linear part and the constant part split
    // out so this can be handed to a shader as-is.
    Eigen::Matrix2f linear;
    linear << cs, -sn,
              sn,  cs;
    const Vec2f translation = centre + island.offset - linear * centre;

    m.block<2, 2>(0, 0) = linear;
    m.col(2)            = translation;
    return m;
}

Vec2f apply_island_transform(const Vec2f &uv, int chart, const PatchUnwrap &unwrap, const std::vector<TextureIsland> &islands)
{
    const Eigen::Matrix<float, 2, 3> m = island_transform_matrix(chart, unwrap, islands);
    return m.block<2, 2>(0, 0) * uv + m.col(2);
}

namespace {
// One chart's copy of a shared mesh edge: the uv-vertex indices of its two endpoints, ordered so that
// .first is always the lower-numbered base vertex (so two charts' copies line up by base vertex).
struct ChartEdge { int chart = -1; int uv_lo = -1; int uv_hi = -1; };

// base edge (lo,hi mesh vertex) -> every chart that has it on its boundary. A mesh edge shared by two
// charts is a seam between them and appears once in each chart's boundary (charts have disjoint uv
// vertices), so its entry lists both charts.
std::map<std::pair<int, int>, std::vector<ChartEdge>> build_shared_edges(const PatchUnwrap &u)
{
    std::map<std::pair<int, int>, std::vector<ChartEdge>> edges;
    for (const auto &[ua, ub] : u.boundary_edges) {
        if (ua < 0 || ub < 0 || size_t(ua) >= u.source_vertex.size() || size_t(ub) >= u.source_vertex.size())
            continue;
        const int ba = u.source_vertex[size_t(ua)], bb = u.source_vertex[size_t(ub)];
        if (ba < 0 || bb < 0 || ba == bb)
            continue;
        const int chart = (size_t(ua) < u.vertex_chart.size()) ? u.vertex_chart[size_t(ua)] : -1;
        if (chart < 0)
            continue;
        const std::pair<int, int> key{ std::min(ba, bb), std::max(ba, bb) };
        // Order the uv endpoints to match the base-vertex order in the key.
        ChartEdge e{ chart, ua, ub };
        if (u.source_vertex[size_t(ua)] != key.first)
            std::swap(e.uv_lo, e.uv_hi);
        edges[key].push_back(e);
    }
    return edges;
}

// Solve the placement that maps child's raw uv edge (rc0->rc1) onto the parent's placed edge
// (pp0->pp1): a rigid rotation about the child chart's centroid plus an offset, scale kept at 1 so the
// texel density is unchanged. cen is the child chart's centroid.
TextureIsland solve_edge_alignment(const Vec2f &rc0, const Vec2f &rc1, const Vec2f &pp0, const Vec2f &pp1, const Vec2f &cen)
{
    const Vec2f dP = pp1 - pp0, dC = rc1 - rc0;
    const float rho = std::atan2(dP.y(), dP.x()) - std::atan2(dC.y(), dC.x());
    const float cs = std::cos(rho), sn = std::sin(rho);
    const auto  rot = [&](const Vec2f &v) { return Vec2f(v.x() * cs - v.y() * sn, v.x() * sn + v.y() * cs); };

    TextureIsland island;
    island.scale        = 1.f;
    island.rotation_deg = rho * 180.f / float(M_PI);
    island.offset       = pp0 - (rot(rc0 - cen) + cen); // island_transform_matrix places rc0 exactly here
    return island;
}
} // namespace

bool join_chart_placement(const PatchUnwrap &unwrap, const std::vector<TextureIsland> &islands, int child, int parent,
                          TextureIsland &out_child)
{
    if (child < 0 || parent < 0 || child == parent || size_t(child) >= unwrap.chart_centroid.size())
        return false;
    const auto edges = build_shared_edges(unwrap);
    for (const auto &[base_edge, list] : edges) {
        const ChartEdge *pe = nullptr;
        const ChartEdge *ce = nullptr;
        for (const ChartEdge &e : list) {
            if (e.chart == parent) pe = &e;
            if (e.chart == child)  ce = &e;
        }
        if (pe == nullptr || ce == nullptr)
            continue;
        // Parent's *placed* edge; child's *raw* edge, matched endpoint-to-endpoint by base vertex.
        const Vec2f pp0 = apply_island_transform(unwrap.uvs[size_t(pe->uv_lo)], parent, unwrap, islands);
        const Vec2f pp1 = apply_island_transform(unwrap.uvs[size_t(pe->uv_hi)], parent, unwrap, islands);
        out_child = solve_edge_alignment(unwrap.uvs[size_t(ce->uv_lo)], unwrap.uvs[size_t(ce->uv_hi)], pp0, pp1,
                                          unwrap.chart_centroid[size_t(child)]);
        return true;
    }
    return false;
}

std::vector<TextureIsland> compute_connected_net(const PatchUnwrap &unwrap)
{
    std::vector<TextureIsland> islands(size_t(std::max(unwrap.chart_count, 0)));
    if (unwrap.chart_count <= 1)
        return islands;

    // Chart adjacency, with one representative shared edge per adjacent pair.
    const auto edges = build_shared_edges(unwrap);
    struct PairEdge { ChartEdge a, b; };
    std::map<std::pair<int, int>, PairEdge> pair_edge;
    std::vector<std::vector<int>>            adj(size_t(unwrap.chart_count));
    for (const auto &[base_edge, list] : edges) {
        for (size_t i = 0; i < list.size(); ++i)
            for (size_t j = i + 1; j < list.size(); ++j) {
                const int c1 = list[i].chart, c2 = list[j].chart;
                if (c1 == c2 || c1 < 0 || c2 < 0 || c1 >= unwrap.chart_count || c2 >= unwrap.chart_count)
                    continue;
                const std::pair<int, int> pk{ std::min(c1, c2), std::max(c1, c2) };
                if (pair_edge.count(pk))
                    continue; // keep the first shared edge as the fold line for this pair
                pair_edge[pk] = (c1 < c2) ? PairEdge{ list[i], list[j] } : PairEdge{ list[j], list[i] };
                adj[size_t(pk.first)].push_back(pk.second);
                adj[size_t(pk.second)].push_back(pk.first);
            }
    }

    std::vector<bool>  placed(size_t(unwrap.chart_count), false);
    std::vector<Vec2f> pmin(size_t(unwrap.chart_count)), pmax(size_t(unwrap.chart_count));
    const auto chart_bbox = [&](int c, Vec2f &lo, Vec2f &hi) {
        lo = Vec2f(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
        hi = Vec2f(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
        for (size_t i = 0; i < unwrap.uvs.size(); ++i)
            if (unwrap.vertex_chart[i] == c) {
                const Vec2f p = apply_island_transform(unwrap.uvs[i], c, unwrap, islands);
                lo = lo.cwiseMin(p);
                hi = hi.cwiseMax(p);
            }
    };
    const auto overlaps_placed = [&](int c, const Vec2f &lo, const Vec2f &hi) {
        // A small inset, so charts that merely share an edge (touching bboxes) aren't judged to overlap.
        const Vec2f eps = 0.02f * (hi - lo).cwiseAbs();
        for (int o = 0; o < unwrap.chart_count; ++o)
            if (placed[size_t(o)] && o != c &&
                lo.x() + eps.x() < pmax[size_t(o)].x() && hi.x() - eps.x() > pmin[size_t(o)].x() &&
                lo.y() + eps.y() < pmax[size_t(o)].y() && hi.y() - eps.y() > pmin[size_t(o)].y())
                return true;
        return false;
    };

    // BFS from chart 0 (kept at its packed position), unfolding each newly reached chart onto its parent.
    std::queue<int> q;
    placed[0] = true;
    chart_bbox(0, pmin[0], pmax[0]);
    q.push(0);
    while (!q.empty()) {
        const int p = q.front();
        q.pop();
        for (const int c : adj[size_t(p)]) {
            if (placed[size_t(c)])
                continue;
            const std::pair<int, int> pk{ std::min(p, c), std::max(p, c) };
            const auto it = pair_edge.find(pk);
            if (it == pair_edge.end())
                continue;
            const ChartEdge &pe = (p < c) ? it->second.a : it->second.b;
            const ChartEdge &ce = (p < c) ? it->second.b : it->second.a;
            const Vec2f pp0 = apply_island_transform(unwrap.uvs[size_t(pe.uv_lo)], p, unwrap, islands);
            const Vec2f pp1 = apply_island_transform(unwrap.uvs[size_t(pe.uv_hi)], p, unwrap, islands);
            islands[size_t(c)] = solve_edge_alignment(unwrap.uvs[size_t(ce.uv_lo)], unwrap.uvs[size_t(ce.uv_hi)], pp0,
                                                       pp1, unwrap.chart_centroid[size_t(c)]);

            Vec2f lo, hi;
            chart_bbox(c, lo, hi);
            if (overlaps_placed(c, lo, hi)) {
                islands[size_t(c)] = TextureIsland{}; // would collide: leave it where the packing put it
                continue;                             // and don't unfold its subtree off a packed chart
            }
            placed[size_t(c)] = true;
            pmin[size_t(c)]   = lo;
            pmax[size_t(c)]   = hi;
            q.push(c);
        }
    }
    return islands;
}

void average_island_scales(std::vector<TextureIsland> &islands)
{
    if (islands.empty())
        return;
    float sum = 0.f;
    for (const TextureIsland &island : islands)
        sum += island.scale;
    const float mean = sum / float(islands.size());
    for (TextureIsland &island : islands)
        island.scale = mean;
}

std::vector<Vec2f> compute_lscm_uvs(const indexed_triangle_set &patch, const TextureDisplacementLayer &layer)
{
    // Padding disabled (0), matching the UV editor: the packed islands the editor shows and the ones the
    // bake/preview samples must be laid out identically, or a hand placement made in the editor would
    // land somewhere else in the baked result.
    const PatchUnwrap unwrap = compute_patch_unwrap(patch, layer.lscm_seam_angle_deg, 0.f, layer.lscm_seam_edges);
    if (unwrap.empty())
        return {};

    // Manual per-vertex UV edits (UV editor Vertex/Edge modes) override the automatic raw unwrap
    // coordinate for a mesh vertex, before the island transform - so the edit rides along with any
    // island move/rotate exactly like the rest of the island. See TextureDisplacementLayer::
    // lscm_uv_overrides. Small (hand edits), so a plain map is ample.
    std::map<int, Vec2f> overrides;
    for (const auto &[mv, uv] : layer.lscm_uv_overrides)
        overrides[mv] = uv;

    // One UV per patch vertex: a seam vertex has several (one per chart it touches) and has to
    // settle on one, since it can only be displaced to a single position. See compute_lscm_uvs()'s
    // header comment - the surface stays watertight regardless.
    std::vector<Vec2f> per_vertex(patch.vertices.size(), Vec2f::Zero());
    std::vector<bool>  assigned(patch.vertices.size(), false);
    for (size_t i = 0; i < unwrap.uvs.size(); ++i) {
        const int pv = unwrap.source_vertex[i];
        if (pv >= 0 && !assigned[size_t(pv)]) {
            Vec2f      raw = unwrap.uvs[i];
            const auto it  = overrides.find(pv);
            if (it != overrides.end())
                raw = it->second;
            per_vertex[size_t(pv)] = apply_island_transform(raw, unwrap.vertex_chart[i], unwrap, layer.islands);
            assigned[size_t(pv)]   = true;
        }
    }
    return per_vertex;
}

Vec2f apply_uv_transform(const Vec2f &planar, const TextureDisplacementLayer &layer, float aspect)
{
    const float scale = (layer.tiling_scale > 1e-6f) ? (1.f / layer.tiling_scale) : 1.f;
    const Vec2f scaled = planar * scale;

    const float rad = layer.rotation_deg * float(M_PI) / 180.f;
    const float cs  = std::cos(rad);
    const float sn  = std::sin(rad);
    Vec2f       rotated(scaled.x() * cs - scaled.y() * sn, scaled.x() * sn + scaled.y() * cs);

    // Non-square textures. Without this the [0,1] square of uv covers the whole image whatever its
    // proportions, so a 2:1 image is squeezed into a square tile and every feature in it comes out
    // half as wide as it should be. `tiling_scale` is the tile's size along u; the tile is
    // `tiling_scale * height / width` mm along v, which is exactly what keeps texels square - so
    // dividing v by that extent is the same as multiplying it by width / height. A square texture has
    // aspect 1 and is untouched, which is why this changes nothing for the shipped library.
    //
    // Applied after the rotation, not before: scaling one axis of an already-rotated coordinate is a
    // shear, and doing it the other way round would make "Rotation" skew the pattern instead of
    // turning it.
    if (aspect > 0.f && aspect != 1.f)
        rotated.y() *= aspect;

    return rotated + layer.offset;
}

float blend_displacement(float accumulated, float value, TextureBlendMode mode)
{
    switch (mode) {
    case TextureBlendMode::Subtract: return accumulated - value;
    // Multiply/Divide scale rather than offset, so they take `value` as a factor relative to 1 mm
    // (see TextureBlendMode): a 1 mm-deep layer sampling a white texel is then exactly neutral.
    case TextureBlendMode::Multiply: return accumulated * value;
    case TextureBlendMode::Divide: {
        // Every height map has black regions, and a black texel samples to *exactly* zero - so this
        // divisor really does hit zero in ordinary use, not just in some contrived edge case. Floor
        // its magnitude: an unbounded 1/0 would not merely look wrong, it would fling vertices
        // thousands of mm away and poison the mesh's bounding box (and with it every plate/print
        // volume check downstream). The floor doubles as a cap on how far Divide can ever amplify
        // the relief beneath it - at most 1/0.05 = 20x.
        constexpr float min_divisor = 0.05f;
        const float     divisor     = (std::abs(value) < min_divisor) ? std::copysign(min_divisor, value < 0.f ? -1.f : 1.f) :
                                                                        value;
        return accumulated / divisor;
    }
    case TextureBlendMode::Add:
    default: return accumulated + value;
    }
}

bool project_uv_projective(const std::array<float, 12> &m, const Vec3f &position, Vec2f &uv)
{
    const float x = position.x(), y = position.y(), z = position.z();
    const float w = m[8] * x + m[9] * y + m[10] * z + m[11];
    // Strictly greater than zero: at w == 0 the point sits on the projector's plane and maps to
    // infinity, and at w < 0 it is behind the projector, where dividing yields a plausible-looking
    // but mirrored uv - the classic way a projected decal reappears on the back of a model.
    if (!(w > 1e-6f))
        return false;
    uv = Vec2f((m[0] * x + m[1] * y + m[2] * z + m[3]) / w, (m[4] * x + m[5] * y + m[6] * z + m[7]) / w);
    return true;
}

float sample_layer_height(const DecodedHeightTexture &texture, const TextureDisplacementLayer &layer,
                          const Vec3f &position, const Vec3f &normal,
                          const Vec3f &patch_center, const Vec3f &patch_axis, const Vec2f *lscm_uv)
{
    if (texture.empty())
        return 0.f;

    // width / height of the height map, so a non-square image keeps its proportions (see
    // apply_uv_transform()). Every projection except the projective "from view" one funnels through
    // here, so this one line is what makes them all aspect-correct.
    const float aspect = (texture.height > 0) ? float(texture.width) / float(texture.height) : 1.f;
    auto sample_at = [&](const Vec2f &planar) {
        return texture.sample(apply_uv_transform(planar, layer, aspect), layer.tile_enabled, layer.tile_method);
    };

    // Precomputed per-patch LSCM solve wins over the layer's own method (see the header): the
    // caller only passes it when the patch actually parameterized successfully, so a patch that
    // failed to unwrap falls through to the analytic methods below as its documented fallback.
    if (lscm_uv != nullptr)
        return sample_at(*lscm_uv);

    switch (layer.projection_method) {
    case TextureProjectionMethod::Cylindrical:
        return sample_at(project_cylindrical(position, patch_center, patch_axis));
    case TextureProjectionMethod::Spherical:
        return sample_at(project_spherical(position, patch_center));
    case TextureProjectionMethod::ViewProjected:
        if (layer.view_project_projective) {
            // Exact projective placement written by the projection-frame overlay. Sampled directly,
            // *without* apply_uv_transform(): the matrix already maps the window's border to the uv
            // unit square, so the tiling/rotation/offset controls would displace it off the frame
            // the user just aligned. A point behind the projector has no uv at all -> no height.
            Vec2f uv;
            if (!project_uv_projective(layer.view_project_matrix, position, uv))
                return 0.f;
            return texture.sample(uv, layer.tile_enabled, layer.tile_method);
        }
        // Flat projection onto the captured projector plane. Single-valued per point, so unlike
        // blended triplanar it is one sample, and it is what "project from view" places.
        return sample_at(Vec2f(position.dot(layer.view_project_right), position.dot(layer.view_project_up)));
    case TextureProjectionMethod::LSCM: // no usable unwrap for this patch - fall back to Triplanar
    case TextureProjectionMethod::Triplanar:
    default: break;
    }

    // Blended tri-planar: sample all three axis-aligned planes and cross-fade between them by the
    // normal's own components, instead of hard-switching to whichever single axis dominates. The
    // hard switch is what produced a visible seam wherever the dominant axis flips (see
    // TextureProjectionMethod::Triplanar); a weighted blend is continuous across that transition
    // by construction, since the weight of the axis being left behind falls smoothly to zero.
    Vec3f w = normal.cwiseAbs();
    w       = Vec3f(std::pow(w.x(), TRIPLANAR_BLEND_SHARPNESS), std::pow(w.y(), TRIPLANAR_BLEND_SHARPNESS),
                    std::pow(w.z(), TRIPLANAR_BLEND_SHARPNESS));
    const float w_sum = w.x() + w.y() + w.z();
    if (w_sum < 1e-8f)
        // Degenerate normal: no axis is meaningfully dominant, so no blend is meaningful either.
        return sample_at(Vec2f(position.x(), position.y()));
    w /= w_sum;

    // Each plane drops the axis it is named for, matching project_planar()'s own convention (which
    // the GUI's on-canvas placement gizmo also relies on).
    return w.x() * sample_at(Vec2f(position.y(), position.z())) +
           w.y() * sample_at(Vec2f(position.x(), position.z())) +
           w.z() * sample_at(Vec2f(position.x(), position.y()));
}

bool sample_layer_color(const DecodedHeightTexture &texture, const TextureDisplacementLayer &layer,
                        const Vec3f &position, const Vec3f &normal, Vec3f &out, const Vec3f &patch_center,
                        const Vec3f &patch_axis, const Vec2f *lscm_uv)
{
    if (!texture.has_color())
        return false;

    // Deliberately a transcription of sample_layer_height()'s dispatch rather than a shared template:
    // the two differ in what "nothing here" means. Height returns 0, which is a perfectly good height
    // (no displacement); colour has no such neutral value - black is a colour - so every path that
    // returns 0 there has to report false here instead, and the caller leaves the triangle uncoloured.
    const float aspect = (texture.height > 0) ? float(texture.width) / float(texture.height) : 1.f;
    auto sample_at = [&](const Vec2f &planar) {
        return texture.sample_color(apply_uv_transform(planar, layer, aspect), layer.tile_enabled,
                                    layer.tile_method);
    };
    // Outside a non-tiled placement there is no texture at all - the same hard edge sample() gives the
    // height. Checked explicitly because sample_color() reports it as black, which is a real colour.
    auto covered = [&](const Vec2f &planar) {
        if (layer.tile_enabled)
            return true;
        const Vec2f uv = apply_uv_transform(planar, layer, aspect);
        return uv.x() >= 0.f && uv.x() < 1.f && uv.y() >= 0.f && uv.y() < 1.f;
    };

    if (lscm_uv != nullptr) {
        if (!covered(*lscm_uv))
            return false;
        out = sample_at(*lscm_uv);
        return true;
    }

    switch (layer.projection_method) {
    case TextureProjectionMethod::Cylindrical: {
        const Vec2f p = project_cylindrical(position, patch_center, patch_axis);
        if (!covered(p))
            return false;
        out = sample_at(p);
        return true;
    }
    case TextureProjectionMethod::Spherical: {
        const Vec2f p = project_spherical(position, patch_center);
        if (!covered(p))
            return false;
        out = sample_at(p);
        return true;
    }
    case TextureProjectionMethod::ViewProjected:
        if (layer.view_project_projective) {
            // The frame's own rectangle is the placement, so no apply_uv_transform() - see
            // sample_layer_height(). A point behind the projector has no uv, hence no colour.
            Vec2f uv;
            if (!project_uv_projective(layer.view_project_matrix, position, uv))
                return false;
            if (!layer.tile_enabled && (uv.x() < 0.f || uv.x() >= 1.f || uv.y() < 0.f || uv.y() >= 1.f))
                return false;
            out = texture.sample_color(uv, layer.tile_enabled, layer.tile_method);
            return true;
        } else {
            const Vec2f p(position.dot(layer.view_project_right), position.dot(layer.view_project_up));
            if (!covered(p))
                return false;
            out = sample_at(p);
            return true;
        }
    case TextureProjectionMethod::LSCM: // no usable unwrap for this patch - fall back to Triplanar
    case TextureProjectionMethod::Triplanar:
    default: break;
    }

    // Blended tri-planar, weighted exactly as the height is, so colour and relief stay registered
    // across the cross-fade band at a 90-degree edge.
    Vec3f w = normal.cwiseAbs();
    w       = Vec3f(std::pow(w.x(), TRIPLANAR_BLEND_SHARPNESS), std::pow(w.y(), TRIPLANAR_BLEND_SHARPNESS),
                    std::pow(w.z(), TRIPLANAR_BLEND_SHARPNESS));
    const float w_sum = w.x() + w.y() + w.z();
    if (w_sum < 1e-8f) {
        const Vec2f p(position.x(), position.y());
        if (!covered(p))
            return false;
        out = sample_at(p);
        return true;
    }
    w /= w_sum;

    // A blend of three planes is only "not covered" where *every* contributing plane is outside the
    // placement; where some are, the covered ones are renormalised so the colour does not fade toward
    // black at the edge of an untiled tri-planar layer.
    const std::array<Vec2f, 3> planes = { Vec2f(position.y(), position.z()), Vec2f(position.x(), position.z()),
                                          Vec2f(position.x(), position.y()) };
    Vec3f acc     = Vec3f::Zero();
    float acc_w   = 0.f;
    for (int i = 0; i < 3; ++i)
        if (w[i] > 0.f && covered(planes[size_t(i)])) {
            acc   += w[i] * sample_at(planes[size_t(i)]);
            acc_w += w[i];
        }
    if (acc_w <= 0.f)
        return false;
    out = acc / acc_w;
    return true;
}

indexed_triangle_set extract_painted_patch(const indexed_triangle_set                    &base_mesh,
                                            const TriangleSelector::TriangleSplittingData &facet_data)
{
    if (facet_data.triangles_to_split.empty())
        return {};

    const TriangleMesh selector_mesh(base_mesh);
    TriangleSelector    selector(selector_mesh);
    selector.deserialize(facet_data, false);
    return selector.get_facets_strict(EnforcerBlockerType::ENFORCER);
}

bool compute_layer_paint_anchor(const indexed_triangle_set                    &base_mesh,
                                 const TriangleSelector::TriangleSplittingData &facet_data,
                                 Vec3f                                         &anchor_pos,
                                 Vec3f                                         &anchor_normal)
{
    const indexed_triangle_set patch = extract_painted_patch(base_mesh, facet_data);
    if (patch.indices.empty())
        return false;

    Vec3f  centroid_sum = Vec3f::Zero();
    Vec3f  normal_sum   = Vec3f::Zero();
    for (const stl_triangle_vertex_indices &tri : patch.indices) {
        const Vec3f &a = patch.vertices[tri[0]];
        const Vec3f &b = patch.vertices[tri[1]];
        const Vec3f &c = patch.vertices[tri[2]];
        // Cross product magnitude is twice the face area, so this area-weights both sums the same
        // way texture_displacement_vertex_normals() does above.
        normal_sum   += (b - a).cross(c - a);
        centroid_sum += (a + b + c) / 3.f;
    }

    anchor_pos    = centroid_sum / float(patch.indices.size());
    anchor_normal = (normal_sum.norm() > 1e-8f) ? Vec3f(normal_sum.normalized()) : Vec3f::UnitZ();
    return true;
}

// Area-weighted vertex normals of the undisplaced mesh. build_texture_displacement() computes
// these once, up front, and every layer both projects and displaces along them - so a vertex
// covered by several layers is pushed along one single, well-defined direction rather than along
// whatever direction the surface happened to be pointing partway through the stack.
static std::vector<Vec3f> texture_displacement_vertex_normals(const indexed_triangle_set &its)
{
    std::vector<Vec3f> normals(its.vertices.size(), Vec3f::Zero());
    for (const stl_triangle_vertex_indices &tri : its.indices) {
        const Vec3f &a = its.vertices[tri[0]];
        const Vec3f &b = its.vertices[tri[1]];
        const Vec3f &c = its.vertices[tri[2]];
        // Cross product magnitude is twice the face area, so this naturally area-weights the
        // contribution of each incident face to its vertices.
        const Vec3f area_weighted_normal = (b - a).cross(c - a);
        normals[tri[0]] += area_weighted_normal;
        normals[tri[1]] += area_weighted_normal;
        normals[tri[2]] += area_weighted_normal;
    }
    for (Vec3f &n : normals) {
        const float len = n.norm();
        n = (len > 1e-8f) ? Vec3f(n / len) : Vec3f::UnitZ();
    }
    return normals;
}

namespace {
// Shortest along-surface distance from every patch vertex to the patch boundary (the pinned vertices
// shared with the untouched surface), by a multi-source Dijkstra over the patch edges. Used for edge
// smoothing: the displacement is faded out as this distance goes to zero.
std::vector<float> patch_boundary_distance(const indexed_triangle_set &patch, const std::vector<bool> &is_boundary)
{
    const size_t n = patch.vertices.size();
    std::vector<std::vector<std::pair<int, float>>> adj(n);
    for (const stl_triangle_vertex_indices &tri : patch.indices)
        for (int i = 0; i < 3; ++i) {
            const int a = tri[i], b = tri[(i + 1) % 3];
            if (a < 0 || b < 0 || size_t(a) >= n || size_t(b) >= n)
                continue;
            const float w = (patch.vertices[size_t(a)] - patch.vertices[size_t(b)]).norm();
            adj[size_t(a)].push_back({ b, w });
            adj[size_t(b)].push_back({ a, w });
        }

    std::vector<float> dist(n, std::numeric_limits<float>::infinity());
    using QN = std::pair<float, int>;
    std::priority_queue<QN, std::vector<QN>, std::greater<QN>> pq;
    for (size_t v = 0; v < n; ++v)
        if (v < is_boundary.size() && is_boundary[v]) {
            dist[v] = 0.f;
            pq.push({ 0.f, int(v) });
        }
    while (!pq.empty()) {
        const auto [d, u] = pq.top();
        pq.pop();
        if (d > dist[size_t(u)])
            continue;
        for (const auto &[w, ew] : adj[size_t(u)]) {
            const float nd = d + ew;
            if (nd < dist[size_t(w)]) {
                dist[size_t(w)] = nd;
                pq.push({ nd, w });
            }
        }
    }
    return dist;
}
} // namespace

namespace {
// Majority filter over face adjacency: each triangle takes the most common colour among itself and
// the (up to three) triangles across its edges. Ties, and a triangle whose own colour is already the
// most common, keep what they had - so the filter only ever removes a facet that disagrees with its
// whole neighbourhood, and cannot drift a large region.
//
// Read from a snapshot of the previous pass, so the result does not depend on triangle order.
// Uncoloured triangles (-1) neither vote nor get voted on: the paint boundary is not noise.
void despeckle_triangle_colors(const indexed_triangle_set &mesh, std::vector<int> &color, int passes)
{
    if (passes <= 0 || color.size() != mesh.indices.size())
        return;
    const std::vector<Vec3i32> neighbors = its_face_neighbors(mesh);
    if (neighbors.size() != mesh.indices.size())
        return;

    std::vector<int> prev;
    for (int pass = 0; pass < passes; ++pass) {
        prev = color;
        tbb::parallel_for(tbb::blocked_range<size_t>(0, color.size()),
                          [&](const tbb::blocked_range<size_t> &range) {
            for (size_t i = range.begin(); i < range.end(); ++i) {
                if (prev[i] < 0)
                    continue;
                // At most four candidates (self plus three neighbours), so counting by a linear scan
                // is cheaper than any map.
                int  cand[4]  = { prev[i], -1, -1, -1 };
                int  count[4] = { 1, 0, 0, 0 };
                int  n        = 1;
                for (int e = 0; e < 3; ++e) {
                    const int nb = neighbors[i][e];
                    if (nb < 0 || size_t(nb) >= prev.size() || prev[size_t(nb)] < 0)
                        continue;
                    const int c = prev[size_t(nb)];
                    int       k = 0;
                    for (; k < n; ++k)
                        if (cand[k] == c) {
                            ++count[k];
                            break;
                        }
                    if (k == n && n < 4) {
                        cand[n]  = c;
                        count[n] = 1;
                        ++n;
                    }
                }
                // Strictly greater, so a tie leaves the triangle alone.
                int best = 0;
                for (int k = 1; k < n; ++k)
                    if (count[k] > count[best])
                        best = k;
                if (count[best] > count[0])
                    color[i] = cand[best];
            }
        });
    }
}
} // namespace

namespace {

// Wired to the same layer stack via make_combined_displacement_sampler(), so layers, blend modes and
// projections behave identically in both paths and the comparison is between the meshing strategies.
indexed_triangle_set build_texture_displacement_v2(const indexed_triangle_set                  &mesh,
                                                   const std::vector<TextureDisplacementLayer> &layers,
                                                   const TextureDisplacementFacetsData         &facets_data,
                                                   const TextureDisplacementOptions            &options,
                                                   const DisplacementProgressFn                &progress)
{
    HeightFieldSampler combined = make_combined_displacement_sampler(mesh, layers, facets_data);
    if (!combined)
        return mesh; // nothing decodable to displace with

    // Unpainted triangles are excluded, keeping them out of refinement and pinned thereafter.
    std::vector<uint8_t> excluded(mesh.indices.size(), 1);
    {
        const TriangleMesh selector_mesh(mesh);
        TriangleSelector   selector(selector_mesh);
        bool               dirty = false;
        for (const TriangleSelector::TriangleSplittingData &data : facets_data) {
            if (data.triangles_to_split.empty())
                continue;
            selector.deserialize(data, dirty);
            dirty = true;
            std::vector<int> piece_src;
            const indexed_triangle_set patch =
                selector.get_facets_strict(EnforcerBlockerType::ENFORCER, &piece_src);
            for (const int src : piece_src)
                if (src >= 0 && size_t(src) < excluded.size())
                    excluded[size_t(src)] = 0;
        }
    }
    if (std::all_of(excluded.begin(), excluded.end(), [](uint8_t e) { return e != 0; }))
        return mesh; // nothing painted

    TextureBake::PipelineSettings settings;
    settings.refine_length = std::max(0.01f, options.v2_refine_mm);
    settings.regularize    = options.v2_regularize;
    settings.max_triangles = size_t(std::max(0, options.v2_max_triangles_k)) * 1000;
    settings.preserve_untextured = true;
    // The sampler already returns millimetres, so the displacement stage must not scale it again.
    settings.displace.amplitude = 1.f;
    settings.displace.symmetric = false;
    // The paint decides what moves here, so the angle limits stay off.
    settings.displace.bottom_angle_limit = 0.f;
    settings.displace.top_angle_limit    = 0.f;

    TextureBake::DisplaceBounds bounds;
    bounds.min = bounds.max = mesh.vertices.empty() ? Vec3f::Zero() : mesh.vertices.front();
    for (const Vec3f &v : mesh.vertices) {
        bounds.min = bounds.min.cwiseMin(v);
        bounds.max = bounds.max.cwiseMax(v);
    }

    const auto sample = [&combined](const Vec3f &pos, const Vec3f &smooth_normal, const Vec3f &) {
        // The smooth normal: the direction the vertex actually moves along.
        return combined(pos, smooth_normal);
    };

    // 0 means no simplification, i.e. Bake mode.
    const TextureBake::PipelineMode mode = settings.max_triangles > 0 ? TextureBake::PipelineMode::Export
                                                                      : TextureBake::PipelineMode::Bake;
    TextureBake::PipelineResult result = TextureBake::run_pipeline(
        TextureBake::to_soup(mesh, excluded), sample, settings, bounds, mode, excluded,
        [&progress](const char *, double f) {
            return !progress || progress(std::clamp(int(f * 100.0), 0, 99));
        });
    if (result.canceled || result.geometry.empty())
        return {};

    indexed_triangle_set out = TextureBake::to_indexed_triangle_set(result.geometry);
    return out.indices.empty() ? mesh : out;
}

} // namespace

indexed_triangle_set build_texture_displacement(const indexed_triangle_set                  &base_mesh,
                                                 const std::vector<TextureDisplacementLayer> &layers,
                                                 const TextureDisplacementFacetsData         &facets_data,
                                                 const TextureDisplacementOptions            &options,
                                                 const DisplacementProgressFn                &progress,
                                                 const TextureColorRequest                   *color)
{
    // Returns true to keep going. An aborted run returns {} (see the header): an empty mesh is the
    // one result no caller can mistake for a finished bake and commit onto the volume.
    const auto report = [&progress](int percent) { return !progress || progress(percent); };

    indexed_triangle_set mesh = base_mesh;
    // TriangleSelector's vertex array starts with the mesh's own vertices (any extra ones, created
    // where a brush stroke split a triangle, are appended after them), and get_facets_strict()
    // emits exactly the *referenced* ones, in order. So selector vertex index i is our vertex i -
    // but only if every vertex of `mesh` is referenced by some triangle, which is precisely what
    // this call establishes. It is a no-op (indices untouched) for any mesh that already is, which
    // in practice is all of them; it exists so an input carrying stray unreferenced vertices can't
    // silently shift the indexing and displace the wrong vertices.
    its_compactify_vertices(mesh);
    if (mesh.vertices.empty() || mesh.indices.empty())
        return mesh;

    if (options.pipeline_v2)
        return build_texture_displacement_v2(mesh, layers, facets_data, options, progress);

    // Layers are combined in slot order, like stacked layers in an image editor: each one folds its
    // own displacement into the running total via its blend mode (see TextureBlendMode).
    std::vector<const TextureDisplacementLayer *> ordered_layers;
    for (const TextureDisplacementLayer &layer : layers)
        if (!layer.empty() && layer.slot >= 0 && size_t(layer.slot) < TEXTURE_DISPLACEMENT_MAX_LAYERS)
            ordered_layers.push_back(&layer);
    std::sort(ordered_layers.begin(), ordered_layers.end(),
               [](const TextureDisplacementLayer *a, const TextureDisplacementLayer *b) { return a->slot < b->slot; });

    // Every layer measures its displacement against the *original* surface - normals included -
    // rather than against whatever the previous layer left behind. That is what lets all the layers
    // be evaluated independently and merged per vertex, instead of having to re-mesh and remap the
    // paint masks between them (see the header for why that earlier design was dropped).
    std::vector<Vec3f> vertex_normals = texture_displacement_vertex_normals(mesh);

    // ... with one correction, applied where the painted area does not cover every triangle around a
    // vertex: there the direction to move in is the normal of the *painted* surface, not of the whole
    // mesh. On the rim of a fully painted top face the whole-mesh normal is the 45 degrees bisector
    // between the face and the side wall it meets, so displacing along it flares the rim outwards
    // instead of raising it. Taken over the union of every layer's paint (triangles_to_split is
    // exactly the set of original triangles a layer's brush touched - serialize() records an entry for
    // each one that is split or carries a non-default state), so a vertex still has one single
    // direction however many layers cover it. Interior vertices are unaffected: all their triangles
    // are painted, so the two normals coincide.
    {
        std::vector<uint8_t> painted_face(mesh.indices.size(), 0);
        bool                 any_paint = false;
        for (const TextureDisplacementLayer *layer : ordered_layers)
            for (const TriangleSelector::TriangleBitStreamMapping &m : facets_data[size_t(layer->slot)].triangles_to_split)
                if (size_t(m.triangle_idx) < mesh.indices.size()) {
                    painted_face[size_t(m.triangle_idx)] = 1;
                    any_paint                            = true;
                }
        if (any_paint) {
            std::vector<Vec3f> painted_normals(mesh.vertices.size(), Vec3f::Zero());
            for (size_t i = 0; i < mesh.indices.size(); ++i) {
                if (!painted_face[i])
                    continue;
                const stl_triangle_vertex_indices &t = mesh.indices[i];
                const Vec3f fn = (mesh.vertices[t[1]] - mesh.vertices[t[0]]).cross(mesh.vertices[t[2]] - mesh.vertices[t[0]]);
                for (int k = 0; k < 3; ++k)
                    painted_normals[size_t(t[k])] += fn; // area-weighted, same convention as the full normals
            }
            for (size_t v = 0; v < vertex_normals.size(); ++v)
                if (const float l = painted_normals[v].norm(); l > 1e-8f)
                    vertex_normals[v] = painted_normals[v] / l;
                // else: no painted triangle touches this vertex, so it will not be displaced anyway -
                // leave the whole-mesh normal in place rather than zeroing it.
        }
    }

    if (!report(5))
        return {};

    std::vector<float> displacement(mesh.vertices.size(), 0.f);
    // uint8_t rather than std::vector<bool>: the sampling loop below writes these from several
    // threads at once, and vector<bool>'s bit packing makes writes to *distinct* elements a data
    // race on the shared word.
    std::vector<uint8_t> displaced(mesh.vertices.size(), 0);
    // Union, over every layer, of that layer's patch border - the vertices the post-process smoothing
    // holds when TextureDisplacementOptions::smooth_skip_border is set. A vertex on any patch's edge
    // counts, which is the conservative choice: hold it rather than let one layer's smoothing melt the
    // rim another layer put there.
    std::vector<bool>  on_patch_border(mesh.vertices.size(), false);
    bool               any_displacement = false;

    // Colour is accumulated per *triangle*, not per vertex: it ends up in the volume's
    // mmu_segmentation_facets, which assigns one filament to a whole facet. Layers are visited in
    // ascending slot order, so a higher layer simply overwrites a lower one's colour where they
    // overlap - the painter's-algorithm reading of a layer stack, and the one that matches how the
    // panel lists them.
    const bool       want_color = color != nullptr && color->out_triangle != nullptr && bool(color->quantize);
    // Palette indices, not filament indices: -1 for "no colour here". Kept in perceived-colour space
    // for the whole pass so the despeckle filter below operates on what the eye sees, and the
    // interleaving that turns a mixed entry into two real filaments happens once, at the very end.
    std::vector<int> triangle_palette;
    if (want_color)
        triangle_palette.assign(mesh.indices.size(), -1);

    const TriangleMesh selector_mesh(mesh);
    // One selector for the whole stack, re-deserialized per layer. Its constructor computes
    // its_face_neighbors() and its_face_normals() over the *entire* mesh, which on a subdivided model
    // is by far the most expensive thing here - building a fresh one per layer paid that cost up to
    // eight times over. reset() (what deserialize(..., true) calls) only rebuilds the vertex/triangle
    // arrays; the neighbour and face-normal tables are immutable members and survive it.
    TriangleSelector selector(selector_mesh);
    bool             selector_dirty = false;

    const int layer_count = std::max(int(ordered_layers.size()), 1);
    int       layer_index = 0;
    for (const TextureDisplacementLayer *layer : ordered_layers) {
        // Progress spans 5..65% across the layers; the apply and smoothing passes take it from there.
        if (!report(5 + (60 * layer_index++) / layer_count))
            return {};

        const TriangleSelector::TriangleSplittingData &data = facets_data[size_t(layer->slot)];
        if (data.triangles_to_split.empty())
            continue;

        const DecodedHeightTexture height = decode_height_texture(*layer);
        if (height.empty())
            continue;

        // needs_reset only from the second layer on: the selector is already pristine on the first.
        selector.deserialize(data, selector_dirty);
        selector_dirty = true;

        const bool       color_this_layer = want_color && layer->color_enabled;
        std::vector<int> patch_source; // sub-triangle -> base mesh triangle, only built when colouring
        const indexed_triangle_set patch =
            selector.get_facets_strict(EnforcerBlockerType::ENFORCER, color_this_layer ? &patch_source : nullptr);
        if (patch.indices.empty())
            continue;
        // get_facets_strict() returns the same vertex array whichever state is asked for (only the
        // triangles are filtered), so `patch` and `rest` share one indexing - and, per the
        // compactify above, it is our own.
        const indexed_triangle_set rest = selector.get_facets_strict(EnforcerBlockerType::NONE);

        // A vertex used by even one *unpainted* triangle sits on this layer's boundary. It is still
        // needed either way - the edge-smoothing falloff measures distance from it - but whether it is
        // held flat is now the user's call (TextureDisplacementOptions::displace_border), because
        // nothing can tear: the bake is topology-preserving, so a border vertex is one vertex shared
        // by both regions and moving it just tilts the unpainted triangles that use it.
        std::vector<bool> is_boundary(patch.vertices.size(), false);
        for (const stl_triangle_vertex_indices &tri : rest.indices)
            for (int i = 0; i < 3; ++i) {
                is_boundary[tri[i]] = true;
                if (tri[i] < int(mesh.vertices.size()))
                    on_patch_border[size_t(tri[i])] = true;
            }
        const bool pin_boundary = !options.displace_border;

        // Only the Cylindrical/Spherical methods need these; Triplanar blends each vertex's own
        // normal and LSCM solves the patch globally.
        Vec3f average_normal = Vec3f::Zero();
        Vec3f patch_centroid = Vec3f::Zero();
        int   patch_vertex_count = 0;
        for (const stl_triangle_vertex_indices &tri : patch.indices)
            for (int i = 0; i < 3; ++i) {
                const int vi = tri[i];
                patch_centroid += patch.vertices[size_t(vi)];
                ++patch_vertex_count;
                // A brush stroke that split a triangle appends new vertices past the base mesh's own
                // (see the get_facets_strict() note above); vertex_normals is sized to the base mesh,
                // so those split indices must be skipped here or this reads out of bounds. The main
                // displacement loop below guards the same way.
                if (vi < int(vertex_normals.size()))
                    average_normal += vertex_normals[size_t(vi)];
            }
        average_normal = (average_normal.norm() > 1e-8f) ? Vec3f(average_normal.normalized()) : Vec3f::UnitZ();
        patch_centroid = (patch_vertex_count > 0) ? Vec3f(patch_centroid / float(patch_vertex_count)) : Vec3f::Zero();

        // Cylinder axis auto-picked as the world axis *least* aligned with the average normal
        // (perpendicular to the outward radial normal, as a cylinder's own axis would be).
        Vec3f       patch_axis = Vec3f::UnitZ();
        const Vec3f an         = average_normal.cwiseAbs();
        if (an.x() <= an.y() && an.x() <= an.z())
            patch_axis = Vec3f::UnitX();
        else if (an.y() <= an.x() && an.y() <= an.z())
            patch_axis = Vec3f::UnitY();

        // A real unwrap of the whole patch, computed once here rather than per vertex - it is a
        // per-chart solve over the whole patch, not a per-point formula. Cached, so repeating this
        // for every slider tweak costs a hash rather than a re-solve (see compute_patch_unwrap()).
        const std::vector<Vec2f> lscm_uvs = (layer->projection_method == TextureProjectionMethod::LSCM) ?
                                                 compute_lscm_uvs(patch, *layer) :
                                                 std::vector<Vec2f>{};

        // Colour, if this layer carries any. Area-weighted over each base triangle's *painted* part,
        // so a triangle the brush only clipped a corner off takes the colour of that corner rather
        // than of the whole triangle's worth of texture - and so a triangle straddling a colour
        // boundary lands on whichever side covers more of it, instead of on whichever sub-triangle
        // happened to be emitted first. One quantize call per triangle, after the averaging.
        if (color_this_layer) {
            const DecodedHeightTexture &tex = height;
            if (tex.has_color()) {
                std::vector<Vec3f> sum(mesh.indices.size(), Vec3f::Zero());
                std::vector<float> sum_area(mesh.indices.size(), 0.f);
                for (size_t j = 0; j < patch.indices.size() && j < patch_source.size(); ++j) {
                    const size_t S = size_t(patch_source[j]);
                    if (S >= mesh.indices.size())
                        continue;
                    const stl_triangle_vertex_indices &t = patch.indices[j];
                    const Vec3f &pa = patch.vertices[size_t(t[0])];
                    const Vec3f &pb = patch.vertices[size_t(t[1])];
                    const Vec3f &pc = patch.vertices[size_t(t[2])];
                    const float  area2 = (pb - pa).cross(pc - pa).norm();
                    if (area2 <= 0.f)
                        continue;
                    const Vec3f centroid = (pa + pb + pc) / 3.f;

                    // The normal the triplanar blend weights by, and the unwrap coordinate the LSCM
                    // path needs, both averaged over the sub-triangle's corners - the same quantities
                    // the per-vertex height sampling uses, evaluated at the centroid instead.
                    Vec3f n = Vec3f::Zero();
                    Vec2f uv = Vec2f::Zero();
                    bool  have_uv = !lscm_uvs.empty();
                    for (int k = 0; k < 3; ++k) {
                        const int vi = t[k];
                        if (vi < int(vertex_normals.size()))
                            n += vertex_normals[size_t(vi)];
                        if (have_uv && size_t(vi) < lscm_uvs.size())
                            uv += lscm_uvs[size_t(vi)];
                        else
                            have_uv = false;
                    }
                    n = (n.norm() > 1e-8f) ? Vec3f(n.normalized()) : average_normal;
                    uv /= 3.f;

                    Vec3f rgb;
                    if (sample_layer_color(tex, *layer, centroid, n, rgb, patch_centroid, patch_axis,
                                           have_uv ? &uv : nullptr)) {
                        sum[S]      += area2 * rgb;
                        sum_area[S] += area2;
                    }
                }
                for (size_t i = 0; i < mesh.indices.size(); ++i)
                    if (sum_area[i] > 0.f) {
                        const int idx = color->quantize(sum[i] / sum_area[i]);
                        // A quantizer that declines this colour leaves whatever a lower layer put
                        // there, rather than punching a hole in it.
                        if (idx >= 0)
                            triangle_palette[i] = idx;
                    }
            }
        }

        // Edge smoothing: a per-vertex weight in [0, 1] that fades the displacement to zero toward the
        // patch boundary. amount->0 leaves only the very edge softened; amount->1 fades the whole patch
        // flat. k = (1-a)/a turns the normalized boundary distance into that weight (see the header).
        std::vector<float> edge_weight;
        if (layer->edge_smoothing && layer->edge_smoothing_amount > 0.f) {
            const std::vector<float> bdist = patch_boundary_distance(patch, is_boundary);
            float max_d = 0.f;
            for (const float d : bdist)
                if (std::isfinite(d))
                    max_d = std::max(max_d, d);
            edge_weight.assign(patch.vertices.size(), 1.f);
            if (max_d > 1e-6f) {
                const float a = std::clamp(layer->edge_smoothing_amount, 0.f, 1.f);
                const float k = (a < 1.f) ? (1.f - a) / std::max(a, 1e-3f) : 0.f;
                for (size_t v = 0; v < bdist.size() && v < edge_weight.size(); ++v) {
                    const float d = std::isfinite(bdist[v]) ? bdist[v] : max_d;
                    edge_weight[v] = std::clamp((d / max_d) * k, 0.f, 1.f);
                }
            }
        }

        const float sign = layer->invert ? -1.f : 1.f;
        // A vertex may be reached by several of the patch's triangles; each must fold into the
        // running total exactly once, or a Multiply/Subtract layer would apply two or three times
        // over depending on how many painted triangles happen to share the vertex. Collecting the
        // unique list up front (cheap, one pass) is also what lets the expensive part - the texture
        // sampling, which is three bilinear fetches plus three pow()s per vertex for triplanar - run
        // in parallel below, instead of serially inside the triangle walk.
        std::vector<int>  layer_vertices;
        std::vector<char> visited(patch.vertices.size(), 0);
        layer_vertices.reserve(patch.vertices.size());
        for (const stl_triangle_vertex_indices &tri : patch.indices)
            for (int i = 0; i < 3; ++i) {
                const int vi = tri[i];
                // Split vertices the brush introduced live past the end of our own vertex array;
                // they carry no displacement of their own and are not part of the output mesh.
                if (vi >= int(mesh.vertices.size()) || (pin_boundary && is_boundary[vi]) || visited[vi])
                    continue;
                visited[vi] = 1;
                layer_vertices.push_back(vi);
            }
        if (layer_vertices.empty())
            continue;

        std::vector<float> sampled(layer_vertices.size(), 0.f);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, layer_vertices.size()),
                          [&](const tbb::blocked_range<size_t> &range) {
            for (size_t k = range.begin(); k < range.end(); ++k) {
                const size_t vi      = size_t(layer_vertices[k]);
                const Vec2f *lscm_uv = lscm_uvs.empty() ? nullptr : &lscm_uvs[vi];
                const float  h       = sample_layer_height(height, *layer, mesh.vertices[vi], vertex_normals[vi],
                                                            patch_centroid, patch_axis, lscm_uv);
                // midlevel is the height that means "stay put", so anything below it displaces
                // *inwards* - see TextureDisplacementLayer::midlevel. At the default of 0 this is
                // exactly the old outward-only behaviour.
                sampled[k] = (h - layer->midlevel) * layer->depth_mm * sign;
            }
        });

        for (size_t k = 0; k < layer_vertices.size(); ++k) {
            const size_t vi = size_t(layer_vertices[k]);
            // The first layer to reach a vertex has nothing underneath it to blend with, so it
            // always starts the total off additively - a Multiply/Divide against an implicit
            // zero base would otherwise annihilate (or blow up) it, which is never what the
            // user means by putting a mask on the bottom of the stack.
            const float accumulated = displacement[vi];
            const float blended     = blend_displacement(accumulated, sampled[k],
                                                          displaced[vi] ? layer->blend_mode : TextureBlendMode::Add);
            // Edge smoothing fades this layer's *effect*, not its input. Scaling the input instead is
            // only correct for Add/Subtract, whose neutral value is 0: on a Multiply layer a faded
            // input approaches 0, which annihilates everything beneath it at the rim rather than
            // leaving it alone, and on a Divide layer it approaches the 0.05 divisor floor, which
            // amplifies the relief underneath by up to 20x exactly where it was meant to fade out.
            // Interpolating the blended result back toward the accumulated total is the neutral
            // element for every mode at once, and reduces to the old formula exactly for Add.
            const float edge_w = edge_weight.empty() ? 1.f : edge_weight[vi];
            displacement[vi]   = accumulated + (blended - accumulated) * edge_w;
            displaced[vi]      = 1;
        }
        any_displacement = true;
    }

    if (!report(65))
        return {};

    if (!any_displacement)
        return mesh;

    for (size_t vi = 0; vi < mesh.vertices.size(); ++vi)
        if (displaced[vi])
            mesh.vertices[vi] += vertex_normals[vi] * displacement[vi];

    // Post-process relaxation of what the height maps left behind, restricted to the vertices that
    // actually moved - the untouched part of the model keeps its exact geometry, and the ring of
    // vertices just outside the displaced set stays put and anchors the smoothing so the relief does
    // not creep outward. `smooth_skip_border` additionally holds the patch's own outermost ring, whose
    // neighbours are those pinned outsiders: relaxing it would drag the rim of the relief back down and
    // leave the pattern looking half-melted right where it meets the edge.
    if (options.smooth_enabled && options.smooth_strength > 0.f && options.smooth_iterations > 0) {
        if (!report(70))
            return {};
        std::vector<uint8_t> movable(mesh.vertices.size(), 0);
        for (size_t vi = 0; vi < mesh.vertices.size(); ++vi)
            movable[vi] = (displaced[vi] && !(options.smooth_skip_border && on_patch_border[vi])) ? 1 : 0;
        // The pass hook only *stops* the relaxation early; the report(99) below is what turns a
        // cancellation into an empty (uncommittable) result, since a cancelled run keeps reporting
        // cancelled.
        smooth_mesh_vertices(mesh, movable, options.smooth_strength, options.smooth_iterations,
                             progress ? DisplacementProgressFn([&report, it = options.smooth_iterations](int pass) {
                                 return report(70 + (29 * (pass + 1)) / std::max(it, 1));
                             }) : DisplacementProgressFn{});
    }

    if (!report(99))
        return {};
    if (want_color) {
        // Despeckle in perceived-colour space, then resolve each entry to a real filament. The order
        // matters both ways round: filtering after the interleave would erase the bands it is supposed
        // to keep, and interleaving before the filter would have the filter treat two halves of one
        // blended colour as a disagreement.
        despeckle_triangle_colors(mesh, triangle_palette, color->despeckle_passes);

        std::vector<uint8_t> out_color(mesh.indices.size(), 0);
        for (size_t i = 0; i < mesh.indices.size(); ++i) {
            if (triangle_palette[i] < 0)
                continue;
            const stl_triangle_vertex_indices &t = mesh.indices[i];
            const Vec3f centroid = (mesh.vertices[size_t(t[0])] + mesh.vertices[size_t(t[1])] +
                                    mesh.vertices[size_t(t[2])]) / 3.f;
            const int filament = color->resolve ? color->resolve(triangle_palette[i], centroid)
                                                : triangle_palette[i];
            if (filament >= 0)
                out_color[i] = uint8_t(std::min(filament + 1, 255));
        }
        // Handed over only on a run that completed: every early return above is a cancellation, and
        // the caller must not commit a half-computed colouring any more than a half-displaced mesh.
        *color->out_triangle = std::move(out_color);
    }
    return mesh;
}

indexed_triangle_set build_texture_displacement(const ModelVolume &volume)
{
    TextureDisplacementFacetsData facets_data;
    for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
        facets_data[size_t(i)] = volume.texture_displacement_facet(i).get_data();

    return build_texture_displacement(volume.mesh().its, volume.texture_displacement_layers, facets_data,
                                      volume.texture_displacement_options);
}

void smooth_mesh_vertices(indexed_triangle_set &mesh, const std::vector<uint8_t> &movable, float strength,
                          int iterations, const DisplacementProgressFn &on_pass)
{
    if (iterations <= 0 || mesh.vertices.empty() || movable.size() != mesh.vertices.size())
        return;
    strength = std::clamp(strength, 0.f, 1.f);
    if (strength <= 0.f)
        return;
    if (std::none_of(movable.begin(), movable.end(), [](uint8_t m) { return m != 0; }))
        return;

    // One-ring neighbours as a CSR-style pair of arrays: counted, prefix-summed, then filled. A
    // triangle contributes each of its edges to both endpoints, so a shared edge is listed once per
    // incident triangle - the duplicates are harmless here, they just weight an interior edge the same
    // way from both sides, and dropping them would cost a sort per vertex for no visible difference.
    const size_t       nv = mesh.vertices.size();
    std::vector<int>   start(nv + 1, 0);
    for (const stl_triangle_vertex_indices &t : mesh.indices)
        for (int e = 0; e < 3; ++e) {
            ++start[size_t(t[e]) + 1];
            ++start[size_t(t[(e + 1) % 3]) + 1];
        }
    for (size_t v = 0; v < nv; ++v)
        start[v + 1] += start[v];
    const size_t     total_refs = size_t(start[nv]);
    std::vector<int> nbr(total_refs, 0);
    std::vector<int> fill(start.begin(), start.begin() + nv);
    for (const stl_triangle_vertex_indices &t : mesh.indices)
        for (int e = 0; e < 3; ++e) {
            const int a = t[e], b = t[(e + 1) % 3];
            nbr[size_t(fill[size_t(a)]++)] = b;
            nbr[size_t(fill[size_t(b)]++)] = a;
        }

    // Read every pass from a snapshot of the previous one, so the result does not depend on the order
    // vertices happen to be visited in (a Gauss-Seidel sweep would smooth several times as hard at the
    // end of the array as at the start).
    std::vector<Vec3f> prev;
    for (int it = 0; it < iterations; ++it) {
        prev = mesh.vertices;
        // Each vertex reads only from `prev` and writes only its own slot, so the sweep parallelises
        // with no synchronisation at all.
        tbb::parallel_for(tbb::blocked_range<size_t>(0, nv), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t v = range.begin(); v < range.end(); ++v) {
                if (!movable[v] || start[v] == start[v + 1])
                    continue;
                Vec3f sum = Vec3f::Zero();
                for (int k = start[v]; k < start[v + 1]; ++k)
                    sum += prev[size_t(nbr[size_t(k)])];
                const Vec3f avg = sum / float(start[v + 1] - start[v]);
                mesh.vertices[v] = prev[v] + (avg - prev[v]) * strength;
            }
        });
        if (on_pass && !on_pass(it))
            return; // cancelled: leave the passes done so far in place, the caller decides what to do
    }
}

namespace {
// One decoded texture + placement per sampleable layer, in blend (slot) order. Held by shared_ptr so
// the returned closure owns it for as long as the subdivider keeps calling back.
struct PreparedLayer {
    DecodedHeightTexture     tex;
    TextureDisplacementLayer layer;  // a copy of the params (depth/tiling/rotation/offset/blend/...)
    Vec3f                    center; // patch centroid, for Cylindrical/Spherical
    Vec3f                    axis;   // cylinder axis, for Cylindrical
};

// Shared by both point samplers, so the height field and the colour field can never disagree about
// where a layer is placed. `need_color` additionally drops layers that cannot contribute colour.
std::shared_ptr<std::vector<PreparedLayer>> prepare_sampleable_layers(
    const indexed_triangle_set &base_mesh, const std::vector<TextureDisplacementLayer> &layers,
    const TextureDisplacementFacetsData &facets_data, bool need_color)
{
    auto prepared = std::make_shared<std::vector<PreparedLayer>>();

    if (base_mesh.indices.empty())
        return prepared;

    std::vector<const TextureDisplacementLayer *> ordered;
    for (const TextureDisplacementLayer &l : layers)
        if (l.slot >= 0 && l.slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS))
            ordered.push_back(&l);
    std::sort(ordered.begin(), ordered.end(),
              [](const TextureDisplacementLayer *a, const TextureDisplacementLayer *b) { return a->slot < b->slot; });

    const std::vector<Vec3f> vertex_normals = texture_displacement_vertex_normals(base_mesh);
    const TriangleMesh       selector_mesh(base_mesh);

    for (const TextureDisplacementLayer *layer : ordered) {
        if (layer->projection_method == TextureProjectionMethod::LSCM)
            continue; // no per-point UV -> not sampleable here (caller falls back to uniform for these)
        if (need_color && !layer->color_enabled)
            continue;
        const TriangleSelector::TriangleSplittingData &data = facets_data[size_t(layer->slot)];
        if (data.triangles_to_split.empty())
            continue;
        const DecodedHeightTexture tex = decode_height_texture(*layer);
        if (tex.empty() || (need_color && !tex.has_color()))
            continue;

        TriangleSelector selector(selector_mesh);
        selector.deserialize(data, false);
        const indexed_triangle_set patch = selector.get_facets_strict(EnforcerBlockerType::ENFORCER);
        if (patch.indices.empty())
            continue;

        // Patch centroid + cylinder axis, computed exactly as build_texture_displacement() does, so a
        // Cylindrical/Spherical layer's detach criterion matches the geometry the bake will produce.
        Vec3f average_normal = Vec3f::Zero();
        Vec3f centroid       = Vec3f::Zero();
        int   count          = 0;
        for (const stl_triangle_vertex_indices &tri : patch.indices)
            for (int i = 0; i < 3; ++i) {
                const int vi = tri[i];
                centroid += patch.vertices[size_t(vi)];
                ++count;
                if (vi < int(vertex_normals.size()))
                    average_normal += vertex_normals[size_t(vi)];
            }
        average_normal = (average_normal.norm() > 1e-8f) ? Vec3f(average_normal.normalized()) : Vec3f::UnitZ();
        centroid       = (count > 0) ? Vec3f(centroid / float(count)) : Vec3f::Zero();

        Vec3f       axis = Vec3f::UnitZ();
        const Vec3f an   = average_normal.cwiseAbs();
        if (an.x() <= an.y() && an.x() <= an.z())
            axis = Vec3f::UnitX();
        else if (an.y() <= an.x() && an.y() <= an.z())
            axis = Vec3f::UnitY();

        prepared->push_back({ tex, *layer, centroid, axis });
    }
    return prepared;
}
} // namespace

ColorFieldSampler make_combined_color_sampler(const indexed_triangle_set                  &base_mesh,
                                              const std::vector<TextureDisplacementLayer> &layers,
                                              const TextureDisplacementFacetsData         &facets_data,
                                              ColorQuantizeFn                              quantize)
{
    if (!quantize)
        return nullptr;
    auto prepared = prepare_sampleable_layers(base_mesh, layers, facets_data, /* need_color */ true);
    if (prepared->empty())
        return nullptr;

    return [prepared, quantize = std::move(quantize)](const Vec3f &pos, const Vec3f &normal) -> int {
        // Last one wins: `prepared` is in ascending slot order and the bake lets a higher layer
        // overwrite a lower one's colour, so the sampler has to resolve overlaps the same way.
        int result = -1;
        for (const PreparedLayer &p : *prepared) {
            Vec3f rgb;
            if (sample_layer_color(p.tex, p.layer, pos, normal, rgb, p.center, p.axis, nullptr))
                if (const int idx = quantize(rgb); idx >= 0)
                    result = idx;
        }
        return result;
    };
}

HeightFieldSampler make_combined_displacement_sampler(const indexed_triangle_set                  &base_mesh,
                                                      const std::vector<TextureDisplacementLayer> &layers,
                                                      const TextureDisplacementFacetsData         &facets_data)
{
    auto prepared = prepare_sampleable_layers(base_mesh, layers, facets_data, /* need_color */ false);
    if (prepared->empty())
        return nullptr;

    return [prepared](const Vec3f &pos, const Vec3f &normal) -> float {
        float total = 0.f;
        bool  any   = false;
        for (const PreparedLayer &p : *prepared) {
            const float h        = sample_layer_height(p.tex, p.layer, pos, normal, p.center, p.axis, nullptr);
            const float sign     = p.layer.invert ? -1.f : 1.f;
            const float signed_h = (h - p.layer.midlevel) * p.layer.depth_mm * sign;
            // The first (lowest) sampleable layer folds additively; the rest use their own blend mode -
            // same rule build_texture_displacement() applies per vertex.
            total = blend_displacement(total, signed_h, any ? p.layer.blend_mode : TextureBlendMode::Add);
            any   = true;
        }
        return total;
    };
}

indexed_triangle_set subdivide_mesh_uniform(const indexed_triangle_set &mesh, float max_edge_length_mm, int max_iterations)
{
    indexed_triangle_set current   = mesh;
    const float          max_edge_sq = max_edge_length_mm * max_edge_length_mm;

    for (int iter = 0; iter < max_iterations; ++iter) {
        // One vertex-index pair (always stored low-index-first) -> the midpoint vertex already
        // created for it in this pass, so the two triangles sharing that edge both get the exact
        // same new vertex instead of two separate, coincident-but-distinct ones (which would leave
        // the mesh non-manifold even though it looks fine).
        std::unordered_map<uint64_t, int> midpoint_cache;
        auto edge_key = [](int a, int b) -> uint64_t {
            if (a > b)
                std::swap(a, b);
            return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
        };
        auto get_midpoint = [&](int a, int b) -> int {
            const uint64_t key = edge_key(a, b);
            auto             it  = midpoint_cache.find(key);
            if (it != midpoint_cache.end())
                return it->second;
            const int idx = int(current.vertices.size());
            current.vertices.push_back((current.vertices[a] + current.vertices[b]) * 0.5f);
            midpoint_cache.emplace(key, idx);
            return idx;
        };

        std::vector<stl_triangle_vertex_indices> new_indices;
        new_indices.reserve(current.indices.size());
        bool any_split = false;
        for (const stl_triangle_vertex_indices &tri : current.indices) {
            const Vec3f &a = current.vertices[tri[0]];
            const Vec3f &b = current.vertices[tri[1]];
            const Vec3f &c = current.vertices[tri[2]];
            if ((b - a).squaredNorm() <= max_edge_sq && (c - b).squaredNorm() <= max_edge_sq && (a - c).squaredNorm() <= max_edge_sq) {
                new_indices.push_back(tri);
                continue;
            }

            any_split = true;
            const int m01 = get_midpoint(tri[0], tri[1]);
            const int m12 = get_midpoint(tri[1], tri[2]);
            const int m20 = get_midpoint(tri[2], tri[0]);
            new_indices.push_back(stl_triangle_vertex_indices(tri[0], m01, m20));
            new_indices.push_back(stl_triangle_vertex_indices(m01, tri[1], m12));
            new_indices.push_back(stl_triangle_vertex_indices(m20, m12, tri[2]));
            new_indices.push_back(stl_triangle_vertex_indices(m01, m12, m20));
        }

        current.indices = std::move(new_indices);
        if (!any_split)
            break;
    }

    return current;
}

indexed_triangle_set subdivide_mesh_adaptive(const indexed_triangle_set &mesh,
                                             const std::vector<uint8_t> &refine_region,
                                             float target_edge_length_mm, int max_triangles,
                                             std::vector<int> *out_source, const HeightFieldSampler &sampler,
                                             float chord_tolerance_mm, float min_edge_length_mm,
                                             float border_edge_length_mm,
                                             const DisplacementProgressFn &progress,
                                             const ColorFieldSampler &color, float color_edge_length_mm)
{
    // Neighbour slots that are not a triangle index.
    constexpr int NB_BOUNDARY    = -1; // open edge: terminal on its own, bisected from this side alone
    constexpr int NB_NONMANIFOLD = -2; // >2 triangles on the edge: never bisected, that would tear it

    // v[] and nb[] are parallel: nb[e] is the triangle across edge (v[e], v[(e+1)%3]). `src` is the
    // input triangle this one descends from - children inherit it, so a caller can carry per-triangle
    // data (a paint mask) across the topology change with no geometric remap.
    struct Tri { int v[3]; int nb[3]; int src; };

    std::vector<Vec3f> verts = mesh.vertices;
    std::vector<Tri>   tris(mesh.indices.size());
    for (size_t i = 0; i < mesh.indices.size(); ++i)
        tris[i] = { { mesh.indices[i][0], mesh.indices[i][1], mesh.indices[i][2] },
                    { NB_BOUNDARY, NB_BOUNDARY, NB_BOUNDARY }, int(i) };

    auto emit = [&]() -> indexed_triangle_set {
        indexed_triangle_set out;
        out.vertices = verts;
        out.indices.reserve(tris.size());
        if (out_source) {
            out_source->clear();
            out_source->reserve(tris.size());
        }
        for (const Tri &t : tris) {
            out.indices.emplace_back(t.v[0], t.v[1], t.v[2]);
            if (out_source)
                out_source->push_back(t.src);
        }
        return out;
    };

    // Feature-adaptive when a sampler and a positive tolerance are supplied; otherwise refinement is
    // driven by the length baseline alone.
    const bool  feature_mode = bool(sampler) && chord_tolerance_mm > 0.f;
    const bool  color_mode   = bool(color) && color_edge_length_mm > 0.f;
    const float color_sq     = color_edge_length_mm > 0.f ? color_edge_length_mm * color_edge_length_mm : 0.f;
    const float min_floor_sq = min_edge_length_mm > 0.f ? min_edge_length_mm * min_edge_length_mm : 0.f;
    const float target_sq    = target_edge_length_mm > 0.f ? target_edge_length_mm * target_edge_length_mm : 0.f;
    const float border_sq    = border_edge_length_mm > 0.f ? border_edge_length_mm * border_edge_length_mm : 0.f;

    // refine_region is indexed by input-triangle index, and every triangle's src stays in that range
    // (children inherit their parent's src), so a wrong size would be an out-of-bounds read. Guard it.
    if (refine_region.size() != mesh.indices.size() || int(tris.size()) + 2 > max_triangles)
        return emit();
    if (!feature_mode && !color_mode && target_sq <= 0.f && border_sq <= 0.f)
        return emit(); // no criterion at all
    if (std::none_of(refine_region.begin(), refine_region.end(), [](uint8_t v) { return v != 0; }))
        return emit(); // nothing flagged: no-op

    auto edge_key = [](int a, int b) -> uint64_t {
        if (a > b)
            std::swap(a, b);
        return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
    };

    // Edge adjacency, built once here and then maintained incrementally by bisect() below. Rebuilding
    // it per refinement pass is what made the previous version's cost scale with the whole model
    // instead of with the refined region, and what forced the tiny pass budget that stopped
    // refinement short.
    {
        // Sort the half-edges by their edge key and walk the equal runs, rather than hashing every one
        // of them twice into an unordered_map. Same result, but the two expensive parts - forming the
        // keys and ordering them - both parallelise, where a shared hash map cannot. The map also cost
        // a second full pass of lookups purely to read back what the first pass had just inserted.
        std::vector<std::pair<uint64_t, int>> he(tris.size() * 3); // (edge key, triangle * 3 + local edge)
        tbb::parallel_for(tbb::blocked_range<size_t>(0, tris.size()),
                          [&](const tbb::blocked_range<size_t> &range) {
                              for (size_t ti = range.begin(); ti < range.end(); ++ti)
                                  for (int e = 0; e < 3; ++e)
                                      he[ti * 3 + size_t(e)] = {
                                          edge_key(tris[ti].v[e], tris[ti].v[(e + 1) % 3]), int(ti) * 3 + e
                                      };
                          });
        tbb::parallel_sort(he.begin(), he.end());
        // Runs of equal key are the half-edges of one edge: one is a boundary, two are neighbours,
        // more is non-manifold. Serial, but it is a single linear pass over an already ordered array.
        for (size_t i = 0; i < he.size();) {
            size_t j = i + 1;
            while (j < he.size() && he[j].first == he[i].first)
                ++j;
            const size_t count = j - i;
            if (count == 2) {
                const int a = he[i].second, b = he[i + 1].second;
                tris[size_t(a / 3)].nb[a % 3] = b / 3;
                tris[size_t(b / 3)].nb[b % 3] = a / 3;
            } else if (count > 2) {
                for (size_t k = i; k < j; ++k)
                    tris[size_t(he[k].second / 3)].nb[he[k].second % 3] = NB_NONMANIFOLD;
            }
            // count == 1 keeps the NB_BOUNDARY it was initialised with.
            i = j;
        }
    }

    // Feature mode: per-vertex surface normal, and the sampled displacement height at each vertex.
    // Heights are filled in lazily - on a big model only a small painted region is ever looked at, and
    // a sampler call is a texture fetch (plus trig) per layer, so sampling every vertex of the whole
    // mesh up front was pure waste. Both arrays grow in lockstep with `verts`.
    std::vector<Vec3f>   vnormal;
    std::vector<float>   vheight;
    std::vector<uint8_t> vheight_valid;
    if (feature_mode) {
        vnormal.assign(verts.size(), Vec3f::Zero());
        for (const Tri &t : tris) {
            const Vec3f fn = (verts[t.v[1]] - verts[t.v[0]]).cross(verts[t.v[2]] - verts[t.v[0]]); // area-weighted
            for (int i = 0; i < 3; ++i)
                vnormal[t.v[i]] += fn;
        }
        for (Vec3f &n : vnormal) {
            const float l = n.norm();
            n = (l > 1e-12f) ? Vec3f(n / l) : Vec3f(Vec3f::UnitZ());
        }
        vheight.assign(verts.size(), 0.f);
        vheight_valid.assign(verts.size(), 0);
    }
    auto height_of = [&](int v) -> float {
        if (!vheight_valid[v]) {
            vheight[v]       = sampler(verts[v], vnormal[v]);
            vheight_valid[v] = 1;
        }
        return vheight[v];
    };

    // Per-vertex filament index, sampled lazily and cached the same way the heights are. Needs the
    // vertex normals, which feature mode also builds - so colour mode builds them when it is on alone.
    std::vector<int>     vcolor;
    std::vector<uint8_t> vcolor_valid;
    if (color_mode) {
        if (vnormal.empty()) {
            vnormal.assign(verts.size(), Vec3f::Zero());
            for (const Tri &t : tris) {
                const Vec3f fn = (verts[t.v[1]] - verts[t.v[0]]).cross(verts[t.v[2]] - verts[t.v[0]]);
                for (int i = 0; i < 3; ++i)
                    vnormal[t.v[i]] += fn;
            }
            for (Vec3f &n : vnormal) {
                const float l = n.norm();
                n = (l > 1e-12f) ? Vec3f(n / l) : Vec3f(Vec3f::UnitZ());
            }
        }
        vcolor.assign(verts.size(), -2); // -2 = not sampled yet; -1 = sampled, no colour there
        vcolor_valid.assign(verts.size(), 0);
    }
    auto color_of = [&](int v) -> int {
        if (!vcolor_valid[v]) {
            vcolor[v]       = color(verts[v], vnormal[v]);
            vcolor_valid[v] = 1;
        }
        return vcolor[v];
    };

    // Sample the input mesh's own vertices up front, in parallel, for the region that is going to be
    // refined. A sampler call is a texture fetch plus the projection's trigonometry per layer, and it
    // is by far the most expensive thing here - but taken one at a time from inside the refinement
    // loop it is also strictly serial. Every one of these vertices is read by the very first scoring
    // pass anyway, so doing them together costs nothing extra and hands the work to every core.
    //
    // Only the region, and only the *initial* vertices: the laziness this replaces exists so that a
    // small painted patch on a big model does not pay for the whole model (see height_of()), and that
    // still holds. Midpoints created later stay lazy, because they do not exist yet.
    if (feature_mode || color_mode) {
        std::vector<uint8_t> wanted(verts.size(), 0);
        for (const Tri &t : tris)
            if (refine_region[t.src] != 0)
                for (int i = 0; i < 3; ++i)
                    wanted[size_t(t.v[i])] = 1;
        // Each index is touched by exactly one iteration, so the lazy caches can be filled without
        // synchronisation - and every value is the one height_of()/color_of() would have produced.
        tbb::parallel_for(tbb::blocked_range<size_t>(0, verts.size()),
                          [&](const tbb::blocked_range<size_t> &range) {
                              for (size_t v = range.begin(); v < range.end(); ++v) {
                                  if (!wanted[v])
                                      continue;
                                  if (feature_mode) {
                                      vheight[v]       = sampler(verts[v], vnormal[v]);
                                      vheight_valid[v] = 1;
                                  }
                                  if (color_mode) {
                                      vcolor[v]       = color(verts[v], vnormal[v]);
                                      vcolor_valid[v] = 1;
                                  }
                              }
                          });
    }

    auto elen_sq = [&](int a, int b) -> float { return (verts[a] - verts[b]).squaredNorm(); };

    // The one edge of a triangle taken as its "longest": greatest squared length, exact ties broken by
    // the smaller (sorted) vertex-index key. Both triangles sharing an edge compute the same key for
    // it, so they can never disagree about which of them is longest - the property the conformality
    // argument and the LEPP walk's termination both rest on.
    auto longest_local = [&](int ti) -> int {
        const Tri &t    = tris[ti];
        int        best = 0;
        float      bl   = elen_sq(t.v[0], t.v[1]);
        uint64_t   bk   = edge_key(t.v[0], t.v[1]);
        for (int e = 1; e < 3; ++e) {
            const float    l = elen_sq(t.v[e], t.v[(e + 1) % 3]);
            const uint64_t k = edge_key(t.v[e], t.v[(e + 1) % 3]);
            if (l > bl || (l == bl && k < bk)) {
                bl   = l;
                bk   = k;
                best = e;
            }
        }
        return best;
    };

    // How far the *displaced* surface departs from the flat triangle, sampled across the WHOLE
    // triangle - the three edge midpoints and the centroid - not just one edge midpoint. Sampling the
    // interior is what catches a bump that sits inside a triangle (the blind spot of an edge-only
    // test). Cached per triangle: it can only change when the triangle is split, and then both
    // children are fresh entries.
    std::vector<float> tri_err;
    if (feature_mode)
        tri_err.assign(tris.size(), -1.f);
    // True when this triangle straddles a colour boundary: its corners, its edge midpoints and its
    // centroid do not all take the same filament. The midpoints and centroid matter for the same
    // reason they do in detail_error() - a boundary can cross a triangle without separating any two of
    // its corners. Cached per triangle; a split invalidates both children.
    std::vector<uint8_t> tri_color_split; // 0 = unknown, 1 = straddles, 2 = uniform
    if (color_mode)
        tri_color_split.assign(tris.size(), 0);
    auto straddles_color = [&](int ti) -> bool {
        if (tri_color_split[ti] != 0)
            return tri_color_split[ti] == 1;
        const Tri  &t  = tris[ti];
        const Vec3f pa = verts[t.v[0]], pb = verts[t.v[1]], pc = verts[t.v[2]];
        const Vec3f na = vnormal[t.v[0]], nb = vnormal[t.v[1]], nc = vnormal[t.v[2]];
        const int   ca = color_of(t.v[0]);
        bool        split = color_of(t.v[1]) != ca || color_of(t.v[2]) != ca;
        if (!split) {
            static const float BARY[4][3] = { { 0.5f, 0.5f, 0.f }, { 0.f, 0.5f, 0.5f },
                                              { 0.5f, 0.f, 0.5f }, { 1.f / 3, 1.f / 3, 1.f / 3 } };
            for (const auto &w : BARY) {
                Vec3f       n  = w[0] * na + w[1] * nb + w[2] * nc;
                const float nl = n.norm();
                n = (nl > 1e-12f) ? Vec3f(n / nl) : na;
                if (color(w[0] * pa + w[1] * pb + w[2] * pc, n) != ca) {
                    split = true;
                    break;
                }
            }
        }
        tri_color_split[ti] = split ? 1 : 2;
        return split;
    };

    auto detail_error = [&](int ti) -> float {
        if (tri_err[ti] >= 0.f)
            return tri_err[ti];
        const Tri  &t  = tris[ti];
        const Vec3f pa = verts[t.v[0]], pb = verts[t.v[1]], pc = verts[t.v[2]];
        const Vec3f na = vnormal[t.v[0]], nb = vnormal[t.v[1]], nc = vnormal[t.v[2]];
        const float ha = height_of(t.v[0]), hb = height_of(t.v[1]), hc = height_of(t.v[2]);
        static const float BARY[4][3] = { { 0.5f, 0.5f, 0.f }, { 0.f, 0.5f, 0.5f },
                                          { 0.5f, 0.f, 0.5f }, { 1.f / 3, 1.f / 3, 1.f / 3 } };
        float maxerr = 0.f;
        for (const auto &w : BARY) {
            Vec3f       n  = w[0] * na + w[1] * nb + w[2] * nc;
            const float nl = n.norm();
            n = (nl > 1e-12f) ? Vec3f(n / nl) : na;
            const float actual = sampler(w[0] * pa + w[1] * pb + w[2] * pc, n);
            maxerr = std::max(maxerr, std::abs(actual - (w[0] * ha + w[1] * hb + w[2] * hc)));
        }
        tri_err[ti] = maxerr;
        return maxerr;
    };

    // How many times over its criteria a triangle is: <= 1 means "good enough, leave it alone", and
    // the larger the value the more a split buys. Driving the heap with this is what makes a run that
    // runs out of budget spend it on the worst offenders instead of wherever a sweep happened to
    // reach. Triangles outside the region always score 0 - they are only ever touched by the conformal
    // closure below, never refined on their own account.
    auto priority = [&](int ti) -> float {
        const Tri    &t     = tris[ti];
        const uint8_t flags = refine_region[t.src];
        if (flags == 0)
            return 0.f;
        const int   le = longest_local(ti);
        const float ll = elen_sq(t.v[le], t.v[(le + 1) % 3]);
        if (ll <= min_floor_sq)
            return 0.f; // at the resolution floor - also what stops a sharp texture step going forever
        float p = 0.f;
        if (flags & REFINE_PAINTED) {
            p = (target_sq > 0.f) ? ll / target_sq : 0.f;
            if (feature_mode)
                p = std::max(p, detail_error(ti) / chord_tolerance_mm);
            // Colour is per facet, so a colour boundary can only be drawn where there are edges along
            // it. Length target, floored by min_edge_length_mm above, exactly like the border band.
            if (color_mode && straddles_color(ti))
                p = std::max(p, ll / color_sq);
        }
        // The band straddling the paint's edge, refined by plain edge length. Deliberately *not* run
        // through detail_error(): outside the paint the sampler still reports full relief (it has no
        // per-point paint test), so the chord test there would chase texture detail on a surface the
        // bake is going to leave flat. Length alone is what this band needs - the error it is fixing
        // is the size of the triangles spanning the displacement step, not the curvature of anything.
        if ((flags & REFINE_BORDER) && border_sq > 0.f)
            p = std::max(p, ll / border_sq);
        return p;
    };

    auto set_nb = [&](int ti, int u, int v, int val) {
        if (ti < 0)
            return;
        Tri &t = tris[ti];
        for (int e = 0; e < 3; ++e)
            if ((t.v[e] == u && t.v[(e + 1) % 3] == v) || (t.v[e] == v && t.v[(e + 1) % 3] == u)) {
                t.nb[e] = val;
                return;
            }
    };

    // Bisects triangle `ti` across its local edge `e`, which the caller has established is terminal.
    // Both triangles on that edge are split in the one operation, around a single shared midpoint -
    // which is exactly why a hanging node (and so a crack) can never appear. `ti` and the opposite
    // triangle are each reused as one of their own children, so only two back-pointers in the
    // surrounding mesh need repointing. Triangles whose geometry changed are left in `touched`.
    std::vector<int> touched;
    auto bisect = [&](int ti, int e) -> bool {
        const int a = tris[ti].v[e], b = tris[ti].v[(e + 1) % 3], c = tris[ti].v[(e + 2) % 3];
        const int n = tris[ti].nb[e];
        // Locate the shared edge from the far side before touching anything: bailing out half way
        // through would be the one way this could leave a crack.
        int f = -1;
        if (n >= 0) {
            for (int k = 0; k < 3; ++k) {
                const int u = tris[n].v[k], w = tris[n].v[(k + 1) % 3];
                if ((u == a && w == b) || (u == b && w == a)) {
                    f = k;
                    break;
                }
            }
            if (f < 0) {
                tris[ti].nb[e] = NB_NONMANIFOLD; // inconsistent adjacency: refuse to split across it
                return false;
            }
        }

        const int m = int(verts.size());
        verts.push_back(0.5f * (verts[a] + verts[b]));
        if (feature_mode || color_mode) {
            const Vec3f mn = vnormal[a] + vnormal[b];
            const float ml = mn.norm();
            vnormal.push_back(ml > 1e-12f ? Vec3f(mn / ml) : vnormal[a]);
        }
        if (feature_mode) {
            vheight.push_back(0.f);
            vheight_valid.push_back(0);
        }
        if (color_mode) {
            vcolor.push_back(-2);
            vcolor_valid.push_back(0);
        }

        // Near side: ti becomes (a, m, c), the new triangle is (m, b, c). Both keep the original
        // a->b->c winding.
        const int nb_bc = tris[ti].nb[(e + 1) % 3];
        const int nb_ca = tris[ti].nb[(e + 2) % 3];
        const int src   = tris[ti].src;
        const int t2    = int(tris.size());
        tris.push_back(Tri{ { m, b, c }, { NB_BOUNDARY, nb_bc, ti }, src });
        {
            Tri &t1 = tris[ti];
            t1.v[0] = a; t1.v[1] = m; t1.v[2] = c;
            t1.nb[0] = NB_BOUNDARY; t1.nb[1] = t2; t1.nb[2] = nb_ca;
        }
        set_nb(nb_bc, b, c, t2); // that outer neighbour borders the second child now, not ti
        if (feature_mode) {
            tri_err.push_back(-1.f);
            tri_err[ti] = -1.f;
        }
        if (color_mode) {
            tri_color_split.push_back(0);
            tri_color_split[ti] = 0;
        }
        touched.assign({ ti, t2 });

        if (n < 0) {
            return true; // boundary edge: nothing on the far side to split
        }

        // Far side, same shape: n becomes (p, m, d), the new triangle is (m, q, d).
        const int p = tris[n].v[f], q = tris[n].v[(f + 1) % 3], d = tris[n].v[(f + 2) % 3];
        const int nb_qd = tris[n].nb[(f + 1) % 3];
        const int nb_dp = tris[n].nb[(f + 2) % 3];
        const int nsrc  = tris[n].src;
        const int n2    = int(tris.size());
        tris.push_back(Tri{ { m, q, d }, { NB_BOUNDARY, nb_qd, n }, nsrc });
        {
            Tri &n1 = tris[n];
            n1.v[0] = p; n1.v[1] = m; n1.v[2] = d;
            n1.nb[0] = NB_BOUNDARY; n1.nb[1] = n2; n1.nb[2] = nb_dp;
        }
        set_nb(nb_qd, q, d, n2);
        if (feature_mode) {
            tri_err.push_back(-1.f);
            tri_err[n] = -1.f;
        }
        if (color_mode) {
            tri_color_split.push_back(0);
            tri_color_split[n] = 0;
        }

        // Stitch the two sides back together: whichever far child holds `a` borders the near child
        // that holds `a`. (Which one that is depends on how n happens to be wound.)
        const int side_a = (p == a) ? n : n2;
        const int side_b = (p == a) ? n2 : n;
        tris[ti].nb[0] = side_a; // near child (a, m, c), edge (a, m)
        tris[t2].nb[0] = side_b; // near child (m, b, c), edge (m, b)
        set_nb(side_a, a, m, ti);
        set_nb(side_b, b, m, t2);
        touched.assign({ ti, t2, n, n2 });
        return true;
    };

    // Worst-first. Entries go stale as their triangle is split; a stale entry is harmless - it is
    // re-scored on pop and dropped or re-pushed. The ordering is a budget-allocation heuristic only:
    // neither correctness nor conformality depends on it.
    std::priority_queue<std::pair<float, int>> queue;
    {
        // Scoring the starting mesh means a detail_error() per triangle - four more sampler calls each
        // - so it is worth spreading, even though the refinement that follows cannot be. Each entry is
        // written by one iteration only, and the caches those calls fill (tri_err, tri_color_split) are
        // likewise per triangle, so there is nothing shared to guard. The heap is then built from the
        // finished array in index order, which is exactly the order the serial loop pushed in.
        std::vector<float> initial(tris.size(), 0.f);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, tris.size()),
                          [&](const tbb::blocked_range<size_t> &range) {
                              for (size_t ti = range.begin(); ti < range.end(); ++ti)
                                  initial[ti] = priority(int(ti));
                          });
        for (int ti = 0; ti < int(tris.size()); ++ti)
            if (initial[size_t(ti)] > 1.f)
                queue.emplace(initial[size_t(ti)], ti);
    }

    // Every iteration either drops one satisfied triangle from the queue or performs exactly one
    // bisection, and bisections are capped by the triangle budget, so this always terminates.
    // Progress is reported against the triangle budget, which is what the loop is bounded by. Polled
    // rather than pushed on every bisection: a refinement spends its budget in hundreds of thousands
    // of them, and every hook call wakes the UI's idle loop to repaint the notification.
    const int start_tris  = int(tris.size());
    const int budget_tris = std::max(1, max_triangles - start_tris);
    int       next_poll   = start_tris;
    while (!queue.empty() && int(tris.size()) + 2 <= max_triangles) {
        if (progress && int(tris.size()) >= next_poll) {
            next_poll = int(tris.size()) + std::max(1024, budget_tris / 100);
            if (!progress(std::clamp((int(tris.size()) - start_tris) * 100 / budget_tris, 0, 100)))
                break; // still conformal - whole bisections only; the caller decides whether to keep it
        }
        const int ti = queue.top().second;
        queue.pop();
        if (priority(ti) <= 1.f)
            continue; // stale: already refined past its criteria

        // Longest-Edge Propagation Path: step to the neighbour across the current longest edge for as
        // long as that neighbour has a strictly longer one, then bisect the terminal edge we land on.
        // Length strictly increases along the path, so it cannot cycle.
        int cur = ti, split_edge = -1;
        for (size_t guard = 0; guard <= tris.size(); ++guard) {
            const int le  = longest_local(cur);
            const int nbr = tris[cur].nb[le];
            if (nbr == NB_NONMANIFOLD)
                break; // cannot split across it without tearing the mesh: give up on this path
            if (nbr == NB_BOUNDARY) {
                split_edge = le; // boundary longest edge -> terminal
                break;
            }
            const int nle = longest_local(nbr);
            if (edge_key(tris[nbr].v[nle], tris[nbr].v[(nle + 1) % 3]) ==
                edge_key(tris[cur].v[le], tris[cur].v[(le + 1) % 3])) {
                split_edge = le; // mutual longest edge -> terminal
                break;
            }
            cur = nbr;
        }
        if (split_edge < 0 || !bisect(cur, split_edge))
            continue; // ti sits in a non-manifold neighbourhood and cannot be refined safely

        for (const int t : touched)
            if (const float p = priority(t); p > 1.f)
                queue.emplace(p, t);
        // The bisection may have been a step on the way to ti rather than ti itself, in which case ti
        // is not in `touched` and has to go back on the heap to be walked again.
        if (cur != ti)
            if (const float p = priority(ti); p > 1.f)
                queue.emplace(p, ti);
    }

    return emit();
}

} // namespace Slic3r
