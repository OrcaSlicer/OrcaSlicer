#ifndef slic3r_CadDocument_hpp_
#define slic3r_CadDocument_hpp_

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"   // FaceGroup
#include "libslic3r/Color.hpp"            // ColorRGBA (per-body display colour override)

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <cereal/cereal.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/string.hpp>
#include <cstdint>
#include <cstddef>
#include <map>
#include <cereal/types/map.hpp>
#include <string>
#include <vector>
#include <utility>
#include <memory>

namespace Slic3r {

enum class CadFeatureType { Sketch, Extrude, Fillet, Chamfer, Hole, Thread, Shell, Revolve, Sweep, Pattern, Plane, Loft, Draft, Import, Boolean, Cut, Mirror, Axis, CoordSys, Helix, Transform, Thicken, Project, DeleteFace, Rib, SurfaceExtrude, SurfaceRevolve, ThickenSurface, SurfaceOffset, SurfaceLoft, SurfaceFill, Mate, Sketch3D,
                             SmRule, SmBase, SmEdgeFlange, SmContourFlange, SmHem, SmUnfold, SmRefold };
enum class SketchShape    { Rectangle, Circle };
enum class PlaneType      { Offset, Angle, Midplane, Tangent, TwoEdges, Coincident };
enum class AxisType       { TwoPoints, FaceNormal, CylinderCenterline, PlaneIntersection, AlongEdge };
enum class CoordSysType   { PointWorld, FaceAndDirection };
enum class BooleanMode    { New, Add, Cut, Intersect };

enum class ExtrudeEnd { Blind, Symmetric, TwoSided, ThroughAll, UpToFace, UpToVertex };

// Serialize a TopoDS_Shape to/from a BRep string (declared before CadFeature so its
// inline cereal save()/load() can resolve these non-dependent calls).
std::string brep_to_string(const TopoDS_Shape& s);
TopoDS_Shape brep_from_string(const std::string& d);

struct CadFeature {
    CadFeatureType type{CadFeatureType::Sketch};
    std::string    name;
    bool           enabled{true};

    // Sketch params (centered on the plane origin)
    SketchShape shape{SketchShape::Rectangle};
    SketchPlane plane{SketchPlane::XY()};
    double      width{20};
    double      height{20};
    double      radius{10};

    // Real 2D sketch geometry (Onshape-style). When non-empty this takes
    // precedence over the shape/width/height/radius enum path in build_sketch_wire.
    SketchProfile profile;

    // Onshape-style multi-entity sketch geometry. When non-empty this takes
    // precedence over both `profile` and the shape-enum path in build_sketch_wire.
    std::vector<SketchEntity> entities;

    // 2D geometric constraints on `profile` (point indices). Solved in place.
    std::vector<SketchConstraintDef> constraints;

    // Onshape-style constraints on `entities` (Fase 4.2). Solved in place against
    // entity endpoints. Used when `entities` is non-empty (the legacy `constraints`
    // vector applies only to the `profile` path).
    std::vector<SketchEntityConstraintDef> entity_constraints;

    // Imported rigid 2D art (Text glyphs / SVG vector paths) as filled regions.
    // Each region: contour[0] = outer loop, contour[1..] = holes; points in
    // plane (u,v) millimetres. Rendered as a sketch overlay and extruded via a
    // faces-with-holes path (SketchEngine::make_extrude_regions) — deliberately
    // NOT solver entities, so imported art contributes zero DoF and never
    // pollutes the constraint solver / DoF readout. When non-empty it takes
    // precedence over the entities/profile/shape paths in the Extrude case.
    std::vector<std::vector<std::vector<Vec2d>>> imported_regions;

    // Imported rigid 3D B-rep solid (STEP). When the feature type is Import this carries
    // the OCCT shape verbatim — it is adopted as a base body in route_feature (no parametric
    // recipe). Downstream face/edge features (fillet/chamfer/cut/shell/...) act on it like any
    // other body. TopoDS_Shape is a cheap handle, so copying it through recompute/checkpoint
    // snapshots is cheap. Saved with the recipe as a BRep string (see save()/load()).
    TopoDS_Shape imported_solid;

    // Non-destructive placement transform for imported_regions (Text/SVG),
    // applied at display + extrude time as
    //   p -> (p.x*import_scale_x + import_offset.x, p.y*import_scale_y + import_offset.y).
    // Lets the art be moved / enlarged / stretched (independent X/Y) repeatedly
    // without re-vectorising. Identity = no change.
    Vec2d  import_offset{0, 0};
    double import_scale_x{1.0};
    double import_scale_y{1.0};
    // Text/SVG dropped ONTO a solid face (centred on it): the extrude then defaults to an
    // inward Cut (engraving) targeting `import_face_body`. False = free art on a plane.
    bool   import_on_face{false};
    int    import_face_body{-1};

    // Extrude params
    int         sketch_ref{-1};   // index into features[] of the consumed sketch
    double      distance{10};
    bool        symmetric{false};
    BooleanMode mode{BooleanMode::New};
    ExtrudeEnd  extrude_end{ExtrudeEnd::Blind};
    double      distance2{0};     // second-side depth for TwoSided
    double      taper_deg{0};     // draft angle (C4-part2)
    bool        flip{false};      // reverse the extrude direction (negate plane normal)
    int         up_to_face{-1};   // target solid-face id for UpToFace (C4-part2)
    int         extrude_src_face{-1}; // global face id on the current body to extrude as a profile; -1 = use sketch wire
    Vec3d       up_to_point{0,0,0}; // target for UpToVertex (C4-part2)
    // Multi-body target: which body (index into CadDocument::bodies) this feature acts on.
    // -1 = auto (last body). A New extrude appends a fresh body; Add/Cut/Intersect, dress-up,
    // hole and face-extrude(non-New) mutate bodies[target]; face-extrude reads its source
    // face from bodies[target] too. The source-face owner for face-extrude lives here.
    int         target_body{-1};

    // Dress-up params (Fillet/Chamfer) — applied to the current body in order
    double      dressup_size{1.0};         // fillet radius or chamfer distance
    FaceGroup   face_group{FaceGroup::All};
    int         dressup_edge{-1};          // global edge id for edge-targeted fillet/chamfer; -1 = use face_group
    // Several picked edges dressed in ONE operation, all resolved against the same body, so
    // the ids cannot drift the way they do across a chain of single-edge features. When set,
    // dressup_edge holds the first of them: a build that predates the list still dresses that
    // one edge instead of falling back to the whole face group.
    std::vector<int> dressup_edges;
    // Fillet size law (see GeometryEngine::FilletLaw): the radius at the far end of a linearly varying fillet (< 0 = constant),
    // whether dressup_size is a chord width, and which end dressup_size belongs to.
    double      dressup_size2{-1.0};
    bool        dressup_chord{false};
    bool        dressup_has_start{false};
    Vec3d       dressup_start{0, 0, 0};
    std::vector<double> dressup_radii;   // spline radius law: control radii after dressup_size, and the knot vector
    std::vector<double> dressup_knots;
    // The edges this dress-up targets: the list, else the single edge, else none (face group).
    // Text feature: a Sketch whose imported_regions were vectorised from this string in this font
    // (a WxFontUtils descriptor, bold/italic included) at this cap height in mm. The regions are
    // what gets built — they are saved too, so the project opens on a machine without the font —
    // and these three are what an edit reopens the Text dialog with. Empty = not a text feature.
    std::string text_string;
    std::string text_font;
    double      text_height{0.0};
    bool is_text() const { return !text_string.empty(); }
    std::vector<int> dressup_edge_ids() const {
        if (!dressup_edges.empty()) return dressup_edges;
        if (dressup_edge >= 0) return { dressup_edge };
        return {};
    }

    // Hole params (positioned circular cut into the current body)
    double      hole_diameter{5};
    double      hole_depth{10};
    bool        hole_through{true};        // true = symmetric through-cut, ignores hole_depth
    double      hole_x{0};                 // position on the plane (plane u/x axis)
    double      hole_y{0};                 // position on the plane (plane v/y axis)

    // Hole standards library (extends the plain bore above).
    // hole_style: 0 = simple, 1 = counterbore, 2 = countersink.
    int         hole_style{0};
    double      hole_cbore_diameter{0};    // counterbore cylinder diameter (mm), style==1
    double      hole_cbore_depth{0};       // counterbore depth from the entry face (mm), style==1
    double      hole_csink_diameter{0};    // countersink major diameter at entry face (mm), style==2
    double      hole_csink_angle{90};      // countersink included angle (degrees), style==2
    double      hole_tip_angle{0};         // blind hole: included angle of the drill point (deg, 0 = flat bottom)
    std::string hole_standard;             // provenance only, e.g. "M6" / "1/4-20"; not used by geometry

