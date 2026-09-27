#ifndef slic3r_GeometryEngine_hpp_
#define slic3r_GeometryEngine_hpp_

#include "libslic3r/TriangleMesh.hpp"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <gp_Ax2.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <vector>
#include <string>

namespace Slic3r {

enum class PrimitiveType { Box, Cylinder, Sphere, Cone, Torus, COUNT };
enum class DressUpType   { Fillet, Chamfer };
enum class FaceGroup     { Top, Bottom, Lateral, All };

struct PrimitiveParams {
    PrimitiveType type{PrimitiveType::Box};
    double box_w{20}, box_h{20}, box_d{20};
    double cyl_radius{10}, cyl_height{20};
    double sph_radius{10};
    double cone_r1{10}, cone_r2{5}, cone_height{20};
    double torus_r1{10}, torus_r2{3};

    // Dress-up
    bool   dressup_enabled{false};
    DressUpType dressup_type{DressUpType::Fillet};
    FaceGroup   dressup_faces{FaceGroup::All};
    double dressup_radius{1.0};    // fillet radius
    double dressup_chamfer_dist{1.0}; // chamfer distance (symmetric)

    // Mesh quality
    double linear_deflection{0.01};
    double angular_deflection{0.5};

    template<class Archive>
    void serialize(Archive& ar) {
        ar(type, box_w, box_h, box_d, cyl_radius, cyl_height, sph_radius,
           cone_r1, cone_r2, cone_height, torus_r1, torus_r2,
           dressup_enabled, dressup_type, dressup_faces, dressup_radius, dressup_chamfer_dist,
           linear_deflection, angular_deflection);
    }
};

class GeometryEngine
{
public:
    static TopoDS_Solid make_primitive(const PrimitiveParams& params);

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

    static TriangleMesh tessellate(const TopoDS_Shape& shape,
                                   double linear_deflection = 0.01,
                                   double angular_deflection = 0.5);
    static std::string  primitive_name(PrimitiveType type);

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
    // Outward unit normal of a face at its UV midpoint (orientation-aware) — for the shell gizmo.
    static Vec3d face_normal_world(const TopoDS_Face& face);
    // Sample an edge into a world-space polyline (>=2 pts) for pick-distance + highlight.
    static std::vector<Vec3d> sample_edge_world(const TopoDS_Edge& edge, double chord_tol = 0.05);
    // 0-based edge index into TopExp::MapShapes(shape, TopAbs_EDGE, map).
    static int          edge_count(const TopoDS_Shape& shape);
    static TopoDS_Edge  edge_by_index(const TopoDS_Shape& shape, int index);
    static int          edge_index_of(const TopoDS_Shape& shape, const TopoDS_Edge& edge);

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
