#ifndef slic3r_SketchEngine_hpp_
#define slic3r_SketchEngine_hpp_

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"

#include <gp_Pln.hxx>
#include <gp_Ax3.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <vector>

namespace Slic3r {

struct SketchSegment {
    enum Type { Line, Arc, Circle, Rectangle, Polygon };
    Type type{Line};
    Vec2d p0{0,0}, p1{0,0};
    Vec2d center{0,0};
    double radius{0}, start_angle{0}, end_angle{0};
    std::vector<Vec2d> points;
    template<class Archive>
    void serialize(Archive& ar) { ar(type, p0, p1, center, radius, start_angle, end_angle, points); }
};

struct SketchEntity {
    enum class Type { Line, Arc, Circle, Point, Ellipse, EllipseArc, BSpline };
    Type   type{Type::Line};
    Vec2d  p0{0,0};            // Line: start; Arc/EllipseArc: start; Circle/Point/Ellipse: center; BSpline: first pole
    Vec2d  p1{0,0};            // Line: end;   Arc/EllipseArc: end;   (unused for Circle/Point/Ellipse); BSpline: last pole
    Vec2d  center{0,0};        // Arc/Circle/Ellipse(Arc) center
    double radius{0};          // Circle/Arc radius; Ellipse(Arc): semi-major axis (a)
    double start_angle{0};     // Arc sweep start; Ellipse(Arc): parametric start angle (radians)
    double end_angle{0};       // Arc sweep end;   Ellipse(Arc): parametric end angle
    bool   construction{false};
    double rminor{0};          // Ellipse(Arc): semi-minor axis (b)
    double rotation{0};        // Ellipse(Arc): major-axis angle phi (radians, about center)
    std::vector<Vec2d> ctrl;   // BSpline: control points (poles); p0/p1 mirror first/last pole
    template<class Archive>
    void serialize(Archive& ar) {
        // Append-only: rminor/rotation added for Ellipse(Arc) (P2 Tier-B.1); ctrl for BSpline (B.2).
        ar(type, p0, p1, center, radius, start_angle, end_angle, construction, rminor, rotation, ctrl);
    }
};

struct SketchPlane {
    Vec3d origin{0,0,0};
    Vec3d normal{0,0,1};
    Vec3d x_axis{1,0,0};
    Vec3d y_axis{0,1,0};

    gp_Pln to_occt() const;
    static SketchPlane from_face(const TopoDS_Face& face);
    static SketchPlane XY() { return {}; }
    static SketchPlane XZ() { return {{0,0,0}, {0,1,0}, {1,0,0}, {0,0,1}}; }
    static SketchPlane YZ() { return {{0,0,0}, {1,0,0}, {0,1,0}, {0,0,1}}; }

    Vec2d project(const Vec3d& ray_origin, const Vec3d& ray_dir) const;
    Vec3d to_world(const Vec2d& pt) const;

    template<class Archive>
    void serialize(Archive& ar) { ar(origin, normal, x_axis, y_axis); }
};

struct SketchProfile {
    std::vector<Vec2d> points;
    bool closed{false};

    bool is_closed(double tolerance = 0.5) const;
    bool try_close(double tolerance = 0.5);
    void clear() { points.clear(); closed = false; }
    TopoDS_Wire to_occt_wire(const SketchPlane& plane) const;