    // Thread params (helical thread about the plane normal at a positioned point)
    double      thread_radius{5};          // MAJOR (nominal) radius when thread_major_nominal,
                                           // else the legacy reference radius (see below)
    double      thread_pitch{2};           // axial advance per turn
    double      thread_height{10};         // total axial length
    double      thread_depth{1};           // radial depth of the thread profile
    bool        thread_internal{false};    // false = external: the thread goes on the target body
                                           // (or on a standalone rod when there is none);
                                           // true = tapped bore cut into the target body
    // How thread_radius is read. true (every thread made since): it is the nominal MAJOR
    // radius, for both kinds — an internal thread bores to R - depth and grooves out to R, an
    // external one is a rod of radius R with its groove cut in to R - depth, so an M6 is 6 mm
    // across its crests either way. false (older recipes, kept bit-for-bit): the ridge was
    // added OUTSIDE R, so an internal thread given the minor diameter came out undersized and
    // an external one given the major diameter came out oversized.
    bool        thread_major_nominal{false};
    double      thread_x{0};               // axis position on the plane (u/x axis)
    double      thread_y{0};               // axis position on the plane (v/y axis)

    // Shell params (hollow the current body to a wall thickness, removing one open face)
    double      shell_thickness{2};        // wall thickness (inward offset)
    int         shell_face{-1};            // global face id to remove (open the shell); -1 = none

    // Draft params (taper a single solid face about a neutral plane = body bbox bottom, pull +Z)
    int         draft_face{-1};            // global face id to draft; -1 = none
    double      draft_angle{5};            // draft angle in degrees (signed: + leans the face inward)

    // Revolve params (sweep a profile about an in-plane axis through the plane origin).
    // Reuses sketch_ref / entities (profile), flip (direction), mode (boolean) and
    // target_body. revolve_axis: 0 = plane X axis, 1 = plane Y axis.
    double      revolve_angle{360};        // sweep angle in degrees (1..360)
    int         revolve_axis{0};           // 0 = plane X, 1 = plane Y
    // A Line of the profile sketch to revolve about instead (index into its entities: a
    // centerline, usually construction, or an edge of the profile itself); -1 = revolve_axis.
    int         revolve_axis_entity{-1};

    // Sweep: profile carried by sketch_ref / entities (like Extrude); the spine is a
    // second Sketch referenced by sweep_path_ref (an open or closed wire). Reuses
    // mode (boolean) and target_body.
    int         sweep_path_ref{-1};        // index into features[] of the path Sketch

    // Loft: build a solid through 2+ closed profile Sketches (loft_profile_refs, in
    // order, each on its own plane). loft_ruled=false → smooth sections, true → ruled.
    // Reuses mode (boolean) and target_body.
    std::vector<int> loft_profile_refs;    // ordered indices into features[] of profile Sketches
    // Parallel to loft_profile_refs: the entities (indices into that sketch) of the ONE closed loop the profile is. Empty, or a
    // missing entry = the whole sketch (it must then be one closed shape). A sketch is not a profile; a closed loop is.
    std::vector<std::vector<int>> loft_profile_loops;
    // Parallel to loft_profile_refs: a profile that is a FACE of the target body instead of a sketch (its outer boundary, so a loft can start or
    // end on the surface of a body). The face's index in the target body; the entry in loft_profile_refs is then -1. Empty / -1 = a sketch.
    std::vector<int> loft_profile_faces;
    bool        loft_ruled{false};

    // Pattern: replicate the target body, copies fused into it. pattern_circular=false
    // → linear (pattern_count instances spaced pattern_spacing along plane axis
    // pattern_dir: 0=X, 1=Y); true → circular (pattern_count instances over
    // pattern_angle° total about the plane normal through the plane origin, so a seed
    // offset from the origin orbits the axis). Reuses target_body + plane.
    bool        pattern_circular{false};
    int         pattern_count{3};          // total instances incl. the seed (>=1)
    double      pattern_spacing{20};       // linear step (mm)
    int         pattern_dir{0};            // linear direction: 0 = plane X, 1 = plane Y
    double      pattern_angle{360};        // circular total angle (degrees)
    // How a circular pattern spreads its copies over pattern_angle. true (every pattern made
    // since): a full turn is split into `count` equal steps, anything less is spanned end to end,
    // first copy at 0 and last at pattern_angle — the way a pattern along a curve spans its
    // curve. false (older recipes, kept as they were built): always angle / count, so 90° with 3
    // copies stopped at 60°.
    bool        pattern_inclusive{false};
    // FEATURE pattern: replicate what these earlier features did to the body (the material they took away and the material they
    // added), instead of copying the whole body. Linear = `pattern_count` x `pattern_spacing` along pattern_axis, optionally a
    // second grid direction; circular (pattern_circular) = pattern_count about the line through pattern_origin along pattern_axis.
    std::vector<int> pattern_features;
    Vec3d       pattern_axis{1, 0, 0};
    Vec3d       pattern_origin{0, 0, 0};
    Vec3d       pattern_axis2{0, 1, 0};
    int         pattern_count2{1};
    double      pattern_spacing2{0};
    // Shell: further faces to open besides shell_face. Draft: further faces to taper besides draft_face, the pull direction
    // and the neutral plane (a point on it; the plane is perpendicular to the pull). Without draft_has_neutral the pull is +Z and
    // the neutral plane is the body's lowest Z.
    std::vector<int> shell_faces;
    std::vector<int> draft_faces;
    bool        draft_has_neutral{false};
    Vec3d       draft_pull{0, 0, 1};
    Vec3d       draft_neutral{0, 0, 0};
    // Thin extrude: the profile is a wall around the sketch's outline instead of its filled area - thin_in thick inside the
    // outline, thin_out outside it (0 = none on that side; both 0 = a normal extrude).
    double      thin_in{0};
    double      thin_out{0};
    // Feature "pattern" that mirrors instead: one copy of what the features did, reflected in the plane through pattern_origin
    // with normal pattern_axis.
    bool        pattern_mirror{false};
    // Extrude of SOME of the sketch's loops: indices into the sketch's entities. Empty = all of them, as one region (the GUI's
    // way: disjoint regions are refused). Set, the chosen loops become separate regions (nested loops are holes) and each is
    // extruded - one feature, several islands.
    std::vector<int> extrude_entities;

    // Pattern along a curve: when pattern_curve_sketch >= 0 this mode takes precedence over
    // linear/circular. Copies are placed at equal-parameter points along entity
    // pattern_curve_entity of sketch pattern_curve_sketch, translated by (P_i - P_0).
    int pattern_curve_sketch{-1};  // feature index of the Sketch holding the guide curve
    int pattern_curve_entity{-1};  // entity index of the guide curve within that sketch

    // Parametric bindings: field-member-name -> expression string. On recompute() each entry
    // is evaluated against the document variables and written into the named numeric field
    // BEFORE geometry runs. Empty (the common case) means the feature uses its literal fields.
    std::map<std::string, std::string> expr;

    // Transient, never serialized. A body is referred to above by its INDEX, and an index is
    // only meaningful against the history it was taken in: delete, reorder or disable a
    // feature that makes a body and every later index points at a different body. So recompute
    // records, for each body field, the identity of the body it resolved to — the feature that
    // made it, and which of that feature's bodies it is — and when the history changes the
    // fields are re-pointed from those identities (see CadDocument::recompute).
    struct BodyId { int src{-1}; int ord{0}; };
    std::vector<BodyId> body_ref_ids;
    bool                body_refs_pending{false};

    // Datum/reference plane: a derived SketchPlane the document offers as a selectable
    // sketch plane (no solid). plane_base selects the reference (0=XY,1=XZ,2=YZ, or 3+N
    // = the Nth earlier datum plane); plane_offset shifts along the base normal;
    // plane_angle tilts plane_angle° about the base axis plane_axis (0=base X, 1=base Y).
    int         plane_base{0};
    double      plane_offset{20};
    double      plane_angle_tilt{0};       // degrees (named *_tilt to avoid clash w/ revolve)
    int         plane_axis{0};             // tilt axis: 0 = base X, 1 = base Y
    PlaneType   plane_type{PlaneType::Offset};
    int         plane_face_body{-1};
    int         plane_face{-1};
    int         plane_face2_body{-1};
    int         plane_face2{-1};
    int         plane_edge_body{-1};
    int         plane_edge{-1};
    int         plane_edge2_body{-1};
    int         plane_edge2{-1};
    double      plane_u_size{60};
    double      plane_v_size{60};

