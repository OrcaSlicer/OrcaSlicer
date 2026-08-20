#ifndef slic3r_CadDocument_hpp_
#define slic3r_CadDocument_hpp_

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"
#include "libslic3r/CAD/GeometryEngine.hpp"   // FaceGroup
#include "libslic3r/Color.hpp"            // ColorRGBA (per-body display colour override)

#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <cereal/cereal.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/string.hpp>
#include <map>
#include <cereal/types/map.hpp>
#include <string>
#include <vector>
#include <utility>

namespace Slic3r {

enum class CadFeatureType { Sketch, Extrude, Fillet, Chamfer, Hole, Thread, Shell, Revolve, Sweep, Pattern, Plane, Loft, Draft, Import, Boolean, Cut, Mirror, Axis, CoordSys, Helix, Transform, Thicken, Project, DeleteFace, Rib, SurfaceExtrude, SurfaceRevolve, ThickenSurface, SurfaceOffset, SurfaceLoft, SurfaceFill, Mate };
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
    // snapshots is cheap. In-session only for now (no BRep serialization yet).
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
    std::string hole_standard;             // provenance only, e.g. "M6" / "1/4-20"; not used by geometry

    // Thread params (helical thread about the plane normal at a positioned point)
    double      thread_radius{5};          // nominal cylinder radius
    double      thread_pitch{2};           // axial advance per turn
    double      thread_height{10};         // total axial length
    double      thread_depth{1};           // radial crest depth of the thread profile
    bool        thread_internal{false};    // false = external threaded rod (New body);
                                           // true = tapped bore cut into the current body
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

    // Sweep: profile carried by sketch_ref / entities (like Extrude); the spine is a
    // second Sketch referenced by sweep_path_ref (an open or closed wire). Reuses
    // mode (boolean) and target_body.
    int         sweep_path_ref{-1};        // index into features[] of the path Sketch

    // Loft: build a solid through 2+ closed profile Sketches (loft_profile_refs, in
    // order, each on its own plane). loft_ruled=false → smooth sections, true → ruled.
    // Reuses mode (boolean) and target_body.
    std::vector<int> loft_profile_refs;    // ordered indices into features[] of profile Sketches
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

    // Pattern along a curve: when pattern_curve_sketch >= 0 this mode takes precedence over
    // linear/circular. Copies are placed at equal-parameter points along entity
    // pattern_curve_entity of sketch pattern_curve_sketch, translated by (P_i - P_0).
    int pattern_curve_sketch{-1};  // feature index of the Sketch holding the guide curve
    int pattern_curve_entity{-1};  // entity index of the guide curve within that sketch

    // Parametric bindings: field-member-name -> expression string. On recompute() each entry
    // is evaluated against the document variables and written into the named numeric field
    // BEFORE geometry runs. Empty (the common case) means the feature uses its literal fields.
    std::map<std::string, std::string> expr;

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
               coordsys_face_kind, coordsys_face_edges);
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

    // Modeling origin: the world point the default XY/XZ/YZ planes pass through. The GUI sets this
    // to the bed centre so sketches/datums land in the middle of the bed (not the bed corner =
    // world 0). Not serialized — the GUI re-applies it from the live bed on every tab show.
    Vec3d modeling_origin{Vec3d::Zero()};

    // Tessellation quality, matched to Orca's OWN STEP importer (Format/STEP.hpp defaults:
    // linear 0.003, angular 0.5 rad) so a body modelled here reaches the screen at the same
    // density as the identical body imported through Prepare. It was 0.01 linear — 3.3x coarser
    // than anything else in the app, which is why curved faces read as faceted next to an
    // imported part. Angular already matched. Same BRepMesh_IncrementalMesh call, same GLVolume
    // path, same shaders: the renderer was never the difference, the mesh fed to it was.
    double linear_deflection{0.003};
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
    int  add_chamfer(double distance, FaceGroup faces, const std::string& name);
    int  add_chamfer(double distance, int edge_id, const std::string& name);
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

    // v5: every feature is length-framed, so a reader can stop early on an older file and skip
    // the tail of a newer one. This is the LAST version that has to break anything — from here a
    // new field only needs appending to save/load, with no bump and no orphaned projects.
    // v4 is still read, by the pre-framing flat path, so existing projects keep opening.
    static constexpr uint32_t SNAPORCA_CAD_RECIPE_VERSION = 5;
    std::string serialize_recipe() const;
    bool deserialize_recipe(const std::string& blob);

    // Export every body to a STEP file as native B-rep (not mesh). body_xforms is the
    // per-body display transform (Move gizmo); when supplied the bodies are written at
    // those positions so the STEP matches what Commit ships. false + err on failure.
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
    bool can_undo() const { return !m_undo.empty(); }
    bool can_redo() const { return !m_redo.empty(); }
    size_t undo_depth() const { return m_undo.size(); }
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
    bool preview(const CadFeature& candidate, TriangleMesh& out_mesh, std::string& err) const;
    // Same, but also returns the per-body meshes (in `bodies` order; the candidate may append
    // one), so the GUI can apply its display-only per-body Move transforms to the ghost and keep
    // it overlaid on the moved body instead of floating back at the untransformed origin.
    bool preview(const CadFeature& candidate, TriangleMesh& out_mesh,
                 std::vector<TriangleMesh>& out_body_meshes, std::string& err) const;

private:
    TopoDS_Wire build_sketch_wire(const CadFeature& sketch) const;
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
    // Boolean between two existing bodies: resolve target + tool, optionally snap the tool so
    // the picked faces mate, run the OCCT op (with fuzzy tolerance), write the result back to the
    // target and erase the consumed tool. Mutates the bodies vector directly (unlike apply_feature,
    // which works on a single result shape). Throws std::runtime_error on a failed op.
    void apply_boolean(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void apply_cut(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void apply_mirror(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void apply_transform(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void apply_thicken(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void apply_thicken_surface(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void apply_surface_offset(std::vector<CadBody>& bodies, const CadFeature& f) const;
    void apply_project(const std::vector<CadBody>& bodies, CadFeature& f) const;
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
