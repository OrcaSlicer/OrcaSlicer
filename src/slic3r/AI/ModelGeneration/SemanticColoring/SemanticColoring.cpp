#include "SemanticColoring.hpp"
#include "SemanticMaskRefinement.hpp"
#include "SemanticMaterialRegions.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace Slic3r::AI::SemanticColoring {
namespace {
constexpr uint32_t no_face = std::numeric_limits<uint32_t>::max();
constexpr size_t maximum_faces = 2000000, maximum_vertices = 6000000;
constexpr size_t maximum_subface_evidence = 1000000;
bool stopped(const Cancel& cancel) { return cancel && cancel(); }
bool valid_color(const Color& c)
{
    return std::all_of(c.begin(), c.end(), [](float v) { return std::isfinite(v) && v >= 0 && v <= 1; });
}
bool valid_label(Label label) { return size_t(label) < label_count; }
bool paintable(Label label) { return valid_label(label) && label != Label::Unknown && label != Label::Background; }
constexpr std::array<Label, 6> face_detail_labels {{
    Label::Lips, Label::MouthInterior, Label::EyeSclera, Label::Iris, Label::Eyebrow, Label::FaceSkin
}};
size_t face_detail_index(Label label)
{
    return size_t(std::find(face_detail_labels.begin(), face_detail_labels.end(), label) - face_detail_labels.begin());
}
Color corner_color(const MeshSnapshot& source, size_t face, int corner)
{
    const auto& color = source.vertex_colors.size() == source.mesh.vertices.size()
        ? source.vertex_colors[source.mesh.indices[face][corner]] : source.face_colors[face];
    return {color[0], color[1], color[2]};
}
std::string validate_snapshot(const MeshSnapshot& source)
{
    if (source.mesh.indices.empty() || source.mesh.indices.size() > maximum_faces ||
        source.mesh.vertices.empty() || source.mesh.vertices.size() > maximum_vertices)
        return "The semantic analysis mesh is empty or exceeds the editable mesh limit.";
    if (source.vertex_colors.size() != source.mesh.vertices.size() && source.face_colors.size() != source.mesh.indices.size())
        return "Semantic analysis requires original vertex or face colors.";
    for (const auto& vertex : source.mesh.vertices)
        if (!vertex.allFinite()) return "The semantic analysis mesh contains invalid positions.";
    for (size_t face = 0; face < source.mesh.indices.size(); ++face)
        for (int corner = 0; corner < 3; ++corner) {
            const int index = source.mesh.indices[face][corner];
            if (index < 0 || size_t(index) >= source.mesh.vertices.size()) return "The semantic analysis mesh contains an invalid face.";
            if (!valid_color(corner_color(source, face, corner))) return "Semantic analysis requires finite normalized sRGB colors.";
        }
    return {};
}

// Public-domain Oklab matrices, matching the existing preview color space.
Color lab(Color rgb)
{
    for (float& c : rgb) c = c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
    const float l = std::cbrt(.4122214708f*rgb[0] + .5363325363f*rgb[1] + .0514459929f*rgb[2]);
    const float m = std::cbrt(.2119034982f*rgb[0] + .6806995451f*rgb[1] + .1073969566f*rgb[2]);
    const float s = std::cbrt(.0883024619f*rgb[0] + .2817188376f*rgb[1] + .6299787005f*rgb[2]);
    return {.2104542553f*l + .793617785f*m - .0040720468f*s,
        1.9779984951f*l - 2.428592205f*m + .4505937099f*s,
        .0259040371f*l + .7827717662f*m - .808675766f*s};
}
float distance(const Color& a, const Color& b)
{
    const float dl = (a[0] - b[0]) * .65f, da = a[1] - b[1], db = a[2] - b[2];
    return dl*dl + da*da + db*db;
}
float chroma(const Color& a) { return std::hypot(a[1], a[2]); }
// Four-color previews have very little room for semantic mistakes. Treat only
// clearly chromatic warm accents as red; pale skin and cream clothing remain
// below this threshold and are therefore not accidentally rejected.
bool red_accent(const Color& color)
{
    return chroma(color) >= .055f && color[1] >= .045f &&
        color[1] > color[2] * 1.25f + .01f;
}
bool compatible_skin(const Color& source, const Color& center, Label label)
{
    // Body segmentation includes eyebrows, eyes, teeth and sometimes lips in
    // face-skin. These source outliers must retain the existing fine color map.
    const float source_chroma = chroma(source), center_chroma = chroma(center);
    if (source_chroma < .015f || source[1] < 0.f || source[2] <= 0.f || center_chroma < .015f) return false;
    const float hue_alignment = (source[1] * center[1] + source[2] * center[2]) / (source_chroma * center_chroma);
    if (label == Label::BodySkin) {
        // Neck and hand shadows do not contain the facial details protected
        // below. Accept supported warm shadows, while rejecting saturated red
        // clothing even when the body mask incorrectly calls it skin.
        return source[0] >= center[0] - .28f && source[0] <= center[0] + .24f &&
            hue_alignment >= .80f && source[1] <= source[2] * 1.6f &&
            source_chroma <= std::min(.16f, center_chroma * 2.f + .02f);
    }
    if (source[0] < center[0] - .16f || source[0] > center[0] + .20f) return false;
    if (hue_alignment < .90f) return false;
    if (source[1] > center[1] + .015f && source[2] < center[2] * .85f) return false;
    return source_chroma <= center_chroma * 1.8f + .02f;
}
bool same_muted_cloth(const Color& source, const Color& center)
{
    const float source_chroma = chroma(source), center_chroma = chroma(center);
    // Weak but consistent dye color is different from an actual gray stripe.
    // Let its local appearance prototype absorb small lighting/texture changes
    // across the neutral cutoff; never borrow color from a saturated garment.
    return source_chroma >= .005f && source_chroma < .035f &&
        center_chroma >= .005f && center_chroma < .035f &&
        std::abs(source[0] - center[0]) < .15f &&
        source[1] * center[1] + source[2] * center[2] >= .94f * source_chroma * center_chroma;
}
bool compatible_sclera(const Color& source, const Color& center)
{
    // Eye masks are geometric and may touch eyeliner, iris or adjacent skin.
    // Require a plausible eye-white source even when the region has support.
    return source[0] >= .40f && source[0] >= center[0] - .18f && chroma(source) <= .055f;
}
bool natural_dark_hair(const Color& color)
{
    // Brown/black appearance, not a red, golden, cool-dyed or white material.
    // This is only used after the region has been recognized as hair.
    const float saturation = chroma(color);
    return color[0] < .67f && saturation < .115f &&
        (saturation >= .008f || color[0] < .40f) &&
        color[1] >= -.012f && color[2] >= -.012f &&
        color[1] <= color[2] * 1.15f + .018f;
}
bool same_hair_material(const Color& source, const Color& center)
{
    if (!natural_dark_hair(center) || source[0] > .75f ||
        std::abs(source[0] - center[0]) > .35f || chroma(source) >= .12f ||
        source[1] < -.012f || source[2] < -.012f || source[1] > source[2] * 1.15f + .018f)
        return false;
    const float source_chroma = chroma(source), center_chroma = chroma(center);
    // Keep a real gray streak separate from brown highlights, including when
    // both belong to one connected hair surface.
    if (source_chroma < .008f && source[0] > center[0] + .12f) return false;
    return source_chroma < .015f || center_chroma < .015f ||
        source[1] * center[1] + source[2] * center[2] >= .85f * source_chroma * center_chroma;
}
Color face_lab(const MeshSnapshot& source, size_t face)
{
    Color rgb {};
    for (int corner = 0; corner < 3; ++corner) {
        const auto c = corner_color(source, face, corner);
        for (int channel = 0; channel < 3; ++channel) rgb[channel] += c[channel] / 3.f;
    }
    return lab(rgb);
}
double area(const MeshSnapshot& source, size_t face)
{
    const auto& f = source.mesh.indices[face];
    return .5 * double((source.mesh.vertices[f[1]] - source.mesh.vertices[f[0]])
        .cross(source.mesh.vertices[f[2]] - source.mesh.vertices[f[0]]).norm());
}
std::string sha256(const std::string& input)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx.get(), input.data(), input.size()) != 1) return {};
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes {};
    unsigned int count = 0;
    if (EVP_DigestFinal_ex(ctx.get(), bytes.data(), &count) != 1 || count != 32) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (unsigned int i = 0; i < count; ++i) { result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15]; }
    return result;
}
int unhex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

std::vector<ViewRegion> face_regions(const RenderedView& view, const Prediction& body)
{
    const int width = view.image.width, height = view.image.height;
    struct Box { int left, top, right, bottom; size_t pixels; };
    std::vector<uint8_t> remaining(size_t(width) * height, 0);
    for (size_t pixel = 0; pixel < remaining.size(); ++pixel)
        remaining[pixel] = view.face_ids[pixel] != no_face && body.labels[pixel] == Label::FaceSkin &&
            body.confidence[pixel] >= .55f;
    std::vector<Box> boxes;
    std::vector<size_t> pending;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const size_t first = size_t(y) * width + x;
        if (!remaining[first]) continue;
        remaining[first] = 0;
        pending.assign(1, first);
        Box box {x, y, x, y, 0};
        for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
            const size_t pixel = pending[cursor];
            const int px = int(pixel % width), py = int(pixel / width);
            box.left = std::min(box.left, px); box.right = std::max(box.right, px);
            box.top = std::min(box.top, py); box.bottom = std::max(box.bottom, py); ++box.pixels;
            constexpr int dx[] {-1, 1, 0, 0}, dy[] {0, 0, -1, 1};
            for (size_t direction = 0; direction < 4; ++direction) {
                const int nx = px + dx[direction], ny = py + dy[direction];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                const size_t neighbor = size_t(ny) * width + nx;
                if (!remaining[neighbor]) continue;
                remaining[neighbor] = 0; pending.push_back(neighbor);
            }
        }
        if (box.pixels < 16) continue;
        const int padding = std::max(4, std::max(box.right - box.left + 1, box.bottom - box.top + 1) / 5);
        box.left = std::max(0, box.left - padding); box.right = std::min(width - 1, box.right + padding);
        box.top = std::max(0, box.top - padding); box.bottom = std::min(height - 1, box.bottom + padding);
        boxes.push_back(box);
    }
    // Eye or mouth holes may split one face-skin mask into nearby islands. The
    // padded boxes overlap for one face, while distinct people remain separate.
    for (size_t first = 0; first < boxes.size(); ++first) for (size_t second = first + 1; second < boxes.size();) {
        auto& a = boxes[first]; const auto& b = boxes[second];
        if (a.right < b.left || b.right < a.left || a.bottom < b.top || b.bottom < a.top) {
            ++second; continue;
        }
        a.left = std::min(a.left, b.left); a.top = std::min(a.top, b.top);
        a.right = std::max(a.right, b.right); a.bottom = std::max(a.bottom, b.bottom);
        a.pixels += b.pixels; boxes.erase(boxes.begin() + second); second = first + 1;
    }
    std::sort(boxes.begin(), boxes.end(), [](const Box& lhs, const Box& rhs) {
        if (lhs.pixels != rhs.pixels) return lhs.pixels > rhs.pixels;
        if (lhs.left != rhs.left) return lhs.left < rhs.left;
        return lhs.top < rhs.top;
    });
    if (boxes.size() > 4) boxes.resize(4);
    std::sort(boxes.begin(), boxes.end(), [](const Box& lhs, const Box& rhs) {
        return lhs.left != rhs.left ? lhs.left < rhs.left : lhs.top < rhs.top;
    });
    std::vector<ViewRegion> result;
    result.reserve(boxes.size());
    for (const Box& box : boxes)
        result.push_back({float(box.left) / width, float(box.top) / height,
            float(box.right - box.left + 1) / width, float(box.bottom - box.top + 1) / height});
    return result;
}

float supported_surface_sample(const RenderedView& view, const Prediction& prediction,
                               size_t face, const std::vector<Color>& original)
{
    const uint32_t pixel = view.surface_samples[face];
    if (pixel == no_face) return 0;
    const uint32_t neighbor = view.face_ids[pixel];
    if (neighbor == no_face || distance(original[face], original[neighbor]) > .0009f) return 0;
    const int x = int(pixel % view.image.width), y = int(pixel / view.image.width);
    if (x < 1 || y < 1 || x + 1 >= view.image.width || y + 1 >= view.image.height) return 0;
    const Label label = prediction.labels[pixel];
    if (!paintable(label)) return 0;
    float confidence = 1.f;
    // Do not infer across silhouettes, semantic edges, or a differently colored
    // overlapping surface. One original view, not iterative label diffusion.
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        const size_t nearby = size_t(y + dy) * view.image.width + x + dx;
        if (view.face_ids[nearby] == no_face || prediction.labels[nearby] != label ||
            prediction.confidence[nearby] < .80f) return 0;
        confidence = std::min(confidence, prediction.confidence[nearby]);
    }
    return confidence;
}

float supported_subface_surface_sample(const MeshSnapshot& source, const RenderedView& view,
                                       const Prediction& prediction, size_t face,
                                       const SubfacePath& path, uint32_t& output_pixel)
{
    output_pixel = no_face;
    if (face >= source.mesh.indices.size() || view.projected_vertices.size() != source.mesh.vertices.size() ||
        view.surface_depth_tolerance <= 0.f || view.facing[face] < .15f) return 0.f;
    std::array<Barycentric, 3> leaf_vertices;
    if (!subface_vertices(path, leaf_vertices)) return 0.f;
    Barycentric centroid {};
    for (const Barycentric& vertex : leaf_vertices)
        for (size_t axis = 0; axis < 3; ++axis) centroid[axis] += vertex[axis] / 3.f;
    const auto& triangle = source.mesh.indices[face];
    const Vec3f& a = view.projected_vertices[triangle[0]];
    const Vec3f& b = view.projected_vertices[triangle[1]];
    const Vec3f& c = view.projected_vertices[triangle[2]];
    const Vec3f projected = centroid[0] * a + centroid[1] * b + centroid[2] * c;
    if (projected.x() < 1.f || projected.y() < 1.f ||
        projected.x() + 1.f >= view.image.width || projected.y() + 1.f >= view.image.height) return 0.f;
    const int x = int(projected.x()), y = int(projected.y());
    const size_t pixel = size_t(y) * view.image.width + x;
    const uint32_t neighbor = view.face_ids[pixel];
    if (neighbor == no_face) return 0.f;
    const auto edge = [](const Vec3f& first, const Vec3f& second, float px, float py) {
        return (second.x() - first.x()) * (py - first.y()) -
               (second.y() - first.y()) * (px - first.x());
    };
    const float cross = edge(a, b, c.x(), c.y());
    if (std::abs(cross) < 1e-9f) return 0.f;
    const float wa = edge(b, c, x + .5f, y + .5f) / cross;
    const float wb = edge(c, a, x + .5f, y + .5f) / cross;
    const float plane_depth = wa * a.z() + wb * b.z() + (1.f - wa - wb) * c.z();
    if (std::abs(plane_depth - view.depth[pixel]) > view.surface_depth_tolerance) return 0.f;
    const auto& other = source.mesh.indices[neighbor];
    const Vec3f normal = (source.mesh.vertices[triangle[1]] - source.mesh.vertices[triangle[0]])
        .cross(source.mesh.vertices[triangle[2]] - source.mesh.vertices[triangle[0]]);
    const Vec3f neighbor_normal = (source.mesh.vertices[other[1]] - source.mesh.vertices[other[0]])
        .cross(source.mesh.vertices[other[2]] - source.mesh.vertices[other[0]]);
    if (normal.squaredNorm() <= 1e-18f || neighbor_normal.squaredNorm() <= 1e-18f ||
        normal.normalized().dot(neighbor_normal.normalized()) < .94f) return 0.f;
    Color source_rgb {};
    for (size_t corner = 0; corner < 3; ++corner) {
        const Color color = corner_color(source, face, int(corner));
        for (size_t channel = 0; channel < 3; ++channel)
            source_rgb[channel] += centroid[corner] * color[channel];
    }
    const Color visible_rgb {
        view.image.pixels[pixel * 3] / 255.f,
        view.image.pixels[pixel * 3 + 1] / 255.f,
        view.image.pixels[pixel * 3 + 2] / 255.f};
    if (distance(lab(source_rgb), lab(visible_rgb)) > .0025f) return 0.f;
    const Label label = prediction.labels[pixel];
    if (label != Label::EyeSclera && label != Label::Iris) return 0.f;
    float confidence = 1.f;
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        const size_t nearby = size_t(y + dy) * view.image.width + x + dx;
        if (view.face_ids[nearby] == no_face || prediction.labels[nearby] != label ||
            prediction.confidence[nearby] < .90f) return 0.f;
        confidence = std::min(confidence, prediction.confidence[nearby]);
    }
    output_pixel = uint32_t(pixel);
    return confidence;
}