    // Boolean: combine two EXISTING bodies. `mode` reuses BooleanMode (Add = union,
    // Cut = subtract tool from target, Intersect = keep overlap; New unused). `target_body`
    // is the body that survives (result written back to it); `bool_tool_body` is the other
    // operand, consumed (erased) unless `bool_keep_tool`. `bool_tolerance` = OCCT fuzzy value
    // (0 = exact). Per-face merge: when both bool_target_face/bool_tool_face are set, the tool
    // is first snapped so those two faces are coincident (gap closed within bool_tolerance),
    // then the boolean welds them and coplanar faces are unified into one clean face.
    int    bool_tool_body{-1};
    bool   bool_keep_tool{false};
    double bool_tolerance{0.0};
    int    bool_target_face{-1};   // global face id on the target body to mate (-1 = none)
    int    bool_tool_face{-1};     // global face id on the tool body to mate (-1 = none)

    // Cut: split one target body with a plane, keeping the upper half, lower half, or both.
    // Reuses `plane` for the cut plane and `target_body` for which body is cut.
    double cut_offset{0.0};       // offset along the cut-plane normal (mm)
    bool   cut_flip{false};       // flip the normal => swaps which side is "upper"
    bool   cut_keep_upper{true};  // keep the +normal half
    bool   cut_keep_lower{false}; // keep the -normal half (both => split into two bodies)

    // Mirror: reflect a body about a plane. Reuses `plane` (mirror plane, as Cut does),
    // `target_body` (body to mirror), and `mode` (New = separate mirrored copy,
    // Add = fuse the mirror back into the source). mirror_keep_original decides whether
    // the source body survives when mode is New.
    bool   mirror_keep_original{true};

    // Datum axis: reference line (no solid). Construction params stored; resolve_datum_axes()
    // computes the world-space origin + unit direction on demand.
    AxisType    axis_type{AxisType::TwoPoints};
    Vec3d       axis_p1{0, 0, 0};
    Vec3d       axis_p2{0, 0, 10};
    int         axis_body{-1};
    int         axis_face{-1};
    int         axis_edge{-1};
    int         axis_plane_a{-1};
    int         axis_plane_b{-1};

    // Datum coordinate system (no solid). Stored as point + two orthonormal axes.
    CoordSysType coordsys_type{CoordSysType::PointWorld};
    Vec3d        coordsys_point{0, 0, 0};
    int          coordsys_body{-1};
    int          coordsys_face{-1};
    int          coordsys_edge{-1};
    Vec3d        coordsys_x_hint{1, 0, 0};

    // Fingerprint of the face this connector was bound to, for drift detection. -1 = not yet
    // recorded (an old recipe, or a connector that has never resolved).
    //
    // Surface TYPE and EDGE COUNT specifically, because they survive every legitimate edit:
    // Transform moves the body, Draft tilts the face, a dimension change resizes it, and none
    // of those change either value. Centroid, area and normal all fail that test — see the
    // issue. The cost is that a slide from one planar 4-edge face to another planar 4-edge face
    // is invisible; a detector that never cries wolf is worth more here than a total one.
    int coordsys_face_kind{-1};    // GeomAbs_SurfaceType as int
    int coordsys_face_edges{-1};   // number of edges bounding the face

    // Helix curve params (consumed as a sweep path to build springs/coils/augers).
    // Axis = plane normal through plane origin. pitch = axial rise per full turn.
    // left_handed flips the winding direction. taper_deg != 0 gives a conical helix.
    double      helix_radius{10};
    double      helix_pitch{5};
    double      helix_height{20};
    bool        helix_left_handed{false};
    double      helix_taper_deg{0};

    // Transform feature: rigid move/rotate of an existing body. Rotation is applied
    // first (about xf_axis through xf_pivot), then the translation.
    Vec3d       xf_translate{0, 0, 0};
    Vec3d       xf_axis{0, 0, 1};
    Vec3d       xf_pivot{0, 0, 0};
    double      xf_angle_deg{0};
    bool        xf_copy{false};      // true: keep the original, append the moved copy as a new body

    // Thicken feature: offset one face of an existing body into a new thin solid body.
    // The face belongs to `target_body`; the offset runs along the face normal.
    int         thicken_face{-1};        // global face id on the target body; -1 = invalid
    double      thicken_thickness{2};    // wall thickness (always used as |value|)
    bool        thicken_flip{false};     // true: offset against the face normal

    // Cut-by-face: when cut_face >= 0, apply_cut derives the cut plane from this face
    // (via SketchPlane::from_face) instead of the base `plane`. cut_offset / cut_flip
    // still apply along the derived normal.
    int         cut_face_body{-1};   // body owning the face; -1 = the target body
    int         cut_face{-1};        // global face id to cut along; -1 = use `plane`

    // Project feature: convert edges of an existing solid into sketch entities on `plane`.
    int              project_source_body{-1};   // body owning the edges; -1 = last body
    std::vector<int> project_edges;             // global edge ids to project; empty => use project_face
    int              project_face{-1};          // if project_edges empty, project every edge of this face
    // "Use": the source is another SKETCH (any plane, incl. a parallel one), not a body. The link is
    // persistent: entities are re-projected on every recompute, so editing the parent moves them,
    // and a parent that is gone is an Error, never a silent keep-the-old-geometry.
    // Sketch3D: a free 3D polyline / spline through `sk3_points` (world mm). It makes no solid; it is a
    // PATH (Sweep) and, later, a loft guide. `smooth` interpolates a C2 spline instead of straight legs.
    std::vector<Vec3d> sk3_points;
    bool               sk3_closed{false};
    bool               sk3_smooth{false};

    // Face attachment of a sketch: the body and the (global) face id the sketch was drawn on. recompute() follows that
    // face through upstream edits (TopoRemap) and carries the sketch plane with it; -1 = a free plane.
    int                attach_body{-1};
    int                attach_face{-1};
    // A sketch drawn on a datum Plane: the index of that Plane feature. recompute() seats the sketch on the plane as it is
    // now, so a plane whose offset is bound to a variable takes the sketch (and what was extruded from it) along. -1 = none.
    int                attach_datum{-1};
    // Names a caller gave to sketch dimensions: entity_constraints index -> name. set_feature_expr takes "dim:<name>" for
    // "dim:<index>", so a binding does not depend on remembering a number. Indices are positions: a constraint deleted
    // from the sketch shifts those after it, and the names stay where they were put.
    std::map<int, std::string> dim_names;

    bool             project_from_sketch{false};
    int              project_source_sketch{-1}; // feature index; remapped on delete (orphan -> -1 = Error)
    std::vector<int> project_sketch_entities;   // entity indices of the source; empty = all
    // Loft end conditions for the FIRST / LAST profile: 0 = Contact (the surface just starts at the
    // profile), 1 = Normal to profile (it leaves along the profile's plane normal). Ignored for a ruled
    // loft, whose straight sections have no end condition to give.
    int loft_start_cond{0};
    int loft_end_cond{0};

    // Direct edit: faces to remove (global face indices into target_body's shape),
    // healed via BRepAlgoAPI_Defeaturing.
    std::vector<int> delete_faces;

    // Rib: a thin wall grown from an open sketch line, fused to the body.
    int    rib_sketch_ref{-1};  // feature index of the Sketch holding the profile
    int    rib_entity{-1};      // index of the open Line entity within that sketch
    double rib_thickness{2};    // wall thickness (mm), centred on the line
    double rib_depth{10};       // extrude distance along the sketch-plane normal (mm)

    // --- Mate (assembly) ---
    // 0 Fastened   — all 6 DOF fixed: B's frame is driven onto A's exactly.
    // 1 Planar     — z axes aligned, normal distance set to mate_offset; in-plane position free.
    // 2 Revolute   — axes collinear, position on the axis fixed; rotation about z free.
    // 3 Slider     — orientation fully fixed, perpendicular position fixed; axial slide free.
    // 4 Cylindrical— axes collinear, perpendicular fixed; both spin and axial slide free.
    // A "free" DOF is preserved from the body's current placement, not zeroed.
    int    mate_kind{0};
    int    mate_cs_a{-1};       // feature index of the FIXED CoordSys (mate connector A)
    int    mate_cs_b{-1};       // feature index of the CoordSys on the body that MOVES
    double mate_offset{0};      // translation along A's z, mm
    double mate_angle{0};       // rotation about A's z, degrees
    bool   mate_flip{false};    // oppose the two z axes (face-to-face)

    // Sheet metal. An SmRule feature sets the rule every later sheet-metal feature uses (bind_sheet_rules copies it into
    // them on each recompute). `distance` is the flange height / contour width / hem length, `flip` the side, dressup_edges
    // the edges, sketch_ref the base or contour sketch.
    double sm_thickness{2.5};
    double sm_radius{-1};     // inner bend radius; <= 0 = the thickness
    double sm_k{0.44};        // K-factor (unfold)
    double sm_gap{0.01};      // hem gap
    double sm_angle{90};      // edge flange angle (deg)
    int    sm_ref{-1};        // refold: the unfold feature it folds back
    bool   dressup_fit_taper{false};   // fillet: where the radius does not fit, follow the room (GeometryEngine::FilletLaw::fit_taper)