    template<class Archive>
    void serialize(Archive& ar) { ar(points, closed); }
};

enum class SketchConstraintType {
    Fix, Coincident, Horizontal, Vertical, Distance,
    LockX, LockY, EqualLength, Parallel, Perpendicular,
    Concentric,
    Tangent, Midpoint, Symmetric, Angle,
    Radius, Diameter,
    PointOnLine,  // a point lies on a line (or at signed perpendicular distance `value`)
    PointOnObject, // a point lies on an entity edge (line -> PT_ON_LINE, circle -> PT_ON_CIRCLE)
    // Append-only: cereal serializes this enum positionally as its underlying int, so
    // inserting anywhere but the end reinterprets every constraint in every saved recipe.
    EqualRadius,
    Collinear,
    DistanceX,     // |dx| between two points, projected onto the sketch X axis
    DistanceY,     // |dy| between two points, projected onto the sketch Y axis
    SymmetricAboutY, // mirror across the sketch's vertical axis (x = 0); axis is implicit
    SymmetricAboutX  // mirror across the sketch's horizontal axis (y = 0); axis is implicit
};

// Constraint on a SketchProfile, referencing profile point indices (a,b,c,d).
// `value` carries the target for Distance/LockX/LockY (ignored otherwise).
struct SketchConstraintDef {
    SketchConstraintType type{SketchConstraintType::Coincident};
    int    a{-1}, b{-1}, c{-1}, d{-1};
    double value{0.0};
    template<class Archive> void serialize(Archive& ar) { ar(type, a, b, c, d, value); }
};

// Which point of an entity a constraint reference names.
//   P0 = SketchEntity::p0 (Line start / Point position)
//   P1 = SketchEntity::p1 (Line end)
//   Center = SketchEntity::center (Arc/Circle center)
enum class SketchPointRole { P0, P1, Center };

// Constraint on coexisting SketchEntity objects (Fase 4.2). Each reference is an
// (entity index, point role) pair. Point-form constraints
// (Fix/Coincident/Horizontal/Vertical/Distance/LockX/LockY) use refs A and B as
// individual points. Segment-form constraints (Parallel/Perpendicular/EqualLength)
// use entity indices `ea`/`eb` as whole line segments (their P0->P1); roles are
// ignored for those. `value` carries the target for Distance/LockX/LockY.
struct SketchEntityConstraintDef {
    SketchConstraintType type{SketchConstraintType::Coincident};
    int             ea{-1}, eb{-1};                     // entity indices
    SketchPointRole ra{SketchPointRole::P0};            // role within ea
    SketchPointRole rb{SketchPointRole::P0};            // role within eb
    double          value{0.0};
    int             ec{-1};                             // third entity ref (Symmetric axis)
    SketchPointRole rc{SketchPointRole::P0};            // role within ec
    template<class Archive> void serialize(Archive& ar) { ar(type, ea, eb, ra, rb, value, ec, rc); }
};

// Implicit references every sketch has, addressable from a constraint's ea/eb/ec without
// existing as SketchEntity objects. NEGATIVE so they cannot collide with an entity index;
// -1 is already "unset" and stays that way. Values are serialized inside existing int
// fields, so they are append-only in spirit: never renumber these.
constexpr int kSketchRefOrigin = -2;   // the sketch origin point (0,0)
constexpr int kSketchRefAxisX  = -3;   // the sketch X axis, through the origin, +X
constexpr int kSketchRefAxisY  = -4;   // the sketch Y axis, through the origin, +Y

inline bool is_sketch_ref(int ei) { return ei <= kSketchRefOrigin; }

// Solve a bare entity list in place against entity-form constraints. Shared by
// CadDocument::solve_sketch_feature (committed features) and the in-session GUI
// sketch tool (live solving as dimensions/constraints are added). Returns true on
// convergence; an empty constraint list is a no-op that returns true.
bool solve_sketch_entities(std::vector<SketchEntity>& entities,
                           const std::vector<SketchEntityConstraintDef>& constraints);

struct SketchParams {
    // Extrude/Revolve
    double extrude_len{10}; bool extrude_sym{false}; double extrude_taper{0};
    double revolve_deg{360};
    bool   is_pocket{false}; // cut into selected object instead of new

    // Dress-up
    bool        dressup_enabled{false};
    DressUpType dressup_type{DressUpType::Fillet};
    FaceGroup   dressup_faces{FaceGroup::All};
    double      dressup_radius{1.0};
    double      dressup_chamfer_dist{1.0};

    // Mesh
    double linear_deflection{0.01};