struct Sample { Color color; double weight; };
std::vector<Color> prototypes(const std::vector<Sample>& samples, size_t limit)
{
    double total = 0;
    for (const auto& sample : samples) total += sample.weight;
    if (total <= 0) return {};
    // A coordinate-wise weighted median does not let baked highlights or a few
    // dark shadow faces dictate the material of an entire semantic region.
    Color median {};
    std::vector<std::pair<float, double>> sorted; sorted.reserve(samples.size());
    for (int channel = 0; channel < 3; ++channel) {
        sorted.clear();
        for (const auto& sample : samples) sorted.emplace_back(sample.color[channel], sample.weight);
        std::sort(sorted.begin(), sorted.end());
        double seen = 0;
        for (const auto& entry : sorted) { seen += entry.second; if (seen >= total * .5) { median[channel] = entry.first; break; } }
    }
    std::vector<Color> centers {median};
    // Supported histogram bins seed additional genuine source colors. A lone
    // tiny texture speck cannot consume a local prototype.
    struct Bin { double weight {0}; std::array<double, 3> sum {}; };
    std::map<std::array<int, 3>, Bin> bins;
    for (const auto& sample : samples) {
        const auto& c = sample.color;
        auto& bin = bins[{int(c[0] * 12), int((c[1] + .5f) * 16), int((c[2] + .5f) * 16)}];
        bin.weight += sample.weight;
        for (int channel = 0; channel < 3; ++channel) bin.sum[channel] += c[channel] * sample.weight;
    }
    while (centers.size() < limit) {
        float best = .0025f; Color chosen {}; bool found = false;
        for (const auto& entry : bins) {
            if (entry.second.weight < total * .08) continue;
            Color color {};
            for (int channel = 0; channel < 3; ++channel) color[channel] = float(entry.second.sum[channel] / entry.second.weight);
            float closest = std::numeric_limits<float>::max();
            for (const auto& center : centers) closest = std::min(closest, distance(color, center));
            if (closest > best) { best = closest; chosen = color; found = true; }
        }
        if (!found) break;
        centers.push_back(chosen);
    }
    return centers;
}

struct HairSeed { Color appearance {}; int slot {-1}; };
void extend_hair_edges(const MeshSnapshot& source, const Analysis& analysis,
                       const std::vector<HairSeed>& seeds, const std::vector<Color>& palette,
                       FaceColors& output)
{
    const size_t count = source.mesh.indices.size();
    if (std::none_of(seeds.begin(), seeds.end(), [](const HairSeed& seed) { return seed.slot >= 0; })) return;
    std::vector<uint32_t> owner(count, no_face);
    std::vector<uint8_t> hops(count, 255), eligible(count, 0);
    std::vector<Color> colors(count);
    std::vector<Vec3f> normals(count, Vec3f::Zero());
    std::vector<Vec3f> positions(count, Vec3f::Zero());
    Vec3f lower = source.mesh.vertices.front(), upper = lower;
    for (const auto& vertex : source.mesh.vertices) { lower = lower.cwiseMin(vertex); upper = upper.cwiseMax(vertex); }
    const float maximum_distance_squared = (upper - lower).squaredNorm() * (.008f * .008f);
    bool have_seed = false, have_border = false;
    for (size_t id = 0; id < count; ++id) {
        const Label label = analysis.face_labels[id];
        const bool seed = seeds[id].slot >= 0 && analysis.face_confidence[id] >= .80f;
        const bool border = analysis.face_confidence[id] < minimum_confidence &&
            (label == Label::Hair || label == Label::Unknown || label == Label::Background || label == Label::Clothes);
        if (!seed && !border) continue;
        colors[id] = face_lab(source, id);
        if (!natural_dark_hair(colors[id])) continue;
        const auto& face = source.mesh.indices[id];
        positions[id] = (source.mesh.vertices[face[0]] + source.mesh.vertices[face[1]] + source.mesh.vertices[face[2]]) / 3.f;
        normals[id] = (source.mesh.vertices[face[1]] - source.mesh.vertices[face[0]]).cross(
            source.mesh.vertices[face[2]] - source.mesh.vertices[face[0]]);
        const float length = normals[id].norm();
        if (length <= 0) continue;
        normals[id] /= length;
        if (seed) { owner[id] = uint32_t(id); hops[id] = 0; have_seed = true; }
        else { eligible[id] = 1; have_border = true; }
    }
    if (!have_seed || !have_border) return;

    struct Edge { uint64_t vertices; uint32_t face; };
    std::vector<Edge> edges; edges.reserve(count * 3);
    for (size_t id = 0; id < count; ++id) {
        const auto& face = source.mesh.indices[id];
        for (int corner = 0; corner < 3; ++corner) {
            const uint32_t a = uint32_t(face[corner]), b = uint32_t(face[(corner + 1) % 3]);
            if (a != b) edges.push_back({(uint64_t(std::min(a, b)) << 32) | std::max(a, b), uint32_t(id)});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.vertices < b.vertices; });
    std::vector<std::pair<uint32_t, uint32_t>> neighbours;
    for (size_t first = 0; first < edges.size();) {
        size_t end = first + 1; bool touches_skin = false;
        while (end < edges.size() && edges[end].vertices == edges[first].vertices) ++end;
        for (size_t edge = first; edge < end; ++edge) {
            const uint32_t id = edges[edge].face;
            if (analysis.face_confidence[id] >= minimum_confidence &&
                (analysis.face_labels[id] == Label::FaceSkin || analysis.face_labels[id] == Label::BodySkin ||
                 analysis.face_labels[id] == Label::EyeSclera || analysis.face_labels[id] == Label::Iris ||
                 analysis.face_labels[id] == Label::Eyebrow)) touches_skin = true;
        }
        // A dark uncertain iris/eyelid or skin contour must not become hair.
        // The face model need not expose eye-specific landmark topology here.
        if (touches_skin) for (size_t edge = first; edge < end; ++edge) eligible[edges[edge].face] = 0;
        if (end - first == 2 && edges[first].face != edges[first + 1].face) {
            const uint32_t a = edges[first].face, b = edges[first + 1].face;
            // With a shared edge, equal sums mean the third index is equal as
            // well: reject duplicate triangles of either winding.
            if (source.mesh.indices[a].sum() != source.mesh.indices[b].sum()) neighbours.emplace_back(a, b);
        }
        first = end;
    }
    // The full edge list was needed to detect nonmanifold and skin boundaries.
    // Propagation now uses just the ordinary shared-edge pairs.
    std::vector<Edge>().swap(edges);
    std::vector<float> cost(count, 0.f), proposed_cost(count);
    std::vector<uint32_t> proposed(count);
    // Fill only a narrow uncertain border. Shared edges, original appearance,
    // and a fixed seed constrain every step; accepted skin/clothes never move.
    // Proposals are applied together, so face traversal cannot cause a flood.
    for (uint8_t step = 0; step < 3; ++step) {
        std::fill(proposed.begin(), proposed.end(), no_face);
        std::fill(proposed_cost.begin(), proposed_cost.end(), std::numeric_limits<float>::max());
        const auto propose = [&](uint32_t from, uint32_t to) {
            if (hops[from] != step || !eligible[to] || owner[to] != no_face) return;
            const auto seed = owner[from];
            // Three very large triangles must not turn a narrow repair into a
            // broad fill. Keep the radius relative to the original model size.
            if ((positions[to] - positions[seed]).squaredNorm() > maximum_distance_squared) return;
            if (normals[from].dot(normals[to]) < .75f || !same_hair_material(colors[to], seeds[seed].appearance)) return;
            const float difference = distance(colors[from], colors[to]), total = cost[from] + difference;
            if (difference > .0016f || total > .0025f) return;
            if (total < proposed_cost[to] || (total == proposed_cost[to] && seed < proposed[to])) {
                proposed_cost[to] = total; proposed[to] = seed;
            }
        };
        for (const auto& pair : neighbours) {
            propose(pair.first, pair.second);
            propose(pair.second, pair.first);
        }
        bool changed = false;
        for (size_t id = 0; id < count; ++id) if (proposed[id] != no_face) {
            owner[id] = proposed[id]; hops[id] = step + 1; cost[id] = proposed_cost[id];
            output.emplace_back(id, palette[size_t(seeds[owner[id]].slot)]); changed = true;
        }
        if (!changed) break;
    }
}

} // namespace

bool RGBImage::valid() const
{
    return width > 0 && height > 0 && width <= 4096 && height <= 4096 &&
        pixels.size() == size_t(width) * height * 3;
}
bool Prediction::valid_for(const RGBImage& image) const
{
    if (!image.valid() || canceled || !error.empty() || labels.size() != size_t(image.width) * image.height ||
        confidence.size() != labels.size()) return false;
    for (size_t i = 0; i < labels.size(); ++i)
        if (!valid_label(labels[i]) || !std::isfinite(confidence[i]) || confidence[i] < 0 || confidence[i] > 1) return false;
    return true;
}

std::string content_fingerprint(const MeshSnapshot& source)
{
    if (source.geometry_id.empty() || !validate_snapshot(source).empty()) return {};
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) return {};
    const std::string prefix = std::string("orca.semantic-source/v1:") + source.geometry_id + ":" + std::to_string(source.mesh.indices.size()) + ":";
    if (EVP_DigestUpdate(ctx.get(), prefix.data(), prefix.size()) != 1) return {};
    std::array<unsigned char, 1024 * 3 * 3 * 4> buffer {};
    size_t used = 0;
    for (size_t face = 0; face < source.mesh.indices.size(); ++face) {
        for (int corner = 0; corner < 3; ++corner) {
            for (float color : corner_color(source, face, corner)) {
                const float normalized = color == 0.f ? 0.f : color;
                uint32_t bits; std::memcpy(&bits, &normalized, sizeof(bits));
                for (size_t byte = 0; byte < 4; ++byte) buffer[used++] = static_cast<unsigned char>(bits >> (byte * 8));
            }
        }
        if (used == buffer.size()) { if (EVP_DigestUpdate(ctx.get(), buffer.data(), used) != 1) return {}; used = 0; }
    }
    if (used && EVP_DigestUpdate(ctx.get(), buffer.data(), used) != 1) return {};
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes {}; unsigned int count = 0;
    if (EVP_DigestFinal_ex(ctx.get(), bytes.data(), &count) != 1 || count != 32) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (unsigned int i = 0; i < count; ++i) { result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15]; }
    return result;
}

RenderedView render_view(const MeshSnapshot& source, float yaw_degrees, int image_size, const Cancel& cancel)
{
    return render_region(source, yaw_degrees, {}, image_size, cancel);
}