    // LEGACY positional list (recipe v5/v6), FROZEN: load() reads old projects, save() stays as the reference the
    // coverage test compares the v7 groups against,. New fields
    // go ONLY into CadFeatureFields.hpp (which also lists the index references the remap follows), never here.
    template<class Archive>
    void save(Archive& ar) const {
        std::string brep = (type == CadFeatureType::Import) ? brep_to_string(imported_solid) : std::string();
        ar(type, name, enabled, shape, plane, width, height, radius,
           profile, entities, constraints, entity_constraints, imported_regions,
           import_offset, import_scale_x, import_scale_y, import_on_face, import_face_body,
           sketch_ref, distance, symmetric, mode, extrude_end, distance2, taper_deg, flip,
           up_to_face, extrude_src_face, up_to_point, target_body,
           dressup_size, face_group, dressup_edge,
           hole_diameter, hole_depth, hole_through, hole_x, hole_y,
           thread_radius, thread_pitch, thread_height, thread_depth, thread_internal, thread_x, thread_y,
           shell_thickness, shell_face,
           draft_face, draft_angle,
           revolve_angle, revolve_axis,
           sweep_path_ref, loft_profile_refs, loft_ruled,
           pattern_circular, pattern_count, pattern_spacing, pattern_dir, pattern_angle,
           plane_base, plane_offset, plane_angle_tilt, plane_axis,
           bool_tool_body, bool_keep_tool, bool_tolerance, bool_target_face, bool_tool_face,
           cut_offset, cut_flip, cut_keep_upper, cut_keep_lower,
           brep,
           plane_type, plane_face_body, plane_face, plane_face2_body, plane_face2,
           plane_edge_body, plane_edge, plane_edge2_body, plane_edge2, plane_u_size, plane_v_size,
           mirror_keep_original,
           axis_type, axis_p1, axis_p2, axis_body, axis_face, axis_edge, axis_plane_a, axis_plane_b,
            coordsys_type, coordsys_point, coordsys_body, coordsys_face, coordsys_edge, coordsys_x_hint,
            helix_radius, helix_pitch, helix_height, helix_left_handed, helix_taper_deg,
             xf_translate, xf_axis, xf_pivot, xf_angle_deg, xf_copy,
             thicken_face, thicken_thickness, thicken_flip,
             cut_face_body, cut_face,
             project_source_body, project_edges, project_face,
             delete_faces,
             hole_style, hole_cbore_diameter, hole_cbore_depth,
             hole_csink_diameter, hole_csink_angle, hole_standard,
              rib_sketch_ref, rib_entity, rib_thickness, rib_depth,
               pattern_curve_sketch, pattern_curve_entity,
               expr,
               mate_kind, mate_cs_a, mate_cs_b, mate_offset, mate_angle, mate_flip,
               coordsys_face_kind, coordsys_face_edges,
               thread_major_nominal, pattern_inclusive,
               dressup_edges,
               text_string, text_font, text_height,
               revolve_axis_entity,
               project_from_sketch, project_source_sketch, project_sketch_entities,
               loft_start_cond, loft_end_cond,
               sk3_points, sk3_closed, sk3_smooth,
               attach_body, attach_face,
               dressup_size2, dressup_chord, dressup_has_start, dressup_start,
               dressup_radii, dressup_knots,
               pattern_features, pattern_axis, pattern_origin, pattern_axis2, pattern_count2, pattern_spacing2,
               shell_faces, draft_faces, draft_has_neutral, draft_pull, draft_neutral,
               thin_in, thin_out, pattern_mirror, extrude_entities,
               hole_tip_angle,
               sm_thickness, sm_radius, sm_k, sm_gap, sm_angle, sm_ref,
               dressup_fit_taper, attach_datum, dim_names);
    }
    template<class Archive>
    void load(Archive& ar) {
        std::string brep;
        ar(type, name, enabled, shape, plane, width, height, radius,
           profile, entities, constraints, entity_constraints, imported_regions,
           import_offset, import_scale_x, import_scale_y, import_on_face, import_face_body,
           sketch_ref, distance, symmetric, mode, extrude_end, distance2, taper_deg, flip,
           up_to_face, extrude_src_face, up_to_point, target_body,
           dressup_size, face_group, dressup_edge,
           hole_diameter, hole_depth, hole_through, hole_x, hole_y,
           thread_radius, thread_pitch, thread_height, thread_depth, thread_internal, thread_x, thread_y,
           shell_thickness, shell_face,
           draft_face, draft_angle,
           revolve_angle, revolve_axis,
           sweep_path_ref, loft_profile_refs, loft_ruled,
           pattern_circular, pattern_count, pattern_spacing, pattern_dir, pattern_angle,
           plane_base, plane_offset, plane_angle_tilt, plane_axis,
           bool_tool_body, bool_keep_tool, bool_tolerance, bool_target_face, bool_tool_face,
           cut_offset, cut_flip, cut_keep_upper, cut_keep_lower,
           brep);
        // A recipe saved before the fields below ends early and `ar` throws there: the body must already be set by then
        imported_solid = brep_from_string(brep);
        ar(
           plane_type, plane_face_body, plane_face, plane_face2_body, plane_face2,
           plane_edge_body, plane_edge, plane_edge2_body, plane_edge2, plane_u_size, plane_v_size,
           mirror_keep_original,
           axis_type, axis_p1, axis_p2, axis_body, axis_face, axis_edge, axis_plane_a, axis_plane_b,
           coordsys_type, coordsys_point, coordsys_body, coordsys_face, coordsys_edge, coordsys_x_hint,
           helix_radius, helix_pitch, helix_height, helix_left_handed, helix_taper_deg,
            xf_translate, xf_axis, xf_pivot, xf_angle_deg, xf_copy,
            thicken_face, thicken_thickness, thicken_flip,
            cut_face_body, cut_face,
             project_source_body, project_edges, project_face,
               delete_faces,
               hole_style, hole_cbore_diameter, hole_cbore_depth,
               hole_csink_diameter, hole_csink_angle, hole_standard,
               rib_sketch_ref, rib_entity, rib_thickness, rib_depth,
                pattern_curve_sketch, pattern_curve_entity,
               expr,
               mate_kind, mate_cs_a, mate_cs_b, mate_offset, mate_angle, mate_flip,
               coordsys_face_kind, coordsys_face_edges,
               thread_major_nominal, pattern_inclusive,
               dressup_edges,
               text_string, text_font, text_height,
               revolve_axis_entity,
               project_from_sketch, project_source_sketch, project_sketch_entities,
               loft_start_cond, loft_end_cond,
               sk3_points, sk3_closed, sk3_smooth,
               attach_body, attach_face,
               dressup_size2, dressup_chord, dressup_has_start, dressup_start,
               dressup_radii, dressup_knots,
               pattern_features, pattern_axis, pattern_origin, pattern_axis2, pattern_count2, pattern_spacing2,
               shell_faces, draft_faces, draft_has_neutral, draft_pull, draft_neutral,
               thin_in, thin_out, pattern_mirror, extrude_entities,
               hole_tip_angle,
               sm_thickness, sm_radius, sm_k, sm_gap, sm_angle, sm_ref,
               dressup_fit_taper, attach_datum, dim_names);
    }
    // The pre-framing (v4) layout, FROZEN. A v4 recipe is one flat stream with no per-feature
    // length, so it can only be read with exactly the field list it was written with; reading it
    // through load() above breaks the moment a field is appended there (every field added since
    // v5 is appended ONLY above, never here).
    template<class Archive>
    void load_flat_v4(Archive& ar) {
        std::string brep;
        ar(type, name, enabled, shape, plane, width, height, radius,
           profile, entities, constraints, entity_constraints, imported_regions,
           import_offset, import_scale_x, import_scale_y, import_on_face, import_face_body,
           sketch_ref, distance, symmetric, mode, extrude_end, distance2, taper_deg, flip,
           up_to_face, extrude_src_face, up_to_point, target_body,
           dressup_size, face_group, dressup_edge,
           hole_diameter, hole_depth, hole_through, hole_x, hole_y,
           thread_radius, thread_pitch, thread_height, thread_depth, thread_internal, thread_x, thread_y,
           shell_thickness, shell_face,
           draft_face, draft_angle,
           revolve_angle, revolve_axis,
           sweep_path_ref, loft_profile_refs, loft_ruled,
           pattern_circular, pattern_count, pattern_spacing, pattern_dir, pattern_angle,
           plane_base, plane_offset, plane_angle_tilt, plane_axis,
           bool_tool_body, bool_keep_tool, bool_tolerance, bool_target_face, bool_tool_face,
           cut_offset, cut_flip, cut_keep_upper, cut_keep_lower,
           brep,
           plane_type, plane_face_body, plane_face, plane_face2_body, plane_face2,
           plane_edge_body, plane_edge, plane_edge2_body, plane_edge2, plane_u_size, plane_v_size,
           mirror_keep_original,
           axis_type, axis_p1, axis_p2, axis_body, axis_face, axis_edge, axis_plane_a, axis_plane_b,
           coordsys_type, coordsys_point, coordsys_body, coordsys_face, coordsys_edge, coordsys_x_hint,
           helix_radius, helix_pitch, helix_height, helix_left_handed, helix_taper_deg,
            xf_translate, xf_axis, xf_pivot, xf_angle_deg, xf_copy,
            thicken_face, thicken_thickness, thicken_flip,
            cut_face_body, cut_face,
             project_source_body, project_edges, project_face,
               delete_faces,
               hole_style, hole_cbore_diameter, hole_cbore_depth,
               hole_csink_diameter, hole_csink_angle, hole_standard,
               rib_sketch_ref, rib_entity, rib_thickness, rib_depth,
                pattern_curve_sketch, pattern_curve_entity,
               expr,
               mate_kind, mate_cs_a, mate_cs_b, mate_offset, mate_angle, mate_flip,
               coordsys_face_kind, coordsys_face_edges);
        imported_solid = brep_from_string(brep);
    }

};

// Serialize a TopoDS_Shape to/from a BRep string for cereal persistence.
std::string brep_to_string(const TopoDS_Shape& s);
TopoDS_Shape brep_from_string(const std::string& d);

// One independent solid in a multi-body document.
struct CadBody {
    TopoDS_Shape shape;
    std::string  name;
    // The name the USER gave this body. A body is NOT its first feature: an Extrude, a Cut and
    // a Fillet all land on the same body, so renaming `source_feature` renames one operation in
    // the history, not the object — which is exactly the bug this field exists to end. `name`
    // above is the DERIVED label (the maker's name, restamped every recompute) and stays that;
    // this one is set only by a rename, carried across recompute() by body index, and written
    // into the recipe so it survives save/load.
    bool         has_user_name{false};
    std::string  user_name;
    // Per-body display colour override (Color tool). When has_color is false the GUI
    // falls back to the auto body-index palette. Carried across recompute() by body index.
    bool         has_color{false};
    ColorRGBA    color;
    // Index into `features` of the feature that CREATED this body, or -1. A body is a
    // recomputed result, so without this there is no way back to its maker and "delete this
    // body" cannot be expressed at all — the GUI could only answer "select the FEATURE that
    // created this body". Stamped in one place, the recompute loop; see the note there for
    // why a single "still unset?" test is sufficient and stays correct for new feature types.
    int          source_feature{-1};
};

// OCCT-only feature tree backing the Design tab. No GUI dependencies (lives in libslic3r).
class CadDocument {
public:
    std::vector<CadFeature> features;
    // Named document variables: name -> expression. Evaluated topologically each recompute();
    // an expression may reference other variables. Feature `expr` bindings resolve against these.
    std::map<std::string, std::string> variables;
    // Can a feature expression drive this numeric field? The single allow-list the evaluator
    // uses, so a caller can refuse a name before it breaks the next recompute.
    static bool is_bindable_field(const std::string& field);
    // Every field name set_feature_expr accepts, sorted (plus "dim:<N>" for a sketch dimension).
    static std::vector<std::string> bindable_fields();
    // Variable housekeeping. rename_variable rewrites every expression that names `from` (token-wise: `w` does not touch
    // `width`) and refuses a name that is taken, malformed or built in. The CSV is "name,expression" per line, quotes as
    // usual; from_csv adds or replaces and returns how many rows it took, naming the first bad row in `err`.
    bool        rename_variable(const std::string& from, const std::string& to, std::string& err);
    std::string variables_csv() const;
    int         variables_from_csv(const std::string& csv, std::string& err);
    // Does a feature of this type leave a body behind? Sketches, datums, the helix curve and
    // Project (which emits sketch entities) do not. The one answer recompute(), the rollback
    // rule and the GUI's preview all use.
    static bool produces_body(CadFeatureType t);
    // Multi-body result of the last replay. A "New" extrude appends a body; other ops
    // mutate a target body. Empty after a failed/empty recompute.
    std::vector<CadBody>    bodies;
    TopoDS_Shape            body;          // compound of all bodies (1 body => that body) — display/compat
    TriangleMesh            display_mesh;      // tessellation of all bodies, concatenated (picking)
    std::vector<TriangleMesh> display_body_meshes; // one mesh per body, in `bodies` order (per-body color)
    std::vector<int>        display_tri_face;  // per-triangle face id WITHIN its source body
    std::vector<int>        display_tri_body;  // per-triangle source body index (into bodies)
    std::string             error;             // last recompute error ("" = ok)

