#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include "libslic3r/Color.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r::AI::SemanticColoring {

using Color = std::array<float, 3>;
using Barycentric = std::array<float, 3>;
using FaceColors = std::vector<std::pair<size_t, Color>>;
using Cancel = std::function<bool()>;
using Progress = std::function<void(int, const std::string&)>;

// A path through the same four-way midpoint split used by TriangleSelector.
// Each level stores one child in two bits, most-significant level first:
// 0/1/2 are the original-corner children and 3 is the centre child.
struct SubfacePath {
    uint8_t depth {0};
    uint8_t value {0};
    friend bool operator==(const SubfacePath& lhs, const SubfacePath& rhs) {
        return lhs.depth == rhs.depth && lhs.value == rhs.value;
    }
    friend bool operator<(const SubfacePath& lhs, const SubfacePath& rhs) {
        return lhs.depth != rhs.depth ? lhs.depth < rhs.depth : lhs.value < rhs.value;
    }
};

struct SubfaceColor {
    size_t face_id {0};
    SubfacePath path;
    Color color {};
    float confidence {0.f};
    friend bool operator==(const SubfaceColor& lhs, const SubfaceColor& rhs) {
        return lhs.face_id == rhs.face_id && lhs.path == rhs.path &&
               lhs.color == rhs.color && lhs.confidence == rhs.confidence;
    }
};
using SubfaceColors = std::vector<SubfaceColor>;

struct SubfaceBudget {
    size_t maximum_added_triangles {200000};
    float maximum_added_ratio {.20f};
    float minimum_confidence {.70f};
};

struct SubfaceBudgetResult {
    SubfaceColors accepted;
    size_t added_triangles {0};
    size_t rejected_candidates {0};
};

enum class Label : uint8_t {
    Unknown, Background, Hair, FaceSkin, BodySkin, Clothes, Accessories, Lips, MouthInterior,
    EyeSclera, Iris, Eyebrow
};
inline constexpr size_t label_count = 12;
inline constexpr float minimum_confidence = .70f;
// Version of stored recognition evidence; palette-mapping edits do not invalidate it.
inline constexpr const char* pipeline_version = "orca.semantic-coloring/v17";

struct SubfaceLabelEvidence {
    size_t face_id {0};
    SubfacePath path;
    Label label {Label::Unknown};
    float confidence {0.f};
    uint32_t samples {0};
    friend bool operator==(const SubfaceLabelEvidence& lhs, const SubfaceLabelEvidence& rhs) {
        return lhs.face_id == rhs.face_id && lhs.path == rhs.path && lhs.label == rhs.label &&
               lhs.confidence == rhs.confidence && lhs.samples == rhs.samples;
    }
};

// Convert between raster evidence and the fixed midpoint tree. Both functions
// reject invalid/non-finite barycentric input without changing the output.
bool locate_subface(const Barycentric&, uint8_t depth, SubfacePath& output);
bool subface_vertices(const SubfacePath&, std::array<Barycentric, 3>& output);

// Top-left origin; tightly packed RGB8, with no alpha or row padding.
struct RGBImage {
    int width {0}, height {0};
    std::vector<uint8_t> pixels;
    bool valid() const;
};

// Providers return masks at exactly the input dimensions. A face provider owns
// its model-specific landmark topology; only these semantic masks cross the port.
struct Prediction {
    std::vector<Label> labels;
    std::vector<float> confidence;
    bool person_detected {false};
    bool face_detected {false};
    bool canceled {false};
    std::string error;
    bool valid_for(const RGBImage& image) const;
};
class IBodyRegionRecognizer {
public:
    virtual ~IBodyRegionRecognizer() = default;
    // Include model/weights hash, label mapping and preprocessing versions.
    virtual std::string identity() const = 0;
    virtual Prediction predict(const RGBImage&, const Cancel&) = 0;
};
class IFaceRegionRecognizer {
public:
    virtual ~IFaceRegionRecognizer() = default;
    virtual std::string identity() const = 0;
    virtual Prediction predict(const RGBImage&, const Cancel&) = 0;
};

// Immutable during analysis. Geometry is the exact preview/import canonical
// Z-up millimetre mesh; colors are original sampled sRGB, before any trial/paint.
struct MeshSnapshot {
    indexed_triangle_set mesh;
    std::vector<RGBA> vertex_colors;
    std::vector<RGBA> face_colors;
    std::string geometry_id;
    std::string content_id;
};
// Geometry identity plus the ordered original face/corner sRGB float32 values.
// Vertex duplication at color seams does not change this identity.
std::string content_fingerprint(const MeshSnapshot&);

struct Analysis {
    std::string geometry_id, content_id, body_identity, face_identity, signature;
    std::vector<Label> face_labels;
    std::vector<float> face_confidence;
    std::vector<SubfaceLabelEvidence> subface_labels;
    bool person_detected {false};
    bool canceled {false};
    int face_roi_size {512};
    size_t rendered_views {0}, face_views {0}, observed_faces {0}, reliable_faces {0};
    std::string error;
};