    template<class Archive>
    void serialize(Archive& ar) {
        ar(extrude_len, extrude_sym, extrude_taper, revolve_deg, is_pocket,
           dressup_enabled, dressup_type, dressup_faces, dressup_radius, dressup_chamfer_dist,
           linear_deflection);
    }
};

class SketchEngine
{
public:
    static TopoDS_Shape make_extrude(const TopoDS_Wire& wire, const SketchPlane& plane,
                                     double length, bool symmetric = false, double taper_deg = 0.0);
    static TopoDS_Shape make_extrude(const TopoDS_Face& face, const SketchPlane& plane,
                                     double length, bool symmetric = false, double taper_deg = 0.0);
    // Asymmetric two-sided prism: extrude the wire's face by `up` along +normal and `down`
    // along -normal, fused into one solid. up/down are non-negative magnitudes.
    // Tapered (draft) extrude of a planar wire: the top profile is the base wire offset in its
    // plane by length*tan(taper_deg), lofted from base to top. Falls back to a straight prism on
    // any failure (self-intersecting offset / loft error). taper_deg>0 widens the top.
    static TopoDS_Shape make_extrude_taper(const TopoDS_Wire& wire, const SketchPlane& plane,
                                           double length, double taper_deg);
    static TopoDS_Shape make_extrude_two_sided(const TopoDS_Wire& wire, const SketchPlane& plane,
                                               double up, double down);
    static TopoDS_Shape make_extrude_two_sided(const TopoDS_Face& face, const SketchPlane& plane,
                                               double up, double down);
    static TopoDS_Shape make_extrude_face(const TopoDS_Face& face, const SketchPlane& plane,
                                          double length, bool symmetric = false, double taper_deg = 0.0);

    // Extrude a set of imported rigid regions (Text/SVG). Each region is
    // contour[0]=outer loop + contour[1..]=hole loops, in plane (u,v) mm. Builds
    // one planar face-with-holes per region, extrudes it, and fuses all region
    // solids into a single shape. Empty/degenerate contours are skipped.
    static TopoDS_Shape make_extrude_regions(
        const std::vector<std::vector<std::vector<Vec2d>>>& regions,
        const SketchPlane& plane, double length, bool symmetric = false);

    // Revolve a planar profile wire about an axis lying in the sketch plane and
    // passing through the plane origin: axis_sel 0 = plane X axis, 1 = plane Y axis.
    // A negative angle_deg sweeps the opposite direction (Flip). The profile must
    // lie to one side of the axis (Onshape rule); a straddling profile self-intersects.
    static TopoDS_Shape make_revolve(const TopoDS_Wire& wire, const SketchPlane& plane,
                                     double angle_deg = 360.0, int axis_sel = 0);

    // Sweep a planar profile wire along a path (spine) wire. The profile is turned
    // into a face and swept with BRepOffsetAPI_MakePipe, which keeps the profile
    // perpendicular to the spine along its length. The path may be open or closed;
    // for a clean solid the path's first point should sit on/near the profile plane.
    static TopoDS_Shape make_sweep(const TopoDS_Wire& profile, const TopoDS_Wire& path);

    // Loft a solid through 2+ closed profile wires (each on its own plane), in the
    // given order. ruled=true => straight (ruled) sections; false => smooth (C2).
    static TopoDS_Shape make_loft(const std::vector<TopoDS_Wire>& profiles, bool ruled);

    // Skin `profiles` WITHOUT end caps -> an open shell (sheet). Same as make_loft but the
    // ThruSections solid flag is false. // ponytail: a sibling instead of a bool param, so no
    // existing call site changes.
    static TopoDS_Shape make_loft_surface(const std::vector<TopoDS_Wire>& profiles, bool ruled);

    static TopoDS_Shape make_pocket(const TopoDS_Wire& wire, const SketchPlane& plane,
                                    const TopoDS_Shape& target, double depth);

    static TriangleMesh tessellate(const TopoDS_Shape& shape,
                                   double linear_deflection = 0.01,
                                   double angular_deflection = 0.5);

    static TriangleMesh tessellate(const TopoDS_Shape& shape,
                                   std::vector<int>& tri_face,
                                   double linear_deflection = 0.01,
                                   double angular_deflection = 0.5);

    static TopoDS_Wire entities_to_wire(const std::vector<SketchEntity>& entities,
                                        const SketchPlane& plane);

    // Every closed loop the sketch holds, in the order each loop's FIRST entity appears in
    // `entities`. A Circle or Ellipse is a loop on its own; Line/Arc/EllipseArc/BSpline
    // entities are grouped into loops by shared endpoints. An OPEN chain is returned too —
    // a sweep path is legitimately open, so open-ness is not an error here.
    // Empty vector = nothing usable; the caller decides whether that is an error.
    static std::vector<TopoDS_Wire> entities_to_wires(const std::vector<SketchEntity>& entities,
                                                      const SketchPlane& plane);