    // Mate diagnostics, refilled by every recompute(). Non-fatal by design: the
    // document still evaluates — this only names what the user should look at.
    // .first = index of the offending Mate feature, .second = human-readable reason.
    // NOT "over-constraint" — this kernel has no solver, so there is no DOF analysis
    // behind these; they are graph facts about which mate drives which body.
    std::vector<std::pair<int, std::string>> mate_conflicts;

    // Non-fatal notes from the last recompute(): a combine mode that could not be honoured and fell back
    // to a new body (Extrude / Revolve / Sweep / Loft). .first = feature index, .second = what happened.
    std::vector<std::pair<int, std::string>> feature_warnings;

    // Modeling origin: the world point the default XY/XZ/YZ planes pass through. The GUI sets this
    // to the bed centre so sketches/datums land in the middle of the bed (not the bed corner =
    // world 0) — for a NEW document. It is saved with the recipe: sketches bake it into their
    // planes while datum planes add it when they are resolved, so a project reopened on another
    // printer must keep the origin it was made with or its datums move and its sketches do not.
    Vec3d modeling_origin{Vec3d::Zero()};
    bool  origin_from_recipe{false};   // modeling_origin came from the loaded project
    // Weld sketch endpoints within kSketchJoinTol (the "Auto-close sketch loops" preference, taken
    // when the document is started). A property of the DOCUMENT, saved with it: as a machine-wide
    // preference it made one project rebuild into a closed solid on one computer and an open
    // loop on another. recompute() pushes it into the kernel.
    bool  auto_close_loops{true};

    // Tessellation quality, matched to Orca's OWN STEP importer (Format/STEP.hpp defaults:
    // linear 0.003, angular 0.5 rad) so a body modelled here reaches the screen at the same
    // density as the identical body imported through Prepare. It was 0.01 linear — 3.3x coarser
    // than anything else in the app, which is why curved faces read as faceted next to an
    // imported part. Angular already matched. Same BRepMesh_IncrementalMesh call, same GLVolume
    // path, same shaders: the renderer was never the difference, the mesh fed to it was.
    double linear_deflection{0.003};
    // Preview ghosts (card open, gizmo drag) are tessellated no finer than this: they are redrawn on
    // every parameter change and replaced by the committed mesh (linear_deflection) on Confirm.
    double preview_linear_deflection{0.05};
    double angular_deflection{0.5};