// CPU z-buffer. A pixel refers to an original mesh face, never a sampled mesh.
// Background face id is UINT32_MAX; depth increases toward the camera.
struct RenderedView {
    RGBImage image;
    std::vector<uint32_t> face_ids;
    std::vector<Barycentric> barycentric;
    std::vector<float> depth;
    std::vector<float> facing;
    std::vector<Vec3f> projected_vertices;
    float surface_depth_tolerance {0.f};
    // For a visible subpixel face with no direct raster sample: an adjacent
    // pixel checked against its plane and normal. Consumers must additionally
    // require source-color compatibility and neighboring semantic agreement.
    std::vector<uint32_t> surface_samples;
    bool canceled {false};
    std::string error;
};
// Coordinates in the full-view camera, normalized to [0,1]. A nonsquare region
// is letterboxed without distortion. Region rendering rerasterizes every source
// triangle; it does not enlarge a previously sampled RGB/face-id image.
struct ViewRegion { float left {0}, top {0}, width {1}, height {1}; };
struct FaceRegionLeafDiagnostic {
    size_t face_id {0};
    SubfacePath path;
    Label label {Label::Unknown};
    float confidence {0.f};
    float dominance {0.f};
    uint32_t samples {0};
};
struct FaceRegionDiagnostic {
    int view_index {0};
    ViewRegion region;
    bool face_detected {false};
    std::vector<FaceRegionLeafDiagnostic> leaves;
};
struct AnalysisDiagnostics {
    std::function<bool(size_t, const SubfacePath&)> include_leaf;
    float yaw_offset_degrees {0.f};
    std::vector<FaceRegionDiagnostic> face_regions;
};
RenderedView render_view(const MeshSnapshot&, float yaw_degrees, int image_size = 512,
                         const Cancel& = {});
RenderedView render_region(const MeshSnapshot&, float yaw_degrees, const ViewRegion&,
                           int image_size = 512, const Cancel& = {});
Analysis analyze(const MeshSnapshot&, IBodyRegionRecognizer&, IFaceRegionRecognizer&,
                 const Cancel& = {}, const Progress& = {});
// Diagnostic-only resolution probe. Non-512 results cannot be cached or mapped
// to materials, so experiments cannot enter the product path accidentally.
Analysis analyze_with_face_roi_size(const MeshSnapshot&, IBodyRegionRecognizer&,
                                    IFaceRegionRecognizer&, int face_roi_size,
                                    const Cancel& = {}, const Progress& = {},
                                    AnalysisDiagnostics* = nullptr);

// portrait_card, when present, is ordered skin/dark/light/lips/cool/mid. Its
// role colors must exist in palette. All outputs are exact members of palette;
// many regions may reuse one color. An invalid/stale analysis returns no paint.
FaceColors map_palette(const MeshSnapshot&, const Analysis&, const std::vector<Color>& palette,
                       const std::vector<Color>& portrait_card = {});
// Edit an established material assignment by slot; do not run recognition or
// nearest-color competition again. Invalid/ambiguous palettes return no auto layer.
FaceColors remap_palette_targets(const FaceColors& suggestions, const std::vector<Color>& original_candidates,
                                const std::vector<Color>& target_candidates);
FaceColors compose(const FaceColors& automatic, const FaceColors& manual, bool automatic_enabled);

// Budgeting is transactional: malformed input produces no accepted leaves.
// Candidates are considered by confidence, then stable face/path order. Each
// unique split node costs three added triangles; omitted leaves inherit the
// existing safe whole-face assignment.
bool enforce_subface_budget(const SubfaceColors& candidates, size_t original_face_count,
                            const SubfaceBudget&, SubfaceBudgetResult&, std::string& error);
bool map_subface_palette(const MeshSnapshot&, const Analysis&, const FaceColors& whole_face,
                         const std::vector<Color>& palette, const std::vector<Color>& portrait_card,
                         const SubfaceBudget&, SubfaceBudgetResult&, std::string& error);
SubfaceColors remap_subface_palette_targets(const SubfaceColors& suggestions,
                                            const std::vector<Color>& original_candidates,
                                            const std::vector<Color>& target_candidates);
// Manual whole-face paint suppresses every automatic child of that face.
SubfaceColors compose_subfaces(const SubfaceColors& automatic, const FaceColors& manual,
                               bool automatic_enabled);

// Cache stores analysis only: changing the physical palette never reruns vision.
// Decode is transactional and validates all identities before allocating masks.
std::string analysis_cache_key(const MeshSnapshot&, const std::string& body_identity,
                               const std::string& face_identity);
nlohmann::json encode_analysis(const Analysis&);
bool decode_analysis(const nlohmann::json&, const MeshSnapshot&, const std::string& body_identity,
                     const std::string& face_identity, Analysis&, std::string& error);

} // namespace Slic3r::AI::SemanticColoring
