#ifndef slic3r_FacetSelectors_hpp_
#define slic3r_FacetSelectors_hpp_

// Geometric facet selectors and headless painting.
//
// A selector is a predicate over the ORIGINAL facets of a model volume, evaluated in the world
// (plate) frame: every vertex is mapped through instance_matrix * volume_matrix, and every facet
// normal through the inverse transpose of its linear part, so non-uniform scaling and mirroring
// give correct world normals. Selectors drive painting (support / seam / fuzzy skin / colour
// layers) without a brush and without the GUI, and the same geometry feeds geometry_query().
//
// Pure libslic3r: no GUI, no wx, so everything here is unit-testable.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "Point.hpp"
#include "TriangleMesh.hpp"
#include "TriangleSelector.hpp"

namespace Slic3r {

class FacetsAnnotation;
class ModelObject;
class ModelVolume;

namespace FacetSelectors {

// ---------------------------------------------------------------------------------------------
// World-frame geometry
// ---------------------------------------------------------------------------------------------

// World-frame data of one volume's original facets.
class VolumeGeometry
{
public:
    // `its` must outlive this object (neighbours are computed lazily from it).
    VolumeGeometry(const indexed_triangle_set &its, const Transform3d &world, size_t volume_id = 0, int volume_index = -1);

    size_t size() const { return m_its->indices.size(); }
    const indexed_triangle_set &its() const { return *m_its; }
    const Transform3d          &world() const { return m_world; }

    // ModelVolume::id().id, 0 when not built from a model.
    size_t volume_id() const { return m_volume_id; }
    // Index of the volume in ModelObject::volumes, -1 when not built from a model.
    int volume_index() const { return m_volume_index; }

    const Vec3d &vertex(size_t facet, int corner) const { return m_vertices[m_its->indices[facet][corner]]; }
    // Unit world normal; zero for a degenerate facet.
    const Vec3d &normal(size_t facet) const { return m_normals[facet]; }
    const Vec3d &centroid(size_t facet) const { return m_centroids[facet]; }
    double       area(size_t facet) const { return m_areas[facet]; }
    // Edge neighbours (its_face_neighbors), -1 on an open edge.
    const std::vector<Vec3i32> &neighbors() const;