RenderedView render_region(const MeshSnapshot& source, float yaw_degrees, const ViewRegion& region,
                           int image_size, const Cancel& cancel)
{
    RenderedView result;
    result.error = validate_snapshot(source);
    if (!result.error.empty()) return result;
    if (image_size < 8 || image_size > 2048 || !std::isfinite(yaw_degrees) ||
        !std::isfinite(region.left) || !std::isfinite(region.top) || !std::isfinite(region.width) || !std::isfinite(region.height) ||
        region.left < 0 || region.top < 0 || region.width < 1.f/4096 || region.height < 1.f/4096 ||
        region.left + region.width > 1.000001f || region.top + region.height > 1.000001f) {
        result.error = "Invalid semantic view dimensions or camera."; return result;
    }
    if (stopped(cancel)) { result.canceled = true; return result; }
    constexpr float radians = 3.14159265358979323846f / 180.f;
    const float yaw = yaw_degrees * radians;
    const Vec3f direction = Vec3f(std::sin(yaw), -std::cos(yaw), .10f).normalized();
    const Vec3f right(std::cos(yaw), std::sin(yaw), 0.f);
    const Vec3f up = direction.cross(right).normalized();
    std::vector<Vec3f> screen; screen.reserve(source.mesh.vertices.size());
    float min_x = std::numeric_limits<float>::max(), min_y = min_x, max_x = -min_x, max_y = -min_x;
    for (const auto& vertex : source.mesh.vertices) {
        const Vec3f p(vertex.dot(right), vertex.dot(up), vertex.dot(direction)); screen.push_back(p);
        min_x = std::min(min_x, p.x()); max_x = std::max(max_x, p.x());
        min_y = std::min(min_y, p.y()); max_y = std::max(max_y, p.y());
    }
    const float span = std::max(max_x - min_x, max_y - min_y);
    if (!std::isfinite(span) || span <= 0) { result.error = "The semantic view has no projected surface."; return result; }
    // Use one normalized full-view camera regardless of target size. ROI
    // magnification changes the viewport transform only, not mesh coordinates
    // or the camera's center, depth convention, and source face ordering.
    const float scale = (.88f * 511.f / 512.f) / span;
    const float magnification = image_size / std::max(region.width, region.height);
    const float offset_x = (image_size - region.width * magnification) * .5f;
    const float offset_y = (image_size - region.height * magnification) * .5f;
    for (auto& p : screen) {
        p.x() = (.5f + (p.x() - (min_x + max_x) * .5f) * scale - region.left) * magnification + offset_x;
        p.y() = (.5f - (p.y() - (min_y + max_y) * .5f) * scale - region.top) * magnification + offset_y;
    }
    const size_t pixel_count = size_t(image_size) * image_size;
    result.image = {image_size, image_size, std::vector<uint8_t>(pixel_count * 3, 128)};
    result.face_ids.assign(pixel_count, no_face);
    result.barycentric.assign(pixel_count, {});
    result.depth.assign(pixel_count, -std::numeric_limits<float>::infinity());
    result.facing.assign(source.mesh.indices.size(), 0.f);
    const auto edge = [](const Vec3f& a, const Vec3f& b, float x, float y) {
        return (b.x() - a.x()) * (y - a.y()) - (b.y() - a.y()) * (x - a.x());
    };
    for (size_t face = 0; face < source.mesh.indices.size(); ++face) {
        if ((face & 1023) == 0 && stopped(cancel)) { result = {}; result.canceled = true; return result; }
        const auto& f = source.mesh.indices[face];
        const auto& a = screen[f[0]]; const auto& b = screen[f[1]]; const auto& c = screen[f[2]];
        const float cross = edge(a, b, c.x(), c.y());
        if (std::abs(cross) < 1e-9f) continue;
        const Vec3f normal = (source.mesh.vertices[f[1]] - source.mesh.vertices[f[0]])
            .cross(source.mesh.vertices[f[2]] - source.mesh.vertices[f[0]]);
        if (normal.squaredNorm() <= 1e-18f) continue;
        result.facing[face] = std::max(.05f, std::abs(normal.normalized().dot(direction)));
        const int left = std::max(0, int(std::floor(std::min({a.x(), b.x(), c.x()}))));
        const int right_pixel = std::min(image_size - 1, int(std::ceil(std::max({a.x(), b.x(), c.x()}))));
        const int top = std::max(0, int(std::floor(std::min({a.y(), b.y(), c.y()}))));
        const int bottom = std::min(image_size - 1, int(std::ceil(std::max({a.y(), b.y(), c.y()}))));
        const auto ca = corner_color(source, face, 0), cb = corner_color(source, face, 1), cc = corner_color(source, face, 2);
        for (int y = top; y <= bottom; ++y) for (int x = left; x <= right_pixel; ++x) {
            const float wa = edge(b, c, x + .5f, y + .5f) / cross;
            const float wb = edge(c, a, x + .5f, y + .5f) / cross;
            const float wc = 1.f - wa - wb;
            if (wa < -1e-6f || wb < -1e-6f || wc < -1e-6f) continue;
            const float depth = wa*a.z() + wb*b.z() + wc*c.z();
            const size_t pixel = size_t(y) * image_size + x;
            if (depth <= result.depth[pixel]) continue;
            result.depth[pixel] = depth; result.face_ids[pixel] = uint32_t(face);
            result.barycentric[pixel] = {wa, wb, wc};
            for (int channel = 0; channel < 3; ++channel)
                result.image.pixels[pixel * 3 + channel] = uint8_t(std::lround(std::clamp(
                    wa*ca[channel] + wb*cb[channel] + wc*cc[channel], 0.f, 1.f) * 255));
        }
    }
    std::vector<uint8_t> directly_seen(source.mesh.indices.size(), 0);
    for (uint32_t face : result.face_ids) if (face != no_face) directly_seen[face] = 1;
    result.surface_samples.assign(source.mesh.indices.size(), no_face);
    const float depth_tolerance = std::max(span * 1e-7f, .10f / (scale * magnification));
    for (size_t face = 0; face < source.mesh.indices.size(); ++face) {
        if ((face & 4095) == 0 && stopped(cancel)) { result = {}; result.canceled = true; return result; }
        if (directly_seen[face] || result.facing[face] < .15f) continue;
        const auto& f = source.mesh.indices[face];
        const auto& a = screen[f[0]]; const auto& b = screen[f[1]]; const auto& c = screen[f[2]];
        const float cross = edge(a, b, c.x(), c.y());
        if (std::abs(cross) < 1e-9f) continue;
        const float cx = (a.x() + b.x() + c.x()) / 3.f, cy = (a.y() + b.y() + c.y()) / 3.f;
        if (cx < 0 || cy < 0 || cx >= image_size || cy >= image_size) continue;
        const int x = int(cx), y = int(cy);
        const size_t pixel = size_t(y) * image_size + x;
        const uint32_t neighbor = result.face_ids[pixel]; if (neighbor == no_face) continue;
        // Evaluate this face's plane at the neighboring pixel center. Comparing
        // raw centroid depth would incorrectly reject an inclined surface.
        const float wa = edge(b, c, x + .5f, y + .5f) / cross;
        const float wb = edge(c, a, x + .5f, y + .5f) / cross;
        const float plane_depth = wa*a.z() + wb*b.z() + (1.f-wa-wb)*c.z();
        if (std::abs(plane_depth - result.depth[pixel]) > depth_tolerance) continue;
        const auto& n = source.mesh.indices[neighbor];
        const Vec3f normal = (source.mesh.vertices[f[1]] - source.mesh.vertices[f[0]])
            .cross(source.mesh.vertices[f[2]] - source.mesh.vertices[f[0]]);
        const Vec3f neighbor_normal = (source.mesh.vertices[n[1]] - source.mesh.vertices[n[0]])
            .cross(source.mesh.vertices[n[2]] - source.mesh.vertices[n[0]]);
        if (normal.squaredNorm() <= 1e-18f || neighbor_normal.squaredNorm() <= 1e-18f ||
            normal.normalized().dot(neighbor_normal.normalized()) < .94f) continue;
        result.surface_samples[face] = uint32_t(pixel);
    }
    result.projected_vertices = std::move(screen);
    result.surface_depth_tolerance = depth_tolerance;
    return result;
}

bool locate_subface(const Barycentric& input, uint8_t depth, SubfacePath& output)
{
    if (depth == 0 || depth > 2 || !std::all_of(input.begin(), input.end(), [](float value) {
            return std::isfinite(value) && value >= -1e-5f && value <= 1.00001f;
        }) || std::abs(input[0] + input[1] + input[2] - 1.f) > 1e-4f)
        return false;
    Barycentric bary {std::max(0.f, input[0]), std::max(0.f, input[1]), std::max(0.f, input[2])};
    uint8_t path = 0;
    for (uint8_t level = 0; level < depth; ++level) {
        uint8_t child = 3;
        if (bary[0] >= .5f) child = 0;
        else if (bary[1] >= .5f) child = 1;
        else if (bary[2] >= .5f) child = 2;
        path = uint8_t((path << 2) | child);
        if (child == 0) bary = {2.f * bary[0] - 1.f, 2.f * bary[1], 2.f * bary[2]};
        else if (child == 1) bary = {2.f * bary[0], 2.f * bary[1] - 1.f, 2.f * bary[2]};
        // TriangleSelector stores corner 2 as (midpoint 1-2, vertex 2,
        // midpoint 2-0), so its local barycentric axes rotate with the child.
        else if (child == 2) bary = {2.f * bary[1], 2.f * bary[2] - 1.f, 2.f * bary[0]};
        else bary = {bary[0] + bary[1] - bary[2], bary[1] + bary[2] - bary[0],
                     bary[2] + bary[0] - bary[1]};
        for (float& value : bary) value = std::max(0.f, value);
        const float sum = bary[0] + bary[1] + bary[2];
        if (sum <= 0.f) return false;
        for (float& value : bary) value /= sum;
    }
    output = {depth, path};
    return true;
}

bool subface_vertices(const SubfacePath& path, std::array<Barycentric, 3>& output)
{
    if (path.depth == 0 || path.depth > 2 || unsigned(path.value) >= (1u << (2u * path.depth)))
        return false;
    std::array<Barycentric, 3> vertices {{{1.f,0.f,0.f}, {0.f,1.f,0.f}, {0.f,0.f,1.f}}};
    const auto midpoint = [](const Barycentric& lhs, const Barycentric& rhs) {
        return Barycentric {(lhs[0] + rhs[0]) * .5f, (lhs[1] + rhs[1]) * .5f,
                            (lhs[2] + rhs[2]) * .5f};
    };
    for (uint8_t level = 0; level < path.depth; ++level) {
        const unsigned shift = 2u * unsigned(path.depth - level - 1);
        const unsigned child = (unsigned(path.value) >> shift) & 3u;
        const auto ab = midpoint(vertices[0], vertices[1]);
        const auto bc = midpoint(vertices[1], vertices[2]);
        const auto ca = midpoint(vertices[2], vertices[0]);
        if (child == 0) vertices = {vertices[0], ab, ca};
        else if (child == 1) vertices = {ab, vertices[1], bc};
        else if (child == 2) vertices = {bc, vertices[2], ca};
        else vertices = {ab, bc, ca};
    }
    output = vertices;
    return true;
}

static bool subfaces_share_edge(const SubfacePath& lhs, const SubfacePath& rhs)
{
    if (lhs.depth != rhs.depth || lhs == rhs) return false;
    std::array<Barycentric, 3> lhs_vertices, rhs_vertices;
    if (!subface_vertices(lhs, lhs_vertices) || !subface_vertices(rhs, rhs_vertices)) return false;
    size_t shared = 0;
    for (const Barycentric& lhs_vertex : lhs_vertices)
        if (std::find(rhs_vertices.begin(), rhs_vertices.end(), lhs_vertex) != rhs_vertices.end())
            ++shared;
    return shared == 2;
}

std::string analysis_cache_key(const MeshSnapshot& source, const std::string& body, const std::string& face)
{
    if (source.geometry_id.empty() || source.content_id.empty() || body.empty() || face.empty()) return {};
    return sha256(nlohmann::json::array({pipeline_version, source.geometry_id, source.content_id,
        source.mesh.indices.size(), body, face, "cpu-zbuffer-8x512-face-roi512-visible-source-samples-v2"}).dump());
}

namespace {
std::string diagnostic_analysis_key(const MeshSnapshot& source, const std::string& body,
                                    const std::string& face, int face_roi_size)
{
    if (face_roi_size == 512) return analysis_cache_key(source, body, face);
    if (source.geometry_id.empty() || source.content_id.empty() || body.empty() || face.empty()) return {};
    return sha256(nlohmann::json::array({pipeline_version, source.geometry_id, source.content_id,
        source.mesh.indices.size(), body, face,
        "cpu-zbuffer-8x512-face-roi" + std::to_string(face_roi_size) + "-visible-source-samples-v2-diagnostic"}).dump());
}
}

