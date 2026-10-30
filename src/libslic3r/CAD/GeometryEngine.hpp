#ifndef slic3r_GeometryEngine_hpp_
#define slic3r_GeometryEngine_hpp_

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <TopoDS_Shape.hxx>
#include <array>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <vector>
#include <string>

namespace Slic3r {

enum class FaceGroup     { Top, Bottom, Lateral, All };

class GeometryEngine
{
public:
    // Read a STEP file into its top-level solids (one TopoDS_Shape per solid; falls back to
    // the whole shape if it contains no closed solids). Reuses OCCT's STEPControl_Reader,
    // already linked via Format/STEP.cpp — no new dependency. err is set on failure (empty result).
    static std::vector<TopoDS_Shape> read_step_solids(const std::string& path, std::string& err);

    // Triangle mesh -> B-rep solid. Native port of mesh2step
    // (github.com/tommasobbianchi/mesh2step): vertices and edges are SHARED across triangles
    // at construction time (vertex cache by deduped index, edge cache by unordered index pair),
    // so there is no BRepBuilderAPI_Sewing pass to reconstruct topology afterwards — which is
    // both faster and what makes watertightness fall out of the edge-usage counts for free.
    // Runs in-process on the OCCT kernel libslic3r already links: no STEP file is written or
    // re-read (a faceted STEP of a 62k-triangle mesh is ~149 MB and takes OCCT's reader >300 s
    // to parse back, so routing the Design tab through a file would hang the GUI).
    struct MeshBrepStats {
        int    input_tris{0};
        int    kept_tris{0};
        int    degenerate_collapsed{0};  // <3 distinct vertices after tolerance quantization
        int    degenerate_sliver{0};     // 3 distinct vertices but near-collinear
        int    faces_built{0};
        int    faces_failed{0};
        int    unique_edges{0};
        int    boundary_edges{0};        // used by exactly 1 triangle -> open shell
        int    nonmanifold_edges{0};     // used by >=3 triangles
        bool   watertight{false};        // every edge used exactly twice
        bool   is_solid{false};          // watertight AND MakeSolid gave a positive volume
        double volume{0.0};
        int    faces_final{0};           // after the optional coplanar merge
    };
    // tolerance: spatial quantization cell used ONLY for vertex dedup and as the
    //   sub-resolution floor below which a triangle is noise. Never a sew tolerance.
    // merge_angle_deg > 0: run ShapeUpgrade_UnifySameDomain to merge coplanar neighbours into
    //   single faces (a 12-triangle cube -> 6 pickable faces). This is what makes the imported
    //   body editable with the face/edge tools; <= 0 keeps the exact one-face-per-triangle form.
    // Never wraps a non-watertight shell as a fake solid: an open mesh comes back as a shell,
    // with the reason (boundary / non-manifold edge counts) reported in stats.
    static TopoDS_Shape mesh_to_brep(const indexed_triangle_set& its,
                                     double tolerance,
                                     double merge_angle_deg,
                                     MeshBrepStats& stats);

    struct MassProps {
        double volume{0.0};
        double surface_area{0.0};
        Vec3d  center_of_mass{Vec3d::Zero()};
        std::array<double, 9> inertia{};
        bool   valid{false};
        // False for a sheet body (an open shell with no solid). Volume and inertia are then
        // meaningless and are reported as zero; surface_area stays meaningful. See the .cpp.
        bool   is_solid{false};
    };
    static MassProps mass_properties(const TopoDS_Shape& shape);

    struct Deviation { double max_mm{0}; double mean_mm{0}; double rms_mm{0}; int sample_count{0}; };
    static Deviation surface_deviation(const TopoDS_Shape& candidate,
                                       const TopoDS_Shape& reference,
                                       double linear_deflection = 0.5);

    static TopoDS_Shape apply_fillet(const TopoDS_Shape& solid, double radius,
                                     FaceGroup faces = FaceGroup::All);
    static TopoDS_Shape apply_fillet(const TopoDS_Shape& solid, double radius,
                                     int edge_id);
    static TopoDS_Shape apply_chamfer(const TopoDS_Shape& solid, double distance,
                                       FaceGroup faces = FaceGroup::All);
    static TopoDS_Shape apply_chamfer(const TopoDS_Shape& solid, double distance,
                                       int edge_id);
    // How a fillet's size is read. Default: `radius` is constant along the edge.
    //  r2 >= 0     the radius runs linearly from `radius` to `r2` along each edge (end 1 = the end nearest `start` when given)
    //  chord       `radius` is the width of the round measured across it, not a radius: r = chord / (2 sin(phi/2)), phi = the
    //              angle between the two faces' outward normals, taken at each edge's midpoint
    struct FilletLaw {
        double r2{-1.0};
        bool   chord{false};
        bool   has_start{false};
        Vec3d  start{0, 0, 0};
        // A radius that follows a B-spline along the edge (u = 0 at the first end .. 1 at the last): the control radii (the
        // fillet's own radius is the first) and the full knot vector, clamped, degree = knots - radii - 1. Overrides r2.
        std::vector<double> radii;
        std::vector<double> knots;
        // Fit "taper": where `radius` does not fit, the radius follows the room there. A clamped cubic B-spline of 8 equal
        // spans along each edge, its control radii min(radius, 0.9 * room) at the Greville abscissae, room being the
        // narrower of the two faces measured square to the edge, times tan(half the angle between the faces). Overrides the rest.
        bool fit_taper{false};
    };
    // Several edges in one operation, all ids resolved against `solid`.
    static TopoDS_Shape apply_fillet(const TopoDS_Shape& solid, double radius,
                                     const std::vector<int>& edge_ids);
    static TopoDS_Shape apply_fillet(const TopoDS_Shape& solid, double radius,
                                     const std::vector<int>& edge_ids, const FilletLaw& law);
    static TopoDS_Shape apply_chamfer(const TopoDS_Shape& solid, double distance,
                                       const std::vector<int>& edge_ids);

