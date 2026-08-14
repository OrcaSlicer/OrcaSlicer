// GENERATED FILE — DO NOT EDIT.
// Source: docs/ux/tool_atlas.json   Generator: docs/ux/mockups/gen_offer_table.py
//
// The object-driven tool offer (charter 4.1): every verb has ONE row index, that index
// is the same in every selection it appears in, and verbs that do not apply are shown
// disabled in place with their reason rather than removed. Row order was ratified
// 2026-07-31; changing an index is a breaking change to every user's muscle memory.
#ifndef slic3r_GUI_DesignOffer_hpp_
#define slic3r_GUI_DesignOffer_hpp_

#include <cstdint>

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
    const char* name;        // drawing-office word (L10); translated at use with wxGetTranslation
    int         row;         // 0..7, the ratified index — NEVER reorder
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
    "Create",
    "Add material",
    "Remove",
    "Fillet / chamfer / draft",
    "Repeat",
    "Transform",
    "Reference",
    "Modify",
};
static const int kOfferRowCount = 8;

static const OfferVerb kOfferVerbs[] = {
    {"sketch", "Sketch", 0, "Shift+S", "key:S+S", "Click a face or a reference plane in the viewport, then a sketch tool", 0x00000403u, 0, 0, false, false, nullptr, "design_sketch", "Click a face or a reference plane, then pick a drawing tool"},
    {"extrude", "Extrude", 1, "Shift+E", "key:S+E", "Create a sketch, or pick a solid face, first", 0x00004002u, 0, 0, false, false, nullptr, "design_extrude", "Extrude a sketch profile, or push/pull a picked face"},
    {"revolve", "Revolve", 1, "Shift+R", "key:S+R", "Create a sketch profile to revolve first", 0x00004000u, 0, 0, false, false, nullptr, "design_revolve", "Revolve a profile about an axis"},
    {"sweep", "Sweep", 1, "Shift+W", "key:S+W", "Create a profile sketch to sweep first", 0x00004000u, 0, 2, false, false, nullptr, "design_sweep", "Sweep a profile along a path"},
    {"loft", "Loft", 1, "Shift+L", "key:S+L", "Create at least two profile sketches to loft", 0x00004000u, 0, 2, false, false, nullptr, "design_loft", "Loft (skin) between two or more profiles"},
    {"thicken", "Thicken", 1, nullptr, "fly:material#4", "Thicken needs a solid body — add or import one first", 0x0000000au, 1, 0, false, false, nullptr, "design_thicken", "Offset a solid face into a thin plate (new body)"},
    {"rib", "Rib", 1, nullptr, "fly:material#5", "Rib needs a solid body — add or import one first", 0x00010000u, 1, 0, false, false, nullptr, "design_rib", "Grow a thin wall from an open sketch line, fused to a body"},
    {"boolean", "Union", 1, "Shift+B", "btn:bool#0", "Boolean needs two bodies — create or import a second solid", 0x00000200u, 2, 0, false, false, nullptr, "design_boolean", "Fuse the tool body into the target — one solid, no seam"},
    {"bool_subtract", "Subtract", 1, nullptr, "btn:bool#1", "Boolean needs two bodies — create or import a second solid", 0x00000200u, 2, 0, false, false, nullptr, "design_boolean", "Cut the tool body out of the target"},
    {"bool_intersect", "Intersect", 1, nullptr, "btn:bool#2", "Boolean needs two bodies — create or import a second solid", 0x00000200u, 2, 0, false, false, nullptr, "design_boolean", "Keep only where the two bodies overlap"},
    {"surf_extrude", "Surface Extrude", 1, "Shift+G", "key:S+G", "Create a sketch first", 0x00004000u, 0, 0, false, false, nullptr, "design_extrude", "Extrude a sketch into a sheet body (no end caps)"},
    {"surf_revolve", "Surface Revolve", 1, nullptr, "fly:surface#1", "Create a sketch profile to revolve first", 0x00004000u, 0, 0, false, false, nullptr, "design_revolve", "Revolve a sketch profile into a sheet body"},
    {"surf_loft", "Surface Loft", 1, nullptr, "fly:surface#2", "Create at least two profile sketches to loft", 0x00004000u, 0, 2, false, false, nullptr, "design_loft", "Loft (skin) between 2+ profiles, open (no end caps)"},
    {"surf_fill", "Surface Fill", 1, nullptr, "fly:surface#3", "Create a closed sketch first", 0x00004000u, 0, 0, false, false, nullptr, "design_surface", "Fill a sketch boundary with a smooth face"},
    {"thicken_surf", "Thicken Surface", 1, nullptr, "fly:surface#5", "target is not a sheet body", 0x00000100u, 0, 0, true, false, nullptr, "design_thicken", "Thicken a sheet body into a solid"},
    {"hole", "Hole", 2, "Shift+H", "key:S+H", "Pick a face or a plane to drill into", 0x00000402u, 1, 0, false, false, nullptr, "design_hole", "Drill a hole, centred on a picked face or placed on a plane"},
    {"thread", "Thread", 2, "Shift+T", "key:S+T", "Pick a cylindrical surface (bore / outer) or a circular edge for a thread", 0x00000024u, 1, 0, false, false, nullptr, "design_thread", "Thread a cylindrical surface (inner bore / outer) or a circular edge"},
    {"shell", "Shell", 2, "Shift+K", "key:S+K", "Shell needs a solid body", 0x00000082u, 1, 0, false, false, nullptr, "design_shell", "Hollow the body to a wall thickness, opening a picked face"},
    {"cut", "Cut", 2, "Shift+X", "key:S+X", "Create a solid body to cut first", 0x000004feu, 1, 0, false, false, nullptr, "design_cut", "Trim the body with a plane — drag the offset arrow; keep one half or both"},
    {"split", "Split", 2, nullptr, nullptr, "Split needs a solid body", 0x000000feu, 1, 0, false, false, nullptr, nullptr, "Split the body along a picked face into two solids"},
    {"fillet", "Fillet", 3, "Shift+F", "btn:dress#0", "Pick an edge to round", 0x000000b2u, 1, 0, false, false, nullptr, "design_filletedge", "Pick an edge, then drag the radius arrow or type it"},
    {"chamfer", "Chamfer", 3, nullptr, "btn:dress#1", "Pick an edge to bevel", 0x000000b2u, 1, 0, false, false, nullptr, "design_chamfer", "Pick an edge, then drag the distance arrow or type it"},
    {"draft", "Draft", 3, "Shift+D", "key:S+D", "Pick a face to taper", 0x0000000au, 1, 0, false, false, nullptr, "design_draft", "Tilt a picked face by a draft angle"},
    {"surf_offset", "Surface Offset", 3, nullptr, "fly:surface#4", "target is not a sheet body", 0x00000100u, 0, 0, true, false, nullptr, "design_offset", "Offset a sheet body's shell by a signed distance"},
    {"pattern", "Linear pattern", 4, "Shift+N", "btn:pat#0", "Create a solid body to pattern first", 0x00006082u, 1, 0, false, false, nullptr, "design_array", "Repeat the body along a direction — drag the spacing, set the count"},
    {"pattern_circular", "Circular pattern", 4, nullptr, "btn:pat#1", "Create a solid body to pattern first", 0x00006082u, 1, 0, false, false, nullptr, "design_polararray", "Repeat the body around an axis — set the count and sweep"},
    {"mirror", "Mirror", 4, "Shift+Z", "key:S+Z", "Mirror needs a body — add or import one first", 0x000004feu, 1, 0, false, false, nullptr, "design_mirror", "Reflect a body about a plane"},
    {"pat_curve", "Pattern on Curve", 4, nullptr, nullptr, "Pattern on curve needs a body and a curve", 0x00000090u, 1, 0, false, false, nullptr, nullptr, "Repeat the body along a picked curve"},
    {"transform", "Move", 5, "Shift+Y", "key:S+Y", "Transform needs a body — add or import one first", 0x000021feu, 1, 0, false, false, nullptr, "design_move", "Move and/or rotate an existing body"},
    {"mate", "Mate", 5, nullptr, "fly:placement#2", "A mate needs two coordinate systems", 0x00001202u, 2, 0, false, false, nullptr, "design_c_coincident", "Assembly: align two CoordSys features (fastened, planar, revolute, slider, cylindrical)"},
    {"align", "Align to", 5, nullptr, nullptr, "Align needs a body", 0x00000002u, 1, 0, false, false, nullptr, nullptr, "Align the body to a picked face or plane"},
    {"plane", "Plane", 6, "Shift+P", "key:S+P", nullptr, 0x00000453u, 0, 0, false, false, nullptr, "design_plane", "Reference plane (offset / tilt / midplane / tangent / two edges / coincident)"},
    {"axis", "Axis", 6, "Shift+A", "key:S+A", nullptr, 0x00000057u, 0, 0, false, false, nullptr, "design_line", "Datum axis (two points, face normal, cylinder centerline, two planes, along edge)"},
    {"coordsys_v", "Coord Sys", 6, "Shift+C", "key:S+C", nullptr, 0x00000043u, 0, 0, false, false, nullptr, "design_point", "Datum coordinate system (world point, or face + direction edge)"},
    {"helix", "Helix", 6, nullptr, "fly:plane#3", nullptr, 0x00000405u, 0, 0, false, false, nullptr, "design_thread", "Helical curve (spring path) — use as a sweep path for coils / springs / augers"},
    {"project", "Project", 6, nullptr, "fly:plane#4", "Project needs a body — add or import one first", 0x00000482u, 1, 0, false, false, nullptr, "design_sketch", "Project body edges onto a plane as sketch entities"},
    {"measure", "Measure", 6, nullptr, nullptr, nullptr, 0x000b03feu, 0, 0, false, false, nullptr, nullptr, "Measure between the picked points, edges or faces"},
    {"mass_props", "Mass", 6, nullptr, "btn:mass", nullptr, 0x000000feu, 1, 0, false, false, nullptr, "info", "Report the volume and surface area of the selected body"},
    {"interference", "Interference", 6, nullptr, nullptr, nullptr, 0x00000200u, 2, 0, false, false, nullptr, nullptr, "Check whether two bodies overlap — reports, changes nothing"},
    {"edit_feature", "Edit", 7, nullptr, "btn:edit", nullptr, 0x00007d8eu, 0, 0, false, false, nullptr, "design_edit", "Reopen the selected feature to change what it was made from"},
    {"delete_face", "Delete Face", 7, nullptr, "fly:dressup#3", "Delete Face needs a body — add or import one first", 0x0000000eu, 1, 0, false, false, nullptr, "design_delete", "Remove faces from a body and heal the solid"},
    {"colour", "Colour", 7, nullptr, "btn:colour", nullptr, 0x000001feu, 1, 0, false, false, nullptr, "color_palette", "Set the selected body's display colour"},
    {"delete", "Delete", 7, "Del", "btn:delete", nullptr, 0x000f7c00u, 0, 0, false, false, nullptr, "design_delete", "Delete what is selected"},
    {"delete_body", "Delete Body", 7, nullptr, "btn:delete_body", nullptr, 0x000001feu, 1, 0, false, false, nullptr, "design_delete", "Delete this whole body — removes the feature it was made from"},
    {"sk_line_t", "Line", 0, "L", "key:L", nullptr, 0x000f8000u, 0, 0, false, true, "Line", "design_line", "Line — click start, then end"},
    {"sk_polyline", "Polyline", 0, nullptr, "fly:design_line#1", nullptr, 0x000f8000u, 0, 0, false, true, "Line", "design_polyline", "Click points; click first / right-click to close the loop"},
    {"sk_rect", "Corner rectangle", 0, "R", "key:R", nullptr, 0x000f8000u, 0, 0, false, true, "Rectangle", "design_rect", "Rectangle — click two opposite corners"},
    {"sk_rect_center", "Centre rectangle", 0, nullptr, "fly:design_rect#1", nullptr, 0x000f8000u, 0, 0, false, true, "Rectangle", "design_crect", "Click center, then a corner"},
    {"sk_rect_oblique", "Oblique rectangle", 0, nullptr, "fly:design_rect#2", nullptr, 0x000f8000u, 0, 0, false, true, "Rectangle", "design_rect_oblique", "Click two corners of one edge, then a point for the width"},
    {"sk_rect_rounded", "Rounded rectangle", 0, nullptr, "fly:design_rect#3", nullptr, 0x000f8000u, 0, 0, false, true, "Rectangle", "design_rect_rounded", "Click two opposite corners, then a point for the corner radius"},
    {"sk_circle", "Centre circle", 0, "C", "key:C", nullptr, 0x000f8000u, 0, 0, false, true, "Circle", "design_circle", "Circle — click center, then radius"},
    {"sk_circle_2pt", "2-point circle", 0, nullptr, "fly:design_circle#1", nullptr, 0x000f8000u, 0, 0, false, true, "Circle", "design_circle2pt", "Click two ends of the diameter"},
    {"sk_circle_3pt", "3-point circle", 0, nullptr, "fly:design_circle#2", nullptr, 0x000f8000u, 0, 0, false, true, "Circle", "design_circle3pt", "Click three points on the circle"},
    {"sk_arc_t", "3-point arc", 0, "A", "key:A", nullptr, 0x000f8000u, 0, 0, false, true, "Arc", "design_arc3pt", "Arc — click start, end, then a point"},
    {"sk_arc_tangent", "Tangent arc", 0, nullptr, "fly:design_arc3pt#1", nullptr, 0x000f8000u, 0, 0, false, true, "Arc", "design_tangentarc", "Click start (on the last entity) then end"},
    {"sk_arc_center", "Centre-point arc", 0, nullptr, "fly:design_arc3pt#2", nullptr, 0x000f8000u, 0, 0, false, true, "Arc", "design_arc_center", "Click center, then start, then a point for the end angle"},
    {"sk_slot", "Slot", 0, "S", "key:S", nullptr, 0x000f8000u, 0, 0, false, true, "Slot", "design_slot", "Slot — two centerline ends, then end radius"},
    {"sk_slot_arc", "Arc slot", 0, nullptr, "fly:design_slot#1", nullptr, 0x000f8000u, 0, 0, false, true, "Slot", "design_slot_arc", "Click center, start, end, then a point for the width"},
    {"sk_ellipse", "Ellipse", 0, "E", "key:E", nullptr, 0x000f8000u, 0, 0, false, true, "Ellipse", "design_ellipse", "Ellipse — center, major end, minor point"},
    {"sk_ellipse_arc", "Elliptical arc", 0, nullptr, "fly:design_ellipse#1", nullptr, 0x000f8000u, 0, 0, false, true, "Ellipse", "design_ellipse_arc", "Click center, major-axis end, minor point, then arc start and end"},
    {"sk_spline", "Spline", 0, "B", "key:B", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_bspline", "Spline — click control points"},
    {"sk_poly_3", "Triangle", 0, nullptr, "btn:poly#3", nullptr, 0x000f8000u, 0, 0, false, true, "Polygon", "design_polygon", "Triangle — click centre, then a vertex"},
    {"sk_poly_4", "Square", 0, nullptr, "btn:poly#4", nullptr, 0x000f8000u, 0, 0, false, true, "Polygon", "design_polygon", "Square — click centre, then a vertex"},
    {"sk_poly_5", "Pentagon", 0, nullptr, "btn:poly#5", nullptr, 0x000f8000u, 0, 0, false, true, "Polygon", "design_polygon", "Pentagon — click centre, then a vertex"},
    {"sk_polygon", "Hexagon", 0, "G", "btn:poly#6", nullptr, 0x000f8000u, 0, 0, false, true, "Polygon", "design_polygon", "Hexagon — click centre, then a vertex"},
    {"sk_poly_8", "Octagon", 0, nullptr, "btn:poly#8", nullptr, 0x000f8000u, 0, 0, false, true, "Polygon", "design_polygon", "Octagon — click centre, then a vertex"},
    {"sk_poly_12", "Dodecagon", 0, nullptr, "btn:poly#12", nullptr, 0x000f8000u, 0, 0, false, true, "Polygon", "design_polygon", "Dodecagon — click centre, then a vertex"},
    {"sk_poly_inscribed", "Inscribed", 0, nullptr, "btn:polyfit#0", nullptr, 0x000f8000u, 0, 0, false, true, "Polygon", "design_polygon", "Measure the polygon to its corners (inscribed)"},
    {"sk_poly_circumscribed", "Circumscribed", 0, nullptr, "btn:polyfit#1", nullptr, 0x000f8000u, 0, 0, false, true, "Polygon", "design_polygon", "Measure the polygon to its flats (circumscribed)"},
    {"sk_point_t", "Point", 0, "P", "key:P", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_point", "Point — click to place"},
    {"sk_text", "Text", 0, nullptr, "btn:text", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_text", "Type text; its outline is added to this sketch as editable lines"},
    {"sk_svg", "SVG", 0, nullptr, "btn:svg", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_svg", "Import an SVG outline into this sketch as editable lines"},
    {"sk_offset", "Offset", 1, "O", "key:O", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_offset", "Offset — pick an entity, drag the distance"},
    {"sk_trim", "Trim", 2, "T", "key:T", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_trim", "Trim — click a segment to trim it"},
    {"sk_fillet", "Fillet", 3, "F", "key:F", nullptr, 0x00090000u, 0, 0, false, true, nullptr, "design_filletedge", "Fillet — pick two lines, set the radius"},
    {"sk_chamfer", "Chamfer", 3, "H", "key:H", nullptr, 0x00090000u, 0, 0, false, true, nullptr, "design_chamfer", "Chamfer — pick two lines, set the distance"},
    {"sk_array", "Linear array", 4, nullptr, "fly:design_array#0", nullptr, 0x000b0000u, 0, 0, false, true, "Array", "design_array", "Pick entities, drag the spacing handle, click the count; click empty to apply"},
    {"sk_array_polar", "Polar array", 4, nullptr, "fly:design_array#1", nullptr, 0x000b0000u, 0, 0, false, true, "Array", "design_polararray", "Pick entities, drag the sweep handle, click the count; click empty to apply"},
    {"sk_mirror", "Mirror", 4, "M", "key:M", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_mirror", "Mirror — pick axis, then entities"},
    {"sk_move", "Move", 5, nullptr, "fly:design_move#0", nullptr, 0x000f0000u, 0, 0, false, true, "Move", "design_move", "Pick entities, then drag the handle or click the distance; click empty to apply"},
    {"sk_rotate", "Rotate", 5, nullptr, "fly:design_move#1", nullptr, 0x000f0000u, 0, 0, false, true, "Move", "design_rotate", "Pick entities, then drag around the pivot or click the angle; click empty to apply"},
    {"sk_scale", "Scale", 5, nullptr, "fly:design_move#2", nullptr, 0x000f0000u, 0, 0, false, true, "Move", "design_scale", "Pick entities, then drag the handle or click the factor; click empty to apply"},
    {"sk_dimension", "Dimension", 6, "D", "key:D", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_dimension", "Dimension — click 2 points or an entity"},
    {"sk_constrain", "Constrain", 6, "K", "key:K", nullptr, 0x000f0000u, 0, 0, false, true, nullptr, "design_constrain", "Constrain the selected sketch entities to each other"},
    // Same verb, model-mode vocabulary: offered when a SKETCH is selected (bit 14, SkLoop), the
    // state a user is in right after finishing one. Without this row the only way in was the
    // toolbar icon, and constraints read as absent — see the Onshape-comparison report.
    {"constrain", "Constrain sketch", 7, nullptr, "btn:constrain", "Select a sketch to constrain it", 0x00004000u, 0, 1, false, false, nullptr, "design_constrain", "Add dimensions and relations (coincident, tangent, parallel...) to the selected sketch"},
    {"sk_construct", "Construction", 6, "Q", "key:Q", nullptr, 0x000b8000u, 0, 0, false, true, nullptr, nullptr, "Toggle construction: geometry that guides but is never built"},
    {"sk_extend", "Extend", 7, "X", "key:X", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_extend", "Extend — click a line/arc to extend it"},
    {"sk_delete", "Delete", 7, "Del", "btn:delete", nullptr, 0x000f0000u, 0, 0, false, true, nullptr, "design_delete", "Delete the selected sketch entities"},
};
static const int kOfferVerbCount = 87;

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_DesignOffer_hpp_
