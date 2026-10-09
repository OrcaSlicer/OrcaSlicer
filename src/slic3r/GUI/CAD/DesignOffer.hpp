// GENERATED FILE — DO NOT EDIT.
// Source: scripts/CAD/tool_atlas.json   Generator: scripts/CAD/gen_offer_table.py
//
// The object-driven tool offer (charter 4.1): every verb has ONE row index, that index
// is the same in every selection it appears in, and verbs that do not apply are shown
// disabled in place with their reason rather than removed. Row order was ratified
// 2026-07-31; changing an index is a breaking change to every user's muscle memory.
#ifndef slic3r_GUI_DesignOffer_hpp_
#define slic3r_GUI_DesignOffer_hpp_

#include <cstdint>

#ifndef L
#define L(s) s   // gettext marker, as in slic3r/GUI/I18N.hpp
#endif
#ifndef L_CONTEXT
#define L_CONTEXT(s, context) s
#endif

namespace Slic3r { namespace GUI {

// What the viewport has selected. Ordered as in tool_atlas.json; the bitmask in
// OfferVerb::accepts indexes these.
enum class OfferSel : int {
    None = 0,
    FacePlanar = 1,
    FaceCyl = 2,
    FaceOther = 3,
    EdgeStr = 4,
    EdgeCirc = 5,
    Vertex = 6,
    BodySolid = 7,
    BodySheet = 8,
    Bodies2 = 9,
    DatumPlane = 10,
    DatumAxis = 11,
    CoordSys = 12,
    Art = 13,
    SkLoop = 14,
    SkNone = 15,
    SkLine = 16,
    SkArc = 17,
    SkPoint = 18,
    Sk2Ent = 19,
    Count = 20
};

inline uint32_t offer_bit(OfferSel s) { return 1u << int(s); }

// One row of the offer. `action` routes to the code that already implements the verb:
//   "key:S+E"        -> m_keys_feature[SHIFT('E')]
//   "key:L"          -> m_keys_sketch['L']
//   "fly:material#4" -> row 4 of the "material" feature flyout
//   "btn:delete"     -> a standalone toolbar button
//   nullptr          -> kernel support exists, no GUI path yet (row shows disabled)
struct OfferVerb {
    const char* id;
    const char* name;        // drawing-office word (L10); msgctxt "Design", translated at use
    int         row;         // index into kOfferRowNames, the ratified address — NEVER reorder
    const char* key;         // shortcut shown in the row, or nullptr
    const char* action;
    const char* refusal;     // why this row is greyed, in the product's own words
    uint32_t    accepts;     // bitmask over OfferSel
    int         need_bodies;
    int         need_sketches;
    bool        need_sheet;
    bool        sketch_mode; // belongs to the sketch-mode vocabulary, not the model one
    // Second level INSIDE a row, for tools that come in variants: "Rectangle" holds corner,
    // centre, oblique and rounded. nullptr = sits directly in the row. Keeps the row's own
    // address fixed (L4.1) while the variants hang one level below it, mirroring the toolbar's
    // grouping instead of flattening 19 create tools into one wall.
    const char* family;
    const char* icon;         // resources/images name, or nullptr — the offer draws it beside the row
    const char* hint;         // what the verb does / what to click; shown on hover
};

// Row labels, in ratified order.
static const char* const kOfferRowNames[] = {
    L("Create"),
    L("Add material"),
    L("Remove"),
    L("Fillet / chamfer / draft"),
    L("Repeat"),
    L("Transform"),
    L("Reference"),
    L("Modify"),
    "Top",
};
static const int kOfferRowCount = 9;
// A flat row is not a family: its verbs come first, at the top level of the offer, each
// an item of its own.
static const bool kOfferRowFlat[] = {
    false,
    false,
    false,
    false,
    false,
    false,
    false,
    false,
    true,
};

static const OfferVerb kOfferVerbs[] = {
    {"sketch", L_CONTEXT("Sketch", "Design"), 0, "Shift+S", "key:S+S", L("Select a flat face or a reference plane to sketch on"), 0x00000403u, 0, 0, false, false, nullptr, "design_sketch", L("Sketch on the selected flat face or plane, or click one next")},
    {"extrude", L_CONTEXT("Extrude", "Design"), 1, "Shift+E", "key:S+E", L("Create a sketch, or pick a solid face, first"), 0x00004002u, 0, 0, false, false, nullptr, "design_extrude", L("Extrude a sketch profile, or push/pull a picked face")},
    {"revolve", L_CONTEXT("Revolve", "Design"), 1, "Shift+R", "key:S+R", L("Create a sketch profile to revolve first"), 0x00004000u, 0, 0, false, false, nullptr, "design_revolve", L("Revolve a profile about an axis")},
    {"sweep", L_CONTEXT("Sweep", "Design"), 1, "Shift+W", "key:S+W", L("Create a profile sketch to sweep first"), 0x00004000u, 0, 2, false, false, nullptr, "design_sweep", L("Sweep a profile along a path")},
    {"loft", L_CONTEXT("Loft", "Design"), 1, "Shift+L", "key:S+L", L("Create at least two profile sketches to loft"), 0x00004000u, 0, 2, false, false, nullptr, "design_loft", L("Loft (skin) between two or more profiles")},
    {"thicken", L_CONTEXT("Thicken", "Design"), 1, nullptr, "fly:material#4", L("Thicken needs a solid body — add or import one first"), 0x0000000au, 1, 0, false, false, nullptr, "design_thicken", L("Offset a solid face into a thin plate (new body)")},
    {"rib", L_CONTEXT("Rib", "Design"), 1, "R", "fly:material#5", L("Rib needs a solid body — add or import one first"), 0x00010000u, 1, 0, false, false, nullptr, "design_rib", L("Grow a thin wall from an open sketch line, fused to a body")},
    {"boolean", L_CONTEXT("Join", "Design"), 1, "Shift+B", "btn:bool#0", L("Boolean needs two bodies — add or import a second one"), 0x00000200u, 2, 0, false, false, nullptr, "design_boolean", L("Fuse the tool body into the target — one solid, no seam")},
    {"bool_subtract", L_CONTEXT("Subtract", "Design"), 1, nullptr, "btn:bool#1", L("Boolean needs two bodies — add or import a second one"), 0x00000200u, 2, 0, false, false, nullptr, "design_boolean", L("Cut the tool body out of the target")},
    {"bool_intersect", L_CONTEXT("Intersect", "Design"), 1, nullptr, "btn:bool#2", L("Boolean needs two bodies — add or import a second one"), 0x00000200u, 2, 0, false, false, nullptr, "design_boolean", L("Keep only where the two bodies overlap")},
    {"surf_extrude", L_CONTEXT("Surface Extrude", "Design"), 1, "Shift+G", "key:S+G", L("Create a sketch first"), 0x00004000u, 0, 0, false, false, nullptr, "design_extrude", L("Extrude a sketch into a sheet body (no end caps)")},
    {"surf_revolve", L_CONTEXT("Surface Revolve", "Design"), 1, nullptr, "fly:surface#1", L("Create a sketch profile to revolve first"), 0x00004000u, 0, 0, false, false, nullptr, "design_revolve", L("Revolve a sketch profile into a sheet body")},
    {"surf_loft", L_CONTEXT("Surface Loft", "Design"), 1, nullptr, "fly:surface#2", L("Create at least two profile sketches to loft"), 0x00004000u, 0, 2, false, false, nullptr, "design_loft", L("Loft (skin) between 2+ profiles, open (no end caps)")},
    {"surf_fill", L_CONTEXT("Surface Fill", "Design"), 1, nullptr, "fly:surface#3", L("Create a closed sketch first"), 0x00004000u, 0, 0, false, false, nullptr, "design_surface", L("Fill a sketch boundary with a smooth face")},
    {"thicken_surf", L_CONTEXT("Thicken Surface", "Design"), 1, nullptr, "fly:surface#5", L("Thicken Surface needs a surface body — make one with a Surface tool first"), 0x00000100u, 0, 0, true, false, nullptr, "design_thicken", L("Thicken a sheet body into a solid")},
    {"hole", L_CONTEXT("Hole", "Design"), 2, "Shift+H", "key:S+H", L("Pick a face or a plane to drill into"), 0x00000402u, 1, 0, false, false, nullptr, "design_hole", L("Drill a hole, centered on a picked face or placed on a plane")},
    {"thread", L_CONTEXT("Thread", "Design"), 2, "Shift+T", "key:S+T", L("Pick a cylindrical surface (bore / outer) or a circular edge for a thread"), 0x00000024u, 1, 0, false, false, nullptr, "design_thread", L("Thread a cylindrical surface (inner bore / outer) or a circular edge")},
    {"shell", L_CONTEXT("Shell", "Design"), 2, "Shift+K", "key:S+K", L("Shell needs a solid body — add or import one first"), 0x00000082u, 1, 0, false, false, nullptr, "design_shell", L("Hollow the body to a wall thickness, opening a picked face")},
    {"cut", L_CONTEXT("Cut", "Design"), 2, "Shift+X", "key:S+X", L("Cut needs a solid body — add or import one first"), 0x000004feu, 1, 0, false, false, nullptr, "design_cut", L("Trim the body with a plane — drag the offset arrow; keep one half or both")},
    {"split", L_CONTEXT("Split", "Design"), 2, nullptr, nullptr, L("Split needs a solid body — add or import one first"), 0x000000feu, 1, 0, false, false, nullptr, nullptr, L("Split the body along a picked face into two solids")},
    {"fillet", L_CONTEXT("Fillet", "Design"), 3, "Shift+F", "btn:dress#0", L("Pick an edge to round"), 0x000000b2u, 1, 0, false, false, nullptr, "design_filletedge", L("Pick an edge, then drag the radius arrow or type it")},
    {"chamfer", L_CONTEXT("Chamfer", "Design"), 3, nullptr, "btn:dress#1", L("Pick an edge to bevel"), 0x000000b2u, 1, 0, false, false, nullptr, "design_chamfer", L("Pick an edge, then drag the distance arrow or type it")},
    {"draft", L_CONTEXT("Draft", "Design"), 3, "Shift+D", "key:S+D", L("Pick a face to taper"), 0x0000000au, 1, 0, false, false, nullptr, "design_draft", L("Tilt a picked face by a draft angle")},
    {"surf_offset", L_CONTEXT("Surface Offset", "Design"), 3, nullptr, "fly:surface#4", L("Surface Offset needs a surface body — make one with a Surface tool first"), 0x00000100u, 0, 0, true, false, nullptr, "design_offset", L("Offset a sheet body's shell by a signed distance")},
    {"pattern", L_CONTEXT("Linear pattern", "Design"), 4, "Shift+N", "btn:pat#0", L("Pattern needs a solid body — add or import one first"), 0x00006082u, 1, 0, false, false, nullptr, "design_array", L("Repeat the body along a direction — drag the spacing, set the count")},
    {"pattern_circular", L_CONTEXT("Circular pattern", "Design"), 4, nullptr, "btn:pat#1", L("Pattern needs a solid body — add or import one first"), 0x00006082u, 1, 0, false, false, nullptr, "design_polararray", L("Repeat the body around an axis — set the count and sweep")},
    {"mirror", L_CONTEXT("Mirror", "Design"), 4, "Shift+Z", "key:S+Z", L("Mirror needs a body — add or import one first"), 0x000004feu, 1, 0, false, false, nullptr, "design_mirror", L("Reflect a body about a plane")},
    {"pat_curve", L_CONTEXT("Pattern on Curve", "Design"), 4, nullptr, nullptr, L("Pattern on Curve needs a body and a curve"), 0x00000090u, 1, 0, false, false, nullptr, nullptr, L("Repeat the body along a picked curve")},
    {"transform", L_CONTEXT("Move", "Design"), 5, "Shift+Y", "key:S+Y", L("Transform needs a body — add or import one first"), 0x000021feu, 1, 0, false, false, nullptr, "design_move", L("Move and/or rotate an existing body")},
    {"mate", L_CONTEXT("Mate", "Design"), 5, nullptr, "fly:placement#2", L("Mate needs two coordinate systems — create them first"), 0x00001202u, 2, 0, false, false, nullptr, "design_c_coincident", L("Assembly: align two coordinate systems (fastened, planar, revolute, slider, cylindrical)")},
    {"align", L_CONTEXT("Align to", "Design"), 5, nullptr, nullptr, L("Align needs a body — add or import one first"), 0x00000002u, 1, 0, false, false, nullptr, nullptr, L("Align the body to a picked face or plane")},
    {"plane", L_CONTEXT("Plane", "Design"), 6, "Shift+P", "key:S+P", nullptr, 0x00000453u, 0, 0, false, false, nullptr, "design_plane", L("Reference plane (offset / tilt / midplane / tangent / two edges / coincident)")},
    {"axis", L_CONTEXT("Axis", "Design"), 6, "Shift+A", "key:S+A", nullptr, 0x00000057u, 0, 0, false, false, nullptr, "design_line", L("Datum axis (two points, face normal, cylinder centerline, two planes, along edge)")},
    {"coordsys_v", L_CONTEXT("Coordinate system", "Design"), 6, "Shift+C", "key:S+C", nullptr, 0x00000043u, 0, 0, false, false, nullptr, "design_point", L("Datum coordinate system (world point, or face + direction edge)")},
    {"helix", L_CONTEXT("Helix", "Design"), 6, nullptr, "fly:plane#3", nullptr, 0x00000405u, 0, 0, false, false, nullptr, "design_thread", L("Helical curve (spring path) — use as a sweep path for coils / springs / augers")},
    {"project", L_CONTEXT("Project", "Design"), 6, nullptr, "fly:plane#4", L("Project needs a body — add or import one first"), 0x00000482u, 1, 0, false, false, nullptr, "design_sketch", L("Project body edges onto a plane as sketch entities")},
    {"measure", L_CONTEXT("Measure", "Design"), 6, nullptr, nullptr, nullptr, 0x000b03feu, 0, 0, false, false, nullptr, nullptr, L("Measure between the picked points, edges or faces")},
    {"mass_props", L_CONTEXT("Volume and area", "Design"), 6, nullptr, "btn:mass", nullptr, 0x000000feu, 1, 0, false, false, nullptr, "info", L("Report the volume and surface area of the selected body")},
    {"interference", L_CONTEXT("Interference", "Design"), 6, nullptr, "btn:interference", L("Interference needs at least two bodies"), 0x00000280u, 2, 0, false, false, nullptr, nullptr, L("Check whether two bodies overlap — reports, changes nothing")},
    {"edit_feature", L_CONTEXT("Edit", "Design"), 7, nullptr, "btn:edit", nullptr, 0x00007d8eu, 0, 0, false, false, nullptr, "design_edit", L("Reopen the selected feature to change what it was made from")},
    {"rename", L_CONTEXT("Rename…", "Design"), 8, "F2", "btn:rename", L("Select a feature, or a body, to rename it"), 0x00004080u, 0, 0, false, false, nullptr, nullptr, L("Give this feature a name you will recognise in the tree (a body takes its name from the feature that makes it)")},
    {"delete_face", L_CONTEXT("Delete Face", "Design"), 7, nullptr, "fly:dressup#3", L("Delete Face needs a body — add or import one first"), 0x0000000eu, 1, 0, false, false, nullptr, "design_delete", L("Remove faces from a body and heal the solid")},
    {"colour", L_CONTEXT("Color", "Design"), 8, nullptr, "btn:colour", nullptr, 0x000001feu, 1, 0, false, false, nullptr, "color_palette", L("Set the selected body's display color")},
    {"zoom_to", L_CONTEXT("Zoom to selection", "Design"), 8, nullptr, "btn:zoom_to", nullptr, 0x000041feu, 0, 0, false, false, nullptr, "design_zoom", L("Frame the selection in the view, keeping the view direction")},
    {"delete", L_CONTEXT("Delete", "Design"), 7, "Del", "btn:delete", nullptr, 0x000f7c00u, 0, 0, false, false, nullptr, "design_delete", L("Delete what is selected")},
    {"delete_body", L_CONTEXT("Delete Body", "Design"), 7, nullptr, "btn:delete_body", nullptr, 0x000001feu, 1, 0, false, false, nullptr, "design_delete", L("Delete this whole body — removes the feature it was made from")},
    {"sk_line_t", L_CONTEXT("Line", "Design"), 0, "L", "key:L", nullptr, 0x000f8000u, 0, 0, false, true, L("Line"), "design_line", L("Line — click start, then end")},
    {"sk_polyline", L_CONTEXT("Polyline", "Design"), 0, nullptr, "fly:design_line#1", nullptr, 0x000f8000u, 0, 0, false, true, L("Line"), "design_polyline", L("Click points; click the first point to close the loop, right-click to end it open")},
    {"sk_rect", L_CONTEXT("Corner rectangle", "Design"), 0, "R", "key:R", nullptr, 0x000f8000u, 0, 0, false, true, L("Rectangle"), "design_rect", L("Rectangle — click two opposite corners")},
    {"sk_rect_center", L_CONTEXT("Center rectangle", "Design"), 0, nullptr, "fly:design_rect#1", nullptr, 0x000f8000u, 0, 0, false, true, L("Rectangle"), "design_crect", L("Click center, then a corner")},
    {"sk_rect_oblique", L_CONTEXT("Oblique rectangle", "Design"), 0, nullptr, "fly:design_rect#2", nullptr, 0x000f8000u, 0, 0, false, true, L("Rectangle"), "design_rect_oblique", L("Click two corners of one edge, then a point for the width")},
    {"sk_rect_rounded", L_CONTEXT("Rounded rectangle", "Design"), 0, nullptr, "fly:design_rect#3", nullptr, 0x000f8000u, 0, 0, false, true, L("Rectangle"), "design_rect_rounded", L("Click two opposite corners, then a point for the corner radius")},
    {"sk_circle", L_CONTEXT("Center circle", "Design"), 0, "C", "key:C", nullptr, 0x000f8000u, 0, 0, false, true, L("Circle"), "design_circle", L("Circle — click center, then radius")},
    {"sk_circle_2pt", L_CONTEXT("2-point circle", "Design"), 0, nullptr, "fly:design_circle#1", nullptr, 0x000f8000u, 0, 0, false, true, L("Circle"), "design_circle2pt", L("Click two ends of the diameter")},
    {"sk_circle_3pt", L_CONTEXT("3-point circle", "Design"), 0, nullptr, "fly:design_circle#2", nullptr, 0x000f8000u, 0, 0, false, true, L("Circle"), "design_circle3pt", L("Click three points on the circle")},
    {"sk_arc_t", L_CONTEXT("3-point arc", "Design"), 0, "A", "key:A", nullptr, 0x000f8000u, 0, 0, false, true, L("Arc"), "design_arc3pt", L("Arc — click start, end, then a point")},
    {"sk_arc_tangent", L_CONTEXT("Tangent arc", "Design"), 0, nullptr, "fly:design_arc3pt#1", nullptr, 0x000f8000u, 0, 0, false, true, L("Arc"), "design_tangentarc", L("Click start (on the last entity) then end")},
    {"sk_arc_center", L_CONTEXT("Center-point arc", "Design"), 0, nullptr, "fly:design_arc3pt#2", nullptr, 0x000f8000u, 0, 0, false, true, L("Arc"), "design_arc_center", L("Click center, then start, then a point for the end angle")},
    {"sk_slot", L_CONTEXT("Slot", "Design"), 0, "S", "key:S", nullptr, 0x000f8000u, 0, 0, false, true, L("Slot"), "design_slot", L("Slot — two centerline ends, then the width")},
    {"sk_slot_arc", L_CONTEXT("Arc slot", "Design"), 0, nullptr, "fly:design_slot#1", nullptr, 0x000f8000u, 0, 0, false, true, L("Slot"), "design_slot_arc", L("Click center, start, end, then a point for the width")},
    {"sk_ellipse", L_CONTEXT("Ellipse", "Design"), 0, "E", "key:E", nullptr, 0x000f8000u, 0, 0, false, true, L("Ellipse"), "design_ellipse", L("Ellipse — center, major end, minor point")},
    {"sk_ellipse_arc", L_CONTEXT("Elliptical arc", "Design"), 0, nullptr, "fly:design_ellipse#1", nullptr, 0x000f8000u, 0, 0, false, true, L("Ellipse"), "design_ellipse_arc", L("Click center, major-axis end, minor point, then arc start and end")},
    {"sk_spline", L_CONTEXT("Spline", "Design"), 0, "B", "key:B", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_bspline", L("Spline — click control points")},
    {"sk_poly_3", L_CONTEXT("Triangle", "Design"), 0, nullptr, "btn:poly#3", nullptr, 0x000f8000u, 0, 0, false, true, L("Polygon"), "design_polygon", L("Triangle — click center, then a vertex")},
    {"sk_poly_4", L_CONTEXT("Square", "Design"), 0, nullptr, "btn:poly#4", nullptr, 0x000f8000u, 0, 0, false, true, L("Polygon"), "design_polygon", L("Square — click center, then a vertex")},
    {"sk_poly_5", L_CONTEXT("Pentagon", "Design"), 0, nullptr, "btn:poly#5", nullptr, 0x000f8000u, 0, 0, false, true, L("Polygon"), "design_polygon", L("Pentagon — click center, then a vertex")},
    {"sk_polygon", L_CONTEXT("Hexagon", "Design"), 0, "G", "btn:poly#6", nullptr, 0x000f8000u, 0, 0, false, true, L("Polygon"), "design_polygon", L("Hexagon — click center, then a vertex")},
    {"sk_poly_8", L_CONTEXT("Octagon", "Design"), 0, nullptr, "btn:poly#8", nullptr, 0x000f8000u, 0, 0, false, true, L("Polygon"), "design_polygon", L("Octagon — click center, then a vertex")},
    {"sk_poly_12", L_CONTEXT("Dodecagon", "Design"), 0, nullptr, "btn:poly#12", nullptr, 0x000f8000u, 0, 0, false, true, L("Polygon"), "design_polygon", L("Dodecagon — click center, then a vertex")},
    {"sk_poly_inscribed", L_CONTEXT("Inscribed", "Design"), 0, nullptr, "btn:polyfit#0", nullptr, 0x000f8000u, 0, 0, false, true, L("Polygon"), "design_polygon", L("Measure the polygon to its corners (inscribed)")},
    {"sk_poly_circumscribed", L_CONTEXT("Circumscribed", "Design"), 0, nullptr, "btn:polyfit#1", nullptr, 0x000f8000u, 0, 0, false, true, L("Polygon"), "design_polygon", L("Measure the polygon to its flats (circumscribed)")},
    {"sk_point_t", L_CONTEXT("Point", "Design"), 0, "P", "key:P", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_point", L("Point — click to place")},
    {"sk_text", L_CONTEXT("Text", "Design"), 0, nullptr, "btn:text", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_text", L("Type text; it becomes a Text feature on this sketch's plane, editable later")},
    {"sk_svg", L_CONTEXT("SVG", "Design"), 0, nullptr, "btn:svg", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_svg", L("Import an SVG outline into this sketch as editable lines")},
    {"sk_offset", L_CONTEXT("Offset", "Design"), 1, "O", "key:O", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_offset", L("Offset — pick an entity, drag the distance")},
    {"sk_trim", L_CONTEXT("Trim", "Design"), 2, "T", "key:T", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_trim", L("Trim — click a segment to trim it")},
    {"sk_fillet", L_CONTEXT("Fillet", "Design"), 3, "F", "key:F", nullptr, 0x00090000u, 0, 0, false, true, nullptr, "design_filletedge", L("Fillet — pick two lines, set the radius")},
    {"sk_chamfer", L_CONTEXT("Chamfer", "Design"), 3, "H", "key:H", nullptr, 0x00090000u, 0, 0, false, true, nullptr, "design_chamfer", L("Chamfer — pick two lines, set the distance")},
    {"sk_array", L_CONTEXT("Linear array", "Design"), 4, nullptr, "fly:design_array#0", nullptr, 0x000b0000u, 0, 0, false, true, L("Array"), "design_array", L("Pick entities, drag the spacing handle, click the count; click empty to apply")},
    {"sk_array_polar", L_CONTEXT("Polar array", "Design"), 4, nullptr, "fly:design_array#1", nullptr, 0x000b0000u, 0, 0, false, true, L("Array"), "design_polararray", L("Pick entities, drag the sweep handle, click the count; click empty to apply")},
    {"sk_mirror", L_CONTEXT("Mirror", "Design"), 4, "M", "key:M", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_mirror", L("Mirror — pick axis, then entities")},
    {"sk_move", L_CONTEXT("Move", "Design"), 5, nullptr, "fly:design_move#0", nullptr, 0x000f0000u, 0, 0, false, true, L("Move"), "design_move", L("Pick entities, then drag the handle or click the distance; click empty to apply")},
    {"sk_rotate", L_CONTEXT("Rotate", "Design"), 5, nullptr, "fly:design_move#1", nullptr, 0x000f0000u, 0, 0, false, true, L("Move"), "design_rotate", L("Pick entities, then drag around the pivot or click the angle; click empty to apply")},
    {"sk_scale", L_CONTEXT("Scale", "Design"), 5, nullptr, "fly:design_move#2", nullptr, 0x000f0000u, 0, 0, false, true, L("Move"), "design_scale", L("Pick entities, then drag the handle or click the factor; click empty to apply")},
    {"sk_dimension", L_CONTEXT("Dimension", "Design"), 6, "D", "key:D", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_dimension", L("Dimension — click 2 points or an entity")},
    {"sk_constrain", L_CONTEXT("Constrain", "Design"), 6, "K", "key:K", nullptr, 0x000f0000u, 0, 0, false, true, nullptr, "design_constrain", L("Constrain the selected sketch entities to each other")},
    // Same verb, model-mode vocabulary: offered when a SKETCH is selected (bit 14, SkLoop), the
    // state a user is in right after finishing one. Without this row the only way in was the
    // toolbar icon, and constraints read as absent — see the Onshape-comparison report.
    // It sits in the Reference row, the address the sketch-mode Constrain has: one verb, one row.
    {"constrain", L_CONTEXT("Constrain sketch", "Design"), 6, nullptr, "btn:constrain", L("Select a sketch to constrain it"), 0x00004000u, 0, 1, false, false, nullptr, "design_constrain", L("Add dimensions and relations (coincident, tangent, parallel...) to the selected sketch")},
    {"sk_construct", L_CONTEXT("Construction", "Design"), 6, "Q", "key:Q", nullptr, 0x000b8000u, 0, 0, false, true, nullptr, nullptr, L("Toggle construction: geometry that guides but is never built")},
    {"sk_extend", L_CONTEXT("Extend", "Design"), 7, "X", "key:X", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_extend", L("Extend — click a line/arc to extend it")},
    {"sk_delete", L_CONTEXT("Delete", "Design"), 7, "Del", "btn:sk_delete", nullptr, 0x000f0000u, 0, 0, false, true, nullptr, "design_delete", L("Delete the selected sketch entities")},
    // Typing the defining number of the element you pointed at. Three rows rather than one so
    // each names the quantity in the drawing-office word for THAT element; all three land on
    // the same handler, because dimension_kind() already resolves the quantity from the
    // selection. Without these, an element's own numbers were reachable only by arming the
    // Dimension tool and re-picking geometry that was already selected.
    {"sk_length", L_CONTEXT("Length…", "Design"), 7, "V", "key:V", nullptr, 0x00010000u, 0, 0, false, true, nullptr, "design_dimension", L("Type the length of this line")},
    {"sk_radius", L_CONTEXT("Radius / diameter…", "Design"), 7, "V", "key:V", nullptr, 0x00020000u, 0, 0, false, true, nullptr, "design_dimension", L("Type the radius of this arc, or the diameter of this circle")},
    {"sk_angdist", L_CONTEXT("Angle / distance…", "Design"), 7, "V", "key:V", nullptr, 0x00080000u, 0, 0, false, true, nullptr, "design_dimension", L("Type the angle between two lines, or the distance between the two picks")},
};
static const int kOfferVerbCount = 93;

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_DesignOffer_hpp_