    static TriangleMesh tessellate(const TopoDS_Shape& shape,
                                   double linear_deflection = 0.003,
                                   double angular_deflection = 0.5);

    // Topology accessors for in-viewport face/edge picking (Design tab). Face index is the
    // TopExp_Explorer(shape, TopAbs_FACE) ordinal — identical to SketchEngine::tessellate's
    // per-triangle face id, so a picked triangle's id maps back to a face here.
    static TopoDS_Face face_by_index(const TopoDS_Shape& shape, int index);  // null if out of range
    static int         face_count(const TopoDS_Shape& shape);
    // Bulk enumeration in the SAME order as face_by_index / edge_by_index, so ids are
    // interchangeable. Walking a body with the _by_index accessors is quadratic (each call
    // rescans the shape — edge_by_index even rebuilds the whole indexed map), which cost
    // ~15 s on a 4.7k-face imported solid; enumerate once instead.
    static std::vector<TopoDS_Face> faces_of(const TopoDS_Shape& shape);
    static std::vector<TopoDS_Edge> edges_of(const TopoDS_Shape& shape);
    static std::vector<TopoDS_Edge> edges_of_face(const TopoDS_Face& face);
    // Centre of mass (world) of a face — used to compute the extrude length for "up to face".
    static Vec3d face_centroid_world(const TopoDS_Face& face);
    // The strategic points of a face, the places a mate connector wants to sit: [0] is always the centroid, then, for EVERY edge of
    // EVERY wire (outer boundary and holes alike) its end vertices and midpoint, plus the centre of a circular or arc edge and the four
    // quadrant points of a full circle. So a rectangle gives 9 (centroid, 4 corners, 4 midpoints); a disc 5; a slot its two arc centres,
    // arc midpoints, line midpoints and corners; a hole adds its own centre and quadrants. Deterministic order, duplicates removed.
    struct FacePoint { Vec3d p{0, 0, 0}; int kind{0}; };   // kind: 0 centroid, 1 vertex, 2 midpoint, 3 arc/circle centre, 4 quadrant
    static std::vector<FacePoint> face_snap_points(const TopoDS_Face& face);
    // Outward unit normal of a face at its UV midpoint (orientation-aware) — for the shell gizmo.
    static Vec3d face_normal_world(const TopoDS_Face& face);
    // Sample an edge into a world-space polyline (>=2 pts) for pick-distance + highlight.
    static std::vector<Vec3d> sample_edge_world(const TopoDS_Edge& edge, double chord_tol = 0.05);
    // The edges a viewer draws over a body, each as a polyline: every edge of the shape once,
    // without degenerate edges (a cone apex) and without the seam of a closed surface (the line
    // down a cylinder's side), which is where OCCT closes the parameter space, not a real edge.
    static std::vector<std::vector<Vec3d>> display_edges(const TopoDS_Shape& shape, double chord_tol);
    // 0-based edge index into TopExp::MapShapes(shape, TopAbs_EDGE, map).
    static int          edge_count(const TopoDS_Shape& shape);
    static TopoDS_Edge  edge_by_index(const TopoDS_Shape& shape, int index);
    static int          edge_index_of(const TopoDS_Shape& shape, const TopoDS_Edge& edge);
    // Every edge reachable from `index` through vertices where the two edges meet tangent (within
    // `deg`, direction-insensitive) — the "tangent loop" a double-click on an edge selects. Includes
    // `index`; ids are the same edge_by_index ids.
    static std::vector<int> tangent_edge_chain(const TopoDS_Shape& shape, int index, double deg = 2.0);

    // Analysis of a cylindrical face for the Thread tool (a hole bore or a cylinder's lateral
    // surface): axis (base at the lower axial end + unit direction), radius, axial extent, and
    // whether it is a bore (face normal points toward the axis = internal thread). ok=false if
    // the face is not a cylinder.
    struct CylinderFace {
        bool   ok{false};
        Vec3d  base{0, 0, 0};
        Vec3d  axis{0, 0, 1};
        double radius{0};
        double height{0};
        bool   internal{false};
    };
    static CylinderFace cylinder_of_face(const TopoDS_Face& face);
    // Circular edge (a cylinder's perimeter): base = circle centre, axis = circle normal,
    // radius = circle radius, height = 0 (unknown from an edge), internal = false. ok=false if
    // the edge is not a circle. Lets the Thread tool be driven by a picked circular rim.
    static CylinderFace circle_of_edge(const TopoDS_Edge& edge);

    // Plane-coordinate (u,v) bounding box of a face's vertices, measured from `origin` along
    // `x_axis`/`y_axis`. Lets the Hole tool dimension the hole from the face SIDES (umin/vmin =
    // two adjacent edges) instead of from the centre. Returns false if the face has no vertices.
    static bool face_plane_bounds(const TopoDS_Face& face, const Vec3d& origin,
                                  const Vec3d& x_axis, const Vec3d& y_axis,
                                  double& umin, double& umax, double& vmin, double& vmax);

private:
    static std::vector<TopoDS_Edge> collect_edges(const TopoDS_Shape& solid, FaceGroup faces);
    static FaceGroup classify_face(const TopoDS_Face& face, const TopoDS_Shape& solid);
};

} // namespace Slic3r

#endif // slic3r_GeometryEngine_hpp_
