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
};

// Row labels, in ratified order.
static const char* const kOfferRowNames[] = {
    "Create",
    "Add material",
    "Remove",
    "Dress-up",
    "Repeat",
    "Transform",
    "Reference",
    "Modify",
};
static const int kOfferRowCount = 8;

static const OfferVerb kOfferVerbs[] = {
    {"sketch", "Sketch", 0, "Shift+S", "key:S+S", "Click a face or a reference plane in the viewport, then a sketch tool", 0x00000403u, 0, 0, false, false, nullptr},
    {"extrude", "Extrude", 1, "Shift+E", "key:S+E", "Create a sketch, or pick a solid face, first", 0x00004002u, 0, 0, false, false, nullptr},
    {"revolve", "Revolve", 1, "Shift+R", "key:S+R", "Create a sketch profile to revolve first", 0x00004000u, 0, 0, false, false, nullptr},
    {"sweep", "Sweep", 1, "Shift+W", "key:S+W", "Create a profile sketch to sweep first", 0x00004000u, 0, 2, false, false, nullptr},
    {"loft", "Loft", 1, "Shift+L", "key:S+L", "Create at least two profile sketches to loft", 0x00004000u, 0, 2, false, false, nullptr},
    {"thicken", "Thicken", 1, nullptr, "fly:material#4", "Thicken needs a solid body — add or import one first", 0x0000000au, 1, 0, false, false, nullptr},
    {"rib", "Rib", 1, nullptr, "fly:material#5", "Rib needs a solid body — add or import one first", 0x00010000u, 1, 0, false, false, nullptr},
    {"boolean", "Combine", 1, "Shift+B", "key:S+B", "Boolean needs two bodies — create or import a second solid", 0x00000200u, 2, 0, false, false, nullptr},
    {"surf_extrude", "Surface Extrude", 1, "Shift+G", "key:S+G", "Create a sketch first", 0x00004000u, 0, 0, false, false, nullptr},
    {"surf_revolve", "Surface Revolve", 1, nullptr, "fly:surface#1", "Create a sketch profile to revolve first", 0x00004000u, 0, 0, false, false, nullptr},
    {"surf_loft", "Surface Loft", 1, nullptr, "fly:surface#2", "Create at least two profile sketches to loft", 0x00004000u, 0, 2, false, false, nullptr},
    {"surf_fill", "Surface Fill", 1, nullptr, "fly:surface#3", "Create a closed sketch first", 0x00004000u, 0, 0, false, false, nullptr},
    {"thicken_surf", "Thicken Surface", 1, nullptr, "fly:surface#5", "target is not a sheet body", 0x00000100u, 0, 0, true, false, nullptr},
    {"hole", "Hole", 2, "Shift+H", "key:S+H", "Pick a face or a plane to drill into", 0x00000402u, 1, 0, false, false, nullptr},
    {"thread", "Thread", 2, "Shift+T", "key:S+T", "Pick a cylindrical surface (bore / outer) or a circular edge for a thread", 0x00000024u, 1, 0, false, false, nullptr},
    {"shell", "Shell", 2, "Shift+K", "key:S+K", "Shell needs a solid body", 0x00000082u, 1, 0, false, false, nullptr},
    {"cut", "Cut", 2, "Shift+X", "key:S+X", "Create a solid body to cut first", 0x00000480u, 1, 0, false, false, nullptr},
    {"split", "Split", 2, nullptr, nullptr, "Split needs a solid body", 0x00000080u, 1, 0, false, false, nullptr},
    {"fillet", "Fillet", 3, "Shift+F", "key:S+F", "Pick an edge to round", 0x000000b2u, 1, 0, false, false, nullptr},
    {"chamfer", "Chamfer", 3, nullptr, "key:S+F", "Pick an edge to bevel", 0x000000b2u, 1, 0, false, false, nullptr},
    {"draft", "Draft", 3, "Shift+D", "key:S+D", "Pick a face to taper", 0x0000000au, 1, 0, false, false, nullptr},
    {"surf_offset", "Surface Offset", 3, nullptr, "fly:surface#4", "target is not a sheet body", 0x00000100u, 0, 0, true, false, nullptr},
    {"pattern", "Pattern", 4, "Shift+N", "key:S+N", "Create a solid body to pattern first", 0x00006082u, 1, 0, false, false, nullptr},
    {"mirror", "Mirror", 4, "Shift+Z", "key:S+Z", "Mirror needs a body — add or import one first", 0x00000480u, 1, 0, false, false, nullptr},
    {"pat_curve", "Pattern on Curve", 4, nullptr, nullptr, "Pattern on curve needs a body and a curve", 0x00000090u, 1, 0, false, false, nullptr},
    {"transform", "Move", 5, "Shift+Y", "key:S+Y", "Transform needs a body — add or import one first", 0x00002180u, 1, 0, false, false, nullptr},
    {"mate", "Mate", 5, nullptr, "fly:placement#2", "A mate needs two coordinate systems", 0x00001202u, 2, 0, false, false, nullptr},
    {"align", "Align to", 5, nullptr, nullptr, "Align needs a body", 0x00000002u, 1, 0, false, false, nullptr},
    {"plane", "Plane", 6, "Shift+P", "key:S+P", nullptr, 0x00000453u, 0, 0, false, false, nullptr},
    {"axis", "Axis", 6, "Shift+A", "key:S+A", nullptr, 0x00000057u, 0, 0, false, false, nullptr},
    {"coordsys_v", "Coord Sys", 6, "Shift+C", "key:S+C", nullptr, 0x00000043u, 0, 0, false, false, nullptr},
    {"helix", "Helix", 6, nullptr, "fly:plane#3", nullptr, 0x00000405u, 0, 0, false, false, nullptr},
    {"project", "Project", 6, nullptr, "fly:plane#4", "Project needs a body — add or import one first", 0x00000482u, 1, 0, false, false, nullptr},
    {"measure", "Measure", 6, nullptr, nullptr, nullptr, 0x000b03feu, 0, 0, false, false, nullptr},
    {"mass_props", "Mass", 6, nullptr, "btn:mass", nullptr, 0x00000080u, 1, 0, false, false, nullptr},
    {"interference", "Interference", 6, nullptr, nullptr, nullptr, 0x00000200u, 2, 0, false, false, nullptr},
    {"edit_feature", "Edit", 7, nullptr, "btn:edit", nullptr, 0x00007d8eu, 0, 0, false, false, nullptr},
    {"delete_face", "Delete Face", 7, nullptr, "fly:dressup#3", "Delete Face needs a body — add or import one first", 0x0000000eu, 1, 0, false, false, nullptr},
    {"colour", "Colour", 7, nullptr, "btn:colour", nullptr, 0x00000180u, 1, 0, false, false, nullptr},
    {"delete", "Delete", 7, "Del", "btn:delete", nullptr, 0x000f7f80u, 0, 0, false, false, nullptr},
    {"sk_line_t", "Line", 0, "L", "key:L", nullptr, 0x000f8000u, 0, 0, false, true, "Line"},
    {"sk_polyline", "Polyline", 0, nullptr, "fly:design_line#1", nullptr, 0x000f8000u, 0, 0, false, true, "Line"},
    {"sk_rect", "Corner rectangle", 0, "R", "key:R", nullptr, 0x000f8000u, 0, 0, false, true, "Rectangle"},
    {"sk_rect_center", "Centre rectangle", 0, nullptr, "fly:design_rect#1", nullptr, 0x000f8000u, 0, 0, false, true, "Rectangle"},
    {"sk_rect_oblique", "Oblique rectangle", 0, nullptr, "fly:design_rect#2", nullptr, 0x000f8000u, 0, 0, false, true, "Rectangle"},
    {"sk_rect_rounded", "Rounded rectangle", 0, nullptr, "fly:design_rect#3", nullptr, 0x000f8000u, 0, 0, false, true, "Rectangle"},
    {"sk_circle", "Centre circle", 0, "C", "key:C", nullptr, 0x000f8000u, 0, 0, false, true, "Circle"},
    {"sk_circle_2pt", "2-point circle", 0, nullptr, "fly:design_circle#1", nullptr, 0x000f8000u, 0, 0, false, true, "Circle"},
    {"sk_circle_3pt", "3-point circle", 0, nullptr, "fly:design_circle#2", nullptr, 0x000f8000u, 0, 0, false, true, "Circle"},
    {"sk_arc_t", "3-point arc", 0, "A", "key:A", nullptr, 0x000f8000u, 0, 0, false, true, "Arc"},
    {"sk_arc_tangent", "Tangent arc", 0, nullptr, "fly:design_arc3pt#1", nullptr, 0x000f8000u, 0, 0, false, true, "Arc"},
    {"sk_arc_center", "Centre-point arc", 0, nullptr, "fly:design_arc3pt#2", nullptr, 0x000f8000u, 0, 0, false, true, "Arc"},
    {"sk_slot", "Slot", 0, "S", "key:S", nullptr, 0x000f8000u, 0, 0, false, true, "Slot"},
    {"sk_slot_arc", "Arc slot", 0, nullptr, "fly:design_slot#1", nullptr, 0x000f8000u, 0, 0, false, true, "Slot"},
    {"sk_ellipse", "Ellipse", 0, "E", "key:E", nullptr, 0x000f8000u, 0, 0, false, true, "Ellipse"},
    {"sk_ellipse_arc", "Elliptical arc", 0, nullptr, "fly:design_ellipse#1", nullptr, 0x000f8000u, 0, 0, false, true, "Ellipse"},
    {"sk_spline", "Spline", 0, "B", "key:B", nullptr, 0x000f8000u, 0, 0, false, true, nullptr},
    {"sk_polygon", "Polygon", 0, "G", "key:G", nullptr, 0x000f8000u, 0, 0, false, true, nullptr},
    {"sk_point_t", "Point", 0, "P", "key:P", nullptr, 0x000f8000u, 0, 0, false, true, nullptr},
    {"sk_offset", "Offset", 1, "O", "key:O", nullptr, 0x000b0000u, 0, 0, false, true, nullptr},
    {"sk_trim", "Trim", 2, "T", "key:T", nullptr, 0x000b0000u, 0, 0, false, true, nullptr},
    {"sk_fillet", "Fillet", 3, "F", "key:F", nullptr, 0x00090000u, 0, 0, false, true, nullptr},
    {"sk_chamfer", "Chamfer", 3, "H", "key:H", nullptr, 0x00090000u, 0, 0, false, true, nullptr},
    {"sk_array", "Linear array", 4, nullptr, "fly:design_array#0", nullptr, 0x000b0000u, 0, 0, false, true, "Array"},
    {"sk_array_polar", "Polar array", 4, nullptr, "fly:design_array#1", nullptr, 0x000b0000u, 0, 0, false, true, "Array"},
    {"sk_mirror", "Mirror", 4, "M", "key:M", nullptr, 0x000b0000u, 0, 0, false, true, nullptr},
    {"sk_move", "Move", 5, nullptr, "fly:design_move#0", nullptr, 0x000f0000u, 0, 0, false, true, "Move"},
    {"sk_rotate", "Rotate", 5, nullptr, "fly:design_move#1", nullptr, 0x000f0000u, 0, 0, false, true, "Move"},
    {"sk_scale", "Scale", 5, nullptr, "fly:design_move#2", nullptr, 0x000f0000u, 0, 0, false, true, "Move"},
    {"sk_dimension", "Dimension", 6, "D", "key:D", nullptr, 0x000f8000u, 0, 0, false, true, nullptr},
    {"sk_constrain", "Constrain", 6, "K", "key:K", nullptr, 0x000f0000u, 0, 0, false, true, nullptr},
    {"sk_construct", "Construction", 6, "Q", "key:Q", nullptr, 0x000b8000u, 0, 0, false, true, nullptr},
    {"sk_extend", "Extend", 7, "X", "key:X", nullptr, 0x000b0000u, 0, 0, false, true, nullptr},
    {"sk_delete", "Delete", 7, "Del", "btn:delete", nullptr, 0x000f0000u, 0, 0, false, true, nullptr},
};
static const int kOfferVerbCount = 74;

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_DesignOffer_hpp_
