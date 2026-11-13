// GENERATED FILE — DO NOT EDIT.
// Source: scripts/CAD/tool_atlas.json   Generator: scripts/CAD/gen_offer_table.py
//
// The Design tab's table. Its verbs are the object-driven tool offer (charter 4.1): every verb
// has ONE row index, that index is the same in every selection it appears in, and verbs that do
// not apply are shown disabled in place with their reason rather than removed. Row order was
// ratified 2026-07-31; changing an index is a breaking change to every user's muscle memory.
// Every entry but align is also a command of the catalog's group Design (Mcp::register_cad_commands()):
// called with no arguments, one with an action does what its menu row does; otherwise, and always for
// an entry with no action, it runs its tool block's silent form. An entry with no row is a command only.
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
// DesignTool::accepts indexes these.
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

// The atlas's id of each selection, in OfferSel's order.
static const char* const kDesignSelectionIds[] = {
    "none",
    "face_planar",
    "face_cyl",
    "face_other",
    "edge_str",
    "edge_circ",
    "vertex",
    "body_solid",
    "body_sheet",
    "bodies_2",
    "datum_plane",
    "datum_axis",
    "coordsys",
    "art",
    "sk_loop",
    "sk_none",
    "sk_line",
    "sk_arc",
    "sk_point",
    "sk_2ent",
};

// What a Design command's silent form does to the document, which decides when the Design tab lets
// it run (DesignPanel::mcp_busy()). Read only looks, so it runs under anything but a rebuild. Sketch
// drives the live sketch session and Change edits the feature list or variables; both are refused under
// an open card, the Text dialog, Constrain mode or a body Move, and Change under a sketch session too,
// which is about to commit into the feature list. None: an entry with no silent form, and the opening
// of a tool, which only a rebuild refuses, as the Design tab's keys and menu do.
enum class CadAccess { None, Read, Change, Sketch };

// One entry of the table. `action` routes to the code that already implements the verb:
//   "key:S+E"        -> m_keys_feature[SHIFT('E')]
//   "key:L"          -> m_keys_sketch['L']
//   "fly:material#4" -> row 4 of the "material" feature flyout
//   "btn:delete"     -> a standalone toolbar button
//   nullptr          -> no GUI path (the row, if it has one, shows disabled)
struct DesignTool {
    const char* id;          // also the key of its Design command
    const char* name;        // drawing-office word (L10); msgctxt "Design", translated at use
    int         row;         // index into kOfferRowNames, the ratified address — NEVER reorder;
                             // -1: no menu row, a command only
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
    bool        modal;        // its action opens a dialog that waits for an answer: a modal one, which holds
                              // the call until it is answered, or Text's, which is modeless and does not
    CadAccess   access;       // its silent form's, None without one
    bool        ok_false;     // its silent form reports a rejected rebuild in its reply (ok false)
    // Its silent form's summary, details, params, examples and ids as JSON text, the parameter
    // schema of record; nullptr for an entry whose command only does what its row does.
    const char* doc;
};