    int  add_sketch(SketchShape shape, const SketchPlane& plane,
                    double width, double height, double radius,
                    const std::string& name);
    int  add_sketch_profile(const SketchProfile& profile, const SketchPlane& plane,
                            const std::string& name);
    // Onshape-style multi-entity sketch: stores the entity list verbatim. When
    // non-empty it takes precedence over profile/enum in build_sketch_wire.
    int  add_sketch_entities(const std::vector<SketchEntity>& entities,
                             const SketchPlane& plane, const std::string& name,
                             const std::vector<SketchEntityConstraintDef>& constraints = {});
    // Project edges of source_body onto plane, producing a sketch feature whose
    // entities are (re)derived on every recompute.
    int  add_project_edges(int source_body, const std::vector<int>& edge_ids, int face,
                           const SketchPlane& plane, const std::string& name);
    // Onshape's "Use" / SolidWorks' "Convert Entities": project a body's edges onto the plane of
    // an EXISTING sketch feature and append them to that sketch as CONSTRUCTION entities, so new
    // geometry can be constrained to them. Returns the number of entities appended, or -1 if the
    // sketch or body reference is invalid. Unlike add_project_edges this creates no feature: the
    // references become part of the sketch that borrows them.
    // "Use": project another sketch (all its entities, or the listed ones) onto `plane`, parent-linked.
    // 3D sketch: points in world coordinates; consumed as a Sweep path.
    int  add_sketch3d(const std::vector<Vec3d>& points, bool smooth, bool closed, const std::string& name);
    // The Sketch3D feature as one wire (polyline, or an interpolated spline). Null + `err` when it
    // cannot be built: fewer than 2 distinct points, or a spline OCCT refuses.
    static TopoDS_Wire build_sketch3d_wire(const CadFeature& f, std::string& err);
    int  add_use_sketch(int source_sketch, const std::vector<int>& entities,
                        const SketchPlane& plane, const std::string& name);
    // The edges of every body that lie in `plane` (all their points within `tol` of it), as 2D CONSTRUCTION entities in that plane's
    // coordinates: what a sketch drawn on a face needs to include of the model it sits on. Edges that only cross the plane are not
    // included (they would be points); use Project for a view along the normal.
    std::vector<SketchEntity> in_plane_edges(const SketchPlane& plane, double tol = 1e-4) const;
    int  project_edges_into_sketch(int sketch_feature, int source_body,
                                   const std::vector<int>& edge_ids, int face);
    // Append a bridging BSpline entity connecting endpoint `end_a` of entity `ent_a` to
    // endpoint `end_b` of entity `ent_b`, both within sketch feature `sketch_ref`. Returns
    // the new entity's index within that sketch's entities vector. Non-parametric: computed
    // once from the current endpoints (does not auto-follow later solver moves).
    int add_bridge(int sketch_ref, int ent_a, int end_a, int ent_b, int end_b,
                   const std::string& name);
    // Solve features[index]'s sketch constraints, writing solved coordinates back
    // into its profile.points. No-op (returns true) if the feature has no
    // constraints. Returns false if index is invalid / not a Sketch / solve fails.
    bool solve_sketch_feature(int index);
    int  add_extrude(int sketch_ref, double distance, bool symmetric,
                     BooleanMode mode, const std::string& name);
    // Extrude a single loop given directly as entities (sketch_ref = -1, plane carried).
    int  add_extrude_entities(const std::vector<SketchEntity>& entities,
                              const SketchPlane& plane, double distance, bool symmetric,
                              BooleanMode mode, const std::string& name);
    // Extrude an existing solid FACE (global face id on the body) as the profile.
    int  add_extrude_face(int src_face, double distance, bool symmetric,
                          BooleanMode mode, const std::string& name);
    int  add_fillet(double radius, FaceGroup faces, const std::string& name);
    int  add_fillet(double radius, int edge_id, const std::string& name);
    int  add_fillet(double radius, const std::vector<int>& edge_ids, const std::string& name);
    int  add_chamfer(double distance, FaceGroup faces, const std::string& name);
    int  add_chamfer(double distance, int edge_id, const std::string& name);
    int  add_chamfer(double distance, const std::vector<int>& edge_ids, const std::string& name);
    int  add_hole(double diameter, double depth, bool through,
                  double x, double y, const SketchPlane& plane,
                  const std::string& name);
    int  add_hole_styled(double diameter, double depth, bool through,
                         double x, double y, const SketchPlane& plane, int style,
                         double cbore_diameter, double cbore_depth,
                         double csink_diameter, double csink_angle,
                         const std::string& standard, const std::string& name);
    int  add_hole_standard(const std::string& designation, int style, bool through,
                           double depth, double x, double y,
                           const SketchPlane& plane, const std::string& name);
    // Sheet metal (see SheetMetal.hpp). Each add_sm_* copies the rule in force so a lone candidate previews right.
    int  add_sm_rule(double thickness, double bend_radius, double k_factor, double hem_gap, const std::string& name);
    int  add_sm_base(int sketch_ref, bool flip, const std::string& name);
    int  add_sm_edge_flange(const std::vector<int>& edges, double height, double angle_deg, bool flip, const std::string& name);
    int  add_sm_contour_flange(int sketch_ref, double width, bool flip, const std::string& name);
    int  add_sm_hem(int edge, double length, const std::string& name);
    int  add_sm_unfold(const std::string& name);
    int  add_sm_refold(int unfold_feature, const std::string& name);
    int  add_thread(double radius, double pitch, double height, double depth,
                    bool internal, double x, double y, const SketchPlane& plane,
                    const std::string& name);
    int  add_revolve(int sketch_ref, double angle, int axis, bool flip,
                     BooleanMode mode, const std::string& name);
    // Self-contained revolve of a single loop given directly as entities (sketch_ref=-1).
    int  add_revolve_entities(const std::vector<SketchEntity>& entities,
                              const SketchPlane& plane, double angle, int axis, bool flip,
                              BooleanMode mode, const std::string& name);
    // Sweep the profile Sketch (profile_sketch_ref) along the path Sketch (path_sketch_ref).
    int  add_pattern(bool circular, int count, double spacing, int dir,
                     double angle_deg, int target_body, const std::string& name);
    // Pattern `count` copies of `target` along entity `curve_entity` of sketch `curve_sketch`.
    int  add_pattern_on_curve(int count, int curve_sketch, int curve_entity, int target,
                              const std::string& name);
    int  add_sweep(int profile_sketch_ref, int path_sketch_ref, BooleanMode mode,
                   const std::string& name);
    // Loft through the ordered profile Sketches (each a closed wire on its own plane).
    int  add_loft(const std::vector<int>& profile_refs, bool ruled, BooleanMode mode,
                  const std::string& name);
    // Skin 2+ profile sketches open (no end caps) -> a sheet body.
    int  add_surface_loft(const std::vector<int>& profile_refs, bool ruled, const std::string& name);
    // The closed loops of a sketch, each as the indices of its entities. The unit a Loft profile is picked in.
    static std::vector<std::vector<int>> sketch_closed_loops(const CadFeature& sketch);
    // Fill sketch sketch_ref's closed boundary wire with a smooth face -> a one-face sheet body.
    int  add_surface_fill(int sketch_ref, const std::string& name);
    int  add_shell(double thickness, int face, int target_body, const std::string& name);
    // Grow a thin rib wall (thickness, depth) from the open Line entity `entity` inside sketch
    // feature `sketch_ref`, fused to `target_body`. Returns the new feature index.
    int  add_rib(int sketch_ref, int entity, double thickness, double depth,
                 int target_body, const std::string& name);
    int  add_draft(double angle, int face, int target_body, const std::string& name);
    // Boolean between two existing bodies. op reuses BooleanMode (Add=union, Cut=subtract,
    // Intersect=common; New invalid). target survives, tool is consumed unless keep_tool.
    // tolerance = OCCT fuzzy value; target_face/tool_face (-1 = none) drive the per-face snap+merge.
    int  add_boolean(BooleanMode op, int target_body, int tool_body, bool keep_tool,
                     double tolerance, int target_face, int tool_face, const std::string& name);
    // Plane Cut (Onshape split-by-plane): trim target_body by the plane (origin offset along
    // its normal by `offset`, normal flipped iff `flip`). keep_upper/keep_lower select the
    // +normal / -normal half; both => the body is split into two coexisting bodies.
    int  add_cut(const SketchPlane& plane, double offset, bool flip,
                  bool keep_upper, bool keep_lower, int target_body, const std::string& name);
    // Split target_body along the plane of face `face` (owned by face_body, -1 = target).
    // keep_upper/keep_lower select which half survives; both => split into two bodies.
    int add_split_by_face(int target_body, int face_body, int face,
                          bool keep_upper, bool keep_lower, const std::string& name);
    int  add_mirror(const SketchPlane& plane, int target_body, BooleanMode mode,
                    const std::string& name);
    // Rigid body transform: rotate `angle_deg` about `axis` through `pivot`, then translate.
    // copy=true keeps the source body and appends the transformed one as a new body.
    int add_transform(int target_body, const Vec3d& translate, const Vec3d& axis,
                      const Vec3d& pivot, double angle_deg, bool copy, const std::string& name);
    // Offset face `face` of `target_body` by `thickness` along its normal, producing a new
    // thin solid appended as a new body. flip=true offsets against the normal.
    int add_thicken(int target_body, int face, double thickness, bool flip, const std::string& name);
    // Thicken an entire SHEET body's shell into a solid.
    int add_thicken_surface(int target_body, double thickness, bool flip, const std::string& name);
    // Offset a SHEET body's shell by a signed distance, producing another SHEET body.
    int add_surface_offset(int target_body, double offset, const std::string& name);
    int add_delete_face(int target_body, const std::vector<int>& faces,
                        const std::string& name);
    int add_surface_extrude(int sketch_ref, double distance, const std::string& name);
    int add_surface_revolve(int sketch_ref, double angle_deg, int axis, const std::string& name);
    // Datum plane: derived from base (0=XY/1=XZ/2=YZ/3+N=Nth earlier datum), offset
    // along its normal, optional tilt about a base axis. Produces no solid.
    int  add_plane(int base, double offset, double angle_tilt, int axis,
                   const std::string& name);
    // Datum axis: construction method axis_type determines which ref fields are read.
    int  add_axis(AxisType axis_type, const std::string& name);
    // Datum coordinate system.
    int  add_coordsys(CoordSysType type, const Vec3d& point, const std::string& name);
    int  add_mate(int kind, int cs_a, int cs_b, double offset, double angle_deg, bool flip,
                  const std::string& name);