Analysis analyze_with_face_roi_size(const MeshSnapshot& source, IBodyRegionRecognizer& body,
                                    IFaceRegionRecognizer& face, int face_roi_size,
                                    const Cancel& cancel, const Progress& progress,
                                    AnalysisDiagnostics* diagnostics)
{
    Analysis result;
    if (diagnostics) diagnostics->face_regions.clear();
    result.face_roi_size = face_roi_size;
    result.geometry_id = source.geometry_id; result.content_id = source.content_id;
    try {
        if (face_roi_size < 128 || face_roi_size > 2048 ||
            (face_roi_size & (face_roi_size - 1)) != 0)
            throw std::invalid_argument("The diagnostic face ROI size must be a power of two from 128 to 2048.");
        const float yaw_offset = diagnostics ? diagnostics->yaw_offset_degrees : 0.f;
        if (!std::isfinite(yaw_offset) || std::abs(yaw_offset) > 22.5f)
            throw std::invalid_argument("The diagnostic yaw offset must be finite and within 22.5 degrees.");
        result.body_identity = body.identity(); result.face_identity = face.identity();
        result.signature = diagnostic_analysis_key(source, result.body_identity, result.face_identity,
                                                   face_roi_size);
        result.error = validate_snapshot(source);
        if (!result.error.empty()) return result;
        if (result.signature.empty()) { result.error = "Semantic analysis requires complete model and recognizer identities."; return result; }
        const size_t count = source.mesh.indices.size();
        std::vector<std::array<float, label_count>> votes(count);
        std::vector<float> weights(count, 0.f), detail_weights(count, 0.f);
        std::vector<std::array<float, face_detail_labels.size()>> detail_votes(count);
        std::vector<std::array<float, face_detail_labels.size()>> peak_detail_confidence(count);
        std::vector<float> view_detail_weights(count, 0.f);
        std::vector<std::array<float, face_detail_labels.size()>> view_detail_votes(count);
        std::vector<size_t> view_detail_faces;
        struct LeafVotes {
            float weight {0.f};
            std::array<float, face_detail_labels.size()> votes {};
            std::array<float, face_detail_labels.size()> peak {};
            uint32_t samples {0};
        };
        std::unordered_map<uint64_t, LeafVotes> leaf_votes;
        std::vector<Color> original; original.reserve(count);
        for (size_t id = 0; id < count; ++id) original.push_back(face_lab(source, id));
        for (int view_index = 0; view_index < 8; ++view_index) {
            if (stopped(cancel)) { result.canceled = true; return result; }
            if (progress) progress(view_index * 100 / 8, "Recognizing model regions");
            const float yaw_degrees = view_index * 45.f + yaw_offset;
            auto view = render_view(source, yaw_degrees, 512, cancel);
            if (view.canceled) { result.canceled = true; return result; }
            if (!view.error.empty()) { result.error = view.error; return result; }
            ++result.rendered_views;
            auto prediction = body.predict(view.image, cancel);
            if (prediction.canceled || stopped(cancel)) { result.canceled = true; return result; }
            if (!prediction.valid_for(view.image)) {
                result.error = prediction.error.empty() ? "The body recognizer returned an invalid mask." : prediction.error; return result;
            }
            if (!refine_dark_hair_mask(view.image, prediction, cancel)) {
                result.canceled = true; return result;
            }
            for (size_t pixel = 0; pixel < view.face_ids.size(); ++pixel) {
                const uint32_t id = view.face_ids[pixel]; if (id == no_face) continue;
                const float weight = view.facing[id]; weights[id] += weight;
                votes[id][size_t(prediction.labels[pixel])] += weight * prediction.confidence[pixel];
            }
            for (size_t id = 0; id < count; ++id) {
                const float confidence = supported_surface_sample(view, prediction, id, original);
                if (confidence <= 0) continue;
                const Label label = prediction.labels[view.surface_samples[id]];
                const float weight = view.facing[id] * .75f;
                weights[id] += weight; votes[id][size_t(label)] += weight * confidence;
            }
            const auto regions = face_regions(view, prediction);
            if (regions.empty()) continue;
            ++result.face_views;
            for (const ViewRegion& region : regions) {
                auto crop = render_region(source, yaw_degrees, region, face_roi_size, cancel);
                if (crop.canceled) { result.canceled = true; return result; }
                if (!crop.error.empty()) { result.error = crop.error; return result; }
                auto details = face.predict(crop.image, cancel);
                if (details.canceled || stopped(cancel)) { result.canceled = true; return result; }
                if (!details.valid_for(crop.image)) {
                    result.error = details.error.empty() ? "The face recognizer returned an invalid mask." : details.error; return result;
                }
                // A segmentation mask alone is not proof that a rendered animal
                // or object is a person. Require the independent face detector.
                if (!details.face_detected) {
                    if (diagnostics) diagnostics->face_regions.push_back({view_index, region, false, {}});
                    continue;
                }
                result.person_detected = true;
                view_detail_faces.clear();
                std::unordered_map<uint64_t, LeafVotes> view_leaf_votes;
                for (size_t pixel = 0; pixel < crop.face_ids.size(); ++pixel) {
                    const uint32_t id = crop.face_ids[pixel]; if (id == no_face) continue;
                    const float weight = crop.facing[id];
                    if (view_detail_weights[id] == 0.f) view_detail_faces.push_back(id);
                    view_detail_weights[id] += weight; detail_weights[id] += weight;
                    const Label label = details.labels[pixel];
                    const size_t detail = face_detail_index(label);
                    if (detail < face_detail_labels.size()) {
                        view_detail_votes[id][detail] += weight * details.confidence[pixel];
                        detail_votes[id][detail] += weight * details.confidence[pixel];
                    }
                    SubfacePath path;
                    if (locate_subface(crop.barycentric[pixel], 2, path)) {
                        LeafVotes& leaf = view_leaf_votes[uint64_t(id) * 16u + path.value];
                        leaf.weight += weight;
                        if (leaf.samples != std::numeric_limits<uint32_t>::max()) ++leaf.samples;
                        if (detail < face_detail_labels.size())
                            leaf.votes[detail] += weight * details.confidence[pixel];
                    }
                }
                std::set<size_t> supported_eye_faces;
                for (const auto& item : view_leaf_votes) {
                    const size_t sclera = face_detail_index(Label::EyeSclera);
                    const size_t iris = face_detail_index(Label::Iris);
                    if (item.second.votes[sclera] > 0.f || item.second.votes[iris] > 0.f)
                        supported_eye_faces.insert(size_t(item.first / 16u));
                }
                for (size_t id = 0; id < count; ++id) {
                    const float confidence = supported_surface_sample(crop, details, id, original);
                    if (confidence <= 0) continue;
                    const Label label = details.labels[crop.surface_samples[id]];
                    const size_t detail = face_detail_index(label);
                    if (detail == face_detail_labels.size()) continue;
                    const float weight = crop.facing[id] * .75f;
                    if (view_detail_weights[id] == 0.f) view_detail_faces.push_back(id);
                    view_detail_weights[id] += weight;
                    view_detail_votes[id][detail] += weight * confidence;
                    detail_weights[id] += weight;
                    detail_votes[id][detail] += weight * confidence;
                    if (label == Label::EyeSclera || label == Label::Iris)
                        supported_eye_faces.insert(id);
                }
                for (size_t face_id : supported_eye_faces) {
                    for (uint8_t value = 0; value < 16; ++value) {
                        const uint64_t key = uint64_t(face_id) * 16u + value;
                        if (view_leaf_votes.count(key) != 0) continue;
                        uint32_t pixel = no_face;
                        const float confidence = supported_subface_surface_sample(
                            source, crop, details, face_id, {2, value}, pixel);
                        if (confidence <= 0.f || pixel == no_face) continue;
                        const size_t detail = face_detail_index(details.labels[pixel]);
                        if (detail >= face_detail_labels.size()) continue;
                        LeafVotes& leaf = view_leaf_votes[key];
                        const float weight = crop.facing[face_id] * .75f;
                        leaf.weight = weight;
                        leaf.samples = 1;
                        leaf.votes[detail] = weight * confidence;
                    }
                }
                if (diagnostics) {
                    FaceRegionDiagnostic diagnostic {view_index, region, true, {}};
                    diagnostic.leaves.reserve(view_leaf_votes.size());
                    for (const auto& item : view_leaf_votes) {
                        if (item.second.weight <= 0.f || item.second.samples == 0) continue;
                        const float recognized = std::accumulate(item.second.votes.begin(),
                            item.second.votes.end(), 0.f);
                        if (recognized <= 0.f) continue;
                        const size_t detail = size_t(std::max_element(item.second.votes.begin(),
                            item.second.votes.end()) - item.second.votes.begin());
                        const size_t face_id = size_t(item.first / 16u);
                        const SubfacePath path {2, uint8_t(item.first % 16u)};
                        if (diagnostics->include_leaf && !diagnostics->include_leaf(face_id, path)) continue;
                        diagnostic.leaves.push_back({face_id, path, face_detail_labels[detail],
                            std::clamp(item.second.votes[detail] / item.second.weight, 0.f, 1.f),
                            std::clamp(item.second.votes[detail] / recognized, 0.f, 1.f),
                            item.second.samples});
                    }
                    std::sort(diagnostic.leaves.begin(), diagnostic.leaves.end(), [](const auto& lhs, const auto& rhs) {
                        return lhs.face_id != rhs.face_id ? lhs.face_id < rhs.face_id : lhs.path < rhs.path;
                    });
                    diagnostics->face_regions.push_back(std::move(diagnostic));
                }
                for (const auto& item : view_leaf_votes) {
                    LeafVotes& accumulated = leaf_votes[item.first];
                    accumulated.weight += item.second.weight;
                    accumulated.samples = item.second.samples > std::numeric_limits<uint32_t>::max() - accumulated.samples
                        ? std::numeric_limits<uint32_t>::max() : accumulated.samples + item.second.samples;
                    for (size_t detail = 0; detail < face_detail_labels.size(); ++detail) {
                        accumulated.votes[detail] += item.second.votes[detail];
                        accumulated.peak[detail] = std::max(accumulated.peak[detail],
                            item.second.weight > 0.f ?
                                std::clamp(item.second.votes[detail] / item.second.weight, 0.f, 1.f) : 0.f);
                    }
                }
                for (size_t id : view_detail_faces) {
                    for (size_t detail = 0; detail < face_detail_labels.size(); ++detail) {
                        peak_detail_confidence[id][detail] = std::max(peak_detail_confidence[id][detail],
                            std::clamp(view_detail_votes[id][detail] / view_detail_weights[id], 0.f, 1.f));
                        view_detail_votes[id][detail] = 0.f;
                    }
                    view_detail_weights[id] = 0.f;
                }
            }
        }
        if (stopped(cancel)) { result.canceled = true; return result; }
        result.face_labels.assign(count, Label::Unknown); result.face_confidence.assign(count, 0.f);
        for (size_t id = 0; id < count; ++id) {
            if (weights[id] <= 0 && detail_weights[id] <= 0) continue;
            ++result.observed_faces;
            if (weights[id] > 0) {
                const size_t chosen = size_t(std::max_element(votes[id].begin(), votes[id].end()) - votes[id].begin());
                result.face_labels[id] = Label(chosen);
                result.face_confidence[id] = std::clamp(votes[id][chosen] / weights[id], 0.f, 1.f);
            }
            // Details replace a face only when they cover most of its visible
            // samples. A tiny lip pixel cannot recolor a coarse cheek triangle.
            if (detail_weights[id] > 0) {
                const size_t detail = size_t(std::max_element(detail_votes[id].begin(), detail_votes[id].end()) - detail_votes[id].begin());
                const Label detail_label = face_detail_labels[detail];
                const float accumulated = std::clamp(detail_votes[id][detail] / detail_weights[id], 0.f, 1.f);
                const float recognized = std::accumulate(detail_votes[id].begin(), detail_votes[id].end(), 0.f);
                const float dominance = recognized > 0.f ? std::clamp(detail_votes[id][detail] / recognized, 0.f, 1.f) : 0.f;
                float confidence = accumulated;
                // Brow geometry may only be visible in one face crop, so a
                // dominant best view may survive Unknown side views. Eye,
                // lip and mouth details retain the stricter accumulated rule;
                // recovering their partial triangles needs subface boundaries.
                if (detail_label == Label::Eyebrow && dominance >= minimum_confidence)
                    confidence = std::max(confidence,
                        std::min(peak_detail_confidence[id][detail], dominance));
                if (confidence >= minimum_confidence) {
                    result.face_labels[id] = detail_label;
                    result.face_confidence[id] = confidence;
                }
            }
            if (paintable(result.face_labels[id]) && result.face_confidence[id] >= minimum_confidence) ++result.reliable_faces;
        }
        result.subface_labels.reserve(std::min(leaf_votes.size(), maximum_subface_evidence));
        for (const auto& item : leaf_votes) {
            if (item.second.weight <= 0.f || item.second.samples == 0) continue;
            const size_t detail = size_t(std::max_element(item.second.votes.begin(), item.second.votes.end()) -
                                         item.second.votes.begin());
            const Label label = face_detail_labels[detail];
            const size_t face_id = size_t(item.first / 16u);
            if (face_id >= count) continue;
            const bool supported_skin_boundary = label == Label::FaceSkin &&
                (result.face_labels[face_id] == Label::EyeSclera ||
                 result.face_labels[face_id] == Label::Hair);
            if (label != Label::EyeSclera && label != Label::Iris && label != Label::Eyebrow &&
                !supported_skin_boundary) continue;
            const float recognized = std::accumulate(item.second.votes.begin(), item.second.votes.end(), 0.f);
            const float accumulated = std::clamp(item.second.votes[detail] / item.second.weight, 0.f, 1.f);
            const float dominance = recognized > 0.f ?
                std::clamp(item.second.votes[detail] / recognized, 0.f, 1.f) : 0.f;
            const float confidence = std::max(accumulated,
                dominance >= minimum_confidence ? std::min(item.second.peak[detail], dominance) : 0.f);
            // Dense generated meshes often give a depth-2 leaf only one direct
            // pixel in the 512px face crop. Keep such evidence only when the
            // model is correspondingly more certain; three-pixel leaves retain
            // the ordinary semantic threshold.
            const float evidence_threshold = supported_skin_boundary ?
                (item.second.samples >= 2 ? .82f : .86f) :
                (item.second.samples >= 3 ? minimum_confidence : item.second.samples == 2 ? .80f : .90f);
            if (confidence < evidence_threshold || dominance < evidence_threshold) continue;
            if (result.subface_labels.size() >= maximum_subface_evidence) {
                result.error = "The face-detail boundary evidence exceeds the local processing limit.";
                result.subface_labels.clear();
                return result;
            }
            result.subface_labels.push_back({face_id, {2, uint8_t(item.first % 16u)}, label,
                                             confidence, item.second.samples});
        }
        std::sort(result.subface_labels.begin(), result.subface_labels.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.face_id != rhs.face_id ? lhs.face_id < rhs.face_id : lhs.path < rhs.path;
        });
        if (progress) progress(100, "Model region recognition complete");
    } catch (const std::exception& error) { result.error = error.what(); result.face_labels.clear(); result.face_confidence.clear(); }
    return result;
}

Analysis analyze(const MeshSnapshot& source, IBodyRegionRecognizer& body, IFaceRegionRecognizer& face,
                 const Cancel& cancel, const Progress& progress)
{
    return analyze_with_face_roi_size(source, body, face, 512, cancel, progress);
}