// Row labels, in ratified order.
static const char* const kOfferRowNames[] = {
    L_CONTEXT("Create", "Design"),
    L_CONTEXT("Add material", "Design"),
    L_CONTEXT("Remove", "Design"),
    L_CONTEXT("Fillet / chamfer / draft", "Design"),
    L_CONTEXT("Repeat", "Design"),
    L_CONTEXT("Transform", "Design"),
    L_CONTEXT("Reference", "Design"),
    L_CONTEXT("Modify", "Design"),
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

static const DesignTool kDesignTools[] = {
    {"sketch", L_CONTEXT("Sketch", "Design"), 0, "Shift+S", "key:S+S", L("Select a flat face or a reference plane to sketch on"), 0x00000403u, 0, 0, false, false, nullptr, "design_sketch", L("Sketch on the selected flat face or plane, or click one next"), false, CadAccess::None, false,
     nullptr},
    {"extrude", L_CONTEXT("Extrude", "Design"), 1, "Shift+E", "key:S+E", L("Create a sketch, or pick a solid face, first"), 0x00004002u, 0, 0, false, false, nullptr, "design_extrude", L("Extrude a sketch profile, or push/pull a picked face"), false, CadAccess::Change, false,
     "{\"summary\":\"Extrude a committed sketch, one of its loops, a face or a drawn profile into a body."
     "\",\"details\":\"A committed sketch, or one loop of it, extrudes as the Extrude card does; push/pull"
     " grows a planar face of a body; a profile drawn here is an explicit closed `profile` (list of [x"
     ",y]) or a centred width x height rectangle on `plane`. End conditions match Onshape. Reply: extr"
     "ude_index, the index of its sketch (-1 for a face), the boolean and body used, end, frame.\",\"par"
     "ams\":[{\"name\":\"sketch\",\"type\":\"integer\",\"default\":null,\"description\":\"a committed Sketch feature"
     " to extrude (sketch_commit's sketch_index); profile, width, height and plane are then refused\"},"
     "{\"name\":\"loop\",\"type\":\"integer\",\"default\":null,\"description\":\"with sketch: one closed loop (clos"
     "ed_loops[].index of sketch_describe {sketch}) with the loops inside it as holes; a hole is a loo"
     "p of its own, so its index extrudes the disc. Omit for the whole sketch\"},{\"name\":\"face\",\"type\":"
     "\"integer\",\"default\":null,\"description\":\"push/pull: a planar face (query_topology id) of `body` g"
     "rown along its outward normal (flip for inward); boolean auto joins that body. sketch, loop, pro"
     "file, width, height, plane and taper are then refused, and end is blind or symmetric\"},{\"name\":\""
     "width\",\"type\":\"number\",\"unit\":\"mm\",\"default\":20},{\"name\":\"height\",\"type\":\"number\",\"unit\":\"mm\",\"d"
     "efault\":20},{\"name\":\"distance\",\"type\":\"number\",\"unit\":\"mm\",\"default\":10,\"exclusive_min\":0},{\"nam"
     "e\":\"plane\",\"type\":\"string|object\",\"default\":\"XY\",\"description\":\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through"
     " modeling_origin; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\": b, \\\"face\\\": f}, a planar face "
     "(origin at its centroid, normal out of the body, x from the world axes). The reply's frame gives"
     " the axes.\"},{\"name\":\"profile\",\"type\":\"array\",\"default\":[],\"description\":\"optional closed contou"
     "r [[x,y],...] in plane mm; overrides width/height\"},{\"name\":\"boolean\",\"type\":\"string\",\"default\":"
     "null,\"enum\":[\"auto\",\"new\",\"union\",\"subtract\",\"intersect\"],\"description\":\"auto joins the body the"
     " profile touches, else makes a new body, as the card's default; default auto with sketch, new wi"
     "thout\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"the body to join, subtract f"
     "rom or intersect, or whose face is pushed; omit for the body a sketch touches, else the last bod"
     "y\"},{\"name\":\"end\",\"type\":\"string\",\"default\":\"blind\",\"enum\":[\"blind\",\"symmetric\",\"two_sided\",\"thr"
     "ough_all\",\"up_to_face\"]},{\"name\":\"distance2\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0,\"descriptio"
     "n\":\"second-side depth when end=two_sided (else falls back to distance)\"},{\"name\":\"up_to_face\",\"t"
     "ype\":\"integer\",\"default\":-1,\"description\":\"face id (query_topology) of the body the extrude work"
     "s on, when end=up_to_face\"},{\"name\":\"taper\",\"type\":\"number\",\"unit\":\"deg\",\"default\":0,\"descriptio"
     "n\":\"draft/taper of the side wall\"},{\"name\":\"flip\",\"type\":\"boolean\",\"default\":false}],\"examples\":"
     "[{\"title\":\"A 20 x 20 x 5 mm plate on the XY plane\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\""
     ":5,\"height\":20,\"width\":20}}]},{\"title\":\"A committed sketch extruded 5 mm: a rectangle around a c"
     "ircle makes a plate with a bore\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":"
     "\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"center\""
     ":[0,0],\"radius\":4,\"type\":\"circle\"}]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"extrude\",\"args"
     "\":{\"distance\":5,\"sketch\":0}}]},{\"title\":\"A plate's top face pushed up 3 mm\",\"calls\":[{\"tool\":\"ex"
     "trude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"query_topology\",\"args\":{\"at_extrem"
     "e\":\"+z\",\"body\":0}},{\"tool\":\"extrude\",\"args\":{\"body\":0,\"distance\":3,\"face\":5,\"generation\":{\"from_"
     "call\":1,\"key\":\"generation\"}}}]}],\"ids\":true}"},
    {"revolve", L_CONTEXT("Revolve", "Design"), 1, "Shift+R", "key:S+R", L("Create a sketch profile to revolve first"), 0x00004000u, 0, 0, false, false, nullptr, "design_revolve", L("Revolve a profile about an axis"), false, CadAccess::Change, false,
     "{\"summary\":\"Revolve a committed sketch as the Revolve card does, or a profile drawn here, about "
     "an axis.\",\"details\":\"A sketch turns about a line of it or a plane axis; a profile drawn here is "
     "an explicit `profile` offset from the axis, or a width x height rectangle with one edge on the a"
     "xis, centred along it, so a full turn is a cylinder width long of radius height. Reply: revolve_"
     "index, the index of its sketch, the boolean, body and axis used, frame.\",\"params\":[{\"name\":\"sket"
     "ch\",\"type\":\"integer\",\"default\":null,\"description\":\"a committed Sketch feature to revolve (sketch"
     "_commit's sketch_index); profile, width, height and plane are then refused\"},{\"name\":\"axis_entit"
     "y\",\"type\":\"integer\",\"default\":null,\"description\":\"with sketch: a line of the sketch (entity inde"
     "x) as the axis, or -1 for the plane axis; omit for the sketch's only centerline (construction li"
     "ne) when it has one and axis is not given\"},{\"name\":\"width\",\"type\":\"number\",\"unit\":\"mm\",\"default"
     "\":20,\"description\":\"without sketch or profile: the rectangle's length along the axis\"},{\"name\":\""
     "height\",\"type\":\"number\",\"unit\":\"mm\",\"default\":10,\"description\":\"without sketch or profile: how f"
     "ar the rectangle reaches out from the axis, the solid's radius\"},{\"name\":\"angle\",\"type\":\"number\""
     ",\"unit\":\"deg\",\"default\":360},{\"name\":\"axis\",\"type\":\"integer\",\"default\":0,\"enum\":[0,1],\"descripti"
     "on\":\"the plane's X (0) or Y (1) axis through its origin\"},{\"name\":\"flip\",\"type\":\"boolean\",\"defau"
     "lt\":false},{\"name\":\"plane\",\"type\":\"string|object\",\"default\":\"XY\",\"description\":\"\\\"XY\\\", \\\"XZ\\\" o"
     "r \\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\": b, \\\"face\\\": f}"
     ", a planar face (origin at its centroid, normal out of the body, x from the world axes). The rep"
     "ly's frame gives the axes.\"},{\"name\":\"profile\",\"type\":\"array\",\"default\":[],\"description\":\"option"
     "al closed contour [[x,y],...] in plane mm; overrides width/height\"},{\"name\":\"boolean\",\"type\":\"st"
     "ring\",\"default\":\"new\",\"enum\":[\"new\",\"union\",\"subtract\",\"intersect\"]},{\"name\":\"body\",\"type\":\"inte"
     "ger\",\"default\":-1,\"description\":\"the body to join, subtract from or intersect; omit for the last"
     " body\"}],\"examples\":[{\"title\":\"A ring: a profile 10 to 15 mm out from the XZ plane's vertical ax"
     "is, turned a full circle\",\"calls\":[{\"tool\":\"revolve\",\"args\":{\"axis\":1,\"plane\":\"XZ\",\"profile\":[[1"
     "0,0],[15,0],[15,10],[10,10]]}}]},{\"title\":\"A committed sketch turned about its centerline, a con"
     "struction line\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XZ\"}},{\"tool\":\"sketch_add\",\"arg"
     "s\":{\"rect\":[10,0,15,10]}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"construction\":true,\"p0\":[0,"
     "0],\"p1\":[0,10],\"type\":\"line\"}]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"revolve\",\"args\":{\"s"
     "ketch\":0}}]}],\"ids\":true}"},
    {"sweep", L_CONTEXT("Sweep", "Design"), 1, "Shift+W", "key:S+W", L("Create a profile sketch to sweep first"), 0x00004000u, 0, 2, false, false, nullptr, "design_sweep", L("Sweep a profile along a path"), false, CadAccess::Change, false,
     "{\"summary\":\"Sweep a committed sketch's closed profile along a path, a sketch or a helix, as the "
     "Sweep card does.\",\"details\":\"Draw the profile across the path's start (a helix's start and tange"
     "nt are in its reply); along a helix the profile keeps its angle to the helix axis. Long sweeps t"
     "ake seconds and the reply may be a job. Reply: sweep_index, bodies, boolean, body.\",\"params\":[{\""
     "name\":\"sketch\",\"type\":\"integer\",\"description\":\"the profile: a committed sketch's feature index\"}"
     ",{\"name\":\"path\",\"type\":\"integer\",\"description\":\"feature index of a sketch or a helix, other than"
     " the profile\"},{\"name\":\"boolean\",\"type\":\"string\",\"default\":\"new\",\"enum\":[\"new\",\"union\",\"subtract"
     "\",\"intersect\"]},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"the body a union, su"
     "btract or intersect works on; omit for the last body\"}],\"examples\":[{\"title\":\"A spring: a 2 mm c"
     "ircle across a helix's start, swept along it\",\"calls\":[{\"tool\":\"helix\",\"args\":{\"height\":20,\"pitc"
     "h\":5,\"radius\":10}},{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XZ\"}},{\"tool\":\"sketch_add\",\"args\":{\"e"
     "ntities\":[{\"center\":[10,0],\"radius\":1,\"type\":\"circle\"}]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"t"
     "ool\":\"sweep\",\"args\":{\"path\":0,\"sketch\":1}}]},{\"title\":\"A 6 mm rod swept along a 40 mm line\",\"cal"
     "ls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"p0"
     "\":[0,0],\"p1\":[0,40],\"type\":\"line\"}]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"sketch_begin\","
     "\"args\":{\"plane\":\"XZ\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":3,\"type"
     "\":\"circle\"}]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"sweep\",\"args\":{\"path\":0,\"sketch\":1}}]"
     "}],\"ids\":false}"},
    {"loft", L_CONTEXT("Loft", "Design"), 1, "Shift+L", "key:S+L", L("Create at least two profile sketches to loft"), 0x00004000u, 0, 2, false, false, nullptr, "design_loft", L("Loft (skin) between two or more profiles"), false, CadAccess::Change, false,
     "{\"summary\":\"Loft a solid through two or more committed sketches, in order, as the Loft card does"
     ".\",\"details\":\"Each sketch is on its own plane (sketch on datum planes). Lofts take seconds and t"
     "he reply may be a job. Reply: loft_index, bodies, boolean, body.\",\"params\":[{\"name\":\"profiles\",\""
     "type\":\"array\",\"description\":\"sketch feature indices, in loft order\"},{\"name\":\"ruled\",\"type\":\"boo"
     "lean\",\"default\":false,\"description\":\"straight sections between the profiles instead of a smooth "
     "skin\"},{\"name\":\"boolean\",\"type\":\"string\",\"default\":\"new\",\"enum\":[\"new\",\"union\",\"subtract\",\"inter"
     "sect\"]},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"the body a union, subtract o"
     "r intersect works on; omit for the last body\"}],\"examples\":[{\"title\":\"A square-to-round transiti"
     "on 20 mm tall, the circle sketched on a datum plane\",\"calls\":[{\"tool\":\"plane\",\"args\":{\"base\":\"XY"
     "\",\"offset\":20,\"type\":\"offset\"}},{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_ad"
     "d\",\"args\":{\"rect\":[-10,-10,10,10]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"sketch_begin\",\"a"
     "rgs\":{\"plane\":{\"datum\":0}}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":6,"
     "\"type\":\"circle\"}]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"loft\",\"args\":{\"profiles\":[1,2]}}"
     "]}],\"ids\":false}"},
    {"thicken", L_CONTEXT("Thicken", "Design"), 1, nullptr, "fly:material#4", L("Thicken needs a solid body — add or import one first"), 0x0000000au, 1, 0, false, false, nullptr, "design_thicken", L("Offset a solid face into a thin plate (new body)"), false, CadAccess::Change, true,
     "{\"summary\":\"Offset a face of a body by a wall thickness, producing a new thin solid body.\",\"deta"
     "ils\":\"\",\"params\":[{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"target body; omit "
     "for the last body\"},{\"name\":\"face\",\"type\":\"integer\",\"description\":\"face id to thicken (query_top"
     "ology)\"},{\"name\":\"thickness\",\"type\":\"number\",\"unit\":\"mm\",\"default\":2},{\"name\":\"flip\",\"type\":\"boo"
     "lean\",\"default\":false}],\"examples\":[{\"title\":\"A 2 mm skin grown from a plate's top face as a new"
     " body\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"query_t"
     "opology\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"thicken\",\"args\":{\"body\":0,\"face\":5,\"gener"
     "ation\":{\"from_call\":1,\"key\":\"generation\"},\"thickness\":2}}]}],\"ids\":true}"},
    {"rib", L_CONTEXT("Rib", "Design"), 1, "R", "fly:material#5", L("Rib needs a solid body — add or import one first"), 0x00010000u, 1, 0, false, false, nullptr, "design_rib", L("Grow a thin wall from an open sketch line, fused to a body"), false, CadAccess::Change, true,
     "{\"summary\":\"Grow a thin rib wall (stiffener) from an open Line sketch entity, fused to a body.\","
     "\"details\":\"The wall is centred on the line in the sketch plane, thickness wide, and grows depth "
     "along the sketch plane's normal.\",\"params\":[{\"name\":\"sketch\",\"type\":\"integer\",\"description\":\"ske"
     "tch feature index holding the open line\"},{\"name\":\"entity\",\"type\":\"integer\",\"description\":\"entit"
     "y index of the open Line within the sketch\"},{\"name\":\"thickness\",\"type\":\"number\",\"unit\":\"mm\",\"de"
     "fault\":2},{\"name\":\"depth\",\"type\":\"number\",\"unit\":\"mm\",\"default\":10,\"description\":\"along the sket"
     "ch plane's normal, or against it when negative\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"d"
     "escription\":\"target body; omit for the last body\"}],\"examples\":[{\"title\":\"A 2 mm rib 8 mm tall a"
     "cross a plate's top face\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":3,\"height\":20,\"width\":40"
     "}},{\"tool\":\"query_topology\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"sketch_begin\",\"args\":{"
     "\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"plane\":{\"body\":0,\"face\":5}}},{\"tool\":\"sketch_ad"
     "d\",\"args\":{\"entities\":[{\"p0\":[-15,0],\"p1\":[15,0],\"type\":\"line\"}]}},{\"tool\":\"sketch_commit\",\"args"
     "\":{}},{\"tool\":\"rib\",\"args\":{\"depth\":8,\"entity\":0,\"sketch\":2,\"thickness\":2}}]}],\"ids\":false}"},
    {"boolean", L_CONTEXT("Join", "Design"), 1, "Shift+B", "btn:bool#0", L("Boolean needs two bodies — add or import a second one"), 0x00000200u, 2, 0, false, false, nullptr, "design_boolean", L("Fuse the tool body into the target — one solid, no seam"), false, CadAccess::Change, true,
     "{\"summary\":\"Combine two bodies: union | subtract (tool from target) | intersect.\",\"details\":\"\",\""
     "params\":[{\"name\":\"op\",\"type\":\"string\",\"default\":\"subtract\",\"enum\":[\"union\",\"subtract\",\"intersect"
     "\"]},{\"name\":\"target\",\"type\":\"integer\",\"default\":0},{\"name\":\"tool\",\"type\":\"integer\",\"default\":1},"
     "{\"name\":\"keep_tool\",\"type\":\"boolean\",\"default\":false},{\"name\":\"tolerance\",\"type\":\"number\",\"unit\""
     ":\"mm\",\"default\":0}],\"examples\":[{\"title\":\"A post subtracted from a block\",\"calls\":[{\"tool\":\"extr"
     "ude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"extrude\",\"args\":{\"boolean\":\"new\",\"d"
     "istance\":20,\"height\":6,\"width\":6}},{\"tool\":\"boolean\",\"args\":{\"op\":\"subtract\",\"target\":0,\"tool\":1"
     "}}]},{\"title\":\"Two overlapping blocks joined into one body\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\""
     "distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"extrude\",\"args\":{\"boolean\":\"new\",\"distance\":10,\"p"
     "rofile\":[[5,-5],[20,-5],[20,5],[5,5]]}},{\"tool\":\"boolean\",\"args\":{\"op\":\"union\",\"target\":0,\"tool\""
     ":1}}]}],\"ids\":false}"},
    {"bool_subtract", L_CONTEXT("Subtract", "Design"), 1, nullptr, "btn:bool#1", L("Boolean needs two bodies — add or import a second one"), 0x00000200u, 2, 0, false, false, nullptr, "design_boolean", L("Cut the tool body out of the target"), false, CadAccess::None, false,
     nullptr},
    {"bool_intersect", L_CONTEXT("Intersect", "Design"), 1, nullptr, "btn:bool#2", L("Boolean needs two bodies — add or import a second one"), 0x00000200u, 2, 0, false, false, nullptr, "design_boolean", L("Keep only where the two bodies overlap"), false, CadAccess::None, false,
     nullptr},
    {"surf_extrude", L_CONTEXT("Surface Extrude", "Design"), 1, "Shift+G", "key:S+G", L("Create a sketch first"), 0x00004000u, 0, 0, false, false, nullptr, "design_extrude", L("Extrude a sketch into a sheet body (no end caps)"), false, CadAccess::Change, true,
     "{\"summary\":\"Extrude a sketch wire with no end caps -> an open sheet body.\",\"details\":\"\",\"params\""
     ":[{\"name\":\"sketch\",\"type\":\"integer\",\"description\":\"sketch feature index\"},{\"name\":\"distance\",\"ty"
     "pe\":\"number\",\"unit\":\"mm\",\"default\":10,\"description\":\"along the sketch plane's normal, or against"
     " it when negative\"}],\"examples\":[{\"title\":\"A 30 mm line extruded 10 mm into a flat sheet\",\"calls"
     "\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"p0\":"
     "[-15,0],\"p1\":[15,0],\"type\":\"line\"}]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"surf_extrude\","
     "\"args\":{\"distance\":10,\"sketch\":0}}]},{\"title\":\"A circle extruded 20 mm into an open tube, with n"
     "o end caps\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{"
     "\"entities\":[{\"center\":[0,0],\"radius\":10,\"type\":\"circle\"}]}},{\"tool\":\"sketch_commit\",\"args\":{}},{"
     "\"tool\":\"surf_extrude\",\"args\":{\"distance\":20,\"sketch\":0}}]}],\"ids\":false}"},
    {"surf_revolve", L_CONTEXT("Surface Revolve", "Design"), 1, nullptr, "fly:surface#1", L("Create a sketch profile to revolve first"), 0x00004000u, 0, 0, false, false, nullptr, "design_revolve", L("Revolve a sketch profile into a sheet body"), false, CadAccess::Change, true,
     "{\"summary\":\"Revolve a sketch wire with no caps -> an open sheet body.\",\"details\":\"\",\"params\":[{\""
     "name\":\"sketch\",\"type\":\"integer\",\"description\":\"sketch feature index\"},{\"name\":\"angle\",\"type\":\"nu"
     "mber\",\"unit\":\"deg\",\"default\":360},{\"name\":\"axis\",\"type\":\"integer\",\"default\":0,\"enum\":[0,1],\"desc"
     "ription\":\"the sketch plane's X (0) or Y (1) axis through its origin\"}],\"examples\":[{\"title\":\"A c"
     "ylinder's wall: a line 10 mm out from the XZ plane's vertical axis, turned a full circle\",\"calls"
     "\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XZ\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"p0\":"
     "[10,0],\"p1\":[10,20],\"type\":\"line\"}]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"surf_revolve\","
     "\"args\":{\"axis\":1,\"sketch\":0}}]},{\"title\":\"Half a truncated cone's wall: a sloping line turned 18"
     "0 degrees about the XZ plane's vertical axis\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"X"
     "Z\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"p0\":[5,0],\"p1\":[15,20],\"type\":\"line\"}]}},{\"tool\""
     ":\"sketch_commit\",\"args\":{}},{\"tool\":\"surf_revolve\",\"args\":{\"angle\":180,\"axis\":1,\"sketch\":0}}]}],"
     "\"ids\":false}"},
    {"surf_loft", L_CONTEXT("Surface Loft", "Design"), 1, nullptr, "fly:surface#2", L("Create at least two profile sketches to loft"), 0x00004000u, 0, 2, false, false, nullptr, "design_loft", L("Loft (skin) between 2+ profiles, open (no end caps)"), false, CadAccess::Change, true,
     "{\"summary\":\"Skin two or more profile sketches with no end caps -> an open sheet body.\",\"details\""
     ":\"\",\"params\":[{\"name\":\"profiles\",\"type\":\"array\",\"description\":\"sketch feature indices, in order\""
     "},{\"name\":\"ruled\",\"type\":\"boolean\",\"default\":false}],\"examples\":[{\"title\":\"An open funnel skinne"
     "d from a circle on XY to a wider one on a datum plane 20 mm up\",\"calls\":[{\"tool\":\"plane\",\"args\":"
     "{\"base\":\"XY\",\"offset\":20,\"type\":\"offset\"}},{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\""
     ":\"sketch_add\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":5,\"type\":\"circle\"}]}},{\"tool\":\"sketch"
     "_commit\",\"args\":{}},{\"tool\":\"sketch_begin\",\"args\":{\"plane\":{\"datum\":0}}},{\"tool\":\"sketch_add\",\"a"
     "rgs\":{\"entities\":[{\"center\":[0,0],\"radius\":15,\"type\":\"circle\"}]}},{\"tool\":\"sketch_commit\",\"args\""
     ":{}},{\"tool\":\"surf_loft\",\"args\":{\"profiles\":[1,2]}}]}],\"ids\":false}"},
    {"surf_fill", L_CONTEXT("Surface Fill", "Design"), 1, nullptr, "fly:surface#3", L("Create a closed sketch first"), 0x00004000u, 0, 0, false, false, nullptr, "design_surface", L("Fill a sketch boundary with a smooth face"), false, CadAccess::Change, true,
     "{\"summary\":\"Fill a sketch's closed boundary with one smooth face -> a sheet body.\",\"details\":\"\","
     "\"params\":[{\"name\":\"sketch\",\"type\":\"integer\",\"description\":\"sketch feature index\"}],\"examples\":[{"
     "\"title\":\"A circle filled with a disc-shaped sheet\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plan"
     "e\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":10,\"type\":\"circle\"}]"
     "}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"surf_fill\",\"args\":{\"sketch\":0}}]},{\"title\":\"A rec"
     "tangle filled with a flat sheet\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":"
     "\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"surf_f"
     "ill\",\"args\":{\"sketch\":0}}]}],\"ids\":false}"},
    {"thicken_surf", L_CONTEXT("Thicken Surface", "Design"), 1, nullptr, "fly:surface#5", L("Thicken Surface needs a surface body — make one with a Surface tool first"), 0x00000100u, 0, 0, true, false, nullptr, "design_thicken", L("Thicken a sheet body into a solid"), false, CadAccess::Change, true,
     "{\"summary\":\"Thicken a whole sheet body into a solid.\",\"details\":\"A sheet of several faces whose "
     "free rims close into planar loops (the walls of a box with no lids) is capped at them and grows "
     "inward, into the closed shape that makes, whatever flip says; one whose rim can't be capped is r"
     "efused. A single face or a closed shell is offset face by face along each face's normal, the one"
     " query_topology gives, or against it with flip; a closed shell can fail to thicken this way.\",\"p"
     "arams\":[{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"sheet body; omit for the las"
     "t body\"},{\"name\":\"thickness\",\"type\":\"number\",\"unit\":\"mm\",\"default\":2},{\"name\":\"flip\",\"type\":\"boo"
     "lean\",\"default\":false}],\"examples\":[{\"title\":\"A 30 x 20 mm sheet thickened into a 2 mm plate, a "
     "new body\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"r"
     "ect\":[-15,-10,15,10]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"surf_fill\",\"args\":{\"sketch\":0"
     "}},{\"tool\":\"thicken_surf\",\"args\":{\"thickness\":2}}]},{\"title\":\"An open tube thickened 1.5 mm outw"
     "ard into a pipe: flip, as its normal points inward\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"pla"
     "ne\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":10,\"type\":\"circle\"}"
     "]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"surf_extrude\",\"args\":{\"distance\":20,\"sketch\":0}}"
     ",{\"tool\":\"thicken_surf\",\"args\":{\"body\":0,\"flip\":true,\"thickness\":1.5}}]}],\"ids\":false}"},
    {"hole", L_CONTEXT("Hole", "Design"), 2, "Shift+H", "key:S+H", L("Pick a face or a plane to drill into"), 0x00000402u, 1, 0, false, false, nullptr, "design_hole", L("Drill a hole, centered on a picked face or placed on a plane"), false, CadAccess::Change, false,
     "{\"summary\":\"Drill a circular hole at (x,y) or `at` on a plane.\",\"details\":\"Reply: hole_index, bo"
     "dy, x and y in the frame, off_plane with at, frame.\",\"params\":[{\"name\":\"diameter\",\"type\":\"number"
     "\",\"unit\":\"mm\",\"default\":5,\"exclusive_min\":0},{\"name\":\"depth\",\"type\":\"number\",\"unit\":\"mm\",\"defaul"
     "t\":10},{\"name\":\"through\",\"type\":\"boolean\",\"default\":false},{\"name\":\"x\",\"type\":\"number\",\"unit\":\"m"
     "m\",\"default\":0,\"description\":\"offset in the plane's frame from its origin, NOT world\"},{\"name\":\""
     "y\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0,\"description\":\"offset in the plane's frame from its o"
     "rigin, NOT world\"},{\"name\":\"plane\",\"type\":\"string|object\",\"default\":\"XY\",\"description\":\"\\\"XY\\\", "
     "\\\"XZ\\\" or \\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\": b, \\\"fa"
     "ce\\\": f}, a planar face to drill into that body (origin at its centroid, normal into the body, x"
     " along its longest edge). The reply's frame gives the axes.\"},{\"name\":\"at\",\"type\":\"array|object\""
     ",\"default\":null,\"description\":\"instead of x and y: a world point [x, y, z], projected onto the p"
     "lane along its normal (off_plane says how far), or {\\\"body\\\": b, \\\"face\\\": f} a cylindrical face"
     " or {\\\"body\\\": b, \\\"edge\\\": e} a circular edge whose axis the hole shares. A world point moves w"
     "ith modeling_origin; x and y replay on any printer\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-"
     "1,\"description\":\"the body to drill; omit for a face plane's body, else the last body\"}],\"example"
     "s\":[{\"title\":\"A 5 mm hole through the middle of a plate\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"dis"
     "tance\":5,\"height\":20,\"width\":20}},{\"tool\":\"hole\",\"args\":{\"diameter\":5,\"through\":true}}]},{\"title"
     "\":\"A 4 mm hole 3 mm deep into a plate's top face, 5 mm off centre\",\"calls\":[{\"tool\":\"extrude\",\"a"
     "rgs\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"query_topology\",\"args\":{\"at_extreme\":\"+z\",\""
     "body\":0}},{\"tool\":\"hole\",\"args\":{\"depth\":3,\"diameter\":4,\"generation\":{\"from_call\":1,\"key\":\"gener"
     "ation\"},\"plane\":{\"body\":0,\"face\":5},\"x\":5}}]}],\"ids\":true}"},
    {"thread", L_CONTEXT("Thread", "Design"), 2, "Shift+T", "key:S+T", L("Pick a cylindrical surface (bore / outer) or a circular edge for a thread"), 0x00000024u, 1, 0, false, false, nullptr, "design_thread", L("Thread a cylindrical surface (inner bore / outer) or a circular edge"), false, CadAccess::Change, false,
     "{\"summary\":\"Cut a thread as the Thread card does, along a cylindrical face or a circular edge, o"
     "r on a plane.\",\"details\":\"On a face a bore is tapped, a boss threaded; on a plane the thread goe"
     "s at (x,y) or `at`. Without designation, a face or edge takes the nearest standard size, ISO met"
     "ric and UTS alike. Long threads take seconds and the reply may be a job. Reply: thread_index, de"
     "signation (null when custom), internal, diameter, pitch, depth, length, body, x, y, frame.\",\"par"
     "ams\":[{\"name\":\"face\",\"type\":\"integer\",\"default\":null,\"description\":\"a cylindrical face id of `bo"
     "dy` (query_topology); the thread runs its length from its lower end\"},{\"name\":\"edge\",\"type\":\"int"
     "eger\",\"default\":null,\"description\":\"instead of face: a circular edge id of `body`, the thread ri"
     "sing along its normal; needs length\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\""
     ":\"the body of face or edge, or the body threaded on a plane; omit for the last body (on a plane,"
     " a face plane's body)\"},{\"name\":\"designation\",\"type\":\"string\",\"default\":null,\"description\":\"e.g."
     " M5, M8x1, 1/4-20 UNC: sets diameter, pitch and depth\"},{\"name\":\"internal\",\"type\":\"boolean\",\"def"
     "ault\":null,\"description\":\"true taps a hole, false threads a rod; required on a plane, else from "
     "the face (a bore is internal), external for an edge\"},{\"name\":\"diameter\",\"type\":\"number\",\"unit\":"
     "\"mm\",\"default\":null,\"description\":\"nominal (major) diameter, replacing the standard's; with pitc"
     "h, a custom thread\"},{\"name\":\"pitch\",\"type\":\"number\",\"unit\":\"mm\",\"default\":null,\"description\":\"r"
     "eplaces the standard's\"},{\"name\":\"depth\",\"type\":\"number\",\"unit\":\"mm\",\"default\":null,\"description"
     "\":\"radial depth; default the 60 degree V's for the pitch and role\"},{\"name\":\"length\",\"type\":\"num"
     "ber\",\"unit\":\"mm\",\"default\":null,\"description\":\"default the face's length; required for an edge o"
     "r a plane\"},{\"name\":\"plane\",\"type\":\"string|object\",\"default\":null,\"nullable\":false,\"description\""
     ":\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\"
     "\": b, \\\"face\\\": f}, a planar face to drill into that body (origin at its centroid, normal into t"
     "he body, x along its longest edge). The reply's frame gives the axes.\"},{\"name\":\"x\",\"type\":\"numb"
     "er\",\"unit\":\"mm\",\"default\":0,\"description\":\"on a plane: offset in the plane's frame from its orig"
     "in, NOT world\"},{\"name\":\"y\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0,\"description\":\"on a plane: o"
     "ffset in the plane's frame from its origin, NOT world\"},{\"name\":\"at\",\"type\":\"array|object\",\"defa"
     "ult\":null,\"description\":\"on a plane, instead of x and y: a world point [x, y, z], or {\\\"body\\\": "
     "b, \\\"face\\\": f} a cylindrical face or {\\\"body\\\": b, \\\"edge\\\": e} a circular edge whose axis the "
     "thread shares, as hole takes it\"}],\"examples\":[{\"title\":\"An M8 thread tapped into a 6.8 mm hole "
     "through a block\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"too"
     "l\":\"hole\",\"args\":{\"diameter\":6.8,\"through\":true}},{\"tool\":\"query_topology\",\"args\":{\"body\":0,\"kin"
     "ds\":[\"cylindrical\"]}},{\"tool\":\"thread\",\"args\":{\"body\":0,\"designation\":\"M8\",\"face\":6,\"generation\""
     ":{\"from_call\":2,\"key\":\"generation\"}}}]},{\"title\":\"An M6 threaded rod 10 mm long, standing on the"
     " XY plane\",\"calls\":[{\"tool\":\"thread\",\"args\":{\"designation\":\"M6\",\"internal\":false,\"length\":10,\"pl"
     "ane\":\"XY\"}}]},{\"title\":\"A 10 mm boss threaded at the nearest standard size, which the reply's de"
     "signation names\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"ar"
     "gs\":{\"entities\":[{\"center\":[0,0],\"radius\":5,\"type\":\"circle\"}]}},{\"tool\":\"sketch_commit\",\"args\":{"
     "}},{\"tool\":\"extrude\",\"args\":{\"distance\":12,\"sketch\":0}},{\"tool\":\"query_topology\",\"args\":{\"body\":"
     "0,\"kinds\":[\"cylindrical\"]}},{\"tool\":\"thread\",\"args\":{\"body\":0,\"face\":0,\"generation\":{\"from_call\""
     ":4,\"key\":\"generation\"}}}]}],\"ids\":true}"},
    {"shell", L_CONTEXT("Shell", "Design"), 2, "Shift+K", "key:S+K", L("Shell needs a solid body — add or import one first"), 0x00000082u, 1, 0, false, false, nullptr, "design_shell", L("Hollow the body to a wall thickness, opening a picked face"), false, CadAccess::Change, true,
     "{\"summary\":\"Hollow a body to a wall thickness (inward); optionally leave one face open.\",\"detail"
     "s\":\"\",\"params\":[{\"name\":\"thickness\",\"type\":\"number\",\"unit\":\"mm\",\"default\":1,\"exclusive_min\":0},{"
     "\"name\":\"face\",\"type\":\"integer\",\"default\":-1,\"description\":\"face id to leave open (query_topology"
     "); omit for a closed hollow\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"target"
     " body; omit for the last body\"}],\"examples\":[{\"title\":\"A 20 x 20 x 10 mm box hollowed to 1.5 mm "
     "walls, open at the top\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}"
     "},{\"tool\":\"query_topology\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"shell\",\"args\":{\"body\":0"
     ",\"face\":5,\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"thickness\":1.5}}]},{\"title\":\"A block "
     "hollowed to closed 2 mm walls\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"wid"
     "th\":20}},{\"tool\":\"shell\",\"args\":{\"thickness\":2}}]}],\"ids\":true}"},
    {"cut", L_CONTEXT("Cut", "Design"), 2, "Shift+X", "key:S+X", L("Cut needs a solid body — add or import one first"), 0x000004feu, 1, 0, false, false, nullptr, "design_cut", L("Trim the body with a plane — drag the offset arrow; keep one half or both"), false, CadAccess::None, false,
     nullptr},
    {"split", L_CONTEXT("Split", "Design"), 2, nullptr, nullptr, L("Split needs a solid body — add or import one first"), 0x000000feu, 1, 0, false, false, nullptr, nullptr, L("Split the body along a picked face into two solids"), false, CadAccess::Change, false,
     "{\"summary\":\"Split a body with a plane, as the Cut card does, moved by offset and turned over by "
     "flip; or along the plane of a face.\",\"details\":\"The upper piece is on the plane's normal side; k"
     "eeping both makes two bodies, the lower one right after the upper. Reply: split_index, bodies, b"
     "ody, upper_body and lower_body (the pieces kept), frame (a plane's, after offset and flip).\",\"pa"
     "rams\":[{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"target body; omit for the las"
     "t body\"},{\"name\":\"plane\",\"type\":\"string|object\",\"default\":null,\"description\":\"\\\"XY\\\", \\\"XZ\\\" or "
     "\\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\": b, \\\"face\\\": f}, "
     "a planar face (origin at its centroid, normal out of the body, x from the world axes). The reply"
     "'s frame gives the axes.\"},{\"name\":\"offset\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0,\"description"
     "\":\"with plane: along its normal\"},{\"name\":\"flip\",\"type\":\"boolean\",\"default\":false,\"description\":"
     "\"with plane: turn it over, swapping upper and lower\"},{\"name\":\"face\",\"type\":\"integer\",\"default\":"
     "null,\"description\":\"instead of plane: a face id (query_topology) to split along\"},{\"name\":\"face_"
     "body\",\"type\":\"integer\",\"default\":-1,\"description\":\"with face: the body that owns it; -1 = target"
     "\"},{\"name\":\"keep_upper\",\"type\":\"boolean\",\"default\":true},{\"name\":\"keep_lower\",\"type\":\"boolean\",\""
     "default\":true}],\"examples\":[{\"title\":\"A block cut in two at half its height\",\"calls\":[{\"tool\":\"e"
     "xtrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"split\",\"args\":{\"offset\":5,\"plane"
     "\":\"XY\"}}]},{\"title\":\"A block cut at half its height, keeping only the lower half\",\"calls\":[{\"too"
     "l\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"split\",\"args\":{\"keep_upper\""
     ":false,\"offset\":5,\"plane\":\"XY\"}}]},{\"title\":\"A block split along the top face of a wider plate\","
     "\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"extrude\",\"arg"
     "s\":{\"boolean\":\"new\",\"distance\":4,\"height\":30,\"width\":30}},{\"tool\":\"query_topology\",\"args\":{\"at_e"
     "xtreme\":\"+z\",\"body\":1}},{\"tool\":\"split\",\"args\":{\"body\":0,\"face\":5,\"face_body\":1,\"generation\":{\"f"
     "rom_call\":2,\"key\":\"generation\"}}}]}],\"ids\":true}"},
    {"fillet", L_CONTEXT("Fillet", "Design"), 3, "Shift+F", "btn:dress#0", L("Pick an edge to round"), 0x000000b2u, 1, 0, false, false, nullptr, "design_filletedge", L("Pick an edge, then drag the radius arrow or type it"), false, CadAccess::Change, true,
     "{\"summary\":\"Round measured edges of a body (edge ids from query_topology on that body).\",\"detail"
     "s\":\"\",\"params\":[{\"name\":\"edge\",\"type\":\"integer|array\",\"description\":\"one edge id, or an array of"
     " ids rounded together in one feature\"},{\"name\":\"radius\",\"type\":\"number\",\"unit\":\"mm\",\"default\":1,"
     "\"exclusive_min\":0},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"target body; omit"
     " for the last body. edge id is resolved against THIS body.\"}],\"examples\":[{\"title\":\"A plate's fo"
     "ur vertical corners rounded to 2 mm\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20"
     ",\"width\":20}},{\"tool\":\"query_topology\",\"args\":{\"body\":0,\"convex\":true,\"direction\":[0,0,1],\"edge_"
     "kinds\":[\"line\"]}},{\"tool\":\"fillet\",\"args\":{\"body\":0,\"edge\":[0,1,4,7],\"generation\":{\"from_call\":1"
     ",\"key\":\"generation\"},\"radius\":2}}]},{\"title\":\"One top edge of a plate rounded to 1 mm\",\"calls\":["
     "{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"query_topology\",\"args\":"
     "{\"adjacency\":true,\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"fillet\",\"args\":{\"body\":0,\"edge\":3,\"gener"
     "ation\":{\"from_call\":1,\"key\":\"generation\"},\"radius\":1}}]}],\"ids\":true}"},
    {"chamfer", L_CONTEXT("Chamfer", "Design"), 3, nullptr, "btn:dress#1", L("Pick an edge to bevel"), 0x000000b2u, 1, 0, false, false, nullptr, "design_chamfer", L("Pick an edge, then drag the distance arrow or type it"), false, CadAccess::Change, true,
     "{\"summary\":\"Chamfer measured edges of a body (edge ids from query_topology on that body).\",\"deta"
     "ils\":\"\",\"params\":[{\"name\":\"edge\",\"type\":\"integer|array\",\"description\":\"one edge id, or an array "
     "of ids chamfered together in one feature\"},{\"name\":\"distance\",\"type\":\"number\",\"unit\":\"mm\",\"defau"
     "lt\":1,\"exclusive_min\":0},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"target body"
     "; omit for the last body. edge id is resolved against THIS body.\"}],\"examples\":[{\"title\":\"A plat"
     "e's top edges chamfered 1 mm\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width"
     "\":20}},{\"tool\":\"query_topology\",\"args\":{\"adjacency\":true,\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"c"
     "hamfer\",\"args\":{\"body\":0,\"distance\":1,\"edge\":[3,6,9,11],\"generation\":{\"from_call\":1,\"key\":\"gener"
     "ation\"}}}]}],\"ids\":true}"},
    {"draft", L_CONTEXT("Draft", "Design"), 3, "Shift+D", "key:S+D", L("Pick a face to taper"), 0x0000000au, 1, 0, false, false, nullptr, "design_draft", L("Tilt a picked face by a draft angle"), false, CadAccess::Change, true,
     "{\"summary\":\"Taper a body face by an angle about its base (pull direction +Z).\",\"details\":\"\",\"par"
     "ams\":[{\"name\":\"face\",\"type\":\"integer\",\"description\":\"face id to draft (query_topology)\"},{\"name\""
     ":\"angle\",\"type\":\"number\",\"unit\":\"deg\",\"default\":5},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,"
     "\"description\":\"target body; omit for the last body\"}],\"examples\":[{\"title\":\"A block's +X side ta"
     "pered 5 degrees\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"too"
     "l\":\"query_topology\",\"args\":{\"body\":0,\"direction\":[1,0,0]}},{\"tool\":\"draft\",\"args\":{\"angle\":5,\"bo"
     "dy\":0,\"face\":1,\"generation\":{\"from_call\":1,\"key\":\"generation\"}}}]}],\"ids\":true}"},
    {"surf_offset", L_CONTEXT("Surface Offset", "Design"), 3, nullptr, "fly:surface#4", L("Surface Offset needs a surface body — make one with a Surface tool first"), 0x00000100u, 0, 0, true, false, nullptr, "design_offset", L("Offset a sheet body's shell by a signed distance"), false, CadAccess::Change, true,
     "{\"summary\":\"Offset a sheet body by a signed distance -> another sheet body.\",\"details\":\"A positi"
     "ve offset moves each face along its normal, the one query_topology gives, and a negative one aga"
     "inst it, so the side depends on how the sheet was made: an extruded circle's normal points inwar"
     "d.\",\"params\":[{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"sheet body; omit for t"
     "he last body\"},{\"name\":\"offset\",\"type\":\"number\",\"unit\":\"mm\",\"default\":1}],\"examples\":[{\"title\":\""
     "A rectangular sheet copied 5 mm above itself\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"X"
     "Y\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\""
     "tool\":\"surf_fill\",\"args\":{\"sketch\":0}},{\"tool\":\"surf_offset\",\"args\":{\"offset\":5}}]},{\"title\":\"An"
     " open tube of radius 10 mm offset -2 mm, against its inward normal, to one of radius 12 mm\",\"cal"
     "ls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"ce"
     "nter\":[0,0],\"radius\":10,\"type\":\"circle\"}]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"surf_ext"
     "rude\",\"args\":{\"distance\":20,\"sketch\":0}},{\"tool\":\"surf_offset\",\"args\":{\"body\":0,\"offset\":-2}}]}]"
     ",\"ids\":false}"},
    {"pattern", L_CONTEXT("Linear pattern", "Design"), 4, "Shift+N", "btn:pat#0", L("Pattern needs a solid body — add or import one first"), 0x00006082u, 1, 0, false, false, nullptr, "design_array", L("Repeat the body along a direction — drag the spacing, set the count"), false, CadAccess::Change, true,
     "{\"summary\":\"Replicate a body: linear (count along a plane axis at spacing) or circular (count ov"
     "er an angle about the plane normal).\",\"details\":\"\",\"params\":[{\"name\":\"circular\",\"type\":\"boolean\""
     ",\"default\":false},{\"name\":\"count\",\"type\":\"integer\",\"default\":3,\"min\":1},{\"name\":\"spacing\",\"type\""
     ":\"number\",\"unit\":\"mm\",\"default\":10,\"description\":\"linear step\"},{\"name\":\"dir\",\"type\":\"integer\",\""
     "default\":0,\"enum\":[0,1],\"description\":\"linear axis: 0=plane X, 1=plane Y\"},{\"name\":\"angle\",\"type"
     "\":\"number\",\"unit\":\"deg\",\"default\":360,\"description\":\"circular total sweep\"},{\"name\":\"plane\",\"typ"
     "e\":\"string|object\",\"default\":\"XY\",\"description\":\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through modeling_origi"
     "n; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\": b, \\\"face\\\": f}, a planar face (origin at its "
     "centroid, normal out of the body, x from the world axes). The reply's frame gives the axes.\"},{\""
     "name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"target body; omit for the last body\"}]"
     ",\"examples\":[{\"title\":\"Three plates 30 mm apart along X\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"dis"
     "tance\":5,\"height\":20,\"width\":20}},{\"tool\":\"pattern\",\"args\":{\"count\":3,\"spacing\":30}}]},{\"title\":"
     "\"Six blocks in a circle about the XY plane's normal\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distanc"
     "e\":5,\"profile\":[[20,-3],[30,-3],[30,3],[20,3]]}},{\"tool\":\"pattern\",\"args\":{\"circular\":true,\"coun"
     "t\":6}}]}],\"ids\":true}"},
    {"pattern_circular", L_CONTEXT("Circular pattern", "Design"), 4, nullptr, "btn:pat#1", L("Pattern needs a solid body — add or import one first"), 0x00006082u, 1, 0, false, false, nullptr, "design_polararray", L("Repeat the body around an axis — set the count and sweep"), false, CadAccess::None, false,
     nullptr},
    {"mirror", L_CONTEXT("Mirror", "Design"), 4, "Shift+Z", "key:S+Z", L("Mirror needs a body — add or import one first"), 0x000004feu, 1, 0, false, false, nullptr, "design_mirror", L("Reflect a body about a plane"), false, CadAccess::Change, true,
     "{\"summary\":\"Mirror a body about a base plane.\",\"details\":\"mode=new creates a mirrored copy; mode"
     "=add fuses the mirror back into the source.\",\"params\":[{\"name\":\"plane\",\"type\":\"string|object\",\"d"
     "efault\":\"XZ\",\"description\":\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}, a "
     "Plane feature; or {\\\"body\\\": b, \\\"face\\\": f}, a planar face (origin at its centroid, normal out "
     "of the body, x from the world axes). The reply's frame gives the axes.\"},{\"name\":\"mode\",\"type\":\""
     "string\",\"default\":\"new\",\"enum\":[\"new\",\"add\"]},{\"name\":\"keep_original\",\"type\":\"boolean\",\"default\""
     ":true,\"description\":\"when mode=new, keep the source body\"},{\"name\":\"body\",\"type\":\"integer\",\"defa"
     "ult\":-1,\"description\":\"target body; omit for the last body\"}],\"examples\":[{\"title\":\"A mirrored c"
     "opy of a block across the XZ plane\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"profile\":[["
     "-5,5],[5,5],[5,15],[-5,15]]}},{\"tool\":\"mirror\",\"args\":{\"plane\":\"XZ\"}}]},{\"title\":\"A block agains"
     "t the XZ plane and its mirror image fused into one body\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"dis"
     "tance\":5,\"profile\":[[-5,0],[5,0],[5,10],[-5,10]]}},{\"tool\":\"mirror\",\"args\":{\"mode\":\"add\",\"plane\""
     ":\"XZ\"}}]}],\"ids\":true}"},
    {"pat_curve", L_CONTEXT("Pattern on Curve", "Design"), 4, nullptr, nullptr, L("Pattern on Curve needs a body and a curve"), 0x00000090u, 1, 0, false, false, nullptr, nullptr, L("Repeat the body along a picked curve"), false, CadAccess::Change, true,
     "{\"summary\":\"Replicate a body along a sketch curve.\",\"details\":\"`count` copies are placed at equa"
     "l-parameter points on the entity, each translated by (P_i - P_0).\",\"params\":[{\"name\":\"count\",\"ty"
     "pe\":\"integer\",\"default\":3,\"min\":1},{\"name\":\"sketch\",\"type\":\"integer\",\"description\":\"feature inde"
     "x of the sketch holding the guide curve\"},{\"name\":\"entity\",\"type\":\"integer\",\"description\":\"entit"
     "y index of the guide curve within that sketch\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"de"
     "scription\":\"target body; omit for the last body\"}],\"examples\":[{\"title\":\"Four pegs spaced along "
     "a half circle\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args"
     "\":{\"entities\":[{\"center\":[0,0],\"end_angle_deg\":180,\"radius\":30,\"start_angle_deg\":0,\"type\":\"arc\"}"
     "]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"profile\":[[28,-2"
     "],[32,-2],[32,2],[28,2]]}},{\"tool\":\"pat_curve\",\"args\":{\"count\":4,\"entity\":0,\"sketch\":0}}]}],\"ids"
     "\":false}"},
    {"body_transform", L_CONTEXT("Move", "Design"), 5, "Shift+Y", "key:S+Y", L("Transform needs a body — add or import one first"), 0x000021feu, 1, 0, false, false, nullptr, "design_move", L("Move and/or rotate an existing body"), false, CadAccess::Change, true,
     "{\"summary\":\"Move and/or rotate a body (B-rep transform).\",\"details\":\"copy=true keeps the source "
     "and appends the transformed copy as a new body.\",\"params\":[{\"name\":\"body\",\"type\":\"integer\",\"defa"
     "ult\":-1,\"description\":\"target body; omit for the last body\"},{\"name\":\"dx\",\"type\":\"number\",\"unit\""
     ":\"mm\",\"default\":0},{\"name\":\"dy\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0},{\"name\":\"dz\",\"type\":\"nu"
     "mber\",\"unit\":\"mm\",\"default\":0},{\"name\":\"axis_x\",\"type\":\"number\",\"default\":0},{\"name\":\"axis_y\",\"t"
     "ype\":\"number\",\"default\":0},{\"name\":\"axis_z\",\"type\":\"number\",\"default\":1},{\"name\":\"pivot_x\",\"type"
     "\":\"number\",\"unit\":\"mm\",\"default\":0},{\"name\":\"pivot_y\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0},{"
     "\"name\":\"pivot_z\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0},{\"name\":\"angle\",\"type\":\"number\",\"unit\""
     ":\"deg\",\"default\":0},{\"name\":\"copy\",\"type\":\"boolean\",\"default\":false}],\"examples\":[{\"title\":\"A pl"
     "ate moved 30 mm along X\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}"
     "},{\"tool\":\"body_transform\",\"args\":{\"dx\":30}}]},{\"title\":\"A copy of a plate 10 mm above it\",\"call"
     "s\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"body_transform\",\"ar"
     "gs\":{\"copy\":true,\"dz\":10}}]}],\"ids\":false}"},
    {"mate", L_CONTEXT("Mate", "Design"), 5, nullptr, "fly:placement#2", L("Mate needs two coordinate systems — create them first"), 0x00001202u, 2, 0, false, false, nullptr, "design_c_coincident", L("Assembly: align two coordinate systems (fastened, planar, revolute, slider, cylindrical)"), false, CadAccess::Change, true,
     "{\"summary\":\"Mate two bodies: transform the moving body (cs_b) so its connector lands on the fixe"
     "d one (cs_a).\",\"details\":\"kind: 0=Fastened, 1=Planar, 2=Revolute, 3=Slider, 4=Cylindrical.\",\"par"
     "ams\":[{\"name\":\"kind\",\"type\":\"integer\",\"default\":0,\"description\":\"0=Fastened (rigid), 1=Planar (n"
     "ormal only), 2=Revolute (free rotation about axis), 3=Slider (free translation along axis), 4=Cy"
     "lindrical (free rotation+translation)\"},{\"name\":\"cs_a\",\"type\":\"integer\",\"description\":\"feature i"
     "ndex of the fixed CoordSys (mate connector A)\"},{\"name\":\"cs_b\",\"type\":\"integer\",\"description\":\"f"
     "eature index of the CoordSys on the body that moves\"},{\"name\":\"offset\",\"type\":\"number\",\"unit\":\"m"
     "m\",\"default\":0},{\"name\":\"angle\",\"type\":\"number\",\"unit\":\"deg\",\"default\":0},{\"name\":\"flip\",\"type\":"
     "\"boolean\",\"default\":false}],\"examples\":[{\"title\":\"A block stood on a plate: its bottom-face conn"
     "ector fastened, turned over, to the plate's top-face one\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"di"
     "stance\":5,\"height\":20,\"width\":20}},{\"tool\":\"extrude\",\"args\":{\"boolean\":\"new\",\"distance\":10,\"prof"
     "ile\":[[30,-5],[40,-5],[40,5],[30,5]]}},{\"tool\":\"query_topology\",\"args\":{\"at_extreme\":\"+z\",\"body\""
     ":0}},{\"tool\":\"coordsys_v\",\"args\":{\"body\":0,\"face\":5,\"generation\":{\"from_call\":2,\"key\":\"generatio"
     "n\"},\"type\":\"face_and_direction\"}},{\"tool\":\"query_topology\",\"args\":{\"at_extreme\":\"-z\",\"body\":1}},"
     "{\"tool\":\"coordsys_v\",\"args\":{\"body\":1,\"face\":4,\"generation\":{\"from_call\":4,\"key\":\"generation\"},\""
     "type\":\"face_and_direction\"}},{\"tool\":\"mate\",\"args\":{\"cs_a\":4,\"cs_b\":5,\"flip\":true,\"kind\":0}}]}],"
     "\"ids\":false}"},
    {"align", L_CONTEXT("Align to", "Design"), 5, nullptr, nullptr, L("Align needs a body — add or import one first"), 0x00000002u, 1, 0, false, false, nullptr, nullptr, L("Align the body to a picked face or plane"), false, CadAccess::None, false,
     nullptr},
    {"plane", L_CONTEXT("Plane", "Design"), 6, "Shift+P", "key:S+P", nullptr, 0x00000453u, 0, 0, false, false, nullptr, "design_plane", L("Reference plane (offset / tilt / midplane / tangent / two edges / coincident)"), false, CadAccess::Change, false,
     "{\"summary\":\"Add a datum plane as the Plane card does, to sketch or build on as {\\\"datum\\\": plane"
     "_index}.\",\"details\":\"Offset from a base plane or a face, at an angle about an edge, midway betwe"
     "en two faces, tangent to a cylindrical face, through two edges, or coincident with a base plane "
     "or a face. Not the `plane` param of other Design commands. Reply: plane_index, name, frame.\",\"pa"
     "rams\":[{\"name\":\"type\",\"type\":\"string\",\"default\":\"offset\",\"enum\":[\"offset\",\"angle\",\"midplane\",\"ta"
     "ngent\",\"two_edges\",\"coincident\"]},{\"name\":\"base\",\"type\":\"string|object\",\"default\":\"XY\",\"descript"
     "ion\":\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through modeling_origin, or {\\\"datum\\\": i}: what offset, coincide"
     "nt and angle build on without face_a\"},{\"name\":\"offset\",\"type\":\"number\",\"unit\":\"mm\",\"default\":20"
     ",\"description\":\"offset: distance along the normal of base or face_a\"},{\"name\":\"angle\",\"type\":\"nu"
     "mber\",\"unit\":\"deg\",\"default\":0,\"description\":\"offset without face_a: tilt about the base axis `a"
     "xis`; angle: turn about edge_a from the reference plane; tangent: position around the cylinder\"}"
     ",{\"name\":\"axis\",\"type\":\"integer\",\"default\":0,\"enum\":[0,1],\"description\":\"offset without face_a: "
     "tilt about the base plane's X (0) or Y (1) axis\"},{\"name\":\"face_a\",\"type\":\"object\",\"default\":nul"
     "l,\"description\":\"Face A, {\\\"body\\\": b, \\\"face\\\": f}: offset or coincident on a face; angle's ref"
     "erence plane; midplane's first face; tangent's cylindrical face\"},{\"name\":\"face_b\",\"type\":\"objec"
     "t\",\"default\":null,\"description\":\"Face B, {\\\"body\\\": b, \\\"face\\\": f}: midplane's second face\"},{\""
     "name\":\"edge_a\",\"type\":\"object\",\"default\":null,\"description\":\"Edge A, {\\\"body\\\": b, \\\"edge\\\": e}:"
     " angle's hinge; two_edges' first edge\"},{\"name\":\"edge_b\",\"type\":\"object\",\"default\":null,\"descrip"
     "tion\":\"Edge B, {\\\"body\\\": b, \\\"edge\\\": e}: two_edges' second edge\"},{\"name\":\"size\",\"type\":\"array"
     "\",\"unit\":\"mm\",\"default\":[60,60],\"description\":\"[u, v]: the size the plane is drawn at\"}],\"exampl"
     "es\":[{\"title\":\"A datum plane 20 mm above XY\",\"calls\":[{\"tool\":\"plane\",\"args\":{\"base\":\"XY\",\"offse"
     "t\":20,\"type\":\"offset\"}}]},{\"title\":\"A datum plane midway between a block's top and bottom faces\""
     ",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"query_topolo"
     "gy\",\"args\":{\"body\":0,\"kinds\":[\"planar\"]}},{\"tool\":\"plane\",\"args\":{\"face_a\":{\"body\":0,\"face\":4},\""
     "face_b\":{\"body\":0,\"face\":5},\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"type\":\"midplane\"}}]"
     "},{\"title\":\"A datum plane turned 30 degrees from a plate's top face about its front edge\",\"calls"
     "\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"query_topology\",\"arg"
     "s\":{\"adjacency\":true,\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"plane\",\"args\":{\"angle\":30,\"edge_a\":{\""
     "body\":0,\"edge\":3},\"face_a\":{\"body\":0,\"face\":5},\"generation\":{\"from_call\":1,\"key\":\"generation\"},\""
     "type\":\"angle\"}}]}],\"ids\":true}"},
    {"axis", L_CONTEXT("Axis", "Design"), 6, "Shift+A", "key:S+A", nullptr, 0x00000057u, 0, 0, false, false, nullptr, "design_line", L("Datum axis (two points, face normal, cylinder centerline, two planes, along edge)"), false, CadAccess::Change, true,
     "{\"summary\":\"Create a datum axis (reference line): two points, face normal, cylinder centreline, "
     "plane intersection, or along edge.\",\"details\":\"\",\"params\":[{\"name\":\"type\",\"type\":\"string\",\"defau"
     "lt\":\"two_points\",\"enum\":[\"two_points\",\"face_normal\",\"cylinder\",\"plane_intersection\",\"along_edge\""
     "]},{\"name\":\"p1\",\"type\":\"array\",\"default\":[0,0,0],\"description\":\"first point [x,y,z] for two_poin"
     "ts\"},{\"name\":\"p2\",\"type\":\"array\",\"default\":[0,0,10],\"description\":\"second point [x,y,z] for two_"
     "points\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"body for face/edge refs\"},{"
     "\"name\":\"face\",\"type\":\"integer\",\"default\":-1,\"description\":\"face id (query_topology) for face_nor"
     "mal/cylinder\"},{\"name\":\"edge\",\"type\":\"integer\",\"default\":-1,\"description\":\"edge id (query_topolo"
     "gy) for along_edge\"},{\"name\":\"plane_a\",\"type\":\"integer\",\"default\":-1,\"description\":\"first datum "
     "plane feature index for plane_intersection\"},{\"name\":\"plane_b\",\"type\":\"integer\",\"default\":-1,\"de"
     "scription\":\"second datum plane feature index for plane_intersection\"}],\"examples\":[{\"title\":\"The"
     " axis of a hole through a plate\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"wi"
     "dth\":20}},{\"tool\":\"hole\",\"args\":{\"diameter\":5,\"through\":true}},{\"tool\":\"query_topology\",\"args\":{"
     "\"body\":0,\"kinds\":[\"cylindrical\"]}},{\"tool\":\"axis\",\"args\":{\"body\":0,\"face\":6,\"generation\":{\"from_"
     "call\":2,\"key\":\"generation\"},\"type\":\"cylinder\"}}]},{\"title\":\"The normal through the centre of a p"
     "late's top face\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool"
     "\":\"query_topology\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"axis\",\"args\":{\"body\":0,\"face\":5"
     ",\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"type\":\"face_normal\"}}]},{\"title\":\"The vertical"
     " line where two datum planes meet\",\"calls\":[{\"tool\":\"plane\",\"args\":{\"base\":\"XZ\",\"offset\":10,\"typ"
     "e\":\"offset\"}},{\"tool\":\"plane\",\"args\":{\"base\":\"YZ\",\"offset\":10,\"type\":\"offset\"}},{\"tool\":\"axis\",\""
     "args\":{\"plane_a\":0,\"plane_b\":1,\"type\":\"plane_intersection\"}}]}],\"ids\":true}"},
    {"coordsys_v", L_CONTEXT("Coordinate system", "Design"), 6, "Shift+C", "key:S+C", nullptr, 0x00000043u, 0, 0, false, false, nullptr, "design_point", L("Datum coordinate system (world point, or face + direction edge)"), false, CadAccess::Change, true,
     "{\"summary\":\"Create a datum coordinate system (origin + orthonormal axes).\",\"details\":\"PointWorld"
     " aligns to world; FaceAndDirection uses a face for Z and an edge/hint for X.\",\"params\":[{\"name\":"
     "\"type\",\"type\":\"string\",\"default\":\"point_world\",\"enum\":[\"point_world\",\"face_and_direction\"]},{\"na"
     "me\":\"point\",\"type\":\"array\",\"default\":[0,0,0],\"description\":\"origin [x,y,z] for point_world\"},{\"n"
     "ame\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"body for face/edge refs\"},{\"name\":\"face"
     "\",\"type\":\"integer\",\"default\":-1,\"description\":\"face id (query_topology) for Z axis\"},{\"name\":\"ed"
     "ge\",\"type\":\"integer\",\"default\":-1,\"description\":\"edge id (query_topology) for X axis hint\"},{\"na"
     "me\":\"x_hint\",\"type\":\"array\",\"default\":[1,0,0],\"description\":\"fallback X direction hint if no edg"
     "e given\"}],\"examples\":[{\"title\":\"A connector on a plate's top face, its x axis along the front e"
     "dge\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"query_top"
     "ology\",\"args\":{\"adjacency\":true,\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"coordsys_v\",\"args\":{\"body\""
     ":0,\"edge\":3,\"face\":5,\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"type\":\"face_and_direction\""
     "}}]}],\"ids\":true}"},
    {"helix", L_CONTEXT("Helix", "Design"), 6, nullptr, "fly:plane#3", nullptr, 0x00000405u, 0, 0, false, false, nullptr, "design_thread", L("Helical curve (spring path) — use as a sweep path for coils / springs / augers"), false, CadAccess::Change, false,
     "{\"summary\":\"Create a helical curve, a sweep path for springs, coils and augers.\",\"details\":\"It s"
     "tarts on the plane's x axis at `radius` from its origin. pitch = axial rise per turn. left_hande"
     "d flips the winding. taper_deg != 0 gives a conical helix. Reply: helix_index, frame, start and "
     "tangent (world), to draw a sweep's profile across the curve there.\",\"params\":[{\"name\":\"radius\",\""
     "type\":\"number\",\"unit\":\"mm\",\"default\":10,\"exclusive_min\":0},{\"name\":\"pitch\",\"type\":\"number\",\"unit"
     "\":\"mm\",\"default\":5,\"exclusive_min\":0},{\"name\":\"height\",\"type\":\"number\",\"unit\":\"mm\",\"default\":20,"
     "\"exclusive_min\":0},{\"name\":\"left_handed\",\"type\":\"boolean\",\"default\":false},{\"name\":\"taper_deg\",\""
     "type\":\"number\",\"unit\":\"deg\",\"default\":0},{\"name\":\"plane\",\"type\":\"string|object\",\"default\":\"XY\",\""
     "description\":\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}, a Plane feature;"
     " or {\\\"body\\\": b, \\\"face\\\": f}, a planar face (origin at its centroid, normal out of the body, x"
     " from the world axes). The reply's frame gives the axes.\"}],\"examples\":[{\"title\":\"A four-turn he"
     "lix on XY: radius 10 mm, pitch 5 mm, 20 mm tall\",\"calls\":[{\"tool\":\"helix\",\"args\":{\"height\":20,\"p"
     "itch\":5,\"radius\":10}}]},{\"title\":\"A left-handed conical helix tapering 10 degrees\",\"calls\":[{\"to"
     "ol\":\"helix\",\"args\":{\"height\":20,\"left_handed\":true,\"pitch\":4,\"radius\":10,\"taper_deg\":10}}]}],\"id"
     "s\":true}"},
    {"project", L_CONTEXT("Project", "Design"), 6, nullptr, "fly:plane#4", L("Project needs a body — add or import one first"), 0x00000482u, 1, 0, false, false, nullptr, "design_sketch", L("Project body edges onto a plane as sketch entities"), false, CadAccess::Change, true,
     "{\"summary\":\"Project edges of a solid onto a sketch plane, producing a new sketch feature.\",\"deta"
     "ils\":\"\",\"params\":[{\"name\":\"source_body\",\"type\":\"integer\",\"default\":-1,\"description\":\"body owning"
     " the edges; -1 = last body\"},{\"name\":\"face\",\"type\":\"integer\",\"default\":-1,\"description\":\"global "
     "face id on the source body; when set, all its edges are projected\"},{\"name\":\"edges\",\"type\":\"arra"
     "y\",\"default\":[],\"description\":\"global edge ids to project; empty => project the face\"},{\"name\":\""
     "plane\",\"type\":\"string|object\",\"default\":\"XY\",\"description\":\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through mod"
     "eling_origin; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\": b, \\\"face\\\": f}, a planar face (ori"
     "gin at its centroid, normal out of the body, x from the world axes). The reply's frame gives the"
     " axes.\"}],\"examples\":[{\"title\":\"A plate's top-face outline projected onto the XY plane as a new "
     "sketch\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"query_"
     "topology\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"project\",\"args\":{\"face\":5,\"generation\":{"
     "\"from_call\":1,\"key\":\"generation\"},\"plane\":\"XY\",\"source_body\":0}}]},{\"title\":\"Two top edges of a "
     "plate, meeting at a corner, projected onto the XY plane\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"dis"
     "tance\":5,\"height\":20,\"width\":20}},{\"tool\":\"query_topology\",\"args\":{\"adjacency\":true,\"at_extreme\""
     ":\"+z\",\"body\":0}},{\"tool\":\"project\",\"args\":{\"edges\":[3,6],\"generation\":{\"from_call\":1,\"key\":\"gene"
     "ration\"},\"plane\":\"XY\",\"source_body\":0}}]}],\"ids\":true}"},
    {"measure", L_CONTEXT("Measure", "Design"), 6, nullptr, nullptr, nullptr, 0x000b03feu, 0, 0, false, false, nullptr, nullptr, L("Measure between the picked points, edges or faces"), false, CadAccess::Read, false,
     "{\"summary\":\"Measure two refs, on one body or two: distance, closest points, angle and the gap be"
     "tween parallel faces.\",\"details\":\"distance is between their representative points (a face's cent"
     "roid, an edge's middle, an axis's middle); min_distance comes with closest_a and closest_b; angl"
     "e_deg when both have a direction; parallel_gap across two parallel planar faces.\",\"params\":[{\"na"
     "me\":\"body\",\"type\":\"integer\",\"default\":0,\"description\":\"the body of a ref that names none\"},{\"nam"
     "e\":\"a\",\"type\":\"object\",\"description\":\"{\\\"face\\\": f}, {\\\"edge\\\": e} or {\\\"point\\\": [x, y, z]}, wi"
     "th an optional \\\"body\\\"; {\\\"face\\\": f, \\\"axis\\\": true} is a cylindrical face's axis over its len"
     "gth\"},{\"name\":\"b\",\"type\":\"object\",\"description\":\"as a\"}],\"examples\":[{\"title\":\"A plate's thickne"
     "ss, between its top and bottom faces\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":2"
     "0,\"width\":20}},{\"tool\":\"query_topology\",\"args\":{\"body\":0,\"kinds\":[\"planar\"]}},{\"tool\":\"measure\","
     "\"args\":{\"a\":{\"face\":5},\"b\":{\"face\":4},\"body\":0,\"generation\":{\"from_call\":1,\"key\":\"generation\"}}}"
     "]},{\"title\":\"The 10 mm gap between two blocks' facing sides\",\"calls\":[{\"tool\":\"extrude\",\"args\":{"
     "\"distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"extrude\",\"args\":{\"boolean\":\"new\",\"distance\":10,\""
     "profile\":[[20,-5],[30,-5],[30,5],[20,5]]}},{\"tool\":\"query_topology\",\"args\":{\"body\":0,\"direction\""
     ":[1,0,0]}},{\"tool\":\"query_topology\",\"args\":{\"body\":1,\"direction\":[-1,0,0]}},{\"tool\":\"measure\",\"a"
     "rgs\":{\"a\":{\"body\":0,\"face\":1},\"b\":{\"body\":1,\"face\":3},\"generation\":{\"from_call\":3,\"key\":\"generat"
     "ion\"}}}]}],\"ids\":true}"},
    {"mass_props", L_CONTEXT("Volume and area", "Design"), 6, nullptr, "btn:mass", nullptr, 0x000000feu, 1, 0, false, false, nullptr, "info", L("Report the volume and surface area of the selected body"), false, CadAccess::Read, false,
     "{\"summary\":\"Volume / surface area / centre of mass / inertia tensor of a body.\",\"details\":\"\",\"pa"
     "rams\":[{\"name\":\"body\",\"type\":\"integer\",\"default\":0}],\"examples\":[{\"title\":\"The volume and centre"
     " of mass of a 20 x 20 x 5 mm plate\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,"
     "\"width\":20}},{\"tool\":\"mass_props\",\"args\":{\"body\":0}}]}],\"ids\":false}"},
    {"interference", L_CONTEXT("Interference", "Design"), 6, nullptr, "btn:interference", L("Interference needs at least two bodies"), 0x00000280u, 2, 0, false, false, nullptr, "design_boolean", L("Check whether two bodies overlap — reports, changes nothing"), false, CadAccess::Read, false,
     "{\"summary\":\"Solid bodies that overlap, as {body_a, body_b, volume}.\",\"details\":\"Read-only; bodie"
     "s that merely touch enclose no volume and are not reported.\",\"params\":[{\"name\":\"min_volume\",\"typ"
     "e\":\"number\",\"unit\":\"mm^3\",\"default\":1e-06,\"description\":\"overlap volume above which a pair count"
     "s as interfering\"}],\"examples\":[{\"title\":\"A post standing in a block: the two overlap\",\"calls\":["
     "{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"extrude\",\"args\":{\"bool"
     "ean\":\"new\",\"distance\":20,\"height\":10,\"width\":10}},{\"tool\":\"interference\",\"args\":{\"silent\":true}}"
     "]}],\"ids\":false}"},
    {"edit_feature", L_CONTEXT("Edit", "Design"), 7, nullptr, "btn:edit", nullptr, 0x00007d8eu, 0, 0, false, false, nullptr, "design_edit", L("Reopen the selected feature to change what it was made from"), false, CadAccess::Change, true,
     "{\"summary\":\"Bind a feature's numeric field to an expression over the design variables, or unbind"
     " it with an empty expr.\",\"details\":\"Unbound, the field keeps its value.\",\"params\":[{\"name\":\"feat"
     "ure\",\"type\":\"integer\",\"description\":\"feature index (describe_scene)\"},{\"name\":\"field\",\"type\":\"st"
     "ring\",\"description\":\"a numeric field of describe_feature's params, e.g. width, distance, hole_di"
     "ameter, thread_diameter\"},{\"name\":\"expr\",\"type\":\"string\",\"description\":\"\\\"\\\" removes the binding"
     "\"}],\"examples\":[{\"title\":\"A plate's thickness bound to the variable t\",\"calls\":[{\"tool\":\"extrude"
     "\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"set_variable\",\"args\":{\"expr\":\"8\",\"name\""
     ":\"t\"}},{\"tool\":\"edit_feature\",\"args\":{\"expr\":\"t\",\"feature\":1,\"field\":\"distance\"}}]},{\"title\":\"A "
     "plate's thickness unbound, keeping the 8 mm the binding gave it\",\"calls\":[{\"tool\":\"extrude\",\"arg"
     "s\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"set_variable\",\"args\":{\"expr\":\"8\",\"name\":\"t\"}}"
     ",{\"tool\":\"edit_feature\",\"args\":{\"expr\":\"t\",\"feature\":1,\"field\":\"distance\"}},{\"tool\":\"edit_featur"
     "e\",\"args\":{\"expr\":\"\",\"feature\":1,\"field\":\"distance\"}}]}],\"ids\":false}"},
    {"rename", L_CONTEXT("Rename…", "Design"), 8, "F2", "btn:rename", L("Select a feature, or a body, to rename it"), 0x00004080u, 0, 0, false, false, nullptr, "rename_edit", L("Give this feature a name you will recognise in the tree (a body takes its name from the feature that makes it)"), false, CadAccess::None, false,
     nullptr},
    {"delete_face", L_CONTEXT("Delete Face", "Design"), 7, nullptr, "fly:dressup#3", L("Delete Face needs a body — add or import one first"), 0x0000000eu, 1, 0, false, false, nullptr, "design_delete", L("Remove faces from a body and heal the solid"), false, CadAccess::Change, true,
     "{\"summary\":\"Remove faces from a solid, healing the gap via OCCT defeaturing.\",\"details\":\"\",\"para"
     "ms\":[{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"target body; omit for the last "
     "body\"},{\"name\":\"faces\",\"type\":\"array\",\"description\":\"global face ids to delete\"}],\"examples\":[{\""
     "title\":\"A plate's bore filled in by deleting its cylindrical face\",\"calls\":[{\"tool\":\"extrude\",\"a"
     "rgs\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"hole\",\"args\":{\"diameter\":5,\"through\":true}}"
     ",{\"tool\":\"query_topology\",\"args\":{\"body\":0,\"kinds\":[\"cylindrical\"]}},{\"tool\":\"delete_face\",\"args"
     "\":{\"body\":0,\"faces\":[6],\"generation\":{\"from_call\":2,\"key\":\"generation\"}}}]}],\"ids\":true}"},
    {"colour", L_CONTEXT("Color", "Design"), 8, nullptr, "btn:colour", nullptr, 0x000001feu, 1, 0, false, false, nullptr, "color_palette", L("Set the selected body's display color"), true, CadAccess::None, false,
     nullptr},
    {"zoom_to", L_CONTEXT("Zoom to selection", "Design"), 8, nullptr, "btn:zoom_to", nullptr, 0x000041feu, 0, 0, false, false, nullptr, "design_zoom", L("Frame the selection in the view, keeping the view direction"), false, CadAccess::None, false,
     nullptr},
    {"delete", L_CONTEXT("Delete", "Design"), 7, "Del", "btn:delete", nullptr, 0x000f7c00u, 0, 0, false, false, nullptr, "design_delete", L("Delete what is selected"), false, CadAccess::None, false,
     nullptr},
    {"delete_body", L_CONTEXT("Delete Body", "Design"), 7, nullptr, "btn:delete_body", nullptr, 0x000001feu, 1, 0, false, false, nullptr, "design_delete", L("Delete this whole body — removes the feature it was made from"), false, CadAccess::None, false,
     nullptr},
    {"sk_line_t", L_CONTEXT("Line", "Design"), 0, "L", "key:L", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Line", "Design"), "design_line", L("Line — click start, then end"), false, CadAccess::None, false,
     nullptr},
    {"sk_polyline", L_CONTEXT("Polyline", "Design"), 0, nullptr, "fly:design_line#1", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Line", "Design"), "design_polyline", L("Click points; click the first point to close the loop, right-click to end it open"), false, CadAccess::None, false,
     nullptr},
    {"sk_rect", L_CONTEXT("Corner rectangle", "Design"), 0, "R", "key:R", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Rectangle", "Design"), "design_rect", L("Rectangle — click two opposite corners"), false, CadAccess::None, false,
     nullptr},
    {"sk_rect_center", L_CONTEXT("Center rectangle", "Design"), 0, nullptr, "fly:design_rect#1", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Rectangle", "Design"), "design_crect", L("Click center, then a corner"), false, CadAccess::None, false,
     nullptr},
    {"sk_rect_oblique", L_CONTEXT("Oblique rectangle", "Design"), 0, nullptr, "fly:design_rect#2", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Rectangle", "Design"), "design_rect_oblique", L("Click two corners of one edge, then a point for the width"), false, CadAccess::None, false,
     nullptr},
    {"sk_rect_rounded", L_CONTEXT("Rounded rectangle", "Design"), 0, nullptr, "fly:design_rect#3", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Rectangle", "Design"), "design_rect_rounded", L("Click two opposite corners, then a point for the corner radius"), false, CadAccess::None, false,
     nullptr},
    {"sk_circle", L_CONTEXT("Center circle", "Design"), 0, "C", "key:C", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Circle", "Design"), "design_circle", L("Circle — click center, then radius"), false, CadAccess::None, false,
     nullptr},
    {"sk_circle_2pt", L_CONTEXT("2-point circle", "Design"), 0, nullptr, "fly:design_circle#1", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Circle", "Design"), "design_circle2pt", L("Click two ends of the diameter"), false, CadAccess::None, false,
     nullptr},
    {"sk_circle_3pt", L_CONTEXT("3-point circle", "Design"), 0, nullptr, "fly:design_circle#2", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Circle", "Design"), "design_circle3pt", L("Click three points on the circle"), false, CadAccess::None, false,
     nullptr},
    {"sk_arc_t", L_CONTEXT("3-point arc", "Design"), 0, "A", "key:A", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Arc", "Design"), "design_arc3pt", L("Arc — click start, end, then a point"), false, CadAccess::None, false,
     nullptr},
    {"sk_arc_tangent", L_CONTEXT("Tangent arc", "Design"), 0, nullptr, "fly:design_arc3pt#1", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Arc", "Design"), "design_tangentarc", L("Click start (on the last entity) then end"), false, CadAccess::None, false,
     nullptr},
    {"sk_arc_center", L_CONTEXT("Center-point arc", "Design"), 0, nullptr, "fly:design_arc3pt#2", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Arc", "Design"), "design_arc_center", L("Click center, then start, then a point for the end angle"), false, CadAccess::None, false,
     nullptr},
    {"sk_slot", L_CONTEXT("Slot", "Design"), 0, "S", "key:S", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Slot", "Design"), "design_slot", L("Slot — two centerline ends, then the width"), false, CadAccess::None, false,
     nullptr},
    {"sk_slot_arc", L_CONTEXT("Arc slot", "Design"), 0, nullptr, "fly:design_slot#1", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Slot", "Design"), "design_slot_arc", L("Click center, start, end, then a point for the width"), false, CadAccess::None, false,
     nullptr},
    {"sk_ellipse", L_CONTEXT("Ellipse", "Design"), 0, "E", "key:E", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Ellipse", "Design"), "design_ellipse", L("Ellipse — center, major end, minor point"), false, CadAccess::None, false,
     nullptr},
    {"sk_ellipse_arc", L_CONTEXT("Elliptical arc", "Design"), 0, nullptr, "fly:design_ellipse#1", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Ellipse", "Design"), "design_ellipse_arc", L("Click center, major-axis end, minor point, then arc start and end"), false, CadAccess::None, false,
     nullptr},
    {"sk_spline", L_CONTEXT("Spline", "Design"), 0, "B", "key:B", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_bspline", L("Spline — click control points"), false, CadAccess::None, false,
     nullptr},
    {"sk_poly_3", L_CONTEXT("Triangle", "Design"), 0, nullptr, "btn:poly#3", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Triangle — click center, then a vertex"), false, CadAccess::None, false,
     nullptr},
    {"sk_poly_4", L_CONTEXT("Square", "Design"), 0, nullptr, "btn:poly#4", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Square — click center, then a vertex"), false, CadAccess::None, false,
     nullptr},
    {"sk_poly_5", L_CONTEXT("Pentagon", "Design"), 0, nullptr, "btn:poly#5", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Pentagon — click center, then a vertex"), false, CadAccess::None, false,
     nullptr},
    {"sk_polygon", L_CONTEXT("Hexagon", "Design"), 0, "G", "btn:poly#6", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Hexagon — click center, then a vertex"), false, CadAccess::None, false,
     nullptr},
    {"sk_poly_8", L_CONTEXT("Octagon", "Design"), 0, nullptr, "btn:poly#8", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Octagon — click center, then a vertex"), false, CadAccess::None, false,
     nullptr},
    {"sk_poly_12", L_CONTEXT("Dodecagon", "Design"), 0, nullptr, "btn:poly#12", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Dodecagon — click center, then a vertex"), false, CadAccess::None, false,
     nullptr},
    {"sk_poly_inscribed", L_CONTEXT("Inscribed", "Design"), 0, nullptr, "btn:polyfit#0", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Measure the polygon to its corners (inscribed)"), false, CadAccess::None, false,
     nullptr},
    {"sk_poly_circumscribed", L_CONTEXT("Circumscribed", "Design"), 0, nullptr, "btn:polyfit#1", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Measure the polygon to its flats (circumscribed)"), false, CadAccess::None, false,
     nullptr},
    {"sk_point_t", L_CONTEXT("Point", "Design"), 0, "P", "key:P", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_point", L("Point — click to place"), false, CadAccess::None, false,
     nullptr},
    {"sk_text", L_CONTEXT("Text", "Design"), 0, nullptr, "btn:text", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_text", L("Type text; it becomes a Text feature on this sketch's plane, editable later"), true, CadAccess::None, false,
     nullptr},
    {"sk_svg", L_CONTEXT("SVG", "Design"), 0, nullptr, "btn:svg", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_svg", L("Import an SVG outline into this sketch as editable lines"), true, CadAccess::None, false,
     nullptr},
    {"sk_offset", L_CONTEXT("Offset", "Design"), 1, "O", "key:O", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_offset", L("Offset — pick an entity, drag the distance"), false, CadAccess::None, false,
     nullptr},
    {"sk_trim", L_CONTEXT("Trim", "Design"), 2, "T", "key:T", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_trim", L("Trim — click a segment to trim it"), false, CadAccess::None, false,
     nullptr},
    {"sk_fillet", L_CONTEXT("Fillet", "Design"), 3, "F", "key:F", nullptr, 0x00090000u, 0, 0, false, true, nullptr, "design_filletedge", L("Fillet — pick two lines, set the radius"), false, CadAccess::None, false,
     nullptr},
    {"sk_chamfer", L_CONTEXT("Chamfer", "Design"), 3, "H", "key:H", nullptr, 0x00090000u, 0, 0, false, true, nullptr, "design_chamfer", L("Chamfer — pick two lines, set the distance"), false, CadAccess::None, false,
     nullptr},
    {"sk_array", L_CONTEXT("Linear array", "Design"), 4, nullptr, "fly:design_array#0", nullptr, 0x000b0000u, 0, 0, false, true, L_CONTEXT("Array", "Design"), "design_array", L("Pick entities, drag the spacing handle, click the count; click empty to apply"), false, CadAccess::None, false,
     nullptr},
    {"sk_array_polar", L_CONTEXT("Polar array", "Design"), 4, nullptr, "fly:design_array#1", nullptr, 0x000b0000u, 0, 0, false, true, L_CONTEXT("Array", "Design"), "design_polararray", L("Pick entities, drag the sweep handle, click the count; click empty to apply"), false, CadAccess::None, false,
     nullptr},
    {"sk_mirror", L_CONTEXT("Mirror", "Design"), 4, "M", "key:M", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_mirror", L("Mirror — pick axis, then entities"), false, CadAccess::None, false,
     nullptr},
    {"sk_move", L_CONTEXT("Move", "Design"), 5, nullptr, "fly:design_move#0", nullptr, 0x000f0000u, 0, 0, false, true, L_CONTEXT("Move", "Design"), "design_move", L("Pick entities, then drag the handle or click the distance; click empty to apply"), false, CadAccess::None, false,
     nullptr},
    {"sk_rotate", L_CONTEXT("Rotate", "Design"), 5, nullptr, "fly:design_move#1", nullptr, 0x000f0000u, 0, 0, false, true, L_CONTEXT("Move", "Design"), "design_rotate", L("Pick entities, then drag around the pivot or click the angle; click empty to apply"), false, CadAccess::None, false,
     nullptr},
    {"sk_scale", L_CONTEXT("Scale", "Design"), 5, nullptr, "fly:design_move#2", nullptr, 0x000f0000u, 0, 0, false, true, L_CONTEXT("Move", "Design"), "design_scale", L("Pick entities, then drag the handle or click the factor; click empty to apply"), false, CadAccess::None, false,
     nullptr},
    {"sk_dimension", L_CONTEXT("Dimension", "Design"), 6, "D", "key:D", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_dimension", L("Dimension — click 2 points or an entity"), false, CadAccess::None, false,
     nullptr},
    {"sk_constrain", L_CONTEXT("Constrain", "Design"), 6, "K", "key:K", nullptr, 0x000f0000u, 0, 0, false, true, nullptr, "design_constrain", L("Constrain the selected sketch entities to each other"), false, CadAccess::None, false,
     nullptr},
    // Same verb, model-mode vocabulary: offered when a SKETCH is selected (bit 14, SkLoop), the
    // state a user is in right after finishing one. Without this row the only way in was the
    // toolbar icon, and constraints read as absent — see the Onshape-comparison report.
    // It sits in the Reference row, the address the sketch-mode Constrain has: one verb, one row.
    {"constrain", L_CONTEXT("Constrain sketch", "Design"), 6, nullptr, "btn:constrain", L("Select a sketch to constrain it"), 0x00004000u, 0, 1, false, false, nullptr, "design_constrain", L("Add dimensions and relations (coincident, tangent, parallel...) to the selected sketch"), false, CadAccess::None, false,
     nullptr},
    {"sk_construct", L_CONTEXT("Construction", "Design"), 6, "Q", "key:Q", nullptr, 0x000b8000u, 0, 0, false, true, nullptr, "design_construction", L("Toggle construction: geometry that guides but is never built"), false, CadAccess::None, false,
     nullptr},
    {"sk_extend", L_CONTEXT("Extend", "Design"), 7, "X", "key:X", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_extend", L("Extend — click a line/arc to extend it"), false, CadAccess::None, false,
     nullptr},
    {"sk_delete", L_CONTEXT("Delete", "Design"), 7, "Del", "btn:sk_delete", nullptr, 0x000f0000u, 0, 0, false, true, nullptr, "design_delete", L("Delete the selected sketch entities"), false, CadAccess::None, false,
     nullptr},
    // Typing the defining number of the element you pointed at. Three rows rather than one so
    // each names the quantity in the drawing-office word for THAT element; all three land on
    // the same handler, because dimension_kind() already resolves the quantity from the
    // selection. Without these, an element's own numbers were reachable only by arming the
    // Dimension tool and re-picking geometry that was already selected.
    {"sk_length", L_CONTEXT("Length…", "Design"), 7, "V", "key:V", nullptr, 0x00010000u, 0, 0, false, true, nullptr, "design_dimension", L("Type the length of this line"), false, CadAccess::None, false,
     nullptr},
    {"sk_radius", L_CONTEXT("Radius / diameter…", "Design"), 7, "V", "key:V", nullptr, 0x00020000u, 0, 0, false, true, nullptr, "design_dimension", L("Type the radius of this arc, or the diameter of this circle"), false, CadAccess::None, false,
     nullptr},
    {"sk_angdist", L_CONTEXT("Angle / distance…", "Design"), 7, "V", "key:V", nullptr, 0x00080000u, 0, 0, false, true, nullptr, "design_dimension", L("Type the angle between two lines, or the distance between the two picks"), false, CadAccess::None, false,
     nullptr},
    {"describe_scene", L_CONTEXT("Describe scene", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"The Design document: features, variables with their values, bodies, undo_depth / red"
     "o_depth and generation.\",\"details\":\"A feature's bound lists the fields an expression drives; a b"
     "ody gives bbox, source_feature, visible, color and moved (by the Move gizmo). selection is what "
     "the right-click menu would be built for: its kind, the selection_kind a tool's opening reply giv"
     "es, and its id (none, face_planar, body_solid, sk_line and the like).\",\"params\":[],\"examples\":[{"
     "\"title\":\"An empty design\",\"calls\":[{\"tool\":\"describe_scene\",\"args\":{}}]},{\"title\":\"A plate's two"
     " features and its body\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}}"
     ",{\"tool\":\"describe_scene\",\"args\":{}}]}],\"ids\":false}"},
    {"describe_feature", L_CONTEXT("Describe feature", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"One feature as its card shows it: params, the editable ones with their ranges, expr,"
     " refs, used_by and bodies_made.\",\"details\":\"expr holds its bindings, and refs the feature and bo"
     "dy indices and face and edge ids it uses; a sketch adds its plane, entities and constraints (ang"
     "les in degrees). Imported Text or SVG art shows its regions (a count), region_loops (how many cl"
     "osed loops each region has, its outer loop plus its holes; counts, not the loops' geometry) and "
     "region_areas (each region's filled area in mm² as placed, its outer loop less its holes).\",\"para"
     "ms\":[{\"name\":\"feature\",\"type\":\"integer\",\"description\":\"feature index (describe_scene)\"}],\"exampl"
     "es\":[{\"title\":\"A plate's extrude, with the fields its card edits\",\"calls\":[{\"tool\":\"extrude\",\"ar"
     "gs\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"describe_feature\",\"args\":{\"feature\":1}}]},{\""
     "title\":\"A committed sketch's plane, entities and constraints\",\"calls\":[{\"tool\":\"sketch_begin\",\"a"
     "rgs\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch_commi"
     "t\",\"args\":{}},{\"tool\":\"describe_feature\",\"args\":{\"feature\":0}}]}],\"ids\":true}"},
    {"query_topology", L_CONTEXT("Query topology", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"A body's faces and edges, with their kinds and geometry, narrowed by face and edge f"
     "ilters.\",\"details\":\"Faces give kind, area, centroid, normal, and a cylinder's, cone's, sphere's "
     "or torus's numbers; edges give kind, length, ends, a circle's centre and radius. Face filters (k"
     "inds, direction, at_extreme) and edge filters (edge_kinds, direction, convex) narrow their own l"
     "ist; when filters are given, a list none of its filters names is empty, or with adjacency holds "
     "the neighbours of the other list. Reply: face_count and edge_count of the body, faces_total and "
     "edges_total after filtering, next_offset while a page has more, generation.\",\"params\":[{\"name\":\""
     "body\",\"type\":\"integer\",\"default\":0},{\"name\":\"kinds\",\"type\":\"array\",\"default\":null,\"description\":"
     "\"face kinds to list: planar, cylindrical, conical, spherical, toroidal, bezier, bspline, revolut"
     "ion, extrusion, offset, other\"},{\"name\":\"edge_kinds\",\"type\":\"array\",\"default\":null,\"description\""
     ":\"edge kinds to list: line, circle, ellipse, bspline, bezier, other\"},{\"name\":\"direction\",\"type\""
     ":\"array\",\"default\":null,\"description\":\"[x, y, z]: planar faces whose outward normal points this "
     "way, and line edges along it either way, within 1 degree\"},{\"name\":\"at_extreme\",\"type\":\"string\","
     "\"default\":null,\"enum\":[\"+x\",\"-x\",\"+y\",\"-y\",\"+z\",\"-z\"],\"description\":\"faces whose centroid lies o"
     "n that side of the body's bounding box, e.g. +z for the top face\"},{\"name\":\"convex\",\"type\":\"bool"
     "ean\",\"default\":null,\"description\":\"edges where two faces meet in an outside (true) or inside (fa"
     "lse) corner; implies adjacency\"},{\"name\":\"adjacency\",\"type\":\"boolean\",\"default\":false,\"descripti"
     "on\":\"faces list their edges; edges list their faces (one for a seam) and say convex true or fals"
     "e, or tangent where the faces meet smoothly\"},{\"name\":\"limit\",\"type\":\"integer\",\"default\":null,\"m"
     "in\":1,\"description\":\"page size of both lists; omit for all\"},{\"name\":\"offset\",\"type\":\"integer\",\""
     "default\":0}],\"examples\":[{\"title\":\"Every face and edge of a 20 x 20 x 5 mm plate\",\"calls\":[{\"too"
     "l\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"query_topology\",\"args\":{\"bod"
     "y\":0}}]},{\"title\":\"A plate's top face\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":"
     "20,\"width\":20}},{\"tool\":\"query_topology\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}}]},{\"title\":\"A plat"
     "e's vertical outside corners, the edges a fillet rounds\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"dis"
     "tance\":5,\"height\":20,\"width\":20}},{\"tool\":\"query_topology\",\"args\":{\"body\":0,\"convex\":true,\"direc"
     "tion\":[0,0,1],\"edge_kinds\":[\"line\"]}}]}],\"ids\":true}"},
    {"slice_body", L_CONTEXT("Slice body", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"Cross-section of a body by a base plane at an offset (sections-as-evidence).\",\"detai"
     "ls\":\"Returns ordered world contours, each flagged closed/open.\",\"params\":[{\"name\":\"body\",\"type\":"
     "\"integer\",\"default\":0},{\"name\":\"plane\",\"type\":\"string\",\"default\":\"XY\",\"enum\":[\"XY\",\"XZ\",\"YZ\"]},{"
     "\"name\":\"offset\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0}],\"examples\":[{\"title\":\"A plate's cross-"
     "section halfway up\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"t"
     "ool\":\"slice_body\",\"args\":{\"body\":0,\"offset\":2.5,\"plane\":\"XY\"}}]},{\"title\":\"The two contours thro"
     "ugh a plate with a bore\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_"
     "add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"center\":[0,0],\""
     "radius\":4,\"type\":\"circle\"}]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"extrude\",\"args\":{\"dist"
     "ance\":5,\"sketch\":0}},{\"tool\":\"slice_body\",\"args\":{\"body\":0,\"offset\":2.5,\"plane\":\"XY\"}}]}],\"ids\":"
     "true}"},
    {"validate_against", L_CONTEXT("Validate against", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"Compare a body with a reference: volume, bbox, centroid and surface deviation (the R"
     "E acceptance metric).\",\"details\":\"The reference is {step:path|body:id}; the surface deviation is"
     " max/mean/rms in mm.\",\"params\":[{\"name\":\"body\",\"type\":\"integer\",\"default\":0},{\"name\":\"reference\""
     ",\"type\":\"object\"}],\"examples\":[{\"title\":\"A 5 mm plate against a 6 mm one\",\"calls\":[{\"tool\":\"extr"
     "ude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"extrude\",\"args\":{\"boolean\":\"new\",\"di"
     "stance\":6,\"height\":20,\"width\":20}},{\"tool\":\"validate_against\",\"args\":{\"body\":0,\"reference\":{\"bod"
     "y\":1}}}]},{\"title\":\"A plate against a reference part's STEP file\",\"calls\":[{\"tool\":\"extrude\",\"ar"
     "gs\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"validate_against\",\"args\":{\"body\":0,\"referenc"
     "e\":{\"step\":\"/path/to/reference.step\"}}}],\"needs\":\"a STEP file of the reference part\"}],\"ids\":fal"
     "se}"},
    {"sketch_describe", L_CONTEXT("Sketch describe", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"The open sketch, or a committed one: entities, constraint count and its loops.\",\"det"
     "ails\":\"The loops are closed_loops {index, entities, holes, area}, open_ends, defects, buildable."
     " The open sketch adds dof, the armed tool, the selection (entity indices) and selected_constrain"
     "t, the index of the constraint picked by its badge or the Constraints list, null for none; a com"
     "mitted one adds its name and plane.\",\"params\":[{\"name\":\"sketch\",\"type\":\"integer\",\"default\":null,"
     "\"description\":\"a committed Sketch feature's index; omit for the open sketch\"}],\"examples\":[{\"tit"
     "le\":\"The open sketch: a rectangle with a circle inside it as a hole\",\"calls\":[{\"tool\":\"sketch_be"
     "gin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketc"
     "h_add\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":4,\"type\":\"circle\"}]}},{\"tool\":\"sketch_descri"
     "be\",\"args\":{}}]},{\"title\":\"A committed sketch's plane, entities and loop\",\"calls\":[{\"tool\":\"sket"
     "ch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\""
     "sketch_commit\",\"args\":{}},{\"tool\":\"sketch_describe\",\"args\":{\"sketch\":0}}]}],\"ids\":false}"},
    {"sketch_validate", L_CONTEXT("Sketch validate", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"The open sketch's loops (closed_loops, open_ends, defects, buildable) and dof.\",\"det"
     "ails\":\"\",\"params\":[{\"name\":\"tolerance\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0.001,\"description\""
     ":\"reported back\"}],\"examples\":[{\"title\":\"A triangle with a 0.05 mm gap: two open ends, not build"
     "able\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"entit"
     "ies\":[{\"p0\":[0,0],\"p1\":[20,0],\"type\":\"line\"},{\"p0\":[20,0.05],\"p1\":[20,10],\"type\":\"line\"},{\"p0\":["
     "20,10],\"p1\":[0,0],\"type\":\"line\"}]}},{\"tool\":\"sketch_validate\",\"args\":{}}]}],\"ids\":false}"},
    {"sketch_constraints", L_CONTEXT("Sketch constraints", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"The constraints of the open sketch, or of a committed one: index, type, refs and val"
     "ue.\",\"details\":\"A ref is {entity, role} for a point and {entity} for a whole entity; value is mm"
     ", or degrees for an angle. Reply: scope (live or committed), items, and dof and solve_ok for the"
     " open sketch.\",\"params\":[{\"name\":\"sketch\",\"type\":\"integer\",\"default\":null,\"description\":\"a commi"
     "tted Sketch feature's index; omit for the open sketch\"}],\"examples\":[{\"title\":\"The coincident, h"
     "orizontal and vertical constraints a drawn rectangle gets\",\"calls\":[{\"tool\":\"sketch_begin\",\"args"
     "\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch_constrai"
     "nts\",\"args\":{}}]},{\"title\":\"A committed sketch's constraints\",\"calls\":[{\"tool\":\"sketch_begin\",\"a"
     "rgs\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch_commi"
     "t\",\"args\":{}},{\"tool\":\"sketch_constraints\",\"args\":{\"sketch\":0}}]}],\"ids\":false}"},
    {"import_step", L_CONTEXT("Import step", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, true,
     "{\"summary\":\"Import a STEP file as native B-rep bodies (the reference part to measure).\",\"details"
     "\":\"\",\"params\":[{\"name\":\"path\",\"type\":\"string\",\"description\":\"absolute path\"}],\"examples\":[{\"titl"
     "e\":\"A part read from a STEP file as a body\",\"calls\":[{\"tool\":\"import_step\",\"args\":{\"path\":\"/path"
     "/to/part.step\"}}],\"needs\":\"a STEP file of the part\"}],\"ids\":false}"},
    {"import_mesh", L_CONTEXT("Import mesh", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, true,
     "{\"summary\":\"Convert a triangle mesh (STL/OBJ) into an editable B-rep body.\",\"details\":\"Reports w"
     "hether the result is a real solid or an open shell, and why.\",\"params\":[{\"name\":\"path\",\"type\":\"s"
     "tring\",\"description\":\"absolute path\"},{\"name\":\"tolerance\",\"type\":\"number\",\"default\":0.01},{\"name"
     "\":\"merge_angle_deg\",\"type\":\"number\",\"default\":5.0}],\"examples\":[{\"title\":\"An STL mesh turned int"
     "o an editable body\",\"calls\":[{\"tool\":\"import_mesh\",\"args\":{\"path\":\"/path/to/part.stl\"}}],\"needs\""
     ":\"an STL or OBJ mesh file\"}],\"ids\":false}"},
    {"hole_styled", L_CONTEXT("Hole styled", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Drill a hole with optional counterbore (style=1) or countersink (style=2) at (x,y) o"
     "r `at` on a plane.\",\"details\":\"Reply as hole.\",\"params\":[{\"name\":\"diameter\",\"type\":\"number\",\"uni"
     "t\":\"mm\",\"default\":5,\"exclusive_min\":0},{\"name\":\"depth\",\"type\":\"number\",\"unit\":\"mm\",\"default\":10}"
     ",{\"name\":\"through\",\"type\":\"boolean\",\"default\":true},{\"name\":\"x\",\"type\":\"number\",\"unit\":\"mm\",\"def"
     "ault\":0,\"description\":\"offset in the plane's frame from its origin, NOT world\"},{\"name\":\"y\",\"typ"
     "e\":\"number\",\"unit\":\"mm\",\"default\":0,\"description\":\"offset in the plane's frame from its origin, "
     "NOT world\"},{\"name\":\"plane\",\"type\":\"string|object\",\"default\":\"XY\",\"description\":\"\\\"XY\\\", \\\"XZ\\\" "
     "or \\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\": b, \\\"face\\\": f"
     "}, a planar face to drill into that body (origin at its centroid, normal into the body, x along "
     "its longest edge). The reply's frame gives the axes.\"},{\"name\":\"at\",\"type\":\"array|object\",\"defau"
     "lt\":null,\"description\":\"instead of x and y: a world point [x, y, z], projected onto the plane al"
     "ong its normal (off_plane says how far), or {\\\"body\\\": b, \\\"face\\\": f} a cylindrical face or {\\\""
     "body\\\": b, \\\"edge\\\": e} a circular edge whose axis the hole shares. A world point moves with mod"
     "eling_origin; x and y replay on any printer\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"desc"
     "ription\":\"the body to drill; omit for a face plane's body, else the last body\"},{\"name\":\"style\","
     "\"type\":\"integer\",\"default\":0,\"description\":\"0=simple, 1=counterbore, 2=countersink\"},{\"name\":\"cb"
     "ore_diameter\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0},{\"name\":\"cbore_depth\",\"type\":\"number\",\"un"
     "it\":\"mm\",\"default\":0},{\"name\":\"csink_diameter\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0},{\"name\":"
     "\"csink_angle\",\"type\":\"number\",\"unit\":\"deg\",\"default\":90},{\"name\":\"standard\",\"type\":\"string\",\"def"
     "ault\":\"\",\"description\":\"provenance designation, e.g. M6\"}],\"examples\":[{\"title\":\"A counterbored "
     "hole for an M6 cap screw through a 10 mm block's top face\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"d"
     "istance\":10,\"height\":30,\"width\":30}},{\"tool\":\"query_topology\",\"args\":{\"at_extreme\":\"+z\",\"body\":0"
     "}},{\"tool\":\"hole_styled\",\"args\":{\"cbore_depth\":6,\"cbore_diameter\":11,\"diameter\":6.6,\"generation\""
     ":{\"from_call\":1,\"key\":\"generation\"},\"plane\":{\"body\":0,\"face\":5},\"style\":1}}]},{\"title\":\"A counte"
     "rsunk hole through a plate's top face\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":"
     "20,\"width\":20}},{\"tool\":\"query_topology\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"hole_styl"
     "ed\",\"args\":{\"csink_angle\":90,\"csink_diameter\":9,\"diameter\":4.5,\"generation\":{\"from_call\":1,\"key\""
     ":\"generation\"},\"plane\":{\"body\":0,\"face\":5},\"style\":2}}]}],\"ids\":true}"},
    {"hole_standard", L_CONTEXT("Hole standard", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Drill a standard clearance hole (ISO 273 / ANSI) at (x,y) or `at` on a plane.\",\"deta"
     "ils\":\"style: 0=simple, 1=counterbore, 2=countersink. Reply as hole.\",\"params\":[{\"name\":\"designat"
     "ion\",\"type\":\"string\",\"description\":\"e.g. M6, 1/4-20\"},{\"name\":\"style\",\"type\":\"integer\",\"default\""
     ":0},{\"name\":\"through\",\"type\":\"boolean\",\"default\":true},{\"name\":\"depth\",\"type\":\"number\",\"unit\":\"m"
     "m\",\"default\":10},{\"name\":\"x\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0,\"description\":\"offset in th"
     "e plane's frame from its origin, NOT world\"},{\"name\":\"y\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0"
     ",\"description\":\"offset in the plane's frame from its origin, NOT world\"},{\"name\":\"plane\",\"type\":"
     "\"string|object\",\"default\":\"XY\",\"description\":\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through modeling_origin; "
     "{\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\": b, \\\"face\\\": f}, a planar face to drill into that"
     " body (origin at its centroid, normal into the body, x along its longest edge). The reply's fram"
     "e gives the axes.\"},{\"name\":\"at\",\"type\":\"array|object\",\"default\":null,\"description\":\"instead of "
     "x and y: a world point [x, y, z], projected onto the plane along its normal (off_plane says how "
     "far), or {\\\"body\\\": b, \\\"face\\\": f} a cylindrical face or {\\\"body\\\": b, \\\"edge\\\": e} a circular "
     "edge whose axis the hole shares. A world point moves with modeling_origin; x and y replay on any"
     " printer\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"the body to drill; omit f"
     "or a face plane's body, else the last body\"}],\"examples\":[{\"title\":\"An M5 clearance hole through"
     " a plate's top face\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\""
     "tool\":\"query_topology\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"hole_standard\",\"args\":{\"des"
     "ignation\":\"M5\",\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"plane\":{\"body\":0,\"face\":5}}}]},{"
     "\"title\":\"An M6 counterbored clearance hole through a 10 mm block's top face\",\"calls\":[{\"tool\":\"e"
     "xtrude\",\"args\":{\"distance\":10,\"height\":30,\"width\":30}},{\"tool\":\"query_topology\",\"args\":{\"at_extr"
     "eme\":\"+z\",\"body\":0}},{\"tool\":\"hole_standard\",\"args\":{\"designation\":\"M6\",\"generation\":{\"from_call"
     "\":1,\"key\":\"generation\"},\"plane\":{\"body\":0,\"face\":5},\"style\":1}}]}],\"ids\":true}"},
    {"bridge", L_CONTEXT("Bridge", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, true,
     "{\"summary\":\"Build a cubic-Bezier G1 bridge (BSpline) between two sketch-entity endpoints within "
     "a sketch feature.\",\"details\":\"\",\"params\":[{\"name\":\"sketch\",\"type\":\"integer\",\"description\":\"sketc"
     "h feature index\"},{\"name\":\"ent_a\",\"type\":\"integer\",\"description\":\"first entity index within the "
     "sketch\"},{\"name\":\"end_a\",\"type\":\"integer\",\"default\":1,\"enum\":[0,1],\"description\":\"0 = start/p0 s"
     "ide, 1 = end/p1 side\"},{\"name\":\"ent_b\",\"type\":\"integer\",\"description\":\"second entity index withi"
     "n the sketch\"},{\"name\":\"end_b\",\"type\":\"integer\",\"default\":0,\"enum\":[0,1],\"description\":\"0 = star"
     "t/p0 side, 1 = end/p1 side\"}],\"examples\":[{\"title\":\"A smooth curve joining the facing ends of tw"
     "o lines in a committed sketch\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"s"
     "ketch_add\",\"args\":{\"entities\":[{\"p0\":[-20,0],\"p1\":[-5,0],\"type\":\"line\"},{\"p0\":[5,10],\"p1\":[20,10"
     "],\"type\":\"line\"}]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\"bridge\",\"args\":{\"end_a\":1,\"end_b"
     "\":0,\"ent_a\":0,\"ent_b\":1,\"sketch\":0}}]}],\"ids\":false}"},
    {"set_variable", L_CONTEXT("Set variable", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, true,
     "{\"summary\":\"Set a design variable to an expression, or remove it with an empty expr.\",\"details\":"
     "\"Feature fields bound with edit_feature follow it.\",\"params\":[{\"name\":\"name\",\"type\":\"string\"},{\""
     "name\":\"expr\",\"type\":\"string\",\"description\":\"e.g. \\\"20\\\" or \\\"width / 2 + 3\\\"; \\\"\\\" removes the v"
     "ariable\"}],\"examples\":[{\"title\":\"A variable w of 20\",\"calls\":[{\"tool\":\"set_variable\",\"args\":{\"ex"
     "pr\":\"20\",\"name\":\"w\"}}]},{\"title\":\"A variable worked out from another: h = w / 2 + 3, which is 13"
     "\",\"calls\":[{\"tool\":\"set_variable\",\"args\":{\"expr\":\"20\",\"name\":\"w\"}},{\"tool\":\"set_variable\",\"args\""
     ":{\"expr\":\"w / 2 + 3\",\"name\":\"h\"}}]},{\"title\":\"A variable removed with an empty expression\",\"call"
     "s\":[{\"tool\":\"set_variable\",\"args\":{\"expr\":\"20\",\"name\":\"w\"}},{\"tool\":\"set_variable\",\"args\":{\"expr"
     "\":\"\",\"name\":\"w\"}}]}],\"ids\":false}"},
    {"set_feature", L_CONTEXT("Set feature", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Change a feature's editable fields as its card's Confirm does when editing: one undo"
     " step and a rebuild.\",\"details\":\"A field an expression drives is refused. Reply: changed {field:"
     " [old, new]}.\",\"params\":[{\"name\":\"feature\",\"type\":\"integer\",\"description\":\"feature index (descri"
     "be_scene)\"},{\"name\":\"fields\",\"type\":\"object\",\"description\":\"{field: value} from describe_feature"
     "'s editable, e.g. {\\\"distance\\\": 15}: numbers within its ranges; mode new|union|subtract|interse"
     "ct; extrude_end blind|symmetric|two_sided|through_all; shape rectangle|circle; xf_* vectors [x, "
     "y, z]\"}],\"examples\":[{\"title\":\"A plate made 15 mm thick\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"dis"
     "tance\":5,\"height\":20,\"width\":20}},{\"tool\":\"set_feature\",\"args\":{\"feature\":1,\"fields\":{\"distance\""
     ":15}}}]},{\"title\":\"A plate's hole widened from 5 to 8 mm\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"di"
     "stance\":5,\"height\":20,\"width\":20}},{\"tool\":\"hole\",\"args\":{\"diameter\":5,\"through\":true}},{\"tool\":"
     "\"set_feature\",\"args\":{\"feature\":2,\"fields\":{\"hole_diameter\":8}}}]}],\"ids\":false}"},
    {"feature_delete", L_CONTEXT("Feature delete", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Delete a feature as its tree row's Delete does; deleting a sketch also deletes the f"
     "eatures built on it.\",\"details\":\"One undo step. Later indices shift. Reply: removed (how many fe"
     "atures went), features, bodies.\",\"params\":[{\"name\":\"feature\",\"type\":\"integer\",\"description\":\"fea"
     "ture index (describe_scene)\"}],\"examples\":[{\"title\":\"A plate's hole deleted\",\"calls\":[{\"tool\":\"e"
     "xtrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"hole\",\"args\":{\"diameter\":5,\"throu"
     "gh\":true}},{\"tool\":\"feature_delete\",\"args\":{\"feature\":2}}]},{\"title\":\"A committed sketch deleted"
     ", and the extrude built on it with it\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\""
     "tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch_commit\",\"args\":{}},{\"tool\":\""
     "extrude\",\"args\":{\"distance\":5,\"sketch\":0}},{\"tool\":\"feature_delete\",\"args\":{\"feature\":0}}]}],\"id"
     "s\":false}"},
    {"feature_move", L_CONTEXT("Feature move", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Move a feature one place up or down the history, as Move up / Move down do.\",\"detail"
     "s\":\"A feature cannot come before one it uses. One undo step. Reply: feature (its new index), fro"
     "m.\",\"params\":[{\"name\":\"feature\",\"type\":\"integer\",\"description\":\"feature index (describe_scene)\"}"
     ",{\"name\":\"delta\",\"type\":\"integer\",\"enum\":[-1,1],\"description\":\"-1 up, 1 down\"}],\"examples\":[{\"ti"
     "tle\":\"A datum plane moved up the history, above the extrude before it\",\"calls\":[{\"tool\":\"extrude"
     "\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"plane\",\"args\":{\"base\":\"XY\",\"offset\":20,"
     "\"type\":\"offset\"}},{\"tool\":\"feature_move\",\"args\":{\"delta\":-1,\"feature\":2}}]}],\"ids\":false}"},
    {"feature_set_enabled", L_CONTEXT("Feature set enabled", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Show or hide (suppress) a feature as its row's eye does.\",\"details\":\"A hidden featur"
     "e is left out of the rebuild and bodies may renumber. One undo step, none when it already was. R"
     "eply: enabled, changed, bodies.\",\"params\":[{\"name\":\"feature\",\"type\":\"integer\",\"description\":\"fea"
     "ture index (describe_scene)\"},{\"name\":\"enabled\",\"type\":\"boolean\"}],\"examples\":[{\"title\":\"A plate"
     "'s hole hidden: the plate rebuilds without it\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\""
     "height\":20,\"width\":20}},{\"tool\":\"hole\",\"args\":{\"diameter\":5,\"through\":true}},{\"tool\":\"feature_se"
     "t_enabled\",\"args\":{\"enabled\":false,\"feature\":2}}]},{\"title\":\"A hidden hole shown again\",\"calls\":"
     "[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"hole\",\"args\":{\"diamete"
     "r\":5,\"through\":true}},{\"tool\":\"feature_set_enabled\",\"args\":{\"enabled\":false,\"feature\":2}},{\"tool"
     "\":\"feature_set_enabled\",\"args\":{\"enabled\":true,\"feature\":2}}]}],\"ids\":false}"},
    {"feature_rename", L_CONTEXT("Feature rename", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Rename a feature as F2 on its row does.\",\"details\":\"Its own undo step. Reply: name, "
     "changed.\",\"params\":[{\"name\":\"feature\",\"type\":\"integer\",\"description\":\"feature index (describe_sc"
     "ene)\"},{\"name\":\"name\",\"type\":\"string\",\"description\":\"trimmed; not empty\"}],\"examples\":[{\"title\":"
     "\"A plate's extrude renamed Base plate\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":"
     "20,\"width\":20}},{\"tool\":\"feature_rename\",\"args\":{\"feature\":1,\"name\":\"Base plate\"}}]}],\"ids\":fals"
     "e}"},
    {"body_set", L_CONTEXT("Body set", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Rename, colour or show/hide a body, as its Bodies row and the Color button do; give "
     "one or more.\",\"details\":\"Not in the undo history. The name and colour are saved with the project"
     " and the name names its committed object; visibility is view state, and commit leaves hidden bod"
     "ies out. Reply: name, color, visible.\",\"params\":[{\"name\":\"body\",\"type\":\"integer\",\"description\":\""
     "body index (describe_scene)\"},{\"name\":\"name\",\"type\":\"string\",\"default\":null,\"description\":\"trimm"
     "ed; not empty\"},{\"name\":\"color\",\"type\":\"string\",\"default\":null,\"description\":\"\\\"#RRGGBB\\\"\"},{\"na"
     "me\":\"visible\",\"type\":\"boolean\",\"default\":null}],\"examples\":[{\"title\":\"A plate's body named Base "
     "and coloured blue\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"to"
     "ol\":\"body_set\",\"args\":{\"body\":0,\"color\":\"#3366CC\",\"name\":\"Base\"}}]},{\"title\":\"The second of two "
     "bodies hidden\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":"
     "\"extrude\",\"args\":{\"boolean\":\"new\",\"distance\":10,\"profile\":[[20,-5],[30,-5],[30,5],[20,5]]}},{\"to"
     "ol\":\"body_set\",\"args\":{\"body\":1,\"visible\":false}}]}],\"ids\":false}"},
    {"body_delete", L_CONTEXT("Body delete", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Delete a body as its row's Delete does: the feature that made it goes, which can tak"
     "e other features and bodies with it.\",\"details\":\"One undo step. Reply: deleted_feature {index, t"
     "ype, name}, removed, features, bodies.\",\"params\":[{\"name\":\"body\",\"type\":\"integer\",\"description\":"
     "\"body index (describe_scene)\"}],\"examples\":[{\"title\":\"The second of two bodies deleted, with the"
     " extrude that made it\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},"
     "{\"tool\":\"extrude\",\"args\":{\"boolean\":\"new\",\"distance\":10,\"profile\":[[20,-5],[30,-5],[30,5],[20,5]"
     "]}},{\"tool\":\"body_delete\",\"args\":{\"body\":1}}]}],\"ids\":false}"},
    {"commit", L_CONTEXT("Commit", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Commit to Plate: send the visible bodies to Prepare at their Move-gizmo positions an"
     "d show Prepare.\",\"details\":\"It is one Prepare undo step. A lone body is sent even when hidden. A"
     " body named in the Bodies list or with body_set names its object or part. Reply: objects (list_o"
     "bjects index after replace, name, parts: how many), bodies_sent, bodies_hidden, replaced.\",\"para"
     "ms\":[{\"name\":\"mode\",\"type\":\"string\",\"default\":\"\",\"enum\":[\"assembly\",\"bodies\"],\"description\":\"ass"
     "embly: one object with a part per body; bodies: an object per body. Default: the Commit button's"
     " mode, which this leaves as it is.\"},{\"name\":\"replace\",\"type\":\"array\",\"default\":[],\"description\""
     ":\"list_objects indices to delete in the same undo step, such as an earlier commit. Their setting"
     "s, modifiers and paint go with them.\"}],\"examples\":[{\"title\":\"A plate sent to Prepare as one obj"
     "ect\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"commit\",\""
     "args\":{}}]},{\"title\":\"Two bodies sent to Prepare as an object each\",\"calls\":[{\"tool\":\"extrude\",\""
     "args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"extrude\",\"args\":{\"boolean\":\"new\",\"distance"
     "\":10,\"profile\":[[20,-5],[30,-5],[30,5],[20,5]]}},{\"tool\":\"commit\",\"args\":{\"mode\":\"bodies\"}}]}],\""
     "ids\":false}"},
    {"new_design", L_CONTEXT("New design", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"New Design: erase every feature, body and variable and the Design undo history.\",\"de"
     "tails\":\"Committed objects stay on the plate. Reply: cleared {features, bodies, variables}.\",\"par"
     "ams\":[{\"name\":\"discard_changes\",\"type\":\"boolean\",\"default\":false,\"description\":\"needed when the "
     "design is not empty\"}],\"examples\":[{\"title\":\"A plate's design erased\",\"calls\":[{\"tool\":\"extrude\""
     ",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"new_design\",\"args\":{\"discard_changes\":tr"
     "ue}}]},{\"title\":\"An empty design started over, which needs no discard_changes\",\"calls\":[{\"tool\":"
     "\"new_design\",\"args\":{}}]}],\"ids\":false}"},
    {"export_step", L_CONTEXT("Export step", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Export STEP: write every body to a file as native B-rep, at its Move-gizmo position."
     "\",\"details\":\"Reply: path, bytes, bodies, moved_bodies.\",\"params\":[{\"name\":\"path\",\"type\":\"string\""
     ",\"description\":\"absolute path; its directory must exist\"},{\"name\":\"overwrite\",\"type\":\"boolean\",\""
     "default\":false,\"description\":\"replace an existing file\"}],\"examples\":[{\"title\":\"A plate written "
     "to a STEP file\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\""
     ":\"export_step\",\"args\":{\"path\":\"/path/to/part.step\"}}],\"needs\":\"a folder to write the STEP file i"
     "n\"}],\"ids\":false}"},
    {"sketch_begin", L_CONTEXT("Sketch begin", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Open a sketch session on a plane, as picking a plane or face for Sketch does.\",\"deta"
     "ils\":\"Sketch points are [x, y] in that plane's frame (mm), from its origin: modeling_origin for "
     "a base plane. Reply: frame.\",\"params\":[{\"name\":\"plane\",\"type\":\"string|object\",\"default\":\"XY\",\"de"
     "scription\":\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}, a Plane feature; o"
     "r {\\\"body\\\": b, \\\"face\\\": f}, a planar face (origin at its centroid, normal out of the body, x f"
     "rom the world axes). The reply's frame gives the axes.\"}],\"examples\":[{\"title\":\"A sketch on the "
     "XY plane\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}}]},{\"title\":\"A sketch on a plate"
     "'s top face\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"q"
     "uery_topology\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"sketch_begin\",\"args\":{\"generation\":"
     "{\"from_call\":1,\"key\":\"generation\"},\"plane\":{\"body\":0,\"face\":5}}}]},{\"title\":\"A sketch on a datum"
     " plane 20 mm above XY\",\"calls\":[{\"tool\":\"plane\",\"args\":{\"base\":\"XY\",\"offset\":20,\"type\":\"offset\"}"
     "},{\"tool\":\"sketch_begin\",\"args\":{\"plane\":{\"datum\":0}}}]}],\"ids\":true}"},
    {"sketch_commit", L_CONTEXT("Sketch commit", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Commit the open sketch as a Sketch feature, as the sketch's Confirm does, and leave "
     "sketch mode.\",\"details\":\"An empty sketch is refused and stays open. Reply: sketch_index, name, f"
     "eatures (the new count) and its loops (closed_loops with the index extrude's loop takes, open_en"
     "ds, defects, buildable).\",\"params\":[],\"examples\":[{\"title\":\"A rectangle around a circle, committ"
     "ed as a sketch of two loops\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"ske"
     "tch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"center\":[0,"
     "0],\"radius\":4,\"type\":\"circle\"}]}},{\"tool\":\"sketch_commit\",\"args\":{}}]}],\"ids\":false}"},
    {"sketch_cancel", L_CONTEXT("Sketch cancel", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Discard the open sketch and leave sketch mode.\",\"details\":\"\",\"params\":[],\"examples\":"
     "[{\"title\":\"Discard a sketch with a rectangle drawn\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"pla"
     "ne\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch_cancel\",\"args\":"
     "{}}]}],\"ids\":false}"},
    {"sketch_add", L_CONTEXT("Sketch add", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Add entities or shapes to the open sketch: `entities` (a list), or one item's fields"
     " beside `type`, or `rect`.\",\"details\":\"Entities: line {p0, p1}, circle {center, radius}, arc {ce"
     "nter, radius, start_angle, end_angle}, point {p}. Shapes, built as their drawing tools build the"
     "m: polygon {center, sides, radius (to a vertex) or across_flats, angle (of that vertex, or of th"
     "e first edge's midpoint)}, slot {c0, c1, width}, ellipse {center, radius (semi-major), rminor, r"
     "otation}, ellipse_arc {the ellipse's fields, start_angle, end_angle: parametric, counter-clockwi"
     "se}, spline {ctrl: 2 or more poles}. Each item of entities may carry its own construction: true;"
     " the top-level construction applies only to rect and to the single-type form. Points are [x, y] "
     "in the sketch plane (mm); angles are radians, or degrees as start_angle_deg and the like. A poly"
     "gon is lines; its side count is not kept once committed. Reply: first_index, added, shapes [{fir"
     "st, count}] per item, entities, dof.\",\"params\":[{\"name\":\"entities\",\"type\":\"array\",\"default\":[],\""
     "description\":\"entity or shape objects, each with a type\"},{\"name\":\"type\",\"type\":\"string\",\"defaul"
     "t\":\"\",\"enum\":[\"line\",\"arc\",\"circle\",\"point\",\"polygon\",\"slot\",\"ellipse\",\"ellipse_arc\",\"spline\"],\""
     "description\":\"one item, its fields beside it\"},{\"name\":\"rect\",\"type\":\"array\",\"default\":[],\"descr"
     "iption\":\"[x0, y0, x1, y1]: a closed rectangle of four lines\"},{\"name\":\"start_angle\",\"type\":\"numb"
     "er\",\"unit\":\"rad\",\"default\":0,\"description\":\"arc start, counter-clockwise from the plane's x axis"
     "\"},{\"name\":\"end_angle\",\"type\":\"number\",\"unit\":\"rad\",\"default\":0},{\"name\":\"construction\",\"type\":\""
     "boolean\",\"default\":false,\"description\":\"for rect and the single-type form; an item of entities c"
     "arries its own\"}],\"examples\":[{\"title\":\"A 30 x 20 mm rectangle\",\"calls\":[{\"tool\":\"sketch_begin\","
     "\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}}]},{\"title\":\"A hexag"
     "on 10 mm across flats inside a circle\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\""
     "tool\":\"sketch_add\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":12,\"type\":\"circle\"},{\"across_fla"
     "ts\":10,\"center\":[0,0],\"sides\":6,\"type\":\"polygon\"}]}}]},{\"title\":\"A quarter arc given in degrees,"
     " and a slot\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":"
     "{\"entities\":[{\"center\":[0,0],\"end_angle_deg\":90,\"radius\":20,\"start_angle_deg\":0,\"type\":\"arc\"},{\""
     "c0\":[-10,-20],\"c1\":[10,-20],\"type\":\"slot\",\"width\":6}]}}]}],\"ids\":false}"},
    {"sketch_select", L_CONTEXT("Sketch select", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Select sketch entities by index; an empty list clears the selection.\",\"details\":\"Rep"
     "ly: selected.\",\"params\":[{\"name\":\"entities\",\"type\":\"array\",\"default\":[],\"description\":\"entity in"
     "dices\"}],\"examples\":[{\"title\":\"Select a rectangle's two long sides\",\"calls\":[{\"tool\":\"sketch_beg"
     "in\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch"
     "_select\",\"args\":{\"entities\":[0,2]}}]},{\"title\":\"Clear the selection\",\"calls\":[{\"tool\":\"sketch_be"
     "gin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketc"
     "h_select\",\"args\":{\"entities\":[0]}},{\"tool\":\"sketch_select\",\"args\":{\"entities\":[]}}]}],\"ids\":fals"
     "e}"},
    {"sketch_delete", L_CONTEXT("Sketch delete", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Delete sketch entities.\",\"details\":\"Reply: removed, entities.\",\"params\":[{\"name\":\"en"
     "tities\",\"type\":\"array\",\"default\":[],\"description\":\"entity indices; omit for the selection\"}],\"ex"
     "amples\":[{\"title\":\"Delete a rectangle's top side\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane"
     "\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch_delete\",\"args\":{\""
     "entities\":[2]}}]},{\"title\":\"Delete the selected sides\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\""
     "plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch_select\",\"arg"
     "s\":{\"entities\":[1,3]}},{\"tool\":\"sketch_delete\",\"args\":{}}]}],\"ids\":false}"},
    {"sketch_construction", L_CONTEXT("Sketch construction", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, true,
     "{\"summary\":\"Toggle construction on sketch entities.\",\"details\":\"Reply: changed.\",\"params\":[{\"nam"
     "e\":\"entities\",\"type\":\"array\",\"default\":[],\"description\":\"entity indices; omit for the selection\""
     "}],\"examples\":[{\"title\":\"A centre line across a rectangle, made construction so it splits no loo"
     "p\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-"
     "15,-10,15,10]}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"p0\":[0,-10],\"p1\":[0,10],\"type\":\"line\""
     "}]}},{\"tool\":\"sketch_construction\",\"args\":{\"entities\":[4]}}]}],\"ids\":false}"},
    {"sketch_offset", L_CONTEXT("Sketch offset", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Add an offset copy of sketch lines, arcs and circles.\",\"details\":\"Reply: first_index"
     ", added, dof.\",\"params\":[{\"name\":\"distance\",\"type\":\"number\",\"unit\":\"mm\",\"description\":\"signed of"
     "fset\"},{\"name\":\"entities\",\"type\":\"array\",\"default\":[],\"description\":\"entity indices; omit for th"
     "e selection, else every entity\"}],\"examples\":[{\"title\":\"A rectangle's 2 mm inner wall\",\"calls\":["
     "{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10"
     "]}},{\"tool\":\"sketch_offset\",\"args\":{\"distance\":2}}]},{\"title\":\"A circle grown 2 mm by a negative"
     " offset\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"en"
     "tities\":[{\"center\":[0,0],\"radius\":5,\"type\":\"circle\"}]}},{\"tool\":\"sketch_offset\",\"args\":{\"distanc"
     "e\":-2,\"entities\":[0]}}]}],\"ids\":false}"},
    {"sketch_mirror", L_CONTEXT("Sketch mirror", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Add a mirrored copy of sketch entities about the line through axis_a and axis_b.\",\"d"
     "etails\":\"Reply: first_index, added, dof.\",\"params\":[{\"name\":\"axis_a\",\"type\":\"array\",\"default\":[0"
     ",0],\"description\":\"[x, y] in the sketch plane, mm\"},{\"name\":\"axis_b\",\"type\":\"array\",\"default\":[0"
     ",1],\"description\":\"[x, y] in the sketch plane, mm\"},{\"name\":\"entities\",\"type\":\"array\",\"default\":"
     "[],\"description\":\"entity indices; omit for the selection, else every entity\"}],\"examples\":[{\"tit"
     "le\":\"A circle mirrored across the sketch's y axis\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plan"
     "e\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"center\":[10,0],\"radius\":3,\"type\":\"circle\"}]"
     "}},{\"tool\":\"sketch_mirror\",\"args\":{\"axis_a\":[0,0],\"axis_b\":[0,1]}}]}],\"ids\":false}"},
    {"sketch_heal", L_CONTEXT("Sketch heal", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Weld sketch endpoints that nearly meet and constrain them coincident, so loops are c"
     "losed by construction.\",\"details\":\"Reply: welded and the loops.\",\"params\":[{\"name\":\"tolerance\",\""
     "type\":\"number\",\"unit\":\"mm\",\"default\":0.001},{\"name\":\"ignore_construction\",\"type\":\"boolean\",\"defa"
     "ult\":true}],\"examples\":[{\"title\":\"Close a triangle's 0.05 mm gap\",\"calls\":[{\"tool\":\"sketch_begin"
     "\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"p0\":[0,0],\"p1\":[20,0],\"type\""
     ":\"line\"},{\"p0\":[20,0.05],\"p1\":[20,10],\"type\":\"line\"},{\"p0\":[20,10],\"p1\":[0,0],\"type\":\"line\"}]}},"
     "{\"tool\":\"sketch_heal\",\"args\":{\"tolerance\":0.1}}]}],\"ids\":false}"},
    {"sketch_set_value", L_CONTEXT("Sketch set value", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Set the dimension of the selected sketch entities, as the value field does.\",\"detail"
     "s\":\"A line's length, a circle's diameter, an arc's radius, or the angle or distance between two."
     " Select them with sketch_select first. Reply: kind, before, value, dof and the loops.\",\"params\":"
     "[{\"name\":\"value\",\"type\":\"number\",\"description\":\"mm, or degrees for an angle\"}],\"examples\":[{\"tit"
     "le\":\"A rectangle's bottom side made 40 mm long\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":"
     "\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch_select\",\"args\":{\"en"
     "tities\":[0]}},{\"tool\":\"sketch_set_value\",\"args\":{\"value\":40}}]},{\"title\":\"A circle's diameter se"
     "t to 12 mm\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{"
     "\"entities\":[{\"center\":[0,0],\"radius\":4,\"type\":\"circle\"}]}},{\"tool\":\"sketch_select\",\"args\":{\"enti"
     "ties\":[0]}},{\"tool\":\"sketch_set_value\",\"args\":{\"value\":12}}]}],\"ids\":false}"},
    {"sketch_fillet", L_CONTEXT("Sketch fillet", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Round the corner of two lines of the open sketch, as the Fillet tool does.\",\"details"
     "\":\"Both lines are cut back and joined by an arc of `radius`, bound coincident and tangent to the"
     "m; constraints the sharp corner held are released. Reply: entity (the arc), lines, released_cons"
     "traints, entities, constraints, dof, solve_ok.\",\"params\":[{\"name\":\"entities\",\"type\":\"array\",\"def"
     "ault\":[],\"description\":\"the two line indices; omit for the selection\"},{\"name\":\"radius\",\"type\":\""
     "number\",\"unit\":\"mm\"}],\"examples\":[{\"title\":\"A rectangle's corner rounded to 3 mm\",\"calls\":[{\"too"
     "l\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{"
     "\"tool\":\"sketch_fillet\",\"args\":{\"entities\":[0,1],\"radius\":3}}]}],\"ids\":false}"},
    {"sketch_chamfer", L_CONTEXT("Sketch chamfer", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Bevel the corner of two lines of the open sketch, as the Chamfer tool does.\",\"detail"
     "s\":\"Both lines are cut back by `distance` from the corner and joined by a line bound coincident "
     "to them. Reply: entity (the new line), lines, released_constraints, entities, constraints, dof, "
     "solve_ok.\",\"params\":[{\"name\":\"entities\",\"type\":\"array\",\"default\":[],\"description\":\"the two line "
     "indices; omit for the selection\"},{\"name\":\"distance\",\"type\":\"number\",\"unit\":\"mm\",\"description\":\""
     "setback along each line\"}],\"examples\":[{\"title\":\"A rectangle's corner bevelled 2 mm back\",\"calls"
     "\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15"
     ",10]}},{\"tool\":\"sketch_chamfer\",\"args\":{\"distance\":2,\"entities\":[1,2]}}]}],\"ids\":false}"},
    {"sketch_trim", L_CONTEXT("Sketch trim", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Trim or extend the open sketch's entity nearest `at`, as a Trim or Extend click ther"
     "e does.\",\"details\":\"Trim: a line or an arc is cut back from its end nearer `at` to the first cro"
     "ssing with another entity beyond `at`; a circle loses the part between the crossings either side"
     " of `at` and becomes an arc. With extend, as an Extend click: the line's or arc's end nearer `at"
     "` runs out to the next entity. The entity's constraints are released. Reply: entity, geometry (i"
     "ts new fields), released_constraints, entities, dof.\",\"params\":[{\"name\":\"at\",\"type\":\"array\",\"des"
     "cription\":\"[x, y] in the sketch plane, mm: a point on the part to cut, or near the end to extend"
     "\"},{\"name\":\"extend\",\"type\":\"boolean\",\"default\":false},{\"name\":\"tolerance\",\"type\":\"number\",\"unit\""
     ":\"mm\",\"default\":0.05,\"description\":\"how far from `at` the entity may be\"}],\"examples\":[{\"title\":"
     "\"A line cut back to the line crossing it\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}}"
     ",{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"p0\":[-10,0],\"p1\":[10,0],\"type\":\"line\"},{\"p0\":[0,-5],"
     "\"p1\":[0,5],\"type\":\"line\"}]}},{\"tool\":\"sketch_trim\",\"args\":{\"at\":[5,0]}}]},{\"title\":\"A line exten"
     "ded down to the line below it\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"s"
     "ketch_add\",\"args\":{\"entities\":[{\"p0\":[-10,0],\"p1\":[10,0],\"type\":\"line\"},{\"p0\":[0,3],\"p1\":[0,8],\""
     "type\":\"line\"}]}},{\"tool\":\"sketch_trim\",\"args\":{\"at\":[0,3],\"extend\":true}}]}],\"ids\":false}"},
    {"sketch_transform", L_CONTEXT("Sketch transform", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Move, rotate, scale or array entities of the open sketch, as the transform tools' Co"
     "nfirm does.\",\"details\":\"move {dx, dy}, rotate {angle, pivot} and scale {scale, pivot} change the"
     " entities and release the constraints that no longer hold. array {dx, dy, count} adds copies ste"
     "pped by dx, dy; polar_array {pivot, angle, count} adds copies every angle/count about pivot (ang"
     "le is the whole sweep, default 360 degrees). count includes the original, and each copy is bound"
     " to its source (equal length and parallel, or its radius). pivot defaults to the entities' centr"
     "oid. Angles are radians, or degrees as angle_deg. Reply: moved and released_constraints, or firs"
     "t_index, added and bound; pivot; entities, dof.\",\"params\":[{\"name\":\"op\",\"type\":\"string\",\"enum\":["
     "\"move\",\"rotate\",\"scale\",\"array\",\"polar_array\"]},{\"name\":\"entities\",\"type\":\"array\",\"default\":[],\""
     "description\":\"entity indices; omit for the selection\"},{\"name\":\"dx\",\"type\":\"number\",\"unit\":\"mm\","
     "\"default\":0},{\"name\":\"dy\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0},{\"name\":\"angle\",\"type\":\"numbe"
     "r\",\"unit\":\"rad\",\"default\":null,\"nullable\":false,\"description\":\"or angle_deg\"},{\"name\":\"scale\",\"t"
     "ype\":\"number\",\"default\":null,\"nullable\":false},{\"name\":\"count\",\"type\":\"integer\",\"default\":null,\""
     "nullable\":false,\"min\":2,\"max\":1000},{\"name\":\"pivot\",\"type\":\"array\",\"default\":null,\"nullable\":fal"
     "se,\"description\":\"[x, y] in the sketch plane, mm\"}],\"examples\":[{\"title\":\"A circle moved 10 mm a"
     "long x\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"ent"
     "ities\":[{\"center\":[0,0],\"radius\":3,\"type\":\"circle\"}]}},{\"tool\":\"sketch_transform\",\"args\":{\"dx\":1"
     "0,\"entities\":[0],\"op\":\"move\"}}]},{\"title\":\"Six holes on a 40 mm bolt circle\",\"calls\":[{\"tool\":\"s"
     "ketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"center\":[20,0],\"r"
     "adius\":3,\"type\":\"circle\"}]}},{\"tool\":\"sketch_transform\",\"args\":{\"angle_deg\":360,\"count\":6,\"entit"
     "ies\":[0],\"op\":\"polar_array\",\"pivot\":[0,0]}}]},{\"title\":\"A line turned 30 degrees about its start"
     "\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\""
     ":[{\"p0\":[0,0],\"p1\":[20,0],\"type\":\"line\"}]}},{\"tool\":\"sketch_transform\",\"args\":{\"angle_deg\":30,\"e"
     "ntities\":[0],\"op\":\"rotate\",\"pivot\":[0,0]}}]}],\"ids\":false}"},
    {"sketch_constrain", L_CONTEXT("Sketch constrain", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Add a constraint to the open sketch, as a CONSTRAIN bar button does (sketch_set_valu"
     "e is the value field, V).\",\"details\":\"With `entities`, else the selection, the button's own rule"
     "s choose the points, and a value it would ask for is `value` or the current one. With `refs` the"
     " references are taken as written, in sketch_constraints' form: {entity, role} for a point (role "
     "p0, p1 or center; entity also \\\"origin\\\") and {entity} for a whole entity (also \\\"x_axis\\\", \\\"y_"
     "axis\\\"). distance, lock_x, lock_y, point_on_line and point_on_object have no button and take ref"
     "s. A constraint the sketch cannot solve with is rolled back and the call fails. Reply: added (in"
     "dices), items, constraints, dof, solve_ok.\",\"params\":[{\"name\":\"type\",\"type\":\"string\",\"enum\":[\"fi"
     "x\",\"coincident\",\"horizontal\",\"vertical\",\"distance\",\"lock_x\",\"lock_y\",\"equal_length\",\"parallel\",\""
     "perpendicular\",\"concentric\",\"tangent\",\"midpoint\",\"symmetric\",\"angle\",\"radius\",\"diameter\",\"point_"
     "on_line\",\"point_on_object\",\"equal_radius\",\"collinear\",\"distance_x\",\"distance_y\",\"symmetric_about"
     "_y\",\"symmetric_about_x\",\"equal\"],\"description\":\"\\\"equal\\\" is the Equal button: equal length, or "
     "equal radius for two curves\"},{\"name\":\"entities\",\"type\":\"array\",\"default\":[],\"description\":\"1 to"
     " 3 entity indices; omit for the selection\"},{\"name\":\"refs\",\"type\":\"array\",\"default\":[],\"descript"
     "ion\":\"[{entity, role?}], instead of entities\"},{\"name\":\"value\",\"type\":\"number\",\"default\":null,\"d"
     "escription\":\"mm, or degrees for an angle\"}],\"examples\":[{\"title\":\"A sloping line made horizontal"
     "\",\"calls\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\""
     ":[{\"p0\":[0,0],\"p1\":[10,2],\"type\":\"line\"}]}},{\"tool\":\"sketch_constrain\",\"args\":{\"entities\":[0],\"t"
     "ype\":\"horizontal\"}}]},{\"title\":\"Two circles given equal radii\",\"calls\":[{\"tool\":\"sketch_begin\",\""
     "args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":4,\"type\""
     ":\"circle\"},{\"center\":[15,0],\"radius\":6,\"type\":\"circle\"}]}},{\"tool\":\"sketch_constrain\",\"args\":{\"e"
     "ntities\":[0,1],\"type\":\"equal\"}}]},{\"title\":\"A line's ends held 25 mm apart, named by refs\",\"call"
     "s\":[{\"tool\":\"sketch_begin\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"entities\":[{\"p0\""
     ":[0,0],\"p1\":[10,2],\"type\":\"line\"}]}},{\"tool\":\"sketch_constrain\",\"args\":{\"refs\":[{\"entity\":0,\"rol"
     "e\":\"p0\"},{\"entity\":0,\"role\":\"p1\"}],\"type\":\"distance\",\"value\":25}}]}],\"ids\":false}"},
    {"sketch_unconstrain", L_CONTEXT("Sketch unconstrain", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Delete a constraint of the open sketch, as its row's delete button does; the constra"
     "ints after it move down one index.\",\"details\":\"Reply: removed, constraints, dof, solve_ok.\",\"par"
     "ams\":[{\"name\":\"index\",\"type\":\"integer\",\"description\":\"sketch_constraints' items[].index\"}],\"exam"
     "ples\":[{\"title\":\"A rectangle's bottom side freed from horizontal\",\"calls\":[{\"tool\":\"sketch_begin"
     "\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_add\",\"args\":{\"rect\":[-15,-10,15,10]}},{\"tool\":\"sketch_c"
     "onstraints\",\"args\":{}},{\"tool\":\"sketch_unconstrain\",\"args\":{\"index\":4}}]}],\"ids\":false}"},
};
static const int kDesignToolCount = 134;

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_DesignOffer_hpp_