    // Which mate types apply to a connector pair, as reported to the viewport palette.
    struct MateOption {
        int         kind{0};        // 0..4, the five mate types in CadDocument.hpp:308-314
        bool        viable{true};
        std::string reason;         // empty when viable; why not, when not
    };
    // ALWAYS all five entries, ALWAYS in kind order. Never filtered: the caller dims what is
    // not viable rather than hiding it, so the list must be stable in length and order between
    // calls. Pure query over existing data — records nothing, mutates nothing.
    std::vector<MateOption> mate_options(int cs_a, int cs_b) const;

    int  add_helix(const SketchPlane& plane, double radius, double pitch, double height,
                   bool left_handed, double taper_deg, const std::string& name);
    // Build the helix wire from a Helix feature's params (exposed for tests).
    TopoDS_Wire build_helix_wire(const CadFeature& f, std::string& err) const;
    // Every datum plane currently in the recipe, in feature order, as (name, plane).
    // Used by the GUI to populate plane pickers (after the 3 base planes).
    std::vector<std::pair<std::string, SketchPlane>> resolve_datum_planes() const;

    // World-space sketch plane lying on a body's PLANAR face, so a face picked in the viewport can
    // be sketched on directly — no datum plane in between and nothing to choose from a list.
    // Returns false when the indices don't resolve or the face isn't planar (a cylinder or a fillet
    // has no single plane, and guessing one from a mid-parameter normal would silently sketch on a
    // tangent). Same derivation the Coincident datum method uses, shared so the two cannot drift.
    bool plane_of_face(int body_idx, int face_idx, SketchPlane& out) const;
    // Where feature `fi`, a sketch drawn on a body's face, is SHOWN: its plane carried through the moves (non-copy Transforms of that
    // body) that come after it in the history. The feature itself is not rewritten: undo the move and it is back. Same plane as
    // `plane` when the option is off or nothing moved it.
    SketchPlane carried_plane(int fi) const;
    // Resolved datum axes in feature order. axis_err is non-empty if construction failed.
    struct DatumAxis { std::string name; Vec3d origin{0,0,0}; Vec3d direction{0,0,1};
                       std::string error; };
    std::vector<DatumAxis> resolve_datum_axes() const;
    // Resolved datum coordinate systems. X/Y unit, orthonormal (Z = X.cross(Y)).
    struct DatumCoordSys { std::string name; Vec3d origin{0,0,0}; Vec3d x{1,0,0};
                           Vec3d y{0,1,0}; std::string error; };
    std::vector<DatumCoordSys> resolve_datum_coordsys() const;
    void clear();
    bool recompute();   // replay features -> body + display_mesh; false on error

    // CadRecipe serialization contract:
    // - v1 blobs are deliberately not loadable; there is no migration path by design
    // - append fields ONLY at the end of save/load, never reorder (golden fixture enforces this)
    // Bumped every time the bodies are rebuilt, i.e. every time the face and edge MAPS change.
    // Global face/edge ids are indices into TopExp::MapShapes and mean nothing across a rebuild,
    // so any caller holding an id from an earlier state is holding a wrong one. This is the
    // handle that lets it find out instead of silently addressing the wrong edge.
    //
    // Session-scoped and deliberately NOT serialized: an id is only meaningful within the run
    // that produced it, so persisting the counter would imply a promise across loads that the
    // ids themselves cannot keep.
    uint64_t topo_generation{1};

    // Incremental recompute. Each feature's result is cached under a CHAIN fingerprint (hash of every feature up to
    // and including it, plus the document settings that change a build), so a tail edit replays only the tail and
    // setting a parameter back restores the earlier state without any OCCT work. The cache is shared between copies of
    // the document (a preview candidate reuses the prefix of the document it was copied from) and is thread safe.
    struct ReplayStats {
        int replayed{0};                  // features rebuilt by the last recompute()
        int restored{0};                  // features taken from the cache
        std::vector<double> feature_ms;   // per feature, 0 for a restored or non-solid one
        double mesh_ms{0};                // tessellating the finished bodies: paid on every change, whatever was cached
    } replay_stats;
    bool incremental{true};
    // The bodies as each feature last SAW them, keyed by the feature's own bytes: what its face/edge ids were written
    // against. When an upstream edit rebuilds those bodies the ids are carried to where their entities went (TopoRemap).
    std::map<uint64_t, std::vector<CadBody>> m_seen_by_feature;
    bool m_datum_redo{false};   // recompute() is running its second pass, for sketches on a datum plane that moved with the new bodies
    bool remap_refs{true};
    // A sketch, hole or thread drawn on a face goes with that face when the face only MOVED (its body was moved or turned),
    // not just along the face's normal. False keeps the old rule: it stays at its place in the face's plane.
    bool attached_follow_moves{true};   // (also: carried_plane shows an attached sketch where its body was moved to)
    struct ReplayCache;
    std::shared_ptr<ReplayCache> m_replay_cache;

    // v5: every feature is length-framed, so a reader can stop early on an older file and skip
    // the tail of a newer one. This is the LAST version that has to break anything — from here a
    // new field only needs appending to save/load, with no bump and no orphaned projects.
    // v6: no wire-format change — the bytes are v5's, and both are read by the same framed path.
    // The stamp advances only to put a project-container change on the record; the 3MF backends
    // own that story. v4 still opens through the pre-framing flat path.
    // History rollback (the feature tree's draggable bar). Session-only, never serialized:
    // features AFTER this index are not replayed, so the model shows the state at that point in
    // the history while every feature stays in the recipe. -1 = no rollback. Any mutation
    // (checkpoint()) and undo/redo return to the end, so a new feature never lands mid-history.
    int rollback_index{-1};
    // New features go in at the rollback bar, not at the end: with the history rolled back to feature k, a feature added now
    // becomes k + 1 and the bar moves down onto it (what lies below stays unbuilt). `last_added` is where the last one landed.
    int last_added{-1};
    int append_feature(const CadFeature& f);
    // Copies of features, in order, added at the bar. Each item is {index it had, the feature}; a reference to another copied
    // feature follows to its copy, any other reference stays where it was. Returns the index of the first copy (-1: nothing).
    int paste_features(const std::vector<std::pair<int, CadFeature>>& items);
    // The feature whose replay threw in the last recompute(), or -1. Lets the tree mark the row
    // that failed instead of leaving the user a global message and no suspect.
    int error_feature{-1};
    static constexpr uint32_t ORCA_CAD_RECIPE_VERSION = 7;
    std::string serialize_recipe() const;
    bool deserialize_recipe(const std::string& blob);

    // Export every body to a STEP file as native B-rep (not mesh). body_xforms is the
    // per-body display transform (Move gizmo); when supplied the bodies are written at
    // those positions so the STEP matches what Commit ships. false + err on failure.
    // The bodies as one file: "iges" (B-rep), "stl" (binary) or "obj" (tessellated). STEP stays export_step.
    bool export_other(const std::string& format, const std::string& path, const std::vector<Transform3d>& body_xforms, std::string& err) const;
    bool export_step(const std::string& path,
                     const std::vector<Transform3d>& body_xforms,
                     std::string& err) const;

    GeometryEngine::MassProps body_mass_properties(int body_index) const;

    // One overlapping pair of solid bodies. Indices are into `bodies`, a_ < b_.
    struct Interference { int body_a{-1}; int body_b{-1}; double volume{0}; };
    // Every pair of solid bodies whose intersection encloses more than min_volume (mm^3).
    // Reports only — mutates nothing, so mates and placements are unaffected by calling it.
    // Sheet bodies are skipped: an intersection involving one encloses no volume.
    std::vector<Interference> check_interference(double min_volume = 1e-6) const;

    // ponytail: derived from the OCCT shape type; no stored flag, bodies aren't serialized anyway.
    static bool is_sheet_shape(const TopoDS_Shape& s); // true if TopExp finds no TopAbs_SOLID