FaceColors map_palette(const MeshSnapshot& source, const Analysis& analysis, const std::vector<Color>& palette,
                       const std::vector<Color>& portrait_card)
{
    const size_t count = source.mesh.indices.size();
    if (!analysis.person_detected || analysis.canceled || !analysis.error.empty() ||
        analysis.face_roi_size != 512 || palette.empty() || palette.size() > 6 ||
        analysis.geometry_id != source.geometry_id || analysis.content_id != source.content_id ||
        analysis.signature != analysis_cache_key(source, analysis.body_identity, analysis.face_identity) ||
        analysis.face_labels.size() != count || analysis.face_confidence.size() != count || !validate_snapshot(source).empty()) return {};
    for (const auto& color : palette) if (!valid_color(color)) return {};
    for (size_t id = 0; id < count; ++id)
        if (!valid_label(analysis.face_labels[id]) || !std::isfinite(analysis.face_confidence[id]) ||
            analysis.face_confidence[id] < 0 || analysis.face_confidence[id] > 1) return {};
    std::vector<Color> palette_labs; for (const auto& color : palette) palette_labs.push_back(lab(color));
    std::array<size_t, 6> role_slots {}; role_slots.fill(palette.size());
    bool has_card = portrait_card.size() == 6;
    if (has_card) for (size_t role = 0; role < 6; ++role) {
        for (size_t slot = 0; slot < palette.size(); ++slot) {
            bool same = true;
            for (int channel = 0; channel < 3; ++channel)
                same = same && std::abs(palette[slot][channel] - portrait_card[role][channel]) <= .5f / 255.f;
            if (same) { role_slots[role] = slot; break; }
        }
        if (role_slots[role] == palette.size()) has_card = false;
    }
    // These are appearance anchors from the existing portrait-card mapping,
    // not output colors. Comparing source colors with pastel print colors would
    // turn warm gray clothing into skin and dark green clothing into black.
    const std::array<Color, 6> anchors {{
        lab({.76f,.55f,.41f}), lab({.12f,.11f,.12f}), lab({.94f,.94f,.94f}),
        lab({.68f,.26f,.24f}), lab({.24f,.39f,.38f}), lab({.40f,.39f,.38f})
    }};
    const auto choose_unprotected_target = [&](const Color& center, Label label) {
        const float source_chroma = chroma(center);
        if (label == Label::Eyebrow && has_card && center[0] < .72f && source_chroma < .12f &&
            center[1] >= -.012f && center[2] >= -.012f) {
            // Eyebrow pigment is a facial material, not a baked highlight.
            // Preserve genuinely light or dyed brows by leaving them to the
            // regional matcher. The portrait card uses its neutral midtone for
            // natural black/brown brows so they remain distinct from skin
            // without becoming as heavy as hair, iris, or eyeliner.
            return role_slots[5];
        }
        if (label == Label::EyeSclera && source_chroma < .10f) {
            // The eye mask identifies the sclera material. Baked gray shadows
            // and a warm skin reflection must not become gray/skin filaments.
            if (has_card) return role_slots[2];
            size_t lightest = palette.size(); float lightness = -1.f;
            for (size_t slot = 0; slot < palette.size(); ++slot) {
                const auto& target = palette_labs[slot];
                if (chroma(target) < .035f && target[0] > lightness) {
                    lightness = target[0]; lightest = slot;
                }
            }
            if (lightest != palette.size()) return lightest;
        }
        if ((label == Label::Hair || label == Label::Iris || label == Label::Eyebrow) && natural_dark_hair(center)) {
            // Preserve the material's darkness when the limited palette lacks
            // brown. A pastel skin/lip role is not a brown-hair highlight.
            // Real brown filaments still compete by their actual appearance.
            size_t selected = palette.size(); float best = std::numeric_limits<float>::max();
            for (size_t slot = 0; slot < palette.size(); ++slot) {
                const auto& target = palette_labs[slot];
                if (has_card && slot != role_slots[1] && slot != role_slots[5]) continue;
                if (target[0] > center[0] + .10f || !natural_dark_hair(target)) continue;
                const float score = distance(center, target);
                if (score < best) { best = score; selected = slot; }
            }
            if (selected != palette.size()) return selected;
        }
        if (has_card) {
            const bool neutral = source_chroma < .015f;
            const bool muted_cloth = label == Label::Clothes && (source_chroma < .035f ||
                (source_chroma < .05f && center[1] >= 0.f && center[2] >= 0.f));
            const bool skin = label == Label::FaceSkin || label == Label::BodySkin;
            if (skin && !neutral && center[1] >= 0.f && center[2] > 0.f) return role_slots[0];
            if (label == Label::Lips && source_chroma >= .04f && center[1] > .02f && center[2] > -.03f) return role_slots[3];
            size_t role = 1; float best = std::numeric_limits<float>::max();
            for (size_t candidate = 0; candidate < anchors.size(); ++candidate) {
                if ((neutral || muted_cloth) && candidate != 1 && candidate != 2 && candidate != 5) continue;
                const float score = distance(center, anchors[candidate]);
                if (score < best) { best = score; role = candidate; }
            }
            return role_slots[role];
        }
        if (palette.size() <= 4 && label == Label::Clothes && source_chroma < .035f) {
            size_t neutral = palette.size();
            float best = std::numeric_limits<float>::max();
            for (size_t slot = 0; slot < palette.size(); ++slot) {
                const auto& target = palette_labs[slot];
                if (chroma(target) >= .02f) continue;
                const float score = distance(center, target);
                if (score < best) { best = score; neutral = slot; }
            }
            if (neutral != palette.size()) {
                if (source_chroma >= .02f) {
                    size_t colored = palette.size();
                    float colored_distance = std::numeric_limits<float>::max();
                    for (size_t slot = 0; slot < palette.size(); ++slot) {
                        if (chroma(palette_labs[slot]) < .02f) continue;
                        const float score = distance(center, palette_labs[slot]);
                        if (score < colored_distance) { colored_distance = score; colored = slot; }
                    }
                    if (colored != palette.size() && colored_distance < best * .5f) return colored;
                }
                return neutral;
            }
        }
        if (palette.size() <= 4 && (label == Label::FaceSkin || label == Label::BodySkin) &&
            source_chroma >= .015f && center[1] >= 0.f && center[2] > 0.f) {
            size_t skin_slot = palette.size();
            float best = std::numeric_limits<float>::max();
            for (size_t slot = 0; slot < palette.size(); ++slot) {
                const auto& target = palette_labs[slot];
                const float target_chroma = chroma(target);
                if (target[0] < .65f || target_chroma < .015f || target_chroma > .065f ||
                    target[1] < 0.f || target[2] <= 0.f) continue;
                const float score = distance(center, target);
                if (score < best) { best = score; skin_slot = slot; }
            }
            if (skin_slot != palette.size()) return skin_slot;
        }
        const bool has_neutral = std::any_of(palette_labs.begin(), palette_labs.end(),
            [](const Color& c) { return chroma(c) < .035f; });
        size_t selected = 0; float best = std::numeric_limits<float>::max();
        for (size_t slot = 0; slot < palette.size(); ++slot) {
            const auto& target = palette_labs[slot];
            if (source_chroma < .015f && has_neutral && chroma(target) >= .035f) continue;
            const float excess = std::max(0.f, chroma(target) - source_chroma - .025f);
            const float score = distance(center, target) + 2.f * excess * excess;
            if (score < best) { best = score; selected = slot; }
        }
        return selected;
    };
    const auto choose_target = [&](const Color& center, Label label) {
        const size_t selected = choose_unprotected_target(center, label);
        if (palette.size() > 4 || selected >= palette.size()) return selected;

        // In a limited palette, facial materials and non-red garments must not
        // consume a lip/red slot. Explicitly red source materials remain
        // eligible so genuine red hair, clothing and accessories are intact.
        const bool source_is_red = red_accent(center);
        const bool protected_material = label == Label::FaceSkin || label == Label::BodySkin ||
            label == Label::EyeSclera || label == Label::Iris || label == Label::Eyebrow ||
            ((label == Label::Clothes || label == Label::Hair || label == Label::Accessories) && !source_is_red);
        if (!protected_material || !red_accent(palette_labs[selected])) return selected;

        size_t legal = palette.size();
        float best = std::numeric_limits<float>::max();
        for (size_t slot = 0; slot < palette.size(); ++slot) {
            if (red_accent(palette_labs[slot])) continue;
            const float score = distance(center, palette_labs[slot]);
            if (score < best) { best = score; legal = slot; }
        }
        // No legal target means the existing whole-face color is safer than
        // introducing a forced semantic color. Callers skip this assignment.
        return legal;
    };
    // Eyebrows are a detail overlay on the face. Treat them as face skin only
    // while building the underlying material topology, so removing a thin brow
    // strip cannot split the forehead into different skin prototypes.
    const auto topology_label = [](Label label) {
        return label == Label::Eyebrow ? Label::FaceSkin : label;
    };
    // Connected semantic regions share prototypes, but never share centers with
    // a different material label. Reuse of one physical filament is permitted.
    std::vector<size_t> parent(count); std::iota(parent.begin(), parent.end(), 0);
    const auto root = [&parent](size_t id) {
        while (parent[id] != id) { parent[id] = parent[parent[id]]; id = parent[id]; } return id;
    };
    std::vector<size_t> first(source.mesh.vertices.size(), count);
    for (size_t label = 2; label < label_count; ++label) {
        std::fill(first.begin(), first.end(), count);
        for (size_t id = 0; id < count; ++id) {
            if (size_t(topology_label(analysis.face_labels[id])) != label || analysis.face_confidence[id] < minimum_confidence) continue;
            for (int corner = 0; corner < 3; ++corner) {
                const int vertex = source.mesh.indices[id][corner];
                if (first[vertex] == count) first[vertex] = id;
                else { const size_t a = root(first[vertex]), b = root(id); if (a != b) parent[b] = a; }
            }
        }
    }
    std::map<size_t, std::vector<size_t>> regions;
    for (size_t id = 0; id < count; ++id)
        if (paintable(analysis.face_labels[id]) && analysis.face_confidence[id] >= minimum_confidence) regions[root(id)].push_back(id);
    FaceColors output; output.reserve(analysis.reliable_faces);
    std::vector<HairSeed> hair_seeds(count);
    for (const auto& region : regions) {
        const Label label = topology_label(analysis.face_labels[region.second.front()]);
        const bool skin = label == Label::FaceSkin || label == Label::BodySkin;
        const bool eye = label == Label::EyeSclera || label == Label::Iris;
        const bool eyebrow = label == Label::Eyebrow;
        std::vector<Color> appearances; appearances.reserve(region.second.size());
        std::vector<Sample> samples; samples.reserve(region.second.size());
        for (size_t id : region.second) {
            const Color appearance = face_lab(source, id);
            appearances.push_back(appearance);
            // The brow is only a topological bridge in this pass. Its dark
            // pigment belongs to the detail overlay below, not the skin center.
            if (analysis.face_labels[id] != Label::Eyebrow)
                samples.push_back({appearance, area(source, id)});
        }
        const auto centers = prototypes(samples, skin || eye || eyebrow ? 1 : std::min(size_t(3), palette.size()));
        if (centers.empty()) continue;
        size_t sclera_seeds = 0;
        bool bright_sclera = false;
        if (label == Label::EyeSclera) for (const auto& sample : samples) {
            const float source_chroma = chroma(sample.color);
            if (sample.color[0] >= .55f && source_chroma <= .055f) ++sclera_seeds;
            bright_sclera |= sample.color[0] >= .68f && source_chroma <= .035f;
        }
        const bool supported_sclera = label != Label::EyeSclera || sclera_seeds >= 2 || bright_sclera;
        std::vector<size_t> targets;
        for (const auto& center : centers) targets.push_back(choose_target(center, label));
        for (size_t i = 0; i < region.second.size(); ++i) {
            if (analysis.face_labels[region.second[i]] == Label::Eyebrow) continue;
            const auto& original = appearances[i];
            const bool supported_shadow = bright_sclera && original[0] >= .55f &&
                original[0] >= centers.front()[0] - .235f && chroma(original) <= .055f;
            if (label == Label::EyeSclera && (!supported_sclera ||
                (!compatible_sclera(original, centers.front()) && !supported_shadow)))
                continue;
            size_t nearest = 0; float best = std::numeric_limits<float>::max();
            for (size_t center = 0; center < centers.size(); ++center) {
                const float d = distance(original, centers[center]);
                if (d < best) { best = d; nearest = center; }
            }
            if (label == Label::Hair && same_hair_material(original, centers.front()) && targets.front() < palette.size()) {
                output.emplace_back(region.second[i], palette[targets.front()]);
                if (natural_dark_hair(palette_labs[targets.front()]))
                    hair_seeds[region.second[i]] = {centers.front(), int(targets.front())};
                continue;
            }
            // Hair uses area-supported material centers: isolated baked white
            // highlights must not create extra filament colors. Teeth and real
            // cloth stripes retain the finer neutral protection below.
            // Coherent low-chroma cloth keeps its local material across this
            // cutoff instead of alternating black/gray on nearly equal faces.
            if (label != Label::Hair && !eye && chroma(original) < .015f &&
                !(label == Label::Clothes && same_muted_cloth(original, centers[nearest]))) {
                const size_t target = choose_target(original, label);
                if (target < palette.size()) output.emplace_back(region.second[i], palette[target]);
                continue;
            }
            if (skin && !compatible_skin(original, centers.front(), label)) continue;
            if (targets[nearest] < palette.size())
                output.emplace_back(region.second[i], palette[targets[nearest]]);
        }
    }
    // Map each connected brow from its own stable material center after the
    // underlying skin pass. This preserves one supported brow material without
    // allowing it to alter the face-skin prototype or borrow hair across the
    // forehead.
    std::vector<size_t> eyebrow_parent(count); std::iota(eyebrow_parent.begin(), eyebrow_parent.end(), 0);
    const auto eyebrow_root = [&eyebrow_parent](size_t id) {
        while (eyebrow_parent[id] != id) {
            eyebrow_parent[id] = eyebrow_parent[eyebrow_parent[id]];
            id = eyebrow_parent[id];
        }
        return id;
    };
    std::fill(first.begin(), first.end(), count);
    for (size_t id = 0; id < count; ++id) {
        if (analysis.face_labels[id] != Label::Eyebrow || analysis.face_confidence[id] < minimum_confidence) continue;
        for (int corner = 0; corner < 3; ++corner) {
            const int vertex = source.mesh.indices[id][corner];
            if (first[vertex] == count) first[vertex] = id;
            else {
                const size_t a = eyebrow_root(first[vertex]), b = eyebrow_root(id);
                if (a != b) eyebrow_parent[b] = a;
            }
        }
    }
    std::map<size_t, std::vector<size_t>> eyebrow_regions;
    for (size_t id = 0; id < count; ++id)
        if (analysis.face_labels[id] == Label::Eyebrow && analysis.face_confidence[id] >= minimum_confidence)
            eyebrow_regions[eyebrow_root(id)].push_back(id);
    for (const auto& region : eyebrow_regions) {
        std::vector<Sample> samples; samples.reserve(region.second.size());
        for (size_t id : region.second) samples.push_back({face_lab(source, id), area(source, id)});
        const auto centers = prototypes(samples, 1);
        if (centers.empty()) continue;
        const size_t target = choose_target(centers.front(), Label::Eyebrow);
        if (target < palette.size())
            for (size_t id : region.second) output.emplace_back(id, palette[target]);
    }
    extend_hair_edges(source, analysis, hair_seeds, palette, output);
    refine_material_patches(source, analysis, palette, portrait_card, output);
    std::sort(output.begin(), output.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return output;
}

FaceColors remap_palette_targets(const FaceColors& suggestions, const std::vector<Color>& original_candidates,
                                const std::vector<Color>& target_candidates)
{
    if (original_candidates.empty() || original_candidates.size() > 6 || target_candidates.size() != original_candidates.size()) return {};
    for (size_t slot = 0; slot < original_candidates.size(); ++slot) {
        if (!valid_color(original_candidates[slot]) || !valid_color(target_candidates[slot])) return {};
        for (size_t previous = 0; previous < slot; ++previous)
            if (original_candidates[previous] == original_candidates[slot] && target_candidates[previous] != target_candidates[slot]) return {};
    }
    FaceColors result; result.reserve(suggestions.size());
    for (const auto& suggestion : suggestions) {
        const auto found = std::find(original_candidates.begin(), original_candidates.end(), suggestion.second);
        if (found == original_candidates.end()) return {};
        result.emplace_back(suggestion.first, target_candidates[size_t(found - original_candidates.begin())]);
    }
    return result;
}

FaceColors compose(const FaceColors& automatic, const FaceColors& manual, bool automatic_enabled)
{
    std::map<size_t, Color> colors;
    if (automatic_enabled) for (const auto& item : automatic) colors[item.first] = item.second;
    for (const auto& item : manual) colors[item.first] = item.second;
    return {colors.begin(), colors.end()};
}

bool enforce_subface_budget(const SubfaceColors& candidates, size_t original_face_count,
                            const SubfaceBudget& budget, SubfaceBudgetResult& output, std::string& error)
{
    output = {};
    error.clear();
    const auto fail = [&](const char* message) {
        output = {};
        error = message;
        return false;
    };
    if (original_face_count == 0 || original_face_count > maximum_faces ||
        !std::isfinite(budget.maximum_added_ratio) || budget.maximum_added_ratio < 0.f ||
        budget.maximum_added_ratio > 1.f || !std::isfinite(budget.minimum_confidence) ||
        budget.minimum_confidence < 0.f || budget.minimum_confidence > 1.f)
        return fail("Invalid semantic subface budget.");

    std::map<std::pair<size_t, SubfacePath>, SubfaceColor> unique;
    for (const SubfaceColor& candidate : candidates) {
        if (candidate.face_id >= original_face_count || candidate.path.depth == 0 || candidate.path.depth > 2 ||
            !valid_color(candidate.color) ||
            !std::isfinite(candidate.confidence) || candidate.confidence < 0.f || candidate.confidence > 1.f)
            return fail("Invalid semantic subface candidate.");
        const unsigned path_limit = 1u << (2u * candidate.path.depth);
        if (unsigned(candidate.path.value) >= path_limit)
            return fail("Invalid semantic subface candidate.");
        const auto key = std::make_pair(candidate.face_id, candidate.path);
        const auto found = unique.find(key);
        if (found == unique.end() || candidate.confidence > found->second.confidence)
            unique[key] = candidate;
    }

    SubfaceColors ordered;
    ordered.reserve(unique.size());
    for (const auto& item : unique) ordered.push_back(item.second);
    std::sort(ordered.begin(), ordered.end(), [](const SubfaceColor& lhs, const SubfaceColor& rhs) {
        if (lhs.confidence != rhs.confidence) return lhs.confidence > rhs.confidence;
        if (lhs.face_id != rhs.face_id) return lhs.face_id < rhs.face_id;
        return lhs.path < rhs.path;
    });

    const double ratio_product = double(original_face_count) * double(budget.maximum_added_ratio);
    const double ratio_epsilon = std::max(1.0, ratio_product) *
        double(std::numeric_limits<float>::epsilon()) * 2.0;
    const size_t ratio_limit = size_t(std::floor(ratio_product + ratio_epsilon));
    const size_t triangle_limit = std::min(budget.maximum_added_triangles, ratio_limit);
    // Node zero is the original face's root split. Nodes one through four are
    // its first-level children, each of which may be split for a depth-2 path.
    std::set<std::pair<size_t, uint8_t>> split_nodes;
    for (const SubfaceColor& candidate : ordered) {
        if (candidate.confidence < budget.minimum_confidence) {
            ++output.rejected_candidates;
            continue;
        }
        std::array<std::pair<size_t, uint8_t>, 2> required {{
            {candidate.face_id, 0},
            {candidate.face_id, uint8_t(1 + (candidate.path.value >> 2))}
        }};
        const size_t required_count = candidate.path.depth == 1 ? 1 : 2;
        size_t additional_nodes = 0;
        for (size_t i = 0; i < required_count; ++i)
            additional_nodes += split_nodes.count(required[i]) == 0;
        if ((split_nodes.size() + additional_nodes) * 3 > triangle_limit) {
            ++output.rejected_candidates;
            continue;
        }
        for (size_t i = 0; i < required_count; ++i) split_nodes.insert(required[i]);
        output.accepted.push_back(candidate);
    }
    output.added_triangles = split_nodes.size() * 3;
    std::sort(output.accepted.begin(), output.accepted.end(), [](const SubfaceColor& lhs, const SubfaceColor& rhs) {
        if (lhs.face_id != rhs.face_id) return lhs.face_id < rhs.face_id;
        return lhs.path < rhs.path;
    });
    return true;
}

bool map_subface_palette(const MeshSnapshot& source, const Analysis& analysis, const FaceColors& whole_face,
                         const std::vector<Color>& palette, const std::vector<Color>& portrait_card,
                         const SubfaceBudget& budget, SubfaceBudgetResult& output, std::string& error)
{
    output = {};
    error.clear();
    const size_t count = source.mesh.indices.size();
    if (!analysis.person_detected || analysis.canceled || !analysis.error.empty() ||
        analysis.face_roi_size != 512 ||
        analysis.geometry_id != source.geometry_id || analysis.content_id != source.content_id ||
        analysis.signature != analysis_cache_key(source, analysis.body_identity, analysis.face_identity) ||
        analysis.face_labels.size() != count || analysis.face_confidence.size() != count ||
        palette.empty() || palette.size() > 6 || !validate_snapshot(source).empty()) {
        error = "Semantic subface mapping requires current model recognition evidence.";
        return false;
    }
    for (const Color& color : palette) if (!valid_color(color)) {
        error = "Semantic subface mapping requires normalized palette colors.";
        return false;
    }

    std::array<size_t, 6> role_slots {};
    role_slots.fill(palette.size());
    bool has_card = portrait_card.size() == 6;
    if (has_card) for (size_t role = 0; role < role_slots.size(); ++role) {
        for (size_t slot = 0; slot < palette.size(); ++slot) {
            bool same = true;
            for (size_t channel = 0; channel < 3; ++channel)
                same = same && std::abs(palette[slot][channel] - portrait_card[role][channel]) <= .5f / 255.f;
            if (same) { role_slots[role] = slot; break; }
        }
        if (role_slots[role] == palette.size()) has_card = false;
    }
    std::vector<Color> palette_labs;
    palette_labs.reserve(palette.size());
    for (const Color& color : palette) palette_labs.push_back(lab(color));
    const auto safe_subface_target = [&](Label label, const Color& source_color, size_t proposed) {
        if (palette.size() > 4 || proposed >= palette.size() ||
            (label != Label::FaceSkin && label != Label::EyeSclera &&
             label != Label::Iris && label != Label::Eyebrow) ||
            !red_accent(palette_labs[proposed])) return proposed;
        size_t legal = palette.size();
        float best = std::numeric_limits<float>::max();
        for (size_t slot = 0; slot < palette.size(); ++slot) {
            if (red_accent(palette_labs[slot])) continue;
            const float score = distance(source_color, palette_labs[slot]);
            if (score < best) { best = score; legal = slot; }
        }
        return legal;
    };

    const auto appearance = [&source](const SubfaceLabelEvidence& evidence, Color& output_color) {
        std::array<Barycentric, 3> vertices;
        if (evidence.face_id >= source.mesh.indices.size() || !subface_vertices(evidence.path, vertices)) return false;
        Barycentric centroid {};
        for (const Barycentric& vertex : vertices)
            for (size_t channel = 0; channel < 3; ++channel)
                centroid[channel] += vertex[channel] / 3.f;
        Color rgb {};
        for (size_t corner = 0; corner < 3; ++corner) {
            const Color color = corner_color(source, evidence.face_id, int(corner));
            for (size_t channel = 0; channel < 3; ++channel)
                rgb[channel] += centroid[corner] * color[channel];
        }
        output_color = lab(rgb);
        return true;
    };

    std::vector<std::vector<size_t>> faces_at_vertex(source.mesh.vertices.size());
    for (size_t face_id = 0; face_id < count; ++face_id)
        for (int corner = 0; corner < 3; ++corner)
            faces_at_vertex[source.mesh.indices[face_id][corner]].push_back(face_id);

    // A geometric eye mask can extend onto pale skin in an oblique view. Build
    // one robust material center from reliable whole-face sclera, then allow
    // subface recovery only on that supported surface or its direct boundary.
    std::vector<Sample> sclera_samples;
    std::vector<uint8_t> sclera_anchor_candidate(count, 0);
    std::vector<uint8_t> adjacent_sclera_support(count, 0);
    for (size_t face_id = 0; face_id < count; ++face_id) {
        if (analysis.face_labels[face_id] != Label::EyeSclera ||
            analysis.face_confidence[face_id] < minimum_confidence) continue;
        const Color source_color = face_lab(source, face_id);
        const float source_chroma = chroma(source_color);
        if (source_color[0] < .40f || source_chroma > .055f) continue;
        sclera_anchor_candidate[face_id] = 1;
        if (source_color[0] >= .68f && source_chroma <= .035f)
            sclera_samples.push_back({source_color, area(source, face_id)});
    }
    const auto sclera_centers = prototypes(sclera_samples, 1);
    const bool have_sclera_center = sclera_samples.size() >= 2 && !sclera_centers.empty();
    if (have_sclera_center) {
        for (size_t face_id = 0; face_id < count; ++face_id)
            if (sclera_anchor_candidate[face_id] &&
                compatible_sclera(face_lab(source, face_id), sclera_centers.front()))
                adjacent_sclera_support[face_id] = 1;
        const auto anchors = adjacent_sclera_support;
        for (size_t face_id = 0; face_id < count; ++face_id) {
            if (anchors[face_id]) continue;
            for (int corner = 0; corner < 3 && !adjacent_sclera_support[face_id]; ++corner)
                for (size_t neighbor : faces_at_vertex[source.mesh.indices[face_id][corner]])
                    if (anchors[neighbor]) { adjacent_sclera_support[face_id] = 1; break; }
        }
    } else {
        std::fill(adjacent_sclera_support.begin(), adjacent_sclera_support.end(), 0);
    }

    std::vector<uint8_t> compatible_sclera_leaf(analysis.subface_labels.size(), 0);
    for (size_t index = 0; index < analysis.subface_labels.size(); ++index) {
        const auto& evidence = analysis.subface_labels[index];
        if (evidence.label != Label::EyeSclera || evidence.confidence < minimum_confidence ||
            evidence.face_id >= count ||
            !adjacent_sclera_support[evidence.face_id]) continue;
        Color source_color;
        if (!appearance(evidence, source_color) || !compatible_sclera(source_color, sclera_centers.front())) continue;
        const float source_chroma = chroma(source_color);
        // A single direct raster sample is useful on a dense mesh, but a dark,
        // warm singleton at the eye contour is more likely adjacent skin. More
        // samples retain the wider reflected-eye-white tolerance.
        if (evidence.samples == 1 &&
            (source_chroma > .05f || (source_chroma > .03f && source_color[0] < .60f))) continue;
            compatible_sclera_leaf[index] = 1;
    }
    std::map<size_t, Color> roots;
    for (const auto& item : whole_face) roots[item.first] = item.second;
    SubfaceColors candidates;
    candidates.reserve(analysis.subface_labels.size() + roots.size() * 4);
    struct SkinBoundarySeed {
        size_t face_id;
        SubfacePath path;
        Color appearance;
        Color target;
        float confidence;
    };
    struct ScleraBoundarySeed {
        size_t face_id;
        SubfacePath path;
        Color appearance;
        Color target;
        float confidence;
    };
    std::vector<SkinBoundarySeed> skin_boundary_seeds;
    std::vector<ScleraBoundarySeed> sclera_boundary_seeds;
    std::set<std::pair<size_t, SubfacePath>> protected_detail_leaves;
    std::set<std::pair<size_t, SubfacePath>> non_sclera_detail_leaves;
    std::set<std::pair<size_t, SubfacePath>> iris_detail_leaves;
    std::set<size_t> iris_detail_faces;
    for (const SubfaceLabelEvidence& evidence : analysis.subface_labels)
        if (evidence.confidence >= minimum_confidence) {
            if (evidence.label != Label::FaceSkin)
                protected_detail_leaves.emplace(evidence.face_id, evidence.path);
            if (evidence.label != Label::EyeSclera)
                non_sclera_detail_leaves.emplace(evidence.face_id, evidence.path);
            if (evidence.label == Label::Iris) {
                iris_detail_leaves.emplace(evidence.face_id, evidence.path);
                iris_detail_faces.insert(evidence.face_id);
            }
        }
    for (size_t evidence_index = 0; evidence_index < analysis.subface_labels.size(); ++evidence_index) {
        const SubfaceLabelEvidence& evidence = analysis.subface_labels[evidence_index];
        Color source_color;
        if (!appearance(evidence, source_color)) continue;
        size_t target = palette.size();
        if (evidence.label == Label::EyeSclera) {
            if (!compatible_sclera_leaf[evidence_index]) continue;
            if (has_card) target = role_slots[2];
            else {
                float lightness = -1.f;
                for (size_t slot = 0; slot < palette.size(); ++slot)
                    if (chroma(palette_labs[slot]) <= .055f && palette_labs[slot][0] > lightness) {
                        lightness = palette_labs[slot][0]; target = slot;
                    }
            }
        } else if (evidence.label == Label::FaceSkin) {
            const auto root = roots.find(evidence.face_id);
            const Label root_label = analysis.face_labels[evidence.face_id];
            if ((root_label != Label::EyeSclera && root_label != Label::Hair) ||
                analysis.face_confidence[evidence.face_id] < minimum_confidence) continue;
            std::vector<Sample> local_skin;
            std::set<size_t> visited;
            for (int corner = 0; corner < 3; ++corner) {
                for (size_t neighbor : faces_at_vertex[source.mesh.indices[evidence.face_id][corner]]) {
                    if (!visited.insert(neighbor).second ||
                        (analysis.face_labels[neighbor] != Label::FaceSkin &&
                         analysis.face_labels[neighbor] != Label::BodySkin) ||
                        analysis.face_confidence[neighbor] < minimum_confidence) continue;
                    local_skin.push_back({face_lab(source, neighbor), area(source, neighbor)});
                }
            }
            const auto centers = prototypes(local_skin, 1);
            if (centers.empty() || !compatible_skin(source_color, centers.front(), Label::FaceSkin)) continue;
            // FaceSkin detail is emitted by the face adapter for the ear window.
            // On a Hair root it may recover only the locally skin-compatible
            // leaves touching reliable skin; detached brown hair cannot prove
            // itself to be an ear from its own color.
            if (root_label == Label::Hair && root == roots.end()) continue;
            if (has_card) {
                const float source_chroma = chroma(source_color);
                if (source_chroma < .015f || source_color[1] < 0.f || source_color[2] <= 0.f) continue;
                target = role_slots[0];
            } else {
                float best = std::numeric_limits<float>::max();
                for (size_t slot = 0; slot < palette.size(); ++slot) {
                    if (root != roots.end() && palette[slot] == root->second) continue;
                    const float score = distance(centers.front(), palette_labs[slot]);
                    if (score < best) { best = score; target = slot; }
                }
            }
        } else if (evidence.label == Label::Iris || evidence.label == Label::Eyebrow) {
            if (evidence.label == Label::Eyebrow && !natural_dark_hair(source_color)) continue;
            if (has_card && evidence.label == Label::Iris) {
                // A portrait card has no dedicated brown-eye slot. Preserve a
                // clearly blue/green iris with the cool role; all other irises
                // use the dark role. Skin and lip roles are never iris colors.
                target = source_color[2] < -.015f || source_color[1] < -.04f ? role_slots[4] : role_slots[1];
            } else if (has_card && evidence.label == Label::Eyebrow) target = role_slots[5];
            else {
                float best = std::numeric_limits<float>::max();
                for (size_t slot = 0; slot < palette.size(); ++slot) {
                    const float score = distance(source_color, palette_labs[slot]);
                    if (score < best) { best = score; target = slot; }
                }
            }
        }
        target = safe_subface_target(evidence.label, source_color, target);
        if (target >= palette.size()) continue;
        const auto root = roots.find(evidence.face_id);
        if (root != roots.end() && root->second == palette[target]) continue;
        candidates.push_back({evidence.face_id, evidence.path, palette[target], evidence.confidence});
        if (evidence.label == Label::EyeSclera)
            sclera_boundary_seeds.push_back({evidence.face_id, evidence.path, source_color,
                                             palette[target], evidence.confidence});
        if (evidence.label == Label::FaceSkin &&
            analysis.face_labels[evidence.face_id] == Label::Hair)
            skin_boundary_seeds.push_back({evidence.face_id, evidence.path, source_color,
                                           palette[target], evidence.confidence});
    }

    // Close at most two raster-sized gaps inside a coarse eye triangle. Each
    // synchronous ring must share a full sub-edge with the preceding reliable
    // or inferred sclera leaf, remain locally color-continuous, and avoid any
    // explicit skin, iris or brow evidence. The bounded second ring reaches a
    // narrow core gap without allowing a seed to flood its whole root face.
    // The later .85 skin recovery still wins over these .80 inferred leaves on
    // a warm eyelid face.
    std::set<std::pair<size_t, SubfacePath>> accepted_sclera_leaves;
    std::map<std::pair<size_t, SubfacePath>, ScleraBoundarySeed> accepted_sclera_evidence;
    for (const ScleraBoundarySeed& seed : sclera_boundary_seeds)
        accepted_sclera_evidence.emplace(
            std::make_pair(seed.face_id, seed.path), seed);
    for (const auto& item : accepted_sclera_evidence)
        accepted_sclera_leaves.insert(item.first);
    std::vector<ScleraBoundarySeed> sclera_frontier = sclera_boundary_seeds;
    for (uint8_t ring = 0; ring < 2 && !sclera_frontier.empty(); ++ring) {
        std::vector<ScleraBoundarySeed> next_frontier;
        for (const ScleraBoundarySeed& seed : sclera_frontier) {
            for (uint8_t value = 0; value < 16; ++value) {
                const SubfacePath path {2, value};
                const auto key = std::make_pair(seed.face_id, path);
                if (!subfaces_share_edge(seed.path, path) ||
                    accepted_sclera_leaves.count(key) != 0 ||
                    non_sclera_detail_leaves.count(key) != 0) continue;
                const SubfaceLabelEvidence neighbor {seed.face_id, path, Label::EyeSclera,
                                                      seed.confidence, 0};
                Color neighbor_color;
                if (!appearance(neighbor, neighbor_color) ||
                    !compatible_sclera(neighbor_color, sclera_centers.front()) ||
                    distance(neighbor_color, seed.appearance) > .0064f) continue;
                accepted_sclera_leaves.insert(key);
                const float confidence = std::min(seed.confidence, .80f);
                candidates.push_back({seed.face_id, path, seed.target, confidence});
                const ScleraBoundarySeed inferred {seed.face_id, path, neighbor_color, seed.target, confidence};
                accepted_sclera_evidence.emplace(key, inferred);
                next_frontier.push_back(inferred);
            }
        }
        sclera_frontier.swap(next_frontier);
    }

    // Close one eye-white gap across an original-face edge. Only depth-2 leaf
    // edges that coincide exactly in model space are paired; point contacts,
    // projected neighbors and nonmanifold boundaries are excluded. Proposals
    // are synchronous, so a newly inferred leaf cannot continue into another
    // root face during this pass.
    struct BoundaryLeaf { size_t face_id; SubfacePath path; };
    // Depth two divides every original edge into four exact topological
    // segments. Key them by original vertex ids and segment ordinal instead of
    // interpolated positions; adjacent faces may evaluate the same point in a
    // different floating-point order.
    using BoundaryKey = std::array<size_t, 4>;
    std::set<size_t> boundary_faces;
    std::set<std::pair<size_t, size_t>> boundary_face_pairs;
    for (const auto& item : accepted_sclera_evidence) {
        const size_t face_id = item.first.first;
        boundary_faces.insert(face_id);
        std::map<size_t, uint8_t> shared_vertices;
        for (int corner = 0; corner < 3; ++corner)
            for (size_t neighbor : faces_at_vertex[source.mesh.indices[face_id][corner]])
                if (neighbor != face_id) ++shared_vertices[neighbor];
        for (const auto& neighbor : shared_vertices) if (neighbor.second >= 2) {
            boundary_faces.insert(neighbor.first);
            boundary_face_pairs.emplace(std::min(face_id, neighbor.first),
                                        std::max(face_id, neighbor.first));
        }
    }
    std::map<BoundaryKey, std::vector<BoundaryLeaf>> boundary_leaves;
    for (size_t face_id : boundary_faces) for (uint8_t value = 0; value < 16; ++value) {
        const SubfacePath path {2, value};
        std::array<Barycentric, 3> leaf_vertices;
        if (!subface_vertices(path, leaf_vertices)) continue;
        for (int edge = 0; edge < 3; ++edge) {
            const int next = (edge + 1) % 3;
            int zero_axis = -1;
            for (int axis = 0; axis < 3; ++axis)
                if (leaf_vertices[edge][axis] == 0.f && leaf_vertices[next][axis] == 0.f) {
                    zero_axis = axis;
                    break;
                }
            if (zero_axis < 0) continue;
            const int first_axis = (zero_axis + 1) % 3;
            const int second_axis = (zero_axis + 2) % 3;
            const size_t first_vertex = size_t(source.mesh.indices[face_id][first_axis]);
            const size_t second_vertex = size_t(source.mesh.indices[face_id][second_axis]);
            const int parameter_axis = first_vertex < second_vertex ? second_axis : first_axis;
            const size_t first_quarter = size_t(std::lround(leaf_vertices[edge][parameter_axis] * 4.f));
            const size_t second_quarter = size_t(std::lround(leaf_vertices[next][parameter_axis] * 4.f));
            boundary_leaves[{std::min(first_vertex, second_vertex),
                             std::max(first_vertex, second_vertex),
                             std::min(first_quarter, second_quarter),
                             std::max(first_quarter, second_quarter)}].push_back({face_id, path});
        }
    }
    std::map<std::pair<size_t, SubfacePath>, ScleraBoundarySeed> cross_face_proposals;
    const auto propose_cross_face = [&](const BoundaryLeaf& from, const BoundaryLeaf& to) {
        const auto seed = accepted_sclera_evidence.find({from.face_id, from.path});
        const auto key = std::make_pair(to.face_id, to.path);
        if (seed == accepted_sclera_evidence.end() || accepted_sclera_leaves.count(key) != 0 ||
            non_sclera_detail_leaves.count(key) != 0 || to.face_id >= count) return;
        const auto root = roots.find(to.face_id);
        if (root != roots.end() && root->second == seed->second.target) return;
        const Label root_label = analysis.face_labels[to.face_id];
        if (root_label != Label::EyeSclera && root_label != Label::FaceSkin &&
            root_label != Label::Unknown) return;
        const SubfaceLabelEvidence neighbor {to.face_id, to.path, Label::EyeSclera,
                                              seed->second.confidence, 0};
        Color neighbor_color;
        if (!appearance(neighbor, neighbor_color) ||
            !compatible_sclera(neighbor_color, sclera_centers.front()) ||
            distance(neighbor_color, seed->second.appearance) > .0064f) return;
        const float confidence = std::min(seed->second.confidence, .78f);
        cross_face_proposals.emplace(key, ScleraBoundarySeed {
            to.face_id, to.path, neighbor_color, seed->second.target, confidence});
    };
    for (const auto& boundary : boundary_leaves) {
        if (boundary.second.size() != 2 ||
            boundary.second[0].face_id == boundary.second[1].face_id) continue;
        const auto face_pair = std::minmax(boundary.second[0].face_id,
                                           boundary.second[1].face_id);
        if (boundary_face_pairs.count(face_pair) == 0) continue;
        propose_cross_face(boundary.second[0], boundary.second[1]);
        propose_cross_face(boundary.second[1], boundary.second[0]);
    }
    std::map<size_t, size_t> landing_face_seed_count;
    for (const auto& proposal : cross_face_proposals)
        ++landing_face_seed_count[proposal.second.face_id];
    std::set<std::pair<size_t, SubfacePath>> final_sclera_support = accepted_sclera_leaves;
    std::map<size_t, size_t> final_sclera_support_count;
    for (const auto& item : final_sclera_support)
        ++final_sclera_support_count[item.first];
    for (const auto& proposal : cross_face_proposals) {
        candidates.push_back({proposal.second.face_id, proposal.second.path,
                              proposal.second.target, proposal.second.confidence});
        if (final_sclera_support.emplace(proposal.first).second)
            ++final_sclera_support_count[proposal.second.face_id];
        // The raster may miss the leaf immediately behind the shared edge on
        // the landing face. Close exactly one local ring only when at least two
        // independent edge segments support that face and it has no explicit
        // iris detail. The tighter local color threshold admits a continuous
        // eye-white patch without spreading into a similarly pale iris rim.
        // These leaves never enter the cross-face frontier, so they cannot
        // reach a second original face.
        if (landing_face_seed_count[proposal.second.face_id] < 2 ||
            iris_detail_faces.count(proposal.second.face_id) != 0) continue;
        for (uint8_t value = 0; value < 16; ++value) {
            const SubfacePath path {2, value};
            const auto key = std::make_pair(proposal.second.face_id, path);
            if (!subfaces_share_edge(proposal.second.path, path) ||
                accepted_sclera_leaves.count(key) != 0 ||
                non_sclera_detail_leaves.count(key) != 0 ||
                cross_face_proposals.count(key) != 0) continue;
            const SubfaceLabelEvidence neighbor {proposal.second.face_id, path, Label::EyeSclera,
                                                  proposal.second.confidence, 0};
            Color neighbor_color;
            if (!appearance(neighbor, neighbor_color) ||
                !compatible_sclera(neighbor_color, sclera_centers.front()) ||
                distance(neighbor_color, proposal.second.appearance) > .0001f) continue;
            candidates.push_back({proposal.second.face_id, path, proposal.second.target,
                                  std::min(proposal.second.confidence, .76f)});
            if (final_sclera_support.emplace(key).second)
                ++final_sclera_support_count[proposal.second.face_id];
        }
    }

    // Reconsider only direct sclera observations on a face whose final local
    // component is already established. Multiple raster samples may prove a
    // neutral eye-white shadow that falls below the general material floor;
    // a warm dark singleton remains blocked at the eye/skin boundary.
    for (const SubfaceLabelEvidence& evidence : analysis.subface_labels) {
        if (evidence.label != Label::EyeSclera || evidence.confidence < minimum_confidence ||
            evidence.face_id >= count || final_sclera_support_count[evidence.face_id] < 3) continue;
        const auto key = std::make_pair(evidence.face_id, evidence.path);
        if (final_sclera_support.count(key) != 0 ||
            non_sclera_detail_leaves.count(key) != 0) continue;
        const Label root_label = analysis.face_labels[evidence.face_id];
        if (root_label != Label::EyeSclera && root_label != Label::FaceSkin &&
            root_label != Label::Unknown) continue;
        Color source_color;
        if (!appearance(evidence, source_color)) continue;
        const float source_chroma = chroma(source_color);
        size_t shared_sclera_support = 0;
        for (const auto& support : final_sclera_support)
            if (support.first == evidence.face_id &&
                subfaces_share_edge(support.second, evidence.path))
                ++shared_sclera_support;
        bool adjacent_iris = false;
        for (const auto& detail : iris_detail_leaves)
            if (detail.first == evidence.face_id &&
                subfaces_share_edge(detail.second, evidence.path)) {
                adjacent_iris = true;
                break;
            }
        const bool supported_singleton_gap = evidence.samples == 1 &&
            root_label == Label::Unknown && iris_detail_faces.count(evidence.face_id) == 0 &&
            shared_sclera_support > 0 && source_color[0] >= .40f &&
            source_color[0] >= sclera_centers.front()[0] - .19f && source_chroma <= .035f &&
            !(source_chroma > .03f && source_color[0] < .60f);
        const bool supported_deep_iris_edge = evidence.samples >= 2 &&
            root_label == Label::FaceSkin && adjacent_iris && source_color[0] >= .40f &&
            source_color[0] >= sclera_centers.front()[0] - .27f && source_chroma <= .01f;
        if (!supported_singleton_gap && !supported_deep_iris_edge) continue;
        size_t target = palette.size();
        if (has_card) target = role_slots[2];
        else {
            float lightness = -1.f;
            for (size_t slot = 0; slot < palette.size(); ++slot)
                if (chroma(palette_labs[slot]) <= .055f && palette_labs[slot][0] > lightness) {
                    lightness = palette_labs[slot][0]; target = slot;
                }
        }
        target = safe_subface_target(Label::EyeSclera, source_color, target);
        if (target >= palette.size()) continue;
        const auto root = roots.find(evidence.face_id);
        if (root != roots.end() && root->second == palette[target]) continue;
        candidates.push_back({evidence.face_id, evidence.path, palette[target],
                              std::min(evidence.confidence, .82f)});
        if (final_sclera_support.emplace(key).second)
            ++final_sclera_support_count[evidence.face_id];
    }

    // A 512px face crop can miss one depth-2 leaf along a narrow ear rim even
    // when the adjacent leaf is a reliable ear-skin observation. Fill only one
    // shared-edge ring inside the same Hair root. The neighboring leaf must
    // retain the same intrinsic skin appearance, and explicit eye/brow detail
    // evidence always blocks the fill.
    for (const SkinBoundarySeed& seed : skin_boundary_seeds) {
        for (uint8_t value = 0; value < 16; ++value) {
            const SubfacePath path {2, value};
            if (!subfaces_share_edge(seed.path, path) ||
                protected_detail_leaves.count({seed.face_id, path}) != 0) continue;
            const SubfaceLabelEvidence neighbor {seed.face_id, path, Label::FaceSkin,
                                                  seed.confidence, 0};
            Color neighbor_color;
            if (!appearance(neighbor, neighbor_color) ||
                !compatible_skin(neighbor_color, seed.appearance, Label::FaceSkin)) continue;
            candidates.push_back({seed.face_id, path, seed.target,
                                  std::min(seed.confidence, .80f)});
        }
    }

    // Generated portrait meshes may split the visible ear rim across nearby
    // surface patches that share neither an edge nor a vertex. Bridge that
    // seam once from the adapter's direct ear-skin leaves. Candidates must be
    // reliable Hair roots with matching intrinsic skin color and surface
    // direction. Accepted candidates never become new spatial seeds.
    if (!skin_boundary_seeds.empty()) {
        Vec3f minimum = source.mesh.vertices.front(), maximum = minimum;
        for (const Vec3f& vertex : source.mesh.vertices) {
            minimum = minimum.cwiseMin(vertex);
            maximum = maximum.cwiseMax(vertex);
        }
        const float radius = (maximum - minimum).norm() * .012f;
        if (radius > 0.f && std::isfinite(radius)) {
            using Cell = std::array<long long, 3>;
            struct SpatialSkinSeed {
                SkinBoundarySeed seed;
                Vec3f center;
                Vec3f normal;
            };
            const auto cell = [radius](const Vec3f& point) {
                Cell result {};
                for (int axis = 0; axis < 3; ++axis)
                    result[axis] = static_cast<long long>(std::floor(point[axis] / radius));
                return result;
            };
            const auto leaf_center = [&](size_t face_id, const SubfacePath& path, Vec3f& output) {
                std::array<Barycentric, 3> vertices;
                if (face_id >= count || !subface_vertices(path, vertices)) return false;
                Barycentric centroid {};
                for (const Barycentric& vertex : vertices)
                    for (size_t axis = 0; axis < 3; ++axis)
                        centroid[axis] += vertex[axis] / 3.f;
                output = Vec3f::Zero();
                for (size_t corner = 0; corner < 3; ++corner)
                    output += centroid[corner] * source.mesh.vertices[source.mesh.indices[face_id][corner]];
                return output.allFinite();
            };
            const auto face_normal = [&](size_t face_id) {
                const auto& triangle = source.mesh.indices[face_id];
                Vec3f normal = (source.mesh.vertices[triangle[1]] - source.mesh.vertices[triangle[0]])
                    .cross(source.mesh.vertices[triangle[2]] - source.mesh.vertices[triangle[0]]);
                return normal.squaredNorm() > 1e-18f ? normal.normalized() : Vec3f::Zero();
            };
            std::vector<SpatialSkinSeed> spatial_seeds;
            std::map<Cell, std::vector<size_t>> seed_cells;
            for (const SkinBoundarySeed& seed : skin_boundary_seeds) {
                Vec3f center;
                if (!leaf_center(seed.face_id, seed.path, center)) continue;
                const Vec3f normal = face_normal(seed.face_id);
                if (normal.squaredNorm() <= 0.f) continue;
                seed_cells[cell(center)].push_back(spatial_seeds.size());
                spatial_seeds.push_back({seed, center, normal});
            }
            const float radius_squared = radius * radius;
            for (size_t face_id = 0; face_id < count; ++face_id) {
                const auto root = roots.find(face_id);
                if (analysis.face_labels[face_id] != Label::Hair ||
                    analysis.face_confidence[face_id] < minimum_confidence || root == roots.end()) continue;
                const Vec3f normal = face_normal(face_id);
                if (normal.squaredNorm() <= 0.f) continue;
                for (uint8_t value = 0; value < 16; ++value) {
                    const SubfacePath path {2, value};
                    const auto key = std::make_pair(face_id, path);
                    if (protected_detail_leaves.count(key) != 0) continue;
                    Vec3f center;
                    if (!leaf_center(face_id, path, center)) continue;
                    const Cell origin = cell(center);
                    const SpatialSkinSeed* nearest = nullptr;
                    float best_distance = radius_squared;
                    for (int z = -1; z <= 1; ++z)
                        for (int y = -1; y <= 1; ++y)
                            for (int x = -1; x <= 1; ++x) {
                                const auto found = seed_cells.find({origin[0] + x, origin[1] + y, origin[2] + z});
                                if (found == seed_cells.end()) continue;
                                for (size_t index : found->second) {
                                    const SpatialSkinSeed& seed = spatial_seeds[index];
                                    if (root->second == seed.seed.target || normal.dot(seed.normal) < .75f) continue;
                                    const float distance = (center - seed.center).squaredNorm();
                                    if (distance < best_distance) { best_distance = distance; nearest = &seed; }
                                }
                            }
                    if (!nearest) continue;
                    const SubfaceLabelEvidence evidence {face_id, path, Label::FaceSkin,
                                                          nearest->seed.confidence, 0};
                    Color source_color;
                    if (!appearance(evidence, source_color) ||
                        !compatible_skin(source_color, nearest->seed.appearance, Label::FaceSkin)) continue;
                    candidates.push_back({face_id, path, nearest->seed.target,
                                          std::min(nearest->seed.confidence, .76f)});
                }
            }
        }
    }

    // A coarse eyelid triangle can receive a reliable whole-face sclera vote
    // even when its intrinsic material is skin. Correct only a complete warm
    // skin-colored face attached to reliable local skin. Dark sclera shadows
    // remain neutral in hue and do not satisfy this material signature.
    for (size_t face_id = 0; face_id < count; ++face_id) {
        const auto root = roots.find(face_id);
        // Whole-face mapping may omit a warm eye-mask face when it fails the
        // sclera material test. Such a face still needs a skin child override
        // if reliable skin touches it; absence from roots is evidence of
        // rejection, not evidence that the existing material is correct.
        if (!have_sclera_center || face_id >= count || analysis.face_labels[face_id] != Label::EyeSclera ||
            analysis.face_confidence[face_id] < minimum_confidence) continue;
        std::set<size_t> visited;
        std::vector<Sample> local_skin;
        for (int corner = 0; corner < 3; ++corner) {
            for (size_t neighbor : faces_at_vertex[source.mesh.indices[face_id][corner]]) {
                if ((analysis.face_labels[neighbor] != Label::FaceSkin &&
                     analysis.face_labels[neighbor] != Label::BodySkin) ||
                    analysis.face_confidence[neighbor] < minimum_confidence) continue;
                if (visited.insert(neighbor).second)
                    local_skin.push_back({face_lab(source, neighbor), area(source, neighbor)});
            }
        }
        if (local_skin.empty()) continue;
        const auto skin_centers = prototypes(local_skin, 1);
        if (skin_centers.empty()) continue;
        size_t skin_target = palette.size();
        if (has_card) skin_target = role_slots[0];
        else {
            float best = std::numeric_limits<float>::max();
            for (size_t slot = 0; slot < palette.size(); ++slot) {
                if (root != roots.end() && palette[slot] == root->second) continue;
                const float score = distance(skin_centers.front(), palette_labs[slot]);
                if (score < best) { best = score; skin_target = slot; }
            }
        }
        skin_target = safe_subface_target(Label::FaceSkin, skin_centers.front(), skin_target);
        if (skin_target >= palette.size() ||
            (root != roots.end() && palette[skin_target] == root->second)) continue;
        const Color face_color = face_lab(source, face_id);
        const float face_chroma = chroma(face_color);
        const bool dark_warm_skin = face_color[0] < .48f && face_chroma > .037f &&
            face_color[1] > 0.f && face_color[2] > 0.f;
        const bool red_yellow_skin = face_chroma > .045f && face_color[2] > .012f &&
            face_color[1] > face_color[2] * 1.35f;
        if ((!dark_warm_skin && !red_yellow_skin) ||
            !compatible_skin(face_color, skin_centers.front(), Label::FaceSkin)) continue;
        for (uint8_t value = 0; value < 16; ++value)
            candidates.push_back({face_id, {2, value}, palette[skin_target], .85f});
    }
    return enforce_subface_budget(candidates, count, budget, output, error);
}

SubfaceColors remap_subface_palette_targets(const SubfaceColors& suggestions,
                                            const std::vector<Color>& original_candidates,
                                            const std::vector<Color>& target_candidates)
{
    if (original_candidates.empty() || original_candidates.size() > 6 ||
        target_candidates.size() != original_candidates.size()) return {};
    for (size_t slot = 0; slot < original_candidates.size(); ++slot) {
        if (!valid_color(original_candidates[slot]) || !valid_color(target_candidates[slot])) return {};
        for (size_t previous = 0; previous < slot; ++previous)
            if (original_candidates[previous] == original_candidates[slot] &&
                target_candidates[previous] != target_candidates[slot]) return {};
    }
    SubfaceColors result;
    result.reserve(suggestions.size());
    for (const SubfaceColor& suggestion : suggestions) {
        const auto found = std::find(original_candidates.begin(), original_candidates.end(), suggestion.color);
        if (found == original_candidates.end()) return {};
        SubfaceColor mapped = suggestion;
        mapped.color = target_candidates[size_t(found - original_candidates.begin())];
        result.push_back(mapped);
    }
    return result;
}

SubfaceColors compose_subfaces(const SubfaceColors& automatic, const FaceColors& manual,
                               bool automatic_enabled)
{
    if (!automatic_enabled) return {};
    std::set<size_t> manual_faces;
    for (const auto& item : manual) manual_faces.insert(item.first);
    SubfaceColors result;
    result.reserve(automatic.size());
    for (const SubfaceColor& item : automatic)
        if (manual_faces.count(item.face_id) == 0) result.push_back(item);
    return result;
}

nlohmann::json encode_analysis(const Analysis& analysis)
{
    if (analysis.canceled || !analysis.error.empty() || analysis.face_roi_size != 512 || analysis.signature.empty() ||
        analysis.geometry_id.empty() || analysis.content_id.empty() || analysis.body_identity.empty() || analysis.face_identity.empty() ||
        analysis.rendered_views != 8 || analysis.face_views > 8 || analysis.face_labels.empty() ||
        analysis.observed_faces > analysis.face_labels.size() || (analysis.person_detected && analysis.face_views == 0) ||
        analysis.face_labels.size() != analysis.face_confidence.size() || analysis.face_labels.size() > maximum_faces)
        throw std::invalid_argument("Cannot cache incomplete semantic analysis.");
    constexpr char hex[] = "0123456789abcdef";
    std::string labels, confidence;
    labels.reserve(analysis.face_labels.size()); confidence.reserve(analysis.face_labels.size() * 8);
    for (size_t i = 0; i < analysis.face_labels.size(); ++i) {
        const float value = analysis.face_confidence[i];
        if (!valid_label(analysis.face_labels[i]) || !std::isfinite(value) || value < 0 || value > 1)
            throw std::invalid_argument("Invalid semantic label or confidence.");
        labels += hex[size_t(analysis.face_labels[i])];
        // Preserve float32 exactly so a cache round-trip cannot move a face
        // across the automatic-paint confidence threshold.
        uint32_t encoded; std::memcpy(&encoded, &value, sizeof(encoded));
        for (int nibble = 0; nibble < 8; ++nibble) confidence += hex[(encoded >> (nibble * 4)) & 15];
    }
    if (analysis.subface_labels.size() > maximum_subface_evidence)
        throw std::invalid_argument("Too much semantic subface evidence to cache.");
    nlohmann::json subfaces = nlohmann::json::array();
    std::pair<size_t, SubfacePath> previous_key {0, {0, 0}};
    bool has_previous = false;
    for (const SubfaceLabelEvidence& evidence : analysis.subface_labels) {
        const unsigned path_limit = evidence.path.depth > 0 && evidence.path.depth <= 2
            ? 1u << (2u * evidence.path.depth) : 0u;
        const auto key = std::make_pair(evidence.face_id, evidence.path);
        if (evidence.face_id >= analysis.face_labels.size() || path_limit == 0 ||
            unsigned(evidence.path.value) >= path_limit ||
            (evidence.label != Label::EyeSclera && evidence.label != Label::Iris &&
             evidence.label != Label::Eyebrow && evidence.label != Label::FaceSkin) ||
            !std::isfinite(evidence.confidence) || evidence.confidence < minimum_confidence ||
            evidence.confidence > 1.f || evidence.samples == 0 || (has_previous && !(previous_key < key)))
            throw std::invalid_argument("Invalid semantic subface evidence.");
        uint32_t encoded; std::memcpy(&encoded, &evidence.confidence, sizeof(encoded));
        std::string confidence_hex;
        confidence_hex.reserve(8);
        for (int nibble = 0; nibble < 8; ++nibble) confidence_hex += hex[(encoded >> (nibble * 4)) & 15];
        subfaces.push_back({evidence.face_id, evidence.path.depth, evidence.path.value,
                            size_t(evidence.label), confidence_hex, evidence.samples});
        previous_key = key;
        has_previous = true;
    }
    return {{"schema", pipeline_version}, {"signature", analysis.signature},
        {"geometry_id", analysis.geometry_id}, {"content_id", analysis.content_id},
        {"body_identity", analysis.body_identity}, {"face_identity", analysis.face_identity},
        {"face_count", analysis.face_labels.size()}, {"labels", labels}, {"confidence_f32", confidence},
        {"subfaces", std::move(subfaces)},
        {"person_detected", analysis.person_detected}, {"rendered_views", analysis.rendered_views},
        {"face_views", analysis.face_views}, {"observed_faces", analysis.observed_faces}};
}

bool decode_analysis(const nlohmann::json& doc, const MeshSnapshot& source, const std::string& body,
                     const std::string& face, Analysis& output, std::string& error)
{
    error.clear();
    try {
        const size_t count = source.mesh.indices.size();
        const auto signature = analysis_cache_key(source, body, face);
        if (signature.empty() || count == 0 || count > maximum_faces || !doc.is_object() ||
            doc.at("schema") != pipeline_version || doc.at("signature") != signature ||
            doc.at("geometry_id") != source.geometry_id || doc.at("content_id") != source.content_id ||
            doc.at("body_identity") != body || doc.at("face_identity") != face ||
            !doc.at("face_count").is_number_unsigned() || doc.at("face_count").get<size_t>() != count)
            throw std::invalid_argument("The cached semantic analysis belongs to another model or recognizer version.");
        const auto& labels = doc.at("labels").get_ref<const std::string&>();
        const auto& confidence = doc.at("confidence_f32").get_ref<const std::string&>();
        if (labels.size() != count || confidence.size() != count * 8)
            throw std::invalid_argument("The cached semantic mask size is invalid.");
        Analysis restored;
        restored.geometry_id = source.geometry_id; restored.content_id = source.content_id;
        restored.body_identity = body; restored.face_identity = face; restored.signature = signature;
        restored.person_detected = doc.at("person_detected").get<bool>();
        restored.rendered_views = doc.at("rendered_views").get<size_t>();
        restored.face_views = doc.at("face_views").get<size_t>();
        restored.observed_faces = doc.at("observed_faces").get<size_t>();
        if (restored.rendered_views != 8 || restored.face_views > 8 || restored.observed_faces > count ||
            (restored.person_detected && restored.face_views == 0))
            throw std::invalid_argument("The cached semantic analysis is incomplete.");
        restored.face_labels.reserve(count); restored.face_confidence.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const int label = unhex(labels[i]);
            if (label < 0 || label >= int(label_count))
                throw std::invalid_argument("The cached semantic analysis contains invalid mask data.");
            uint32_t bits = 0;
            for (int nibble = 0; nibble < 8; ++nibble) {
                const int digit = unhex(confidence[i * 8 + nibble]);
                if (digit < 0) throw std::invalid_argument("The cached semantic confidence is invalid.");
                bits |= uint32_t(digit) << (nibble * 4);
            }
            float value; std::memcpy(&value, &bits, sizeof(value));
            if (!std::isfinite(value) || value < 0 || value > 1) throw std::invalid_argument("The cached semantic confidence is out of range.");
            restored.face_labels.push_back(Label(label));
            restored.face_confidence.push_back(value);
            if (paintable(Label(label)) && restored.face_confidence.back() >= minimum_confidence) ++restored.reliable_faces;
        }
        const auto& subfaces = doc.at("subfaces");
        if (!subfaces.is_array() || subfaces.size() > maximum_subface_evidence)
            throw std::invalid_argument("The cached semantic subface evidence is invalid.");
        std::pair<size_t, SubfacePath> previous_key {0, {0, 0}};
        bool has_previous = false;
        restored.subface_labels.reserve(subfaces.size());
        for (const auto& entry : subfaces) {
            if (!entry.is_array() || entry.size() != 6 || !entry[0].is_number_unsigned() ||
                !entry[1].is_number_unsigned() || !entry[2].is_number_unsigned() ||
                !entry[3].is_number_unsigned() || !entry[4].is_string() || !entry[5].is_number_unsigned())
                throw std::invalid_argument("The cached semantic subface entry is malformed.");
            const size_t face_id = entry[0].get<size_t>();
            const size_t depth = entry[1].get<size_t>();
            const size_t path_value = entry[2].get<size_t>();
            const size_t label_value = entry[3].get<size_t>();
            const uint32_t samples = entry[5].get<uint32_t>();
            const auto& confidence_hex = entry[4].get_ref<const std::string&>();
            if (face_id >= count || depth == 0 || depth > 2 || path_value >= (size_t(1) << (2 * depth)) ||
                (label_value != size_t(Label::EyeSclera) && label_value != size_t(Label::Iris) &&
                 label_value != size_t(Label::Eyebrow) && label_value != size_t(Label::FaceSkin)) ||
                samples == 0 || confidence_hex.size() != 8)
                throw std::invalid_argument("The cached semantic subface entry is out of range.");
            uint32_t bits = 0;
            for (int nibble = 0; nibble < 8; ++nibble) {
                const int digit = unhex(confidence_hex[nibble]);
                if (digit < 0) throw std::invalid_argument("The cached semantic subface confidence is invalid.");
                bits |= uint32_t(digit) << (nibble * 4);
            }
            float value; std::memcpy(&value, &bits, sizeof(value));
            if (!std::isfinite(value) || value < minimum_confidence || value > 1.f)
                throw std::invalid_argument("The cached semantic subface confidence is out of range.");
            const SubfacePath path {uint8_t(depth), uint8_t(path_value)};
            const auto key = std::make_pair(face_id, path);
            if (has_previous && !(previous_key < key))
                throw std::invalid_argument("The cached semantic subface paths are not canonical.");
            restored.subface_labels.push_back({face_id, path, Label(label_value), value, samples});
            previous_key = key;
            has_previous = true;
        }
        output = std::move(restored); return true;
    } catch (const std::exception& failure) { error = failure.what(); return false; }
}
} // namespace Slic3r::AI::SemanticColoring