    // A planar face from a set of coplanar loops: the largest-area loop is the outer boundary
    // and every other loop is a hole in it. Throws std::runtime_error with a message naming the
    // problem when the loops do not describe one such region.
    static TopoDS_Face wires_to_face(const std::vector<TopoDS_Wire>& wires,
                                     const SketchPlane& plane);

    static std::vector<SketchEntity> mirror_entities(
        const std::vector<SketchEntity>& src, const Vec2d& a, const Vec2d& b);

    // Offset a sketch by `d`, PRESERVING CHAINS. Entities joined by shared endpoints are
    // offset together and their seams repaired (miter join), so a closed profile comes back
    // closed and can still be extruded; per-entity offsetting cannot do that. Sign convention:
    // +d moves each curve to the LEFT of its direction of travel, which for a CCW closed loop
    // is inward. Ellipses and splines are not offset (a parallel of either is not the same
    // kind of curve) and are dropped from the result.
    static std::vector<SketchEntity> offset_entities(
        const std::vector<SketchEntity>& src, double d);

    // Rigid-transform array. Returns the (count-1) copies for instance i=1..count-1
    // (the originals in `src` are NOT included). Each copy i is `src` rigidly
    // transformed by: rotate by i*angle_step about `pivot`, then translate by i*step.
    //   Rectangular/linear array: angle_step = 0, step = spacing*direction (pivot unused).
    //   Polar array:              step = (0,0), angle_step = sweep/count, pivot = centre.
    // Orientation-preserving, so arc/ellipse parametric angles shift by i*angle_step.
    static std::vector<SketchEntity> array_entities(
        const std::vector<SketchEntity>& src, int count,
        const Vec2d& step, double angle_step, const Vec2d& pivot);

    // General affine transform (move / rotate / scale), applied IN PLACE: returns
    // the SAME entities (same count and order), each mapped by
    //   p -> pivot + scale * R(angle) * (p - pivot) + move
    // (radii scale by |scale|; arc/ellipse parametric/rotation angles shift by
    // `angle`). Unlike array_entities this mutates the subjects rather than adding
    // copies. Move: angle=0, scale=1. Rotate-in-place: move=(0,0), scale=1,
    // pivot=centroid. Scale: angle=0.
    static std::vector<SketchEntity> transform_entities(
        const std::vector<SketchEntity>& src,
        const Vec2d& move, double angle, double scale, const Vec2d& pivot);

    static bool fillet_lines(const SketchEntity& a, const SketchEntity& b, double r,
                             SketchEntity& a_out, SketchEntity& b_out, SketchEntity& arc_out);

    // Symmetric chamfer between two lines meeting at a corner: trims each line back
    // by setback distance `d` from the shared corner and returns the connecting
    // straight segment (seg_out) in place of the corner. a_out/b_out are the trimmed
    // lines; seg_out goes seg_out.p0 (on a) -> seg_out.p1 (on b). False if the lines
    // are parallel or `d` overruns either line.
    static bool chamfer_lines(const SketchEntity& a, const SketchEntity& b, double d,
                              SketchEntity& a_out, SketchEntity& b_out, SketchEntity& seg_out);

    static bool trim_entity(SketchEntity& e, const std::vector<SketchEntity>& others,
                            const Vec2d& pick);

    static bool extend_entity(SketchEntity& e, const std::vector<SketchEntity>& others,
                              const Vec2d& pick);

    // Build a cubic-Bezier G1 bridge (as a BSpline entity, 4 poles) connecting endpoint
    // `a_end` of `a` to endpoint `b_end` of `b` (0 = start/p0 side, 1 = end/p1 side).
    // Tangent-continuous with both entities where the endpoint tangent is defined.
    static SketchEntity make_bridge(const SketchEntity& a, int a_end,
                                    const SketchEntity& b, int b_end);
};

} // namespace Slic3r

#endif // slic3r_SketchEngine_hpp_