    // Undo/redo of the feature recipe (Onshape-style Ctrl+Z). The caller marks a
    // user-action boundary by calling checkpoint() BEFORE the mutation(s) for that
    // action (add/delete/move/replace, or a direct features edit). undo()/redo() then
    // restore the snapshot and recompute(). Because everything else (bodies/meshes/
    // body) is derived by recompute(), snapshotting `features` alone is a complete,
    // exact history; one checkpoint == one Ctrl+Z step.
    void checkpoint();   // snapshot `features` for undo + invalidate redo
    // Drop the most recent checkpoint. For a mutation that took a checkpoint, then failed
    // and restored the pre-mutation state itself (the constraint paths reject an
    // over-constrained addition this way): the snapshot now describes a state identical to
    // the current one, and leaving it turns the next Ctrl+Z into a press that does nothing.
    void abandon_checkpoint();
    bool can_undo() const { return !m_undo.empty(); }
    bool can_redo() const { return !m_redo.empty(); }
    size_t undo_depth() const { return m_undo.size(); }
    void   drop_undo_to(size_t depth) { while (m_undo.size() > depth) m_undo.pop_back(); }   // a rolled-back batch leaves no steps behind
    size_t redo_depth() const { return m_redo.size(); }
    bool undo();   // restore the previous feature list + recompute(); false if no history
    bool redo();   // re-apply the most recently undone change; false if none

    // Feature-tree editing (Onshape-style). All are transactional: they snapshot
    // features, mutate, recompute(), and roll back to the snapshot (re-recomputing)
    // if the result is invalid — so a failed edit never leaves a broken body.
    //
    // remove_feature: erase features[index]; deleting a Sketch cascades to the
    //   Extrude(s) that consume it; surviving sketch_ref indices are remapped.
    // move_feature:   shift features[index] by delta (-1 up / +1 down), clamped;
    //   sketch_ref indices of the two swapped slots are remapped.
    // replace_feature: overwrite features[index] with `edited` (its name and, for
    //   an Extrude, its sketch_ref are preserved from the original).
    bool remove_feature(int index);
    bool move_feature(int index, int delta);
    // Show or hide a feature. Transactional like the others, and it keeps body and datum-plane
    // references pointing at the same objects, which a bare `enabled` flip does not.
    bool set_feature_enabled(int index, bool enabled);
    bool replace_feature(int index, const CadFeature& edited);
    // replace_sketch_extrude: a box is two linked features (Sketch + Extrude);
    //   overwrite both slots from one `edited` candidate (sketch params ->
    //   features[sketch_idx], extrude params -> features[extrude_idx]), keeping
    //   each slot's name/type and the sketch_ref link. Transactional like above.
    bool replace_sketch_extrude(int sketch_idx, int extrude_idx, const CadFeature& edited);

    // Apply ONE candidate feature on top of the current committed body and
    // tessellate the result into out_mesh, WITHOUT modifying features/body/
    // display_mesh. Returns false (with err set) if the candidate is invalid.
    // Used by the Design tab to show a translucent ghost before Confirm.
    // The solid body the closed profile of sketch `sketch_ref` lies on or touches, or -1. Drives
    // the Extrude default: a profile drawn on a body joins it, one in free space is a new body.
    int body_touching_sketch(int sketch_ref) const;
    bool preview(const CadFeature& candidate, TriangleMesh& out_mesh, std::string& err) const;
    // Same, but also returns the per-body meshes (in `bodies` order; the candidate may append
    // one), so the GUI can apply its display-only per-body Move transforms to the ghost and keep
    // it overlaid on the moved body instead of floating back at the untransformed origin.
    bool preview(const CadFeature& candidate, TriangleMesh& out_mesh,
                 std::vector<TriangleMesh>& out_body_meshes, std::string& err) const;

    // The faces of the current bodies that features[index] made, as (body, face id) pairs: what
    // the Design tab highlights when that feature is selected. A face counts when it lies on the
    // model's boundary, facing the same way, right after the feature and not right before it, so
    // a face a later feature trimmed still belongs to the one that made it. Positions are
    // compared, no history is kept: a face a later feature rebuilt in place stays the earlier
    // feature's. A feature with no face left of its own (a Boolean union, or one whose faces a
    // later feature removed) answers with every face of each body it changed. Empty for a feature
    // that leaves no body, a hidden one, or a history that no longer rebuilds up to it.
    std::vector<std::pair<int, int>> faces_made_by(int index) const;

private:
    TopoDS_Wire build_sketch_wire(const CadFeature& sketch, bool closed_only = false) const;
    // Profile number `ri` of Loft / Surface Loft `f`: the wire of its one chosen loop, or of the whole sketch when none is chosen.
    TopoDS_Wire loft_profile_wire(const CadFeature& f, size_t ri, bool closed_only, const TopoDS_Shape& context) const;
    static bool loft_profile_is_face(const CadFeature& f, size_t ri) { return ri < f.loft_profile_faces.size() && f.loft_profile_faces[ri] >= 0; }
    // The planar region an Extrude sweeps: the sketch's outer loop with its inner loops as
    // holes. Falls back to a face over build_sketch_wire() for the legacy profile/shape paths,
    // which have no concept of a second loop.
    TopoDS_Face build_sketch_face(const CadFeature& sketch) const;
    // Apply a single feature to (result, have_body), throwing std::runtime_error on
    // failure. `context` is the body whose faces/edges the feature reads (face-extrude
    // source, up-to-face target, dress-up, hole) — it differs from `result` only when the
    // feature builds a NEW body from an existing one (face-extrude New). Shared by route.
    void apply_feature(TopoDS_Shape& result, bool& have_body,
                       const TopoDS_Shape& context, const CadFeature& f) const;
    // Route one feature into the bodies list: resolve its target body, decide whether it
    // starts a new body (empty list, or an Extrude with mode New) vs mutates an existing
    // one, then apply_feature. Shared by recompute() (replay all) and preview() (candidate).
    void route_feature(std::vector<CadBody>& bodies, const CadFeature& f) const;
    // The parametric pass every replay starts with: evaluate the variables and write each
    // feature's expression bindings into its numeric fields.
    void bind_expressions();
    int  m_bar{-1};   // the rollback bar when the last checkpoint() was taken (checkpoint() itself puts the bar away)
    void bind_sheet_rules();   // each sheet-metal feature gets the last SmRule before it (or the default rule)
    // One step of the replay: route features[fi] into `built` and stamp the bodies it created
    // with fi. A hidden feature, a sketch, a helix or a datum leaves `built` alone. Throws on
    // failure.
    void replay_feature(size_t fi, std::vector<CadBody>& built);
    // Boolean between two existing bodies: resolve target + tool, optionally snap the tool so
    // the picked faces mate, run the OCCT op (with fuzzy tolerance), write the result back to the
    // target and erase the consumed tool. Mutates the bodies vector directly (unlike apply_feature,
    // which works on a single result shape). Throws std::runtime_error on a failed op.
    void apply_boolean(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void apply_cut(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void apply_mirror(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void apply_feature_pattern(std::vector<CadBody>& bodies, const CadFeature& f) const;
    // The bodies as they stood BEFORE each feature of the recompute in progress (index = feature index): a feature pattern
    // needs the state around the features it repeats. Filled and cleared by recompute().
    mutable std::vector<std::vector<CadBody>> m_pre_states;
    mutable bool m_in_replay{false};   // recompute() is running its feature loop (m_pre_states has an entry for the feature being applied)
    void apply_transform(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void apply_thicken(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void apply_thicken_surface(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void apply_surface_offset(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void apply_project(const std::vector<CadBody>& bodies, CadFeature& f, size_t fi) const;
    mutable std::vector<std::string> m_pending_warnings;   // filled by route_feature, flushed by replay_feature
    void apply_project_from_sketch(CadFeature& f, size_t fi) const;
    static DatumCoordSys datum_frame(const std::vector<CadBody>& bodies, const CadFeature& f);
    void apply_mate(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void detect_mate_conflicts();   // refills mate_conflicts from the feature list alone

    // Undo/redo stacks of recipe snapshots. checkpoint() pushes onto m_undo and clears
    // m_redo; undo()/redo() shuffle the current state between them. Capped so a long
    // session can't grow unbounded.
    //
    // The snapshot MUST carry `variables` as well as `features`: a caller that sets a bad
    // variable, sees recompute() fail and calls undo() to roll it back would otherwise be
    // left with the bad variable still in the document, so every later recompute fails —
    // the exact corruption the checkpoint/undo pattern exists to prevent. Not serialized,
    // so this changes no on-disk format.
    struct Snapshot {
        std::vector<CadFeature>            features;
        std::map<std::string, std::string> variables;
    };
    std::vector<Snapshot> m_undo;
    std::vector<Snapshot> m_redo;
    static constexpr size_t k_undo_cap = 200;
};

} // namespace Slic3r

#endif // slic3r_CadDocument_hpp_