    BoundingBoxf3 bounding_box() const;

private:
    const indexed_triangle_set   *m_its;
    Transform3d                   m_world;
    size_t                        m_volume_id;
    int                           m_volume_index;
    std::vector<Vec3d>            m_vertices;
    std::vector<Vec3d>            m_normals;
    std::vector<Vec3d>            m_centroids;
    std::vector<double>           m_areas;
    mutable std::vector<Vec3i32>  m_neighbors;
    mutable bool                  m_has_neighbors = false;
};

// Everything one selector evaluation can see: all volumes of the target, so that seeded
// selectors (planarRegion) pick a single seed across the whole object.
struct EvalContext
{
    std::vector<VolumeGeometry> volumes;
};

// Geometry of the model parts of one instance of an object (modifiers, negative volumes and
// support blockers/enforcers are skipped unless `model_parts_only` is false).
// `volume_ids`: when non-empty, only volumes whose id().id is listed.
EvalContext object_geometry(const ModelObject &object, size_t instance_idx, bool model_parts_only = true,
                            const std::vector<size_t> &volume_ids = {});

// ---------------------------------------------------------------------------------------------
// Selectors
// ---------------------------------------------------------------------------------------------

// Which points of a facet a spatial selector tests.
enum class PointTest { Centroid, All, Any };

class Selector
{
public:
    virtual ~Selector() = default;
    // Set mask[i] = 1 for every facet i of ctx.volumes[volume] that matches; leave others 0.
    // `mask` has ctx.volumes[volume].size() entries, all zero on entry.
    virtual void evaluate(const EvalContext &ctx, size_t volume, std::vector<uint8_t> &mask) const = 0;
};
using SelectorPtr = std::shared_ptr<const Selector>;

// Named directions in the plate frame: rear = +Y, front = -Y, right = +X, left = -X, top = +Z, bottom = -Z.
bool side_direction(const std::string &side, Vec3d &dir);

SelectorPtr select_all();
// Facets facing down whose normal is less than max_angle_deg from straight down (the gizmo's
// "highlight overhang by angle" rule). 90 = every downward-facing facet.
SelectorPtr select_overhang(double max_angle_deg);
// Facets whose normal is within within_deg of `direction` (normalized internally).
SelectorPtr select_normal(const Vec3d &direction, double within_deg);
SelectorPtr select_z_range(double z_min, double z_max, PointTest test = PointTest::Centroid);
SelectorPtr select_box(const Vec3d &min, const Vec3d &max, PointTest test = PointTest::Centroid);
SelectorPtr select_sphere(const Vec3d &center, double radius, PointTest test = PointTest::Centroid);
// Facets on the side of the plane that `normal` points to (dot(p - point, normal) >= 0).
SelectorPtr select_halfspace(const Vec3d &point, const Vec3d &normal, PointTest test = PointTest::Centroid);
// Explicit facet indices of one volume; volume_id (ModelVolume::id().id) wins when non-zero,
// else volume_index (index in ModelObject::volumes); both unset (0 / -1) = every volume.
SelectorPtr select_facets(size_t volume_id, int volume_index, std::vector<int> indices);
// Flood over edge neighbours from a seed facet, keeping facets whose normal is within
// tolerance_deg of the seed's normal.
// Seed by side: among facets facing `direction` within tolerance, the outermost one along it
// (largest area on ties); if none faces it, the facet facing it most closely.
SelectorPtr select_planar_region_by_direction(const Vec3d &direction, double tolerance_deg);
// Seed by point: the facet closest to `point` (world frame).
SelectorPtr select_planar_region_by_point(const Vec3d &point, double tolerance_deg);
// The painting gizmos' smart fill: flood from a seed (as a planar region) while each facet's normal
// is within max_neighbour_deg of the NEIGHBOUR it is reached from, so a curved wall (a cylinder, a
// fillet) fills as one region where a planar region stops.
SelectorPtr select_smooth_region_by_direction(const Vec3d &direction, double max_neighbour_deg);
SelectorPtr select_smooth_region_by_point(const Vec3d &point, double max_neighbour_deg);
SelectorPtr select_all_of(std::vector<SelectorPtr> children);
SelectorPtr select_any_of(std::vector<SelectorPtr> children);
SelectorPtr select_not(SelectorPtr child);

// Evaluate a selector over every volume of the context.
std::vector<std::vector<uint8_t>> evaluate(const Selector &selector, const EvalContext &ctx);

struct ParseError
{
    std::string path;    // JSON path of the failing node, e.g. "where.all[1].normal.side"
    std::string message;
};

// Parse the JSON selector dialect:
//   {"all":[...]} {"any":[...]} {"not":{...}}               combinators
//   {"all":{}} / {"all":true}                                every facet
//   {"overhang":{"maxAngleDeg":45}}
//   {"normal":{"side":"rear"|"front"|"left"|"right"|"top"|"bottom" | "direction":[x,y,z], "withinDeg":45}}
//   {"zRange":[min,max]} or {"zRange":{"min":a,"max":b,"test":"centroid|all|any"}}
//   {"box":{"min":[x,y,z],"max":[x,y,z],"test":...}}
//   {"sphere":{"center":[x,y,z],"radius":r,"test":...}}
//   {"halfspace":{"point":[x,y,z],"normal":[x,y,z],"test":...}}
//   {"facets":{"volume":<volume id>|"volumeIndex":i, "indices":[...]}}
//   {"planarRegion":{"seedSide":"rear" | "seedPoint":[x,y,z], "toleranceDeg":5}}
//   {"smoothRegion":{"seedSide":"rear" | "seedPoint":[x,y,z], "maxNeighbourAngleDeg":30}}
// Returns nullptr and fills `error` on failure. `path` prefixes error paths.
SelectorPtr parse_selector(const nlohmann::json &j, ParseError *error = nullptr, const std::string &path = "where");

// ---------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------

enum class PaintLayer { Support, Seam, Fuzzy, Color };
enum class PaintMode {
    Add,     // matched facets get the state; everything else unchanged
    Replace, // the state is removed everywhere first, then matched facets get it (other states kept)
    Clear    // matched facets are erased (any state); the state argument is ignored
};

bool        parse_paint_layer(const std::string &s, PaintLayer &out);
const char *paint_layer_name(PaintLayer layer);
bool        parse_paint_mode(const std::string &s, PaintMode &out);

// "enforce" | "block" | "erase" | {"filament": n} (n 1-based, colour layer only).
// Support / seam accept enforce, block, erase; fuzzy accepts enforce, erase; colour accepts
// {"filament": n} and erase. Returns false with a message otherwise.
bool parse_paint_state(const nlohmann::json &j, PaintLayer layer, EnforcerBlockerType &out, std::string &error);
// Inverse of parse_paint_state, for reports: "enforce", "block", "fuzzy", "filament 3".
std::string paint_state_name(PaintLayer layer, EnforcerBlockerType state);

FacetsAnnotation       &annotation(ModelVolume &volume, PaintLayer layer);
const FacetsAnnotation &annotation(const ModelVolume &volume, PaintLayer layer);

struct StateStats
{
    int    facets   = 0;   // painted triangles, including subdivided brush triangles
    double area_mm2 = 0.;  // world-frame area
};
using StateStatsMap = std::map<int, StateStats>; // EnforcerBlockerType value -> stats, NONE omitted

struct PaintResult
{
    size_t        hits         = 0;     // original facets matched by the selector
    double        hit_area_mm2 = 0.;    // their world-frame area
    bool          changed      = false; // the annotation data differs (false for a dry run)
    bool          would_change = false; // the operation changes (or, for a dry run, would change) the data
    StateStatsMap states;               // the layer after the operation (would-be result for a dry run)
};

// Paint one mesh's annotation data in place. `world` maps mesh coordinates to the plate frame
// (used for areas only). `mask` holds one entry per original facet of `mesh`.
PaintResult paint_data(const TriangleMesh &mesh, const Transform3d &world, TriangleSelector::TriangleSplittingData &data,
                       EnforcerBlockerType state, PaintMode mode, const std::vector<uint8_t> &mask, bool dry_run);

// Paint one model volume of an object instance. The selector is evaluated over `ctx`, which must
// contain this volume (matched by volume id). Writes through FacetsAnnotation::set so the
// undo/redo timestamp moves only when the data changed.
PaintResult paint_volume(ModelVolume &volume, const EvalContext &ctx, size_t ctx_volume, PaintLayer layer,
                         EnforcerBlockerType state, PaintMode mode, const Selector &selector, bool dry_run);

// Painted state statistics of an annotation in the world frame.
StateStatsMap annotation_stats(const TriangleMesh &mesh, const Transform3d &world, const TriangleSelector::TriangleSplittingData &data);

// {"<state name>": {"facets": n, "areaMm2": a}, ...}
nlohmann::json stats_to_json(PaintLayer layer, const StateStatsMap &stats);

// ---------------------------------------------------------------------------------------------
// Geometry queries (read-only, world frame)
// ---------------------------------------------------------------------------------------------

// query: "bbox" | "overhangs" | "sides" | "planarRegions" | "heightAt"
//   bbox          {}                                     object and per-volume boxes
//   overhangs     {maxAngleDeg=45, limit=10}             totals and largest connected regions
//   sides         {withinDeg=45}                         area facing each named side, extremes
//   planarRegions {toleranceDeg=5, minAreaMm2=1, limit=20}
//   heightAt      {x, y}                                 z of every surface crossing the vertical line
// Returns {"ok":false,"error":...,"path":...} on bad params, else {"ok":true, ...}.
nlohmann::json geometry_query(const EvalContext &ctx, const std::string &query, const nlohmann::json &params);

// Closest distance between two point sets' surfaces (vertex-to-mesh both ways; exact for
// vertex-face pairs, an upper bound for edge-edge). Also bounding-box gaps per axis and centroid distance.
nlohmann::json geometry_measure(const EvalContext &a, const EvalContext &b);

} // namespace FacetSelectors
} // namespace Slic3r

#endif // slic3r_FacetSelectors_hpp_
