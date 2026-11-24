// GENERATED FILE — DO NOT EDIT.
// Source: scripts/CAD/tool_atlas.json   Generator: scripts/CAD/gen_offer_table.py
//
// The Design tab's table. Its verbs are the object-driven tool offer (charter 4.1): every verb
// has ONE row index, that index is the same in every selection it appears in, and verbs that do
// not apply are shown disabled in place with their reason rather than removed. Row order was
// ratified 2026-07-31; changing an index is a breaking change to every user's muscle memory.
// An entry with a handler or a tool block, and no fold_into, is a command of the catalog's group Design
// (Mcp::register_design_tools(), Mcp::is_design_command()), under its section in tool_list's index. A
// call whose arguments are exactly those that choose one of its items opens that item's tool for a person
// (Mcp::opened_item()): none choose the entry's own item where it has a handler, else the items folded into it
// with {}; a folded item's args, or the entry's own, choose that item. Any other call runs its tool block's
// silent form. A folded item (fold_into) is no command: it is a way of calling the command it names. An item
// with neither a handler nor a tool block is greyed, and no command. An entry with no menu item is a command only.
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
// drives the live sketch session, and Change edits the feature list or variables, or sets what is
// selected in the model; both are refused under an open card, the Text dialog, Constrain mode or a body
// Move, and Change under a sketch session too, which is about to commit into the feature list and in
// which a click means something else. Ui acts on what the tab has open (its Confirm and Cancel), so only a
// rebuild refuses it, though it changes the document. None: an entry with no silent form, and the opening
// of a tool, which only a rebuild refuses, as the Design tab's keys and menu do.
enum class CadAccess { None, Read, Change, Sketch, Ui };

// One entry of the table. Its handler (`action`) routes to the code that already implements the verb:
//   "key:S+E"        -> m_keys_feature[SHIFT('E')]
//   "key:L"          -> m_keys_sketch['L']
//   "fly:material#4" -> row 4 of the "material" feature flyout
//   "btn:delete"     -> a standalone toolbar button
//   nullptr          -> no GUI path (the item, if it has one, shows disabled)
struct DesignTool {
    const char* id;          // also the key of its Design command
    const char* name;        // drawing-office word (L10); msgctxt "Design", translated at use
    int         row;         // index into kOfferRowNames, the ratified address — NEVER reorder;
                             // -1: no menu item, a command only
    const char* key;         // shortcut shown in the item, or nullptr
    const char* action;
    const char* refusal;     // why this item is greyed, in the product's own words
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
    const char* icon;         // resources/images name, or nullptr — the offer draws it beside the item
    const char* hint;         // what the verb does / what to click; shown on hover
    bool        modal;        // its handler opens a dialog that waits for an answer: a modal one, which holds
                              // the call until it is answered, or Text's, which is modeless and does not
    CadAccess   access;       // its silent form's, None without one
    bool        ok_false;     // its silent form reports a rejected rebuild in its reply (ok false)
    // Its silent form's summary, details, params, examples and ids as JSON text, the parameter
    // schema of record; nullptr for an entry whose command only does what its menu item does.
    const char* doc;
    const char* fold_into;    // the id of the command this item is a variant of, or nullptr
    // The arguments that choose this item, as JSON text: a folded item's, or those that choose the own item of
    // an entry with a handler and a tool block as a call with none does; nullptr for none.
    const char* args;
    // The heading its command is listed under in tool_list's index (kDesignSections,
    // Mcp::design_command_index()); nullptr for an entry that is no command.
    const char* section;
    // Its handler acts on the selected row of the Feature list, which a call of its command, not a person's
    // pick, takes as what the item is for while nothing is picked in the view (Mcp::open_refusal()).
    bool        feature_row;
};

// The headings of tool_list's index of the Design commands, in its order. Menu items holds the commands
// that take no arguments and only do what their menu item does.
static const char* const kDesignSections[] = {
    "Document",
    "Sketch",
    "Solid",
    "Surface",
    "Construct",
    "Inspect",
    "Bodies",
    "History",
    "Menu items",
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
    {"sketch_create", L_CONTEXT("Sketch", "Design"), 0, "Shift+S", "key:S+S", L("Select a flat face or a reference plane to sketch on"), 0x00000403u, 0, 0, false, false, nullptr, "design_sketch", L("Sketch on the selected flat face or plane, or click one next"), false, CadAccess::Sketch, false,
     "{\"summary\":\"Open a sketch session on a plane, as picking a plane or face for Sketch does.\",\"deta"
     "ils\":\"Sketch points are [x, y] in that plane's frame (mm), from its origin: modeling_origin for "
     "a base plane. Reply: frame.\",\"params\":[{\"name\":\"plane\",\"type\":\"string|object\",\"default\":\"XY\",\"de"
     "scription\":\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}, a Plane feature; o"
     "r {\\\"body\\\": b, \\\"face\\\": f}, a planar face (origin at its centroid, normal out of the body, x f"
     "rom the world axes). The reply's frame gives the axes.\"}],\"examples\":[{\"title\":\"A sketch on the "
     "XY plane\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}}]},{\"title\":\"A sketch on a plat"
     "e's top face\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\""
     "body_describe\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"sketch_create\",\"args\":{\"generation\""
     ":{\"from_call\":1,\"key\":\"generation\"},\"plane\":{\"body\":0,\"face\":5}}}]},{\"title\":\"A sketch on a datu"
     "m plane 20 mm above XY\",\"calls\":[{\"tool\":\"construct_plane\",\"args\":{\"base\":\"XY\",\"offset\":20,\"type"
     "\":\"offset\"}},{\"tool\":\"sketch_create\",\"args\":{\"plane\":{\"datum\":0}}}]}],\"ids\":true}", nullptr, nullptr, "Sketch", false},
    {"extrude", L_CONTEXT("Extrude", "Design"), 1, "Shift+E", "key:S+E", L("Create a sketch, or pick a solid face, first"), 0x00004002u, 0, 0, false, false, nullptr, "design_extrude", L("Extrude a sketch profile, or push/pull a picked face"), false, CadAccess::Change, false,
     "{\"summary\":\"Extrude a committed sketch, one of its loops, a face or a drawn profile into a body."
     "\",\"details\":\"A committed sketch, or one loop of it, extrudes as the Extrude card does; push/pull"
     " grows a planar face of a body; a profile drawn here is an explicit closed `profile` (list of [x"
     ",y]) or a centred width x height rectangle on `plane`. End conditions match Onshape. Reply: extr"
     "ude_index, the index of its sketch (-1 for a face), the operation and body used, end, frame.\",\"p"
     "arams\":[{\"name\":\"sketch\",\"type\":\"integer\",\"default\":null,\"description\":\"a committed Sketch featu"
     "re to extrude (sketch_finish's sketch_index); profile, width, height and plane are then refused\""
     "},{\"name\":\"loop\",\"type\":\"integer\",\"default\":null,\"description\":\"with sketch: one closed loop (cl"
     "osed_loops[].index of sketch_describe {sketch}) with the loops inside it as holes; a hole is a l"
     "oop of its own, so its index extrudes the disc. Omit for the whole sketch\"},{\"name\":\"face\",\"type"
     "\":\"integer\",\"default\":null,\"description\":\"push/pull: a planar face (body_describe id) of `body` "
     "grown along its outward normal (flip for inward); operation auto joins that body. sketch, loop, "
     "profile, width, height, plane and taper are then refused, and end is blind or symmetric\"},{\"name"
     "\":\"width\",\"type\":\"number\",\"unit\":\"mm\",\"default\":20},{\"name\":\"height\",\"type\":\"number\",\"unit\":\"mm\""
     ",\"default\":20},{\"name\":\"distance\",\"type\":\"number\",\"unit\":\"mm\",\"default\":10,\"exclusive_min\":0},{\""
     "name\":\"plane\",\"type\":\"string|object\",\"default\":\"XY\",\"description\":\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" thro"
     "ugh modeling_origin; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\": b, \\\"face\\\": f}, a planar fa"
     "ce (origin at its centroid, normal out of the body, x from the world axes). The reply's frame gi"
     "ves the axes.\"},{\"name\":\"profile\",\"type\":\"array\",\"default\":[],\"description\":\"optional closed con"
     "tour [[x,y],...] in plane mm; overrides width/height\"},{\"name\":\"operation\",\"type\":\"string\",\"defa"
     "ult\":null,\"enum\":[\"auto\",\"new_body\",\"join\",\"cut\",\"intersect\"],\"description\":\"what the result doe"
     "s to the bodies: new_body makes a body of its own, join fuses it into `body`, cut removes it fro"
     "m `body`, intersect keeps their overlap; auto joins the body the profile touches, else makes a n"
     "ew body, as the card's default. Default auto with sketch or face, new_body without\"},{\"name\":\"bo"
     "dy\",\"type\":\"integer\",\"default\":-1,\"description\":\"the body to join, cut or intersect, or whose fa"
     "ce is pushed; omit for the body a sketch touches, else the last body\"},{\"name\":\"end\",\"type\":\"str"
     "ing\",\"default\":\"blind\",\"enum\":[\"blind\",\"symmetric\",\"two_sided\",\"through_all\",\"up_to_face\"]},{\"na"
     "me\":\"distance2\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0,\"description\":\"second-side depth when en"
     "d=two_sided (else falls back to distance)\"},{\"name\":\"up_to_face\",\"type\":\"integer\",\"default\":-1,\""
     "description\":\"face id (body_describe) of the body the extrude works on, when end=up_to_face\"},{\""
     "name\":\"taper\",\"type\":\"number\",\"unit\":\"deg\",\"default\":0,\"description\":\"draft/taper of the side wa"
     "ll\"},{\"name\":\"flip\",\"type\":\"boolean\",\"default\":false}],\"examples\":[{\"title\":\"A 20 x 20 x 5 mm pl"
     "ate on the XY plane\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}}]},"
     "{\"title\":\"A committed sketch extruded 5 mm: a rectangle around a circle makes a plate with a bor"
     "e\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-"
     "15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"ce"
     "nter\":[0,0],\"radius\":4,\"type\":\"circle_center\"}]}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"ex"
     "trude\",\"args\":{\"distance\":5,\"sketch\":0}}]},{\"title\":\"A plate's top face pushed up 3 mm\",\"calls\":"
     "[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"body_describe\",\"args\":"
     "{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"extrude\",\"args\":{\"body\":0,\"distance\":3,\"face\":5,\"generati"
     "on\":{\"from_call\":1,\"key\":\"generation\"}}}]}],\"ids\":true}", nullptr, nullptr, "Solid", false},
    {"revolve", L_CONTEXT("Revolve", "Design"), 1, "Shift+R", "key:S+R", L("Create a sketch profile to revolve first"), 0x00004000u, 0, 0, false, false, nullptr, "design_revolve", L("Revolve a profile about an axis"), false, CadAccess::Change, false,
     "{\"summary\":\"Revolve a committed sketch as the Revolve card does, or a profile drawn here, about "
     "an axis.\",\"details\":\"A sketch turns about a line of it or a plane axis; a profile drawn here is "
     "an explicit `profile` offset from the axis, or a width x height rectangle with one edge on the a"
     "xis, centred along it, so a full turn is a cylinder width long of radius height. Reply: revolve_"
     "index, the index of its sketch, the operation, body and axis used, frame.\",\"params\":[{\"name\":\"sk"
     "etch\",\"type\":\"integer\",\"default\":null,\"description\":\"a committed Sketch feature to revolve (sket"
     "ch_finish's sketch_index); profile, width, height and plane are then refused\"},{\"name\":\"axis_ent"
     "ity\",\"type\":\"integer\",\"default\":null,\"description\":\"with sketch: a line of the sketch (entity in"
     "dex) as the axis, or -1 for the plane axis; omit for the sketch's only centerline (construction "
     "line) when it has one and axis is not given\"},{\"name\":\"width\",\"type\":\"number\",\"unit\":\"mm\",\"defau"
     "lt\":20,\"description\":\"without sketch or profile: the rectangle's length along the axis\"},{\"name\""
     ":\"height\",\"type\":\"number\",\"unit\":\"mm\",\"default\":10,\"description\":\"without sketch or profile: how"
     " far the rectangle reaches out from the axis, the solid's radius\"},{\"name\":\"angle\",\"type\":\"numbe"
     "r\",\"unit\":\"deg\",\"default\":360},{\"name\":\"axis\",\"type\":\"integer\",\"default\":0,\"enum\":[0,1],\"descrip"
     "tion\":\"the plane's X (0) or Y (1) axis through its origin\"},{\"name\":\"flip\",\"type\":\"boolean\",\"def"
     "ault\":false},{\"name\":\"plane\",\"type\":\"string|object\",\"default\":\"XY\",\"description\":\"\\\"XY\\\", \\\"XZ\\\""
     " or \\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\": b, \\\"face\\\": "
     "f}, a planar face (origin at its centroid, normal out of the body, x from the world axes). The r"
     "eply's frame gives the axes.\"},{\"name\":\"profile\",\"type\":\"array\",\"default\":[],\"description\":\"opti"
     "onal closed contour [[x,y],...] in plane mm; overrides width/height\"},{\"name\":\"operation\",\"type\""
     ":\"string\",\"default\":\"new_body\",\"enum\":[\"new_body\",\"join\",\"cut\",\"intersect\"],\"description\":\"what "
     "the result does to the bodies: new_body makes a body of its own, join fuses it into `body`, cut "
     "removes it from `body`, intersect keeps their overlap\"},{\"name\":\"body\",\"type\":\"integer\",\"default"
     "\":-1,\"description\":\"the body to join, cut or intersect; omit for the last body\"}],\"examples\":[{\""
     "title\":\"A ring: a profile 10 to 15 mm out from the XZ plane's vertical axis, turned a full circl"
     "e\",\"calls\":[{\"tool\":\"revolve\",\"args\":{\"axis\":1,\"plane\":\"XZ\",\"profile\":[[10,0],[15,0],[15,10],[10"
     ",10]]}}]},{\"title\":\"A committed sketch turned about its centerline, a construction line\",\"calls\""
     ":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XZ\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[10,0],\"p1\":"
     "[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"construction\":t"
     "rue,\"p0\":[0,0],\"p1\":[0,10],\"type\":\"line\"}]}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"revolve"
     "\",\"args\":{\"sketch\":0}}]}],\"ids\":true}", nullptr, nullptr, "Solid", false},
    {"sweep", L_CONTEXT("Sweep", "Design"), 1, "Shift+W", "key:S+W", L("Create a profile sketch to sweep first"), 0x00004000u, 0, 2, false, false, nullptr, "design_sweep", L("Sweep a profile along a path"), false, CadAccess::Change, false,
     "{\"summary\":\"Sweep a committed sketch's closed profile along a path, a sketch or a helix, as the "
     "Sweep card does.\",\"details\":\"Draw the profile across the path's start (a helix's start and tange"
     "nt are in its reply); along a helix the profile keeps its angle to the helix axis. Long sweeps t"
     "ake seconds and the reply may be a job. Reply: sweep_index, bodies, operation, body.\",\"params\":["
     "{\"name\":\"sketch\",\"type\":\"integer\",\"description\":\"the profile: a committed sketch's feature index"
     "\"},{\"name\":\"path\",\"type\":\"integer\",\"description\":\"feature index of a sketch or a helix, other th"
     "an the profile\"},{\"name\":\"operation\",\"type\":\"string\",\"default\":\"new_body\",\"enum\":[\"new_body\",\"jo"
     "in\",\"cut\",\"intersect\"],\"description\":\"what the result does to the bodies: new_body makes a body "
     "of its own, join fuses it into `body`, cut removes it from `body`, intersect keeps their overlap"
     "\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"the body a join, cut or intersect"
     " works on; omit for the last body\"}],\"examples\":[{\"title\":\"A spring: a 2 mm circle across a heli"
     "x's start, swept along it\",\"calls\":[{\"tool\":\"helix\",\"args\":{\"height\":20,\"pitch\":5,\"radius\":10}},"
     "{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XZ\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"cente"
     "r\":[10,0],\"radius\":1,\"type\":\"circle_center\"}]}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"swee"
     "p\",\"args\":{\"path\":0,\"sketch\":1}}]},{\"title\":\"A 6 mm rod swept along a 40 mm line\",\"calls\":[{\"too"
     "l\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"p0\":[0,0],"
     "\"p1\":[0,40],\"type\":\"line\"}]}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"sketch_create\",\"args\":"
     "{\"plane\":\"XZ\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":3,\"type\":\"cir"
     "cle_center\"}]}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"sweep\",\"args\":{\"path\":0,\"sketch\":1}}"
     "]}],\"ids\":false}", nullptr, nullptr, "Solid", false},
    {"loft", L_CONTEXT("Loft", "Design"), 1, "Shift+L", "key:S+L", L("Create at least two profile sketches to loft"), 0x00004000u, 0, 2, false, false, nullptr, "design_loft", L("Loft (skin) between two or more profiles"), false, CadAccess::Change, false,
     "{\"summary\":\"Loft a solid through two or more committed sketches, in order, as the Loft card does"
     ".\",\"details\":\"Each sketch is on its own plane (sketch on datum planes). Lofts take seconds and t"
     "he reply may be a job. Reply: loft_index, bodies, operation, body.\",\"params\":[{\"name\":\"profiles\""
     ",\"type\":\"array\",\"description\":\"sketch feature indices, in loft order\"},{\"name\":\"ruled\",\"type\":\"b"
     "oolean\",\"default\":false,\"description\":\"straight sections between the profiles instead of a smoot"
     "h skin\"},{\"name\":\"operation\",\"type\":\"string\",\"default\":\"new_body\",\"enum\":[\"new_body\",\"join\",\"cut"
     "\",\"intersect\"],\"description\":\"what the result does to the bodies: new_body makes a body of its o"
     "wn, join fuses it into `body`, cut removes it from `body`, intersect keeps their overlap\"},{\"nam"
     "e\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"the body a join, cut or intersect works o"
     "n; omit for the last body\"}],\"examples\":[{\"title\":\"A square-to-round transition 20 mm tall, the "
     "circle sketched on a datum plane\",\"calls\":[{\"tool\":\"construct_plane\",\"args\":{\"base\":\"XY\",\"offset"
     "\":20,\"type\":\"offset\"}},{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"arg"
     "s\":{\"p0\":[-10,-10],\"p1\":[10,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_finish\",\"args\":{}},"
     "{\"tool\":\"sketch_create\",\"args\":{\"plane\":{\"datum\":0}}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":["
     "{\"center\":[0,0],\"radius\":6,\"type\":\"circle_center\"}]}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\""
     ":\"loft\",\"args\":{\"profiles\":[1,2]}}]}],\"ids\":false}", nullptr, nullptr, "Solid", false},
    {"thicken", L_CONTEXT("Thicken", "Design"), 1, nullptr, "fly:material#4", L("Thicken needs a solid body — add or import one first"), 0x0000000au, 1, 0, false, false, nullptr, "design_thicken", L("Offset a solid face into a thin plate (new body)"), false, CadAccess::Change, false,
     "{\"summary\":\"Thicken a face of a body, or a whole sheet body, into a new solid body by a wall thi"
     "ckness.\",\"details\":\"type face offsets one face of a body into a thin plate. type surface thicken"
     "s a sheet body: a sheet of several faces whose free rims close into planar loops (the walls of a"
     " box with no lids) is capped at them and grows inward, into the closed shape that makes, whateve"
     "r flip says; one whose rim can't be capped is refused. A single face or a closed shell is offset"
     " face by face along each face's normal, the one body_describe gives, or against it with flip; a "
     "closed shell can fail to thicken this way. Reply: thicken_index, bodies.\",\"params\":[{\"name\":\"typ"
     "e\",\"type\":\"string\",\"default\":null,\"enum\":[\"face\",\"surface\"],\"description\":\"face thickens `face` "
     "of the body; surface thickens the whole sheet body. Left out: face when `face` is given, else su"
     "rface\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"the body, a sheet body for t"
     "ype surface; omit for the last body\"},{\"name\":\"face\",\"type\":\"integer\",\"default\":null,\"descriptio"
     "n\":\"with type face: the face id to thicken (body_describe); refused with type surface\"},{\"name\":"
     "\"thickness\",\"type\":\"number\",\"unit\":\"mm\",\"default\":2},{\"name\":\"flip\",\"type\":\"boolean\",\"default\":f"
     "alse}],\"examples\":[{\"title\":\"A 2 mm skin grown from a plate's top face as a new body\",\"calls\":[{"
     "\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"body_describe\",\"args\":{\""
     "at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"thicken\",\"args\":{\"body\":0,\"face\":5,\"generation\":{\"from_call"
     "\":1,\"key\":\"generation\"},\"thickness\":2}}]},{\"title\":\"A 30 x 20 mm sheet thickened into a 2 mm pla"
     "te, a new body\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"a"
     "rgs\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_finish\",\"args\":{}"
     "},{\"tool\":\"surface_patch\",\"args\":{\"sketch\":0}},{\"tool\":\"thicken\",\"args\":{\"thickness\":2,\"type\":\"s"
     "urface\"}}]},{\"title\":\"An open tube thickened 1.5 mm outward into a pipe: flip, as its normal poi"
     "nts inward\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\""
     ":{\"entities\":[{\"center\":[0,0],\"radius\":10,\"type\":\"circle_center\"}]}},{\"tool\":\"sketch_finish\",\"ar"
     "gs\":{}},{\"tool\":\"surface_extrude\",\"args\":{\"distance\":20,\"sketch\":0}},{\"tool\":\"thicken\",\"args\":{\""
     "body\":0,\"flip\":true,\"thickness\":1.5,\"type\":\"surface\"}}]}],\"ids\":true}", nullptr, "{\"type\":\"face\"}", "Solid", false},
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
     "}},{\"tool\":\"body_describe\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"sketch_create\",\"args\":{"
     "\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"plane\":{\"body\":0,\"face\":5}}},{\"tool\":\"sketch_dr"
     "aw\",\"args\":{\"entities\":[{\"p0\":[-15,0],\"p1\":[15,0],\"type\":\"line\"}]}},{\"tool\":\"sketch_finish\",\"arg"
     "s\":{}},{\"tool\":\"rib\",\"args\":{\"depth\":8,\"entity\":0,\"sketch\":2,\"thickness\":2}}]}],\"ids\":false}", nullptr, nullptr, "Solid", false},
    {"boolean", L_CONTEXT("Join", "Design"), 1, "Shift+B", "btn:bool#0", L("Boolean needs two bodies — add or import a second one"), 0x00000200u, 2, 0, false, false, nullptr, "design_boolean", L("Fuse the tool body into the target — one solid, no seam"), false, CadAccess::Change, true,
     "{\"summary\":\"Combine two bodies: join them, cut the tool from the target, or keep where they inte"
     "rsect.\",\"details\":\"\",\"params\":[{\"name\":\"operation\",\"type\":\"string\",\"enum\":[\"join\",\"cut\",\"interse"
     "ct\"],\"description\":\"join fuses the tool into the target; cut removes the tool from the target; i"
     "ntersect keeps only where the two overlap\"},{\"name\":\"target\",\"type\":\"integer\",\"default\":0},{\"nam"
     "e\":\"tool\",\"type\":\"integer\",\"default\":1},{\"name\":\"keep_tool\",\"type\":\"boolean\",\"default\":false},{\""
     "name\":\"tolerance\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0}],\"examples\":[{\"title\":\"A post cut fro"
     "m a block\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"ex"
     "trude\",\"args\":{\"distance\":20,\"height\":6,\"operation\":\"new_body\",\"width\":6}},{\"tool\":\"boolean\",\"ar"
     "gs\":{\"operation\":\"cut\",\"target\":0,\"tool\":1}}]},{\"title\":\"Two overlapping blocks joined into one "
     "body\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"extrude"
     "\",\"args\":{\"distance\":10,\"operation\":\"new_body\",\"profile\":[[5,-5],[20,-5],[20,5],[5,5]]}},{\"tool\""
     ":\"boolean\",\"args\":{\"operation\":\"join\",\"target\":0,\"tool\":1}}]}],\"ids\":false}", nullptr, "{\"operation\":\"join\"}", "Solid", false},
    {"bool_subtract", L_CONTEXT("Subtract", "Design"), 1, nullptr, "btn:bool#1", L("Boolean needs two bodies — add or import a second one"), 0x00000200u, 2, 0, false, false, nullptr, "design_boolean", L("Cut the tool body out of the target"), false, CadAccess::None, false,
     nullptr, "boolean", "{\"operation\":\"cut\"}", nullptr, false},
    {"bool_intersect", L_CONTEXT("Intersect", "Design"), 1, nullptr, "btn:bool#2", L("Boolean needs two bodies — add or import a second one"), 0x00000200u, 2, 0, false, false, nullptr, "design_boolean", L("Keep only where the two bodies overlap"), false, CadAccess::None, false,
     nullptr, "boolean", "{\"operation\":\"intersect\"}", nullptr, false},
    {"surface_extrude", L_CONTEXT("Surface Extrude", "Design"), 1, "Shift+G", "key:S+G", L("Create a sketch first"), 0x00004000u, 0, 0, false, false, nullptr, "design_extrude", L("Extrude a sketch into a sheet body (no end caps)"), false, CadAccess::Change, true,
     "{\"summary\":\"Extrude a sketch wire with no end caps -> an open sheet body.\",\"details\":\"\",\"params\""
     ":[{\"name\":\"sketch\",\"type\":\"integer\",\"description\":\"sketch feature index\"},{\"name\":\"distance\",\"ty"
     "pe\":\"number\",\"unit\":\"mm\",\"default\":10,\"description\":\"along the sketch plane's normal, or against"
     " it when negative\"}],\"examples\":[{\"title\":\"A 30 mm line extruded 10 mm into a flat sheet\",\"calls"
     "\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"p0"
     "\":[-15,0],\"p1\":[15,0],\"type\":\"line\"}]}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"surface_extr"
     "ude\",\"args\":{\"distance\":10,\"sketch\":0}}]},{\"title\":\"A circle extruded 20 mm into an open tube, w"
     "ith no end caps\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\""
     "args\":{\"entities\":[{\"center\":[0,0],\"radius\":10,\"type\":\"circle_center\"}]}},{\"tool\":\"sketch_finish"
     "\",\"args\":{}},{\"tool\":\"surface_extrude\",\"args\":{\"distance\":20,\"sketch\":0}}]}],\"ids\":false}", nullptr, nullptr, "Surface", false},
    {"surface_revolve", L_CONTEXT("Surface Revolve", "Design"), 1, nullptr, "fly:surface#1", L("Create a sketch profile to revolve first"), 0x00004000u, 0, 0, false, false, nullptr, "design_revolve", L("Revolve a sketch profile into a sheet body"), false, CadAccess::Change, true,
     "{\"summary\":\"Revolve a sketch wire with no caps -> an open sheet body.\",\"details\":\"The axis is ch"
     "osen as revolve chooses it: axis_entity, else the sketch's only centerline when axis is not give"
     "n, else the plane axis.\",\"params\":[{\"name\":\"sketch\",\"type\":\"integer\",\"description\":\"sketch featu"
     "re index\"},{\"name\":\"angle\",\"type\":\"number\",\"unit\":\"deg\",\"default\":360},{\"name\":\"axis\",\"type\":\"in"
     "teger\",\"default\":0,\"enum\":[0,1],\"description\":\"the sketch plane's X (0) or Y (1) axis through it"
     "s origin\"},{\"name\":\"axis_entity\",\"type\":\"integer\",\"default\":null,\"description\":\"a line of the sk"
     "etch (entity index) as the axis, or -1 for the plane axis; omit for the sketch's only centerline"
     " (construction line) when it has one and axis is not given\"},{\"name\":\"flip\",\"type\":\"boolean\",\"de"
     "fault\":false,\"description\":\"turn the other way about the axis\"}],\"examples\":[{\"title\":\"A cylinde"
     "r's wall: a line 10 mm out from the XZ plane's vertical axis, turned a full circle\",\"calls\":[{\"t"
     "ool\":\"sketch_create\",\"args\":{\"plane\":\"XZ\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"p0\":[10,"
     "0],\"p1\":[10,20],\"type\":\"line\"}]}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"surface_revolve\",\""
     "args\":{\"axis\":1,\"sketch\":0}}]},{\"title\":\"Half a truncated cone's wall: a sloping line turned 180"
     " degrees about the XZ plane's vertical axis\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"X"
     "Z\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"p0\":[5,0],\"p1\":[15,20],\"type\":\"line\"}]}},{\"tool"
     "\":\"sketch_finish\",\"args\":{}},{\"tool\":\"surface_revolve\",\"args\":{\"angle\":180,\"axis\":1,\"sketch\":0}}"
     "]}],\"ids\":false}", nullptr, nullptr, "Surface", false},
    {"surface_loft", L_CONTEXT("Surface Loft", "Design"), 1, nullptr, "fly:surface#2", L("Create at least two profile sketches to loft"), 0x00004000u, 0, 2, false, false, nullptr, "design_loft", L("Loft (skin) between 2+ profiles, open (no end caps)"), false, CadAccess::Change, true,
     "{\"summary\":\"Skin two or more profile sketches with no end caps -> an open sheet body.\",\"details\""
     ":\"\",\"params\":[{\"name\":\"profiles\",\"type\":\"array\",\"description\":\"sketch feature indices, in order\""
     "},{\"name\":\"ruled\",\"type\":\"boolean\",\"default\":false}],\"examples\":[{\"title\":\"An open funnel skinne"
     "d from a circle on XY to a wider one on a datum plane 20 mm up\",\"calls\":[{\"tool\":\"construct_plan"
     "e\",\"args\":{\"base\":\"XY\",\"offset\":20,\"type\":\"offset\"}},{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY"
     "\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":5,\"type\":\"circle_center\"}"
     "]}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"sketch_create\",\"args\":{\"plane\":{\"datum\":0}}},{\"t"
     "ool\":\"sketch_draw\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":15,\"type\":\"circle_center\"}]}},{\""
     "tool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"surface_loft\",\"args\":{\"profiles\":[1,2]}}]}],\"ids\":fals"
     "e}", nullptr, nullptr, "Surface", false},
    {"surface_patch", L_CONTEXT("Surface Fill", "Design"), 1, nullptr, "fly:surface#3", L("Create a closed sketch first"), 0x00004000u, 0, 0, false, false, nullptr, "design_surface", L("Fill a sketch boundary with a smooth face"), false, CadAccess::Change, true,
     "{\"summary\":\"Fill a sketch's closed boundary with one smooth face -> a sheet body.\",\"details\":\"\","
     "\"params\":[{\"name\":\"sketch\",\"type\":\"integer\",\"description\":\"sketch feature index\"}],\"examples\":[{"
     "\"title\":\"A circle filled with a disc-shaped sheet\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"pla"
     "ne\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":10,\"type\":\"circle_"
     "center\"}]}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"surface_patch\",\"args\":{\"sketch\":0}}]},{\""
     "title\":\"A rectangle filled with a flat sheet\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\""
     "XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"t"
     "ool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"surface_patch\",\"args\":{\"sketch\":0}}]}],\"ids\":false}", nullptr, nullptr, "Surface", false},
    {"thicken_surf", L_CONTEXT("Thicken Surface", "Design"), 1, nullptr, "fly:surface#5", L("Thicken Surface needs a surface body — make one with a Surface tool first"), 0x00000100u, 0, 0, true, false, nullptr, "design_thicken", L("Thicken a sheet body into a solid"), false, CadAccess::None, false,
     nullptr, "thicken", "{\"type\":\"surface\"}", nullptr, false},
    {"hole", L_CONTEXT("Hole", "Design"), 2, "Shift+H", "key:S+H", L("Pick a face or a plane to drill into"), 0x00000402u, 1, 0, false, false, nullptr, "design_hole", L("Drill a hole, centered on a picked face or placed on a plane"), false, CadAccess::Change, false,
     "{\"summary\":\"Drill a hole, simple, counterbored or countersunk, at (x,y) or `at` on a plane.\",\"de"
     "tails\":\"type counterbore or countersink seats a screw head: a counterbore sized by cbore_*, a co"
     "untersink by csink_*, each refused with another type. designation drills a standard clearance ho"
     "le (ISO 273 / ANSI) and sizes its seat to the standard, in place of diameter. Reply: hole_index,"
     " body, x and y in the frame, off_plane with at, frame.\",\"params\":[{\"name\":\"type\",\"type\":\"string\""
     ",\"default\":\"simple\",\"enum\":[\"simple\",\"counterbore\",\"countersink\"],\"description\":\"the hole's seat"
     " for a screw head: none, a counterbore or a countersink\"},{\"name\":\"diameter\",\"type\":\"number\",\"un"
     "it\":\"mm\",\"default\":5,\"exclusive_min\":0,\"description\":\"refused with designation, which gives it\"}"
     ",{\"name\":\"designation\",\"type\":\"string\",\"default\":null,\"description\":\"a standard clearance size, "
     "e.g. M6 or 1/4-20: the hole and its seat are the standard's; refused with diameter, cbore_*, csi"
     "nk_* and standard\"},{\"name\":\"depth\",\"type\":\"number\",\"unit\":\"mm\",\"default\":10},{\"name\":\"through\","
     "\"type\":\"boolean\",\"default\":false},{\"name\":\"x\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0,\"descripti"
     "on\":\"offset in the plane's frame from its origin, NOT world\"},{\"name\":\"y\",\"type\":\"number\",\"unit\""
     ":\"mm\",\"default\":0,\"description\":\"offset in the plane's frame from its origin, NOT world\"},{\"name"
     "\":\"plane\",\"type\":\"string|object\",\"default\":\"XY\",\"description\":\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through "
     "modeling_origin; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\": b, \\\"face\\\": f}, a planar face t"
     "o drill into that body (origin at its centroid, normal into the body, x along its longest edge)."
     " The reply's frame gives the axes.\"},{\"name\":\"at\",\"type\":\"array|object\",\"default\":null,\"descript"
     "ion\":\"instead of x and y: a world point [x, y, z], projected onto the plane along its normal (of"
     "f_plane says how far), or {\\\"body\\\": b, \\\"face\\\": f} a cylindrical face or {\\\"body\\\": b, \\\"edge\\"
     "\": e} a circular edge whose axis the hole shares. A world point moves with modeling_origin; x an"
     "d y replay on any printer\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"the body"
     " to drill; omit for a face plane's body, else the last body\"},{\"name\":\"cbore_diameter\",\"type\":\"n"
     "umber\",\"unit\":\"mm\",\"default\":0,\"description\":\"with type counterbore, refused with the others: th"
     "e counterbore's diameter\"},{\"name\":\"cbore_depth\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0,\"descri"
     "ption\":\"with type counterbore, refused with the others: its depth from the face\"},{\"name\":\"csink"
     "_diameter\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0,\"description\":\"with type countersink, refused"
     " with the others: the countersink's diameter at the face\"},{\"name\":\"csink_angle\",\"type\":\"number\""
     ",\"unit\":\"deg\",\"default\":90,\"description\":\"with type countersink, refused with the others: its in"
     "cluded angle\"},{\"name\":\"standard\",\"type\":\"string\",\"default\":\"\",\"description\":\"the designation th"
     "e hole was sized for, e.g. M6, kept with the feature; it sizes nothing\"}],\"examples\":[{\"title\":\""
     "A 5 mm hole through the middle of a plate\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"heig"
     "ht\":20,\"width\":20}},{\"tool\":\"hole\",\"args\":{\"diameter\":5,\"through\":true}}]},{\"title\":\"A countersu"
     "nk hole through a plate's top face\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,"
     "\"width\":20}},{\"tool\":\"body_describe\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"hole\",\"args\":"
     "{\"csink_angle\":90,\"csink_diameter\":9,\"diameter\":4.5,\"generation\":{\"from_call\":1,\"key\":\"generatio"
     "n\"},\"plane\":{\"body\":0,\"face\":5},\"through\":true,\"type\":\"countersink\"}}]},{\"title\":\"An M6 counterb"
     "ored clearance hole through a 10 mm block's top face\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distan"
     "ce\":10,\"height\":30,\"width\":30}},{\"tool\":\"body_describe\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"t"
     "ool\":\"hole\",\"args\":{\"designation\":\"M6\",\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"plane\":{"
     "\"body\":0,\"face\":5},\"through\":true,\"type\":\"counterbore\"}}]}],\"ids\":true}", nullptr, nullptr, "Solid", false},
    {"thread", L_CONTEXT("Thread", "Design"), 2, "Shift+T", "key:S+T", L("Pick a cylindrical surface (bore / outer) or a circular edge for a thread"), 0x00000024u, 1, 0, false, false, nullptr, "design_thread", L("Thread a cylindrical surface (inner bore / outer) or a circular edge"), false, CadAccess::Change, false,
     "{\"summary\":\"Cut a thread as the Thread card does, along a cylindrical face or a circular edge, o"
     "r on a plane.\",\"details\":\"On a face a bore is tapped, a boss threaded; on a plane the thread goe"
     "s at (x,y) or `at`. Without designation, a face or edge takes the nearest standard size, ISO met"
     "ric and UTS alike. Long threads take seconds and the reply may be a job. Reply: thread_index, de"
     "signation (null when custom), internal, diameter, pitch, depth, length, body, x, y, frame.\",\"par"
     "ams\":[{\"name\":\"face\",\"type\":\"integer\",\"default\":null,\"description\":\"a cylindrical face id of `bo"
     "dy` (body_describe); the thread runs its length from its lower end\"},{\"name\":\"edge\",\"type\":\"inte"
     "ger\",\"default\":null,\"description\":\"instead of face: a circular edge id of `body`, the thread ris"
     "ing along its normal; needs length\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":"
     "\"the body of face or edge, or the body threaded on a plane; omit for the last body (on a plane, "
     "a face plane's body)\"},{\"name\":\"designation\",\"type\":\"string\",\"default\":null,\"description\":\"e.g. "
     "M5, M8x1, 1/4-20 UNC: sets diameter, pitch and depth\"},{\"name\":\"internal\",\"type\":\"boolean\",\"defa"
     "ult\":null,\"description\":\"true taps a hole, false threads a rod; required on a plane, else from t"
     "he face (a bore is internal), external for an edge\"},{\"name\":\"diameter\",\"type\":\"number\",\"unit\":\""
     "mm\",\"default\":null,\"description\":\"nominal (major) diameter, replacing the standard's; with pitch"
     ", a custom thread\"},{\"name\":\"pitch\",\"type\":\"number\",\"unit\":\"mm\",\"default\":null,\"description\":\"re"
     "places the standard's\"},{\"name\":\"depth\",\"type\":\"number\",\"unit\":\"mm\",\"default\":null,\"description\""
     ":\"radial depth; default the 60 degree V's for the pitch and role\"},{\"name\":\"length\",\"type\":\"numb"
     "er\",\"unit\":\"mm\",\"default\":null,\"description\":\"default the face's length; required for an edge or"
     " a plane\"},{\"name\":\"plane\",\"type\":\"string|object\",\"default\":null,\"nullable\":false,\"description\":"
     "\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\""
     ": b, \\\"face\\\": f}, a planar face to drill into that body (origin at its centroid, normal into th"
     "e body, x along its longest edge). The reply's frame gives the axes.\"},{\"name\":\"x\",\"type\":\"numbe"
     "r\",\"unit\":\"mm\",\"default\":0,\"description\":\"on a plane: offset in the plane's frame from its origi"
     "n, NOT world\"},{\"name\":\"y\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0,\"description\":\"on a plane: of"
     "fset in the plane's frame from its origin, NOT world\"},{\"name\":\"at\",\"type\":\"array|object\",\"defau"
     "lt\":null,\"description\":\"on a plane, instead of x and y: a world point [x, y, z], or {\\\"body\\\": b"
     ", \\\"face\\\": f} a cylindrical face or {\\\"body\\\": b, \\\"edge\\\": e} a circular edge whose axis the t"
     "hread shares, as hole takes it\"}],\"examples\":[{\"title\":\"An M8 thread tapped into a 6.8 mm hole t"
     "hrough a block\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool"
     "\":\"hole\",\"args\":{\"diameter\":6.8,\"through\":true}},{\"tool\":\"body_describe\",\"args\":{\"body\":0,\"kinds"
     "\":[\"cylindrical\"]}},{\"tool\":\"thread\",\"args\":{\"body\":0,\"designation\":\"M8\",\"face\":6,\"generation\":{"
     "\"from_call\":2,\"key\":\"generation\"}}}]},{\"title\":\"An M6 threaded rod 10 mm long, standing on the X"
     "Y plane\",\"calls\":[{\"tool\":\"thread\",\"args\":{\"designation\":\"M6\",\"internal\":false,\"length\":10,\"plan"
     "e\":\"XY\"}}]},{\"title\":\"A 10 mm boss threaded at the nearest standard size, which the reply's desi"
     "gnation names\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"ar"
     "gs\":{\"entities\":[{\"center\":[0,0],\"radius\":5,\"type\":\"circle_center\"}]}},{\"tool\":\"sketch_finish\",\""
     "args\":{}},{\"tool\":\"extrude\",\"args\":{\"distance\":12,\"sketch\":0}},{\"tool\":\"body_describe\",\"args\":{\""
     "body\":0,\"kinds\":[\"cylindrical\"]}},{\"tool\":\"thread\",\"args\":{\"body\":0,\"face\":0,\"generation\":{\"from"
     "_call\":4,\"key\":\"generation\"}}}]}],\"ids\":true}", nullptr, nullptr, "Solid", false},
    {"shell", L_CONTEXT("Shell", "Design"), 2, "Shift+K", "key:S+K", L("Shell needs a solid body — add or import one first"), 0x00000082u, 1, 0, false, false, nullptr, "design_shell", L("Hollow the body to a wall thickness, opening a picked face"), false, CadAccess::Change, true,
     "{\"summary\":\"Hollow a body to a wall thickness (inward); optionally leave one face open.\",\"detail"
     "s\":\"\",\"params\":[{\"name\":\"thickness\",\"type\":\"number\",\"unit\":\"mm\",\"default\":1,\"exclusive_min\":0},{"
     "\"name\":\"face\",\"type\":\"integer\",\"default\":-1,\"description\":\"face id to leave open (body_describe)"
     "; omit for a closed hollow\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"target "
     "body; omit for the last body\"}],\"examples\":[{\"title\":\"A 20 x 20 x 10 mm box hollowed to 1.5 mm w"
     "alls, open at the top\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}}"
     ",{\"tool\":\"body_describe\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"shell\",\"args\":{\"body\":0,\""
     "face\":5,\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"thickness\":1.5}}]},{\"title\":\"A block ho"
     "llowed to closed 2 mm walls\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width"
     "\":20}},{\"tool\":\"shell\",\"args\":{\"thickness\":2}}]}],\"ids\":true}", nullptr, nullptr, "Solid", false},
    {"split", L_CONTEXT("Cut", "Design"), 2, "Shift+X", "key:S+X", L("Cut needs a solid body — add or import one first"), 0x000004feu, 1, 0, false, false, nullptr, "design_cut", L("Trim the body with a plane — drag the offset arrow; keep one half or both"), false, CadAccess::Change, false,
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
     "null,\"description\":\"instead of plane: a face id (body_describe) to split along\"},{\"name\":\"face_b"
     "ody\",\"type\":\"integer\",\"default\":-1,\"description\":\"with face: the body that owns it; -1 = target\""
     "},{\"name\":\"keep_upper\",\"type\":\"boolean\",\"default\":true},{\"name\":\"keep_lower\",\"type\":\"boolean\",\"d"
     "efault\":true}],\"examples\":[{\"title\":\"A block cut in two at half its height\",\"calls\":[{\"tool\":\"ex"
     "trude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"split\",\"args\":{\"offset\":5,\"plane\""
     ":\"XY\"}}]},{\"title\":\"A block cut at half its height, keeping only the lower half\",\"calls\":[{\"tool"
     "\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"split\",\"args\":{\"keep_upper\":"
     "false,\"offset\":5,\"plane\":\"XY\"}}]},{\"title\":\"A block split along the top face of a wider plate\",\""
     "calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"extrude\",\"args"
     "\":{\"distance\":4,\"height\":30,\"operation\":\"new_body\",\"width\":30}},{\"tool\":\"body_describe\",\"args\":{"
     "\"at_extreme\":\"+z\",\"body\":1}},{\"tool\":\"split\",\"args\":{\"body\":0,\"face\":5,\"face_body\":1,\"generation"
     "\":{\"from_call\":2,\"key\":\"generation\"}}}]}],\"ids\":true}", nullptr, nullptr, "Solid", false},
    {"fillet", L_CONTEXT("Fillet", "Design"), 3, "Shift+F", "btn:dress#0", L("Pick an edge to round"), 0x000000b2u, 1, 0, false, false, nullptr, "design_filletedge", L("Pick an edge, then drag the radius arrow or type it"), false, CadAccess::Change, false,
     "{\"summary\":\"Round measured edges of a body (edge ids from body_describe on that body).\",\"details"
     "\":\"Reply: fillet_index, body, bodies.\",\"params\":[{\"name\":\"edges\",\"type\":\"integer|array|string\",\""
     "description\":\"one edge id, or an array of ids rounded together in one feature; or an edge group,"
     " the card's Edges choice: \\\"top\\\", \\\"bottom\\\", \\\"lateral\\\" or \\\"all\\\"\"},{\"name\":\"radius\",\"type\":"
     "\"number\",\"unit\":\"mm\",\"default\":1,\"exclusive_min\":0},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1"
     ",\"description\":\"target body; omit for the last body. The edge ids are resolved against this body"
     ".\"}],\"examples\":[{\"title\":\"A plate's four vertical corners rounded to 2 mm\",\"calls\":[{\"tool\":\"ex"
     "trude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"body_describe\",\"args\":{\"body\":0,\"c"
     "onvex\":true,\"direction\":[0,0,1],\"edge_kinds\":[\"line\"]}},{\"tool\":\"fillet\",\"args\":{\"body\":0,\"edges"
     "\":[0,1,4,7],\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"radius\":2}}]},{\"title\":\"One top edg"
     "e of a plate rounded to 1 mm\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width"
     "\":20}},{\"tool\":\"body_describe\",\"args\":{\"adjacency\":true,\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"fi"
     "llet\",\"args\":{\"body\":0,\"edges\":3,\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"radius\":1}}]}]"
     ",\"ids\":true}", nullptr, nullptr, "Solid", false},
    {"chamfer", L_CONTEXT("Chamfer", "Design"), 3, nullptr, "btn:dress#1", L("Pick an edge to bevel"), 0x000000b2u, 1, 0, false, false, nullptr, "design_chamfer", L("Pick an edge, then drag the distance arrow or type it"), false, CadAccess::Change, false,
     "{\"summary\":\"Chamfer measured edges of a body (edge ids from body_describe on that body).\",\"detai"
     "ls\":\"Reply: chamfer_index, body, bodies.\",\"params\":[{\"name\":\"edges\",\"type\":\"integer|array|string"
     "\",\"description\":\"one edge id, or an array of ids chamfered together in one feature; or an edge g"
     "roup, the card's Edges choice: \\\"top\\\", \\\"bottom\\\", \\\"lateral\\\" or \\\"all\\\"\"},{\"name\":\"distance\","
     "\"type\":\"number\",\"unit\":\"mm\",\"default\":1,\"exclusive_min\":0},{\"name\":\"body\",\"type\":\"integer\",\"defa"
     "ult\":-1,\"description\":\"target body; omit for the last body. The edge ids are resolved against th"
     "is body.\"}],\"examples\":[{\"title\":\"A plate's top edges chamfered 1 mm\",\"calls\":[{\"tool\":\"extrude\""
     ",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"body_describe\",\"args\":{\"adjacency\":true,"
     "\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"chamfer\",\"args\":{\"body\":0,\"distance\":1,\"edges\":[3,6,9,11],"
     "\"generation\":{\"from_call\":1,\"key\":\"generation\"}}}]}],\"ids\":true}", nullptr, nullptr, "Solid", false},
    {"draft", L_CONTEXT("Draft", "Design"), 3, "Shift+D", "key:S+D", L("Pick a face to taper"), 0x0000000au, 1, 0, false, false, nullptr, "design_draft", L("Tilt a picked face by a draft angle"), false, CadAccess::Change, true,
     "{\"summary\":\"Taper a body face by an angle about its base (pull direction +Z).\",\"details\":\"\",\"par"
     "ams\":[{\"name\":\"face\",\"type\":\"integer\",\"description\":\"face id to draft (body_describe)\"},{\"name\":"
     "\"angle\",\"type\":\"number\",\"unit\":\"deg\",\"default\":5},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\""
     "description\":\"target body; omit for the last body\"}],\"examples\":[{\"title\":\"A block's +X side tap"
     "ered 5 degrees\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool"
     "\":\"body_describe\",\"args\":{\"body\":0,\"direction\":[1,0,0]}},{\"tool\":\"draft\",\"args\":{\"angle\":5,\"body"
     "\":0,\"face\":1,\"generation\":{\"from_call\":1,\"key\":\"generation\"}}}]}],\"ids\":true}", nullptr, nullptr, "Solid", false},
    {"surface_offset", L_CONTEXT("Surface Offset", "Design"), 3, nullptr, "fly:surface#4", L("Surface Offset needs a surface body — make one with a Surface tool first"), 0x00000100u, 0, 0, true, false, nullptr, "design_offset", L("Offset a sheet body's shell by a signed distance"), false, CadAccess::Change, true,
     "{\"summary\":\"Offset a sheet body by a signed distance -> another sheet body.\",\"details\":\"A positi"
     "ve offset moves each face along its normal, the one body_describe gives, and a negative one agai"
     "nst it, so the side depends on how the sheet was made: an extruded circle's normal points inward"
     ".\",\"params\":[{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"sheet body; omit for th"
     "e last body\"},{\"name\":\"offset\",\"type\":\"number\",\"unit\":\"mm\",\"default\":1}],\"examples\":[{\"title\":\"A"
     " rectangular sheet copied 5 mm above itself\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"X"
     "Y\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"to"
     "ol\":\"sketch_finish\",\"args\":{}},{\"tool\":\"surface_patch\",\"args\":{\"sketch\":0}},{\"tool\":\"surface_off"
     "set\",\"args\":{\"offset\":5}}]},{\"title\":\"An open tube of radius 10 mm offset -2 mm, against its inw"
     "ard normal, to one of radius 12 mm\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"to"
     "ol\":\"sketch_draw\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":10,\"type\":\"circle_center\"}]}},{\"t"
     "ool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"surface_extrude\",\"args\":{\"distance\":20,\"sketch\":0}},{\"t"
     "ool\":\"surface_offset\",\"args\":{\"body\":0,\"offset\":-2}}]}],\"ids\":false}", nullptr, nullptr, "Surface", false},
    {"pattern", L_CONTEXT("Linear pattern", "Design"), 4, "Shift+N", "btn:pat#0", L("Pattern needs a solid body — add or import one first"), 0x00006082u, 1, 0, false, false, nullptr, "design_array", L("Repeat the body along a direction — drag the spacing, set the count"), false, CadAccess::Change, false,
     "{\"summary\":\"Replicate a body in a row, around the plane's normal, or along a sketch curve.\",\"det"
     "ails\":\"linear places count copies along a plane axis at spacing; circular spreads them over angl"
     "e about the plane's normal through its origin; curve places them at equal-parameter points on a "
     "sketch entity, each translated by (P_i - P_0). An argument of one type is refused with another. "
     "Reply: pattern_index, bodies, and for linear and circular the frame.\",\"params\":[{\"name\":\"type\",\""
     "type\":\"string\",\"default\":\"linear\",\"enum\":[\"linear\",\"circular\",\"curve\"],\"description\":\"the copies"
     "' layout: in a row, around the plane's normal, or along a sketch curve\"},{\"name\":\"count\",\"type\":"
     "\"integer\",\"default\":3,\"min\":1},{\"name\":\"spacing\",\"type\":\"number\",\"unit\":\"mm\",\"default\":10,\"descr"
     "iption\":\"with type linear, refused with the others: the step between copies\"},{\"name\":\"dir\",\"typ"
     "e\":\"integer\",\"default\":0,\"enum\":[0,1],\"description\":\"with type linear, refused with the others: "
     "the axis, 0=plane X, 1=plane Y\"},{\"name\":\"angle\",\"type\":\"number\",\"unit\":\"deg\",\"default\":360,\"des"
     "cription\":\"with type circular, refused with the others: the total sweep\"},{\"name\":\"plane\",\"type\""
     ":\"string|object\",\"default\":\"XY\",\"description\":\"with type linear or circular, refused with curve:"
     " \\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\""
     ": b, \\\"face\\\": f}, a planar face (origin at its centroid, normal out of the body, x from the wor"
     "ld axes). The reply's frame gives the axes.\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"desc"
     "ription\":\"target body; omit for the last body\"},{\"name\":\"sketch\",\"type\":\"integer\",\"default\":null"
     ",\"description\":\"with type curve, which requires it: the feature index of the sketch holding the "
     "guide curve; refused with the other types\"},{\"name\":\"entity\",\"type\":\"integer\",\"default\":null,\"de"
     "scription\":\"with type curve, which requires it: the guide curve's entity index within that sketc"
     "h; refused with the other types\"}],\"examples\":[{\"title\":\"Three plates 30 mm apart along X\",\"call"
     "s\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"pattern\",\"args\":{\"c"
     "ount\":3,\"spacing\":30}}]},{\"title\":\"Six blocks in a circle about the XY plane's normal\",\"calls\":["
     "{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"profile\":[[20,-3],[30,-3],[30,3],[20,3]]}},{\"tool\":\"patt"
     "ern\",\"args\":{\"count\":6,\"type\":\"circular\"}}]},{\"title\":\"Four pegs spaced along a half circle\",\"ca"
     "lls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{"
     "\"center\":[0,0],\"end_angle_deg\":180,\"radius\":30,\"start_angle_deg\":0,\"type\":\"arc_center\"}]}},{\"too"
     "l\":\"sketch_finish\",\"args\":{}},{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"profile\":[[28,-2],[32,-2]"
     ",[32,2],[28,2]]}},{\"tool\":\"pattern\",\"args\":{\"count\":4,\"entity\":0,\"sketch\":0,\"type\":\"curve\"}}]}],"
     "\"ids\":true}", nullptr, "{\"type\":\"linear\"}", "Solid", false},
    {"pattern_circular", L_CONTEXT("Circular pattern", "Design"), 4, nullptr, "btn:pat#1", L("Pattern needs a solid body — add or import one first"), 0x00006082u, 1, 0, false, false, nullptr, "design_polararray", L("Repeat the body around an axis — set the count and sweep"), false, CadAccess::None, false,
     nullptr, "pattern", "{\"type\":\"circular\"}", nullptr, false},
    {"mirror", L_CONTEXT("Mirror", "Design"), 4, "Shift+Z", "key:S+Z", L("Mirror needs a body — add or import one first"), 0x000004feu, 1, 0, false, false, nullptr, "design_mirror", L("Reflect a body about a plane"), false, CadAccess::Change, true,
     "{\"summary\":\"Mirror a body about a base plane.\",\"details\":\"operation new_body makes a mirrored co"
     "py; join fuses the mirror back into the source.\",\"params\":[{\"name\":\"plane\",\"type\":\"string|object"
     "\",\"default\":\"XZ\",\"description\":\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}"
     ", a Plane feature; or {\\\"body\\\": b, \\\"face\\\": f}, a planar face (origin at its centroid, normal "
     "out of the body, x from the world axes). The reply's frame gives the axes.\"},{\"name\":\"operation\""
     ",\"type\":\"string\",\"default\":\"new_body\",\"enum\":[\"new_body\",\"join\"],\"description\":\"what the mirror "
     "does to the source body: new_body makes a body of its own, join fuses it into the source\"},{\"nam"
     "e\":\"keep_original\",\"type\":\"boolean\",\"default\":true,\"description\":\"with operation new_body, keep "
     "the source body\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"target body; omit "
     "for the last body\"}],\"examples\":[{\"title\":\"A mirrored copy of a block across the XZ plane\",\"call"
     "s\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"profile\":[[-5,5],[5,5],[5,15],[-5,15]]}},{\"tool\":\"mi"
     "rror\",\"args\":{\"plane\":\"XZ\"}}]},{\"title\":\"A block against the XZ plane and its mirror image fused"
     " into one body\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"profile\":[[-5,0],[5,0],[5,10],["
     "-5,10]]}},{\"tool\":\"mirror\",\"args\":{\"operation\":\"join\",\"plane\":\"XZ\"}}]}],\"ids\":true}", nullptr, nullptr, "Solid", false},
    {"pat_curve", L_CONTEXT("Pattern on Curve", "Design"), 4, nullptr, nullptr, L("Pattern on Curve needs a body and a curve"), 0x00000090u, 1, 0, false, false, nullptr, nullptr, L("Repeat the body along a picked curve"), false, CadAccess::None, false,
     nullptr, nullptr, nullptr, nullptr, false},
    {"body_transform", L_CONTEXT("Move", "Design"), 5, "Shift+Y", "key:S+Y", L("Transform needs a body — add or import one first"), 0x000021feu, 1, 0, false, false, nullptr, "design_move", L("Move and/or rotate an existing body"), false, CadAccess::Change, true,
     "{\"summary\":\"Move and/or rotate a body (B-rep transform).\",\"details\":\"copy=true keeps the source "
     "and appends the transformed copy as a new body. The rotation is angle about the axis (axis_x, ax"
     "is_y, axis_z) through the pivot, a world point that defaults to the world origin.\",\"params\":[{\"n"
     "ame\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"target body; omit for the last body\"},{"
     "\"name\":\"dx\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0},{\"name\":\"dy\",\"type\":\"number\",\"unit\":\"mm\",\"d"
     "efault\":0},{\"name\":\"dz\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0},{\"name\":\"axis_x\",\"type\":\"number"
     "\",\"default\":0},{\"name\":\"axis_y\",\"type\":\"number\",\"default\":0},{\"name\":\"axis_z\",\"type\":\"number\",\"d"
     "efault\":1},{\"name\":\"pivot_x\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0},{\"name\":\"pivot_y\",\"type\":\""
     "number\",\"unit\":\"mm\",\"default\":0},{\"name\":\"pivot_z\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0},{\"na"
     "me\":\"angle\",\"type\":\"number\",\"unit\":\"deg\",\"default\":0},{\"name\":\"copy\",\"type\":\"boolean\",\"default\":"
     "false}],\"examples\":[{\"title\":\"A plate moved 30 mm along X\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"d"
     "istance\":5,\"height\":20,\"width\":20}},{\"tool\":\"body_transform\",\"args\":{\"dx\":30}}]},{\"title\":\"A cop"
     "y of a plate 10 mm above it\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\""
     ":20}},{\"tool\":\"body_transform\",\"args\":{\"copy\":true,\"dz\":10}}]}],\"ids\":false}", nullptr, nullptr, "Bodies", false},
    {"align", L_CONTEXT("Mate", "Design"), 5, nullptr, "fly:placement#2", L("Mate needs two coordinate systems — create them first"), 0x00001202u, 2, 0, false, false, nullptr, "design_c_coincident", L("Assembly: align two coordinate systems (fastened, planar, revolute, slider, cylindrical)"), false, CadAccess::Change, false,
     "{\"summary\":\"Align two bodies: transform the moving body (cs_b) so its connector lands on the fix"
     "ed one (cs_a).\",\"details\":\"An align that cannot hold with those before it is listed in describe_"
     "design's align_conflicts, with the reason. Reply: align_index, bodies.\",\"params\":[{\"name\":\"type\""
     ",\"type\":\"string\",\"default\":\"fastened\",\"enum\":[\"fastened\",\"planar\",\"revolute\",\"slider\",\"cylindric"
     "al\"],\"description\":\"fastened is rigid; planar matches the normal only; revolute leaves rotation "
     "about the axis free; slider translation along it; cylindrical both\"},{\"name\":\"cs_a\",\"type\":\"inte"
     "ger\",\"description\":\"feature index of the fixed CoordSys (mate connector A)\"},{\"name\":\"cs_b\",\"typ"
     "e\":\"integer\",\"description\":\"feature index of the CoordSys on the body that moves\"},{\"name\":\"offs"
     "et\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0},{\"name\":\"angle\",\"type\":\"number\",\"unit\":\"deg\",\"defau"
     "lt\":0},{\"name\":\"flip\",\"type\":\"boolean\",\"default\":false}],\"examples\":[{\"title\":\"A block stood on "
     "a plate: its bottom-face connector fastened, turned over, to the plate's top-face one\",\"calls\":["
     "{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"extrude\",\"args\":{\"dista"
     "nce\":10,\"operation\":\"new_body\",\"profile\":[[30,-5],[40,-5],[40,5],[30,5]]}},{\"tool\":\"body_describ"
     "e\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"construct_coordinate_system\",\"args\":{\"body\":0,\""
     "face\":5,\"generation\":{\"from_call\":2,\"key\":\"generation\"},\"type\":\"face_and_direction\"}},{\"tool\":\"b"
     "ody_describe\",\"args\":{\"at_extreme\":\"-z\",\"body\":1}},{\"tool\":\"construct_coordinate_system\",\"args\":"
     "{\"body\":1,\"face\":4,\"generation\":{\"from_call\":4,\"key\":\"generation\"},\"type\":\"face_and_direction\"}}"
     ",{\"tool\":\"align\",\"args\":{\"cs_a\":4,\"cs_b\":5,\"flip\":true,\"type\":\"fastened\"}}]}],\"ids\":false}", nullptr, nullptr, "Solid", false},
    {"align_to", L_CONTEXT("Align to", "Design"), 5, nullptr, nullptr, L("Align needs a body — add or import one first"), 0x00000002u, 1, 0, false, false, nullptr, nullptr, L("Align the body to a picked face or plane"), false, CadAccess::None, false,
     nullptr, nullptr, nullptr, nullptr, false},
    {"construct_plane", L_CONTEXT("Plane", "Design"), 6, "Shift+P", "key:S+P", nullptr, 0x00000453u, 0, 0, false, false, nullptr, "design_plane", L("Reference plane (offset / tilt / midplane / tangent / two edges / coincident)"), false, CadAccess::Change, false,
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
     "es\":[{\"title\":\"A datum plane 20 mm above XY\",\"calls\":[{\"tool\":\"construct_plane\",\"args\":{\"base\":\""
     "XY\",\"offset\":20,\"type\":\"offset\"}}]},{\"title\":\"A datum plane midway between a block's top and bot"
     "tom faces\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"bo"
     "dy_describe\",\"args\":{\"body\":0,\"kinds\":[\"planar\"]}},{\"tool\":\"construct_plane\",\"args\":{\"face_a\":{\""
     "body\":0,\"face\":4},\"face_b\":{\"body\":0,\"face\":5},\"generation\":{\"from_call\":1,\"key\":\"generation\"},\""
     "type\":\"midplane\"}}]},{\"title\":\"A datum plane turned 30 degrees from a plate's top face about its"
     " front edge\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"b"
     "ody_describe\",\"args\":{\"adjacency\":true,\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"construct_plane\",\"a"
     "rgs\":{\"angle\":30,\"edge_a\":{\"body\":0,\"edge\":3},\"face_a\":{\"body\":0,\"face\":5},\"generation\":{\"from_c"
     "all\":1,\"key\":\"generation\"},\"type\":\"angle\"}}]}],\"ids\":true}", nullptr, nullptr, "Construct", false},
    {"construct_axis", L_CONTEXT("Axis", "Design"), 6, "Shift+A", "key:S+A", nullptr, 0x00000057u, 0, 0, false, false, nullptr, "design_line", L("Datum axis (two points, face normal, cylinder centerline, two planes, along edge)"), false, CadAccess::Change, true,
     "{\"summary\":\"Create a datum axis (reference line): two points, face normal, cylinder centreline, "
     "plane intersection, or along edge.\",\"details\":\"\",\"params\":[{\"name\":\"type\",\"type\":\"string\",\"defau"
     "lt\":\"two_points\",\"enum\":[\"two_points\",\"face_normal\",\"cylinder\",\"plane_intersection\",\"along_edge\""
     "]},{\"name\":\"p1\",\"type\":\"array\",\"default\":[0,0,0],\"description\":\"first point [x,y,z] for two_poin"
     "ts\"},{\"name\":\"p2\",\"type\":\"array\",\"default\":[0,0,10],\"description\":\"second point [x,y,z] for two_"
     "points\"},{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"body for face/edge refs\"},{"
     "\"name\":\"face\",\"type\":\"integer\",\"default\":-1,\"description\":\"face id (body_describe) for face_norm"
     "al/cylinder\"},{\"name\":\"edge\",\"type\":\"integer\",\"default\":-1,\"description\":\"edge id (body_describe"
     ") for along_edge\"},{\"name\":\"plane_a\",\"type\":\"integer\",\"default\":-1,\"description\":\"first datum pl"
     "ane feature index for plane_intersection\"},{\"name\":\"plane_b\",\"type\":\"integer\",\"default\":-1,\"desc"
     "ription\":\"second datum plane feature index for plane_intersection\"}],\"examples\":[{\"title\":\"The a"
     "xis of a hole through a plate\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"widt"
     "h\":20}},{\"tool\":\"hole\",\"args\":{\"diameter\":5,\"through\":true}},{\"tool\":\"body_describe\",\"args\":{\"bo"
     "dy\":0,\"kinds\":[\"cylindrical\"]}},{\"tool\":\"construct_axis\",\"args\":{\"body\":0,\"face\":6,\"generation\":"
     "{\"from_call\":2,\"key\":\"generation\"},\"type\":\"cylinder\"}}]},{\"title\":\"The normal through the centre"
     " of a plate's top face\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}}"
     ",{\"tool\":\"body_describe\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"construct_axis\",\"args\":{\""
     "body\":0,\"face\":5,\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"type\":\"face_normal\"}}]},{\"titl"
     "e\":\"The vertical line where two datum planes meet\",\"calls\":[{\"tool\":\"construct_plane\",\"args\":{\"b"
     "ase\":\"XZ\",\"offset\":10,\"type\":\"offset\"}},{\"tool\":\"construct_plane\",\"args\":{\"base\":\"YZ\",\"offset\":1"
     "0,\"type\":\"offset\"}},{\"tool\":\"construct_axis\",\"args\":{\"plane_a\":0,\"plane_b\":1,\"type\":\"plane_inter"
     "section\"}}]}],\"ids\":true}", nullptr, nullptr, "Construct", false},
    {"construct_coordinate_system", L_CONTEXT("Coordinate system", "Design"), 6, "Shift+C", "key:S+C", nullptr, 0x00000043u, 0, 0, false, false, nullptr, "design_point", L("Datum coordinate system (world point, or face + direction edge)"), false, CadAccess::Change, true,
     "{\"summary\":\"Create a datum coordinate system (origin + orthonormal axes).\",\"details\":\"PointWorld"
     " aligns to world; FaceAndDirection uses a face for Z and an edge/hint for X.\",\"params\":[{\"name\":"
     "\"type\",\"type\":\"string\",\"default\":\"point_world\",\"enum\":[\"point_world\",\"face_and_direction\"]},{\"na"
     "me\":\"point\",\"type\":\"array\",\"default\":[0,0,0],\"description\":\"origin [x,y,z] for point_world\"},{\"n"
     "ame\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"body for face/edge refs\"},{\"name\":\"face"
     "\",\"type\":\"integer\",\"default\":-1,\"description\":\"face id (body_describe) for Z axis\"},{\"name\":\"edg"
     "e\",\"type\":\"integer\",\"default\":-1,\"description\":\"edge id (body_describe) for X axis hint\"},{\"name"
     "\":\"x_hint\",\"type\":\"array\",\"default\":[1,0,0],\"description\":\"fallback X direction hint if no edge "
     "given\"}],\"examples\":[{\"title\":\"A connector on a plate's top face, its x axis along the front edg"
     "e\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"body_descri"
     "be\",\"args\":{\"adjacency\":true,\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"construct_coordinate_system\","
     "\"args\":{\"body\":0,\"edge\":3,\"face\":5,\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"type\":\"face_"
     "and_direction\"}}]}],\"ids\":true}", nullptr, nullptr, "Construct", false},
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
     "s\":true}", nullptr, nullptr, "Construct", false},
    {"sketch_project", L_CONTEXT("Project", "Design"), 6, nullptr, "fly:plane#4", L("Project needs a body — add or import one first"), 0x00000482u, 1, 0, false, false, nullptr, "design_sketch", L("Project body edges onto a plane as sketch entities"), false, CadAccess::Sketch, true,
     "{\"summary\":\"Project a body's edges onto a plane as a new sketch, or into the open sketch as cons"
     "truction entities.\",\"details\":\"With no sketch open it adds a Project feature on plane, a sketch "
     "whose entities follow the body as it rebuilds; reply: scope \\\"feature\\\", project_index, bodies, "
     "error, frame. With a sketch open, a new one or one re-opened with sketch_edit, the edges are cop"
     "ied into it on its plane as construction entities, which new geometry can be constrained to and "
     "which do not follow the body; plane is refused there; reply: scope \\\"sketch\\\", first_index, adde"
     "d, entities (how many the sketch has).\",\"params\":[{\"name\":\"source_body\",\"type\":\"integer\",\"defaul"
     "t\":-1,\"description\":\"body owning the edges; -1 = last body\"},{\"name\":\"face\",\"type\":\"integer\",\"de"
     "fault\":-1,\"description\":\"global face id on the source body; when set, all its edges are projecte"
     "d\"},{\"name\":\"edges\",\"type\":\"array\",\"default\":[],\"description\":\"global edge ids to project; empty"
     " => project the face\"},{\"name\":\"plane\",\"type\":\"string|object\",\"default\":null,\"description\":\"with"
     " no sketch open, the plane of the new sketch: \\\"XY\\\" (left out or null), \\\"XZ\\\" or \\\"YZ\\\" throug"
     "h modeling_origin; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\": b, \\\"face\\\": f}, a planar face"
     " (origin at its centroid, normal out of the body, x from the world axes). The reply's frame give"
     "s the axes. With a sketch open, refused: the edges go on its plane.\"}],\"examples\":[{\"title\":\"A p"
     "late's top-face outline projected onto the XY plane as a new sketch\",\"calls\":[{\"tool\":\"extrude\","
     "\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"body_describe\",\"args\":{\"at_extreme\":\"+z\","
     "\"body\":0}},{\"tool\":\"sketch_project\",\"args\":{\"face\":5,\"generation\":{\"from_call\":1,\"key\":\"generati"
     "on\"},\"plane\":\"XY\",\"source_body\":0}}]},{\"title\":\"Two top edges of a plate, meeting at a corner, p"
     "rojected onto the XY plane\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":"
     "20}},{\"tool\":\"body_describe\",\"args\":{\"adjacency\":true,\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"sket"
     "ch_project\",\"args\":{\"edges\":[3,6],\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"plane\":\"XY\",\""
     "source_body\":0}}]},{\"title\":\"A plate's top-face outline drawn into an open sketch as constructio"
     "n entities\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"bo"
     "dy_describe\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}}"
     ",{\"tool\":\"sketch_project\",\"args\":{\"face\":5,\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"sour"
     "ce_body\":0}}]}],\"ids\":true}", nullptr, nullptr, "Sketch", false},
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
     "0,\"width\":20}},{\"tool\":\"body_describe\",\"args\":{\"body\":0,\"kinds\":[\"planar\"]}},{\"tool\":\"measure\",\""
     "args\":{\"a\":{\"face\":5},\"b\":{\"face\":4},\"body\":0,\"generation\":{\"from_call\":1,\"key\":\"generation\"}}}]"
     "},{\"title\":\"The 10 mm gap between two blocks' facing sides\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\""
     "distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"operation\":\"new_b"
     "ody\",\"profile\":[[20,-5],[30,-5],[30,5],[20,5]]}},{\"tool\":\"body_describe\",\"args\":{\"body\":0,\"direc"
     "tion\":[1,0,0]}},{\"tool\":\"body_describe\",\"args\":{\"body\":1,\"direction\":[-1,0,0]}},{\"tool\":\"measure"
     "\",\"args\":{\"a\":{\"body\":0,\"face\":1},\"b\":{\"body\":1,\"face\":3},\"generation\":{\"from_call\":3,\"key\":\"gen"
     "eration\"}}}]}],\"ids\":true}", nullptr, nullptr, "Inspect", false},
    {"body_mass_properties", L_CONTEXT("Volume and area", "Design"), 6, nullptr, "btn:mass", nullptr, 0x000000feu, 1, 0, false, false, nullptr, "info", L("Report the volume and surface area of the selected body"), false, CadAccess::Read, false,
     "{\"summary\":\"Volume / surface area / centre of mass / inertia tensor of a body.\",\"details\":\"\",\"pa"
     "rams\":[{\"name\":\"body\",\"type\":\"integer\",\"default\":0}],\"examples\":[{\"title\":\"The volume and centre"
     " of mass of a 20 x 20 x 5 mm plate\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,"
     "\"width\":20}},{\"tool\":\"body_mass_properties\",\"args\":{\"body\":0}}]}],\"ids\":false}", nullptr, nullptr, "Inspect", false},
    {"interference", L_CONTEXT("Interference", "Design"), 6, nullptr, "btn:interference", L("Interference needs at least two bodies"), 0x00000280u, 2, 0, false, false, nullptr, "design_boolean", L("Check whether two bodies overlap — reports, changes nothing"), false, CadAccess::Read, false,
     "{\"summary\":\"Solid bodies that overlap, as {body_a, body_b, volume}.\",\"details\":\"Read-only; bodie"
     "s that merely touch enclose no volume and are not reported.\",\"params\":[{\"name\":\"min_volume\",\"typ"
     "e\":\"number\",\"unit\":\"mm^3\",\"default\":1e-06,\"description\":\"overlap volume above which a pair count"
     "s as interfering\"}],\"examples\":[{\"title\":\"A post standing in a block: the two overlap\",\"calls\":["
     "{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":20}},{\"tool\":\"extrude\",\"args\":{\"dist"
     "ance\":20,\"height\":10,\"operation\":\"new_body\",\"width\":10}},{\"tool\":\"interference\",\"args\":{\"silent\""
     ":true}}]}],\"ids\":false}", nullptr, nullptr, "Inspect", false},
    {"feature_edit", L_CONTEXT("Edit", "Design"), 7, nullptr, "btn:edit", nullptr, 0x00007d8eu, 0, 0, false, false, nullptr, "design_edit", L("Reopen the selected feature to change what it was made from"), false, CadAccess::Change, false,
     "{\"summary\":\"Change a feature's fields and what it refers to, as its card's Confirm does when edi"
     "ting: one undo step, a rebuild.\",\"details\":\"fields, then refs, are written; a call with neither "
     "is refused, and a field an expression drives is refused. Body indices and face and edge ids in r"
     "efs are those of the model just before the feature, which body_describe {before: feature} reads "
     "and feature_describe's bodies_before lists; a Plane's, an Axis's and a CoordSys's are the finish"
     "ed model's (feature_describe's ids_of says which). An extrude's profile and target are fixed. Re"
     "ply: changed {name: [old, new]}.\",\"params\":[{\"name\":\"feature\",\"type\":\"integer\",\"description\":\"fe"
     "ature index (describe_design)\"},{\"name\":\"fields\",\"type\":\"object\",\"default\":{},\"description\":\"{fi"
     "eld: value} from feature_describe's editable, e.g. {\\\"distance\\\": 15}: numbers within its ranges"
     "; operation new_body|join|cut|intersect; extrude_end blind|symmetric|two_sided|through_all; shap"
     "e rectangle|circle; xf_* vectors [x, y, z]; imported art's import_offset [x, y]; a Text feature'"
     "s text_string, text_height (0.5 to 500 mm) and text_font, a face name (with text_bold, text_ital"
     "ic) or the stored value, which outline it again\"},{\"name\":\"refs\",\"type\":\"object\",\"default\":{},\"d"
     "escription\":\"{name: value} under the names of feature_describe's refs that the feature's card ca"
     "n pick again: revolve_axis_entity; sweep_path_ref; loft_profile_refs; target_body, bool_tool_bod"
     "y; plane (as sketch_create's plane) for a mirror, split, helix, hole or thread; rib_sketch_ref, "
     "rib_entity; mate_cs_a, mate_cs_b; a fillet's or chamfer's edges, or face_group top|bottom|latera"
     "l|all; shell_face, draft_face, thicken_face; delete_faces; a datum's plane_, axis_ and coordsys_"
     " refs. Feature indices must come before the feature\"}],\"examples\":[{\"title\":\"A plate made 15 mm "
     "thick\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"feature"
     "_edit\",\"args\":{\"feature\":1,\"fields\":{\"distance\":15}}}]},{\"title\":\"A plate's hole widened from 5 "
     "to 8 mm\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"hole\""
     ",\"args\":{\"diameter\":5,\"through\":true}},{\"tool\":\"feature_edit\",\"args\":{\"feature\":2,\"fields\":{\"hol"
     "e_diameter\":8}}}]}],\"ids\":false}", nullptr, nullptr, "History", true},
    {"rename_selected", L_CONTEXT("Rename…", "Design"), 8, "F2", "btn:rename", L("Select a feature, or a body, to rename it"), 0x00004080u, 0, 0, false, false, nullptr, "rename_edit", L("Give this feature a name you will recognise in the tree (a body takes its name from the feature that makes it)"), false, CadAccess::None, false,
     nullptr, nullptr, nullptr, "Menu items", true},
    {"delete_face", L_CONTEXT("Delete Face", "Design"), 7, nullptr, "fly:dressup#3", L("Delete Face needs a body — add or import one first"), 0x0000000eu, 1, 0, false, false, nullptr, "design_delete", L("Remove faces from a body and heal the solid"), false, CadAccess::Change, true,
     "{\"summary\":\"Remove faces from a solid, healing the gap via OCCT defeaturing.\",\"details\":\"\",\"para"
     "ms\":[{\"name\":\"body\",\"type\":\"integer\",\"default\":-1,\"description\":\"target body; omit for the last "
     "body\"},{\"name\":\"faces\",\"type\":\"array\",\"description\":\"global face ids to delete\"}],\"examples\":[{\""
     "title\":\"A plate's bore filled in by deleting its cylindrical face\",\"calls\":[{\"tool\":\"extrude\",\"a"
     "rgs\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"hole\",\"args\":{\"diameter\":5,\"through\":true}}"
     ",{\"tool\":\"body_describe\",\"args\":{\"body\":0,\"kinds\":[\"cylindrical\"]}},{\"tool\":\"delete_face\",\"args\""
     ":{\"body\":0,\"faces\":[6],\"generation\":{\"from_call\":2,\"key\":\"generation\"}}}]}],\"ids\":true}", nullptr, nullptr, "Solid", false},
    {"colour", L_CONTEXT("Color", "Design"), 8, nullptr, "btn:colour", nullptr, 0x000001feu, 1, 0, false, false, nullptr, "color_palette", L("Set the selected body's display color"), true, CadAccess::None, false,
     nullptr, "body_set", "{}", nullptr, false},
    {"zoom_to_selection", L_CONTEXT("Zoom to selection", "Design"), 8, nullptr, "btn:zoom_to", nullptr, 0x000041feu, 0, 0, false, false, nullptr, "design_zoom", L("Frame the selection in the view, keeping the view direction"), false, CadAccess::None, false,
     nullptr, nullptr, nullptr, "Menu items", false},
    {"feature_delete", L_CONTEXT("Delete", "Design"), 7, "Del", "btn:delete", nullptr, 0x000f7c00u, 0, 0, false, false, nullptr, "design_delete", L("Delete what is selected"), false, CadAccess::Change, false,
     "{\"summary\":\"Delete a feature as its tree row's Delete does; deleting a sketch also deletes the f"
     "eatures built on it.\",\"details\":\"One undo step. Later indices shift. Reply: removed (how many fe"
     "atures went), features, bodies.\",\"params\":[{\"name\":\"feature\",\"type\":\"integer\",\"description\":\"fea"
     "ture index (describe_design)\"}],\"examples\":[{\"title\":\"A plate's hole deleted\",\"calls\":[{\"tool\":\""
     "extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"hole\",\"args\":{\"diameter\":5,\"thro"
     "ugh\":true}},{\"tool\":\"feature_delete\",\"args\":{\"feature\":2}}]},{\"title\":\"A committed sketch delete"
     "d, and the extrude built on it with it\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},"
     "{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\""
     "sketch_finish\",\"args\":{}},{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"sketch\":0}},{\"tool\":\"feature_d"
     "elete\",\"args\":{\"feature\":0}}]}],\"ids\":false}", nullptr, nullptr, "History", true},
    {"body_delete", L_CONTEXT("Delete Body", "Design"), 7, nullptr, "btn:delete_body", nullptr, 0x000001feu, 1, 0, false, false, nullptr, "design_delete", L("Delete this whole body — removes the feature it was made from"), false, CadAccess::Change, false,
     "{\"summary\":\"Delete a body as its row's Delete does: the feature that made it goes, which can tak"
     "e other features and bodies with it.\",\"details\":\"One undo step. Reply: deleted_feature {index, t"
     "ype, name}, removed, features, bodies.\",\"params\":[{\"name\":\"body\",\"type\":\"integer\",\"description\":"
     "\"body index (describe_design)\"}],\"examples\":[{\"title\":\"The second of two bodies deleted, with th"
     "e extrude that made it\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}}"
     ",{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"operation\":\"new_body\",\"profile\":[[20,-5],[30,-5],[30,5"
     "],[20,5]]}},{\"tool\":\"body_delete\",\"args\":{\"body\":1}}]}],\"ids\":false}", nullptr, nullptr, "Bodies", false},
    {"sk_line_t", L_CONTEXT("Line", "Design"), 0, "L", "key:L", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Line", "Design"), "design_line", L("Line — click start, then end"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"line\"}", nullptr, false},
    {"sk_polyline", L_CONTEXT("Polyline", "Design"), 0, nullptr, "fly:design_line#1", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Line", "Design"), "design_polyline", L("Click points; click the first point to close the loop, right-click to end it open"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"polyline\"}", nullptr, false},
    {"sk_rect", L_CONTEXT("Corner rectangle", "Design"), 0, "R", "key:R", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Rectangle", "Design"), "design_rect", L("Rectangle — click two opposite corners"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"rectangle_2_point\"}", nullptr, false},
    {"sk_rect_center", L_CONTEXT("Center rectangle", "Design"), 0, nullptr, "fly:design_rect#1", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Rectangle", "Design"), "design_crect", L("Click center, then a corner"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"rectangle_center\"}", nullptr, false},
    {"sk_rect_oblique", L_CONTEXT("Oblique rectangle", "Design"), 0, nullptr, "fly:design_rect#2", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Rectangle", "Design"), "design_rect_oblique", L("Click two corners of one edge, then a point for the width"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"rectangle_3_point\"}", nullptr, false},
    {"sk_rect_rounded", L_CONTEXT("Rounded rectangle", "Design"), 0, nullptr, "fly:design_rect#3", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Rectangle", "Design"), "design_rect_rounded", L("Click two opposite corners, then a point for the corner radius"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"rectangle_rounded\"}", nullptr, false},
    {"sk_circle", L_CONTEXT("Center circle", "Design"), 0, "C", "key:C", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Circle", "Design"), "design_circle", L("Circle — click center, then radius"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"circle_center\"}", nullptr, false},
    {"sk_circle_2pt", L_CONTEXT("2-point circle", "Design"), 0, nullptr, "fly:design_circle#1", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Circle", "Design"), "design_circle2pt", L("Click two ends of the diameter"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"circle_2_point\"}", nullptr, false},
    {"sk_circle_3pt", L_CONTEXT("3-point circle", "Design"), 0, nullptr, "fly:design_circle#2", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Circle", "Design"), "design_circle3pt", L("Click three points on the circle"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"circle_3_point\"}", nullptr, false},
    {"sk_arc_t", L_CONTEXT("3-point arc", "Design"), 0, "A", "key:A", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Arc", "Design"), "design_arc3pt", L("Arc — click start, end, then a point"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"arc_3_point\"}", nullptr, false},
    {"sk_arc_tangent", L_CONTEXT("Tangent arc", "Design"), 0, nullptr, "fly:design_arc3pt#1", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Arc", "Design"), "design_tangentarc", L("Click start (on the last entity) then end"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"arc_tangent\"}", nullptr, false},
    {"sk_arc_center", L_CONTEXT("Center-point arc", "Design"), 0, nullptr, "fly:design_arc3pt#2", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Arc", "Design"), "design_arc_center", L("Click center, then start, then a point for the end angle"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"arc_center\"}", nullptr, false},
    {"sk_slot", L_CONTEXT("Slot", "Design"), 0, "S", "key:S", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Slot", "Design"), "design_slot", L("Slot — two centerline ends, then the width"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"slot\"}", nullptr, false},
    {"sk_slot_arc", L_CONTEXT("Arc slot", "Design"), 0, nullptr, "fly:design_slot#1", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Slot", "Design"), "design_slot_arc", L("Click center, start, end, then a point for the width"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"slot_arc\"}", nullptr, false},
    {"sk_ellipse", L_CONTEXT("Ellipse", "Design"), 0, "E", "key:E", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Ellipse", "Design"), "design_ellipse", L("Ellipse — center, major end, minor point"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"ellipse\"}", nullptr, false},
    {"sk_ellipse_arc", L_CONTEXT("Elliptical arc", "Design"), 0, nullptr, "fly:design_ellipse#1", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Ellipse", "Design"), "design_ellipse_arc", L("Click center, major-axis end, minor point, then arc start and end"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"ellipse_arc\"}", nullptr, false},
    {"sk_spline", L_CONTEXT("Spline", "Design"), 0, "B", "key:B", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_bspline", L("Spline — click control points"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"spline\"}", nullptr, false},
    {"sk_poly_3", L_CONTEXT("Triangle", "Design"), 0, nullptr, "btn:poly#3", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Triangle — click center, then a vertex"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"polygon\",\"sides\":3}", nullptr, false},
    {"sk_poly_4", L_CONTEXT("Square", "Design"), 0, nullptr, "btn:poly#4", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Square — click center, then a vertex"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"polygon\",\"sides\":4}", nullptr, false},
    {"sk_poly_5", L_CONTEXT("Pentagon", "Design"), 0, nullptr, "btn:poly#5", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Pentagon — click center, then a vertex"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"polygon\",\"sides\":5}", nullptr, false},
    {"sk_polygon", L_CONTEXT("Hexagon", "Design"), 0, "G", "btn:poly#6", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Hexagon — click center, then a vertex"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"polygon\",\"sides\":6}", nullptr, false},
    {"sk_poly_8", L_CONTEXT("Octagon", "Design"), 0, nullptr, "btn:poly#8", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Octagon — click center, then a vertex"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"polygon\",\"sides\":8}", nullptr, false},
    {"sk_poly_12", L_CONTEXT("Dodecagon", "Design"), 0, nullptr, "btn:poly#12", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Dodecagon — click center, then a vertex"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"polygon\",\"sides\":12}", nullptr, false},
    {"sketch_polygon_inscribed", L_CONTEXT("Inscribed", "Design"), 0, nullptr, "btn:polyfit#0", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Measure the polygon to its corners (inscribed)"), false, CadAccess::None, false,
     nullptr, nullptr, nullptr, "Menu items", false},
    {"sketch_polygon_circumscribed", L_CONTEXT("Circumscribed", "Design"), 0, nullptr, "btn:polyfit#1", nullptr, 0x000f8000u, 0, 0, false, true, L_CONTEXT("Polygon", "Design"), "design_polygon", L("Measure the polygon to its flats (circumscribed)"), false, CadAccess::None, false,
     nullptr, nullptr, nullptr, "Menu items", false},
    {"sk_point_t", L_CONTEXT("Point", "Design"), 0, "P", "key:P", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_point", L("Point — click to place"), false, CadAccess::None, false,
     nullptr, "sketch_draw", "{\"type\":\"point\"}", nullptr, false},
    {"sketch_text", L_CONTEXT("Text", "Design"), 0, nullptr, "btn:text", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_text", L("Type text; it becomes a Text feature on this sketch's plane, editable later"), true, CadAccess::Change, false,
     "{\"summary\":\"Add text as a Text feature on a plane or a planar face, outlined in a font, as the T"
     "ext dialog adds it.\",\"details\":\"It runs on the model, in one undo step, with no dialog and no pl"
     "acement step, and is refused while a sketch is open. Refused for a font that is not installed or"
     " cannot be loaded, and for text with no outline (spaces only). The feature keeps its words, font"
     " and height, as the dialog's does. Reply: text_index, bodies, frame (the plane's).\",\"params\":[{\""
     "name\":\"text\",\"type\":\"string\",\"description\":\"the words\"},{\"name\":\"font\",\"type\":\"string\",\"default\""
     ":\"\",\"description\":\"a font face, as the Text dialog's Font list names it; left out or empty, the "
     "face last used\"},{\"name\":\"bold\",\"type\":\"boolean\",\"default\":false,\"description\":\"the face's bold\""
     "},{\"name\":\"italic\",\"type\":\"boolean\",\"default\":false,\"description\":\"the face's italic\"},{\"name\":\""
     "height\",\"type\":\"number\",\"unit\":\"mm\",\"default\":null,\"min\":0.5,\"max\":500,\"description\":\"the text's"
     " height; left out, the height last used (10 mm at first)\"},{\"name\":\"plane\",\"type\":\"string|object"
     "\",\"default\":\"XY\",\"description\":\"\\\"XY\\\", \\\"XZ\\\" or \\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}"
     ", a Plane feature; or {\\\"body\\\": b, \\\"face\\\": f}, a planar face, whose centre the text is centre"
     "d on. The reply's frame gives the axes.\"},{\"name\":\"position\",\"type\":\"array\",\"default\":[0,0],\"des"
     "cription\":\"[x, y] in the plane, mm: where the text's centre goes\"}],\"examples\":[{\"title\":\"The wo"
     "rd Orca, 12 mm high, on the XY plane\",\"calls\":[{\"tool\":\"sketch_text\",\"args\":{\"height\":12,\"text\":"
     "\"Orca\"}}]},{\"title\":\"A label centred on a plate's top face\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\""
     "distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"body_describe\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}"
     "},{\"tool\":\"sketch_text\",\"args\":{\"generation\":{\"from_call\":1,\"key\":\"generation\"},\"height\":6,\"plan"
     "e\":{\"body\":0,\"face\":5},\"text\":\"A1\"}}]}],\"ids\":true}", nullptr, nullptr, "Sketch", false},
    {"sketch_insert_svg", L_CONTEXT("SVG", "Design"), 0, nullptr, "btn:svg", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_svg", L("Import an SVG outline into this sketch as editable lines"), true, CadAccess::Sketch, false,
     "{\"summary\":\"Add an SVG file's filled outlines to the open sketch as lines, or to the model as ar"
     "t on a plane.\",\"details\":\"The outlines are centred on the plane's origin, scaled by scale and mo"
     "ved by position, with no file dialog and no placement step. With a sketch open, a new one or one"
     " re-opened with sketch_edit, they are added to it as lines, as Insert SVG adds them there, and p"
     "lane is refused; reply: scope \\\"sketch\\\", added, entities (how many the sketch has). With none o"
     "pen, an art feature on plane, as Insert SVG adds one on the model, in one undo step; reply: art_"
     "index, bodies, frame (the plane's). Refused when the file is not there or gives no outline (no f"
     "illed shape).\",\"params\":[{\"name\":\"path\",\"type\":\"string\",\"description\":\"absolute path of the .svg"
     " file\"},{\"name\":\"scale\",\"type\":\"number\",\"default\":1,\"exclusive_min\":0,\"description\":\"times the s"
     "ize the file gives\"},{\"name\":\"position\",\"type\":\"array\",\"default\":[0,0],\"description\":\"[x, y] in "
     "the plane, mm: where the outlines' centre goes\"},{\"name\":\"plane\",\"type\":\"string|object\",\"default"
     "\":null,\"description\":\"with no sketch open, the art's plane: \\\"XY\\\" (left out or null), \\\"XZ\\\" or"
     " \\\"YZ\\\" through modeling_origin; {\\\"datum\\\": i}, a Plane feature; or {\\\"body\\\": b, \\\"face\\\": f},"
     " a planar face, whose centre is the origin. The reply's frame gives the axes. With a sketch open"
     ", refused: the outlines go on its plane.\"}],\"examples\":[{\"title\":\"A logo placed on the XY plane "
     "as art, its centre 10 mm right of the origin\",\"calls\":[{\"tool\":\"sketch_insert_svg\",\"args\":{\"path"
     "\":\"/path/to/logo.svg\",\"position\":[10,0]}}],\"needs\":\"an SVG file with a filled shape\"},{\"title\":\""
     "A logo's outlines drawn into an open sketch at half size\",\"calls\":[{\"tool\":\"sketch_create\",\"args"
     "\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_insert_svg\",\"args\":{\"path\":\"/path/to/logo.svg\",\"scale\":0.5}}],"
     "\"needs\":\"an SVG file with a filled shape\"}],\"ids\":true}", nullptr, nullptr, "Sketch", false},
    {"sketch_offset", L_CONTEXT("Offset", "Design"), 1, "O", "key:O", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_offset", L("Offset — pick an entity, drag the distance"), false, CadAccess::Sketch, false,
     "{\"summary\":\"Add an offset copy of sketch lines, arcs and circles.\",\"details\":\"The copy of one en"
     "tity alone is bound to it, Parallel for a line and Concentric for an arc or a circle, as the Off"
     "set tool binds it. Reply: first_index, added, constraints_added (that binding, as sketch_list_co"
     "nstraints writes it), dof.\",\"params\":[{\"name\":\"distance\",\"type\":\"number\",\"unit\":\"mm\",\"descriptio"
     "n\":\"signed offset\"},{\"name\":\"entities\",\"type\":\"array\",\"default\":[],\"description\":\"entity indices"
     "; omit for the selection, else every entity\"}],\"examples\":[{\"title\":\"A rectangle's 2 mm inner wa"
     "ll\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":["
     "-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_offset\",\"args\":{\"distance\":2}"
     "}]},{\"title\":\"A circle grown 2 mm by a negative offset\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":"
     "{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":5,\"type\":\"cir"
     "cle_center\"}]}},{\"tool\":\"sketch_offset\",\"args\":{\"distance\":-2,\"entities\":[0]}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_trim", L_CONTEXT("Trim", "Design"), 2, "T", "key:T", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_trim", L("Trim — click a segment to trim it"), false, CadAccess::Sketch, false,
     "{\"summary\":\"Trim the open sketch's entity nearest `at`, as a Trim click there does.\",\"details\":\""
     "A line or an arc is cut back from its end nearer `at` to the first crossing with another entity "
     "beyond `at`; a circle loses the part between the crossings either side of `at` and becomes an ar"
     "c. The entity's constraints are released. Reply: entity, geometry (its new fields), released_con"
     "straints, entities, dof.\",\"params\":[{\"name\":\"at\",\"type\":\"array\",\"description\":\"[x, y] in the ske"
     "tch plane, mm: a point on the part to cut\"},{\"name\":\"tolerance\",\"type\":\"number\",\"unit\":\"mm\",\"def"
     "ault\":0.05,\"description\":\"how far from `at` the entity may be\"}],\"examples\":[{\"title\":\"A line cu"
     "t back to the line crossing it\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":"
     "\"sketch_draw\",\"args\":{\"entities\":[{\"p0\":[-10,0],\"p1\":[10,0],\"type\":\"line\"},{\"p0\":[0,-5],\"p1\":[0,"
     "5],\"type\":\"line\"}]}},{\"tool\":\"sketch_trim\",\"args\":{\"at\":[5,0]}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_fillet", L_CONTEXT("Fillet", "Design"), 3, "F", "key:F", nullptr, 0x00090000u, 0, 0, false, true, nullptr, "design_filletedge", L("Fillet — pick two lines, set the radius"), false, CadAccess::Sketch, false,
     "{\"summary\":\"Round the corner of two lines of the open sketch, as the Fillet tool does.\",\"details"
     "\":\"Both lines are cut back and joined by an arc of `radius`, bound coincident and tangent to the"
     "m; constraints the sharp corner held are released. Reply: entity (the arc), lines, released_cons"
     "traints, entities, constraints, dof, solve_ok.\",\"params\":[{\"name\":\"entities\",\"type\":\"array\",\"def"
     "ault\":[],\"description\":\"the two line indices; omit for the selection\"},{\"name\":\"radius\",\"type\":\""
     "number\",\"unit\":\"mm\"}],\"examples\":[{\"title\":\"A rectangle's corner rounded to 3 mm\",\"calls\":[{\"too"
     "l\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\"p1\":[15,"
     "10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_fillet\",\"args\":{\"entities\":[0,1],\"radius\":3}}]}"
     "],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_chamfer", L_CONTEXT("Chamfer", "Design"), 3, "H", "key:H", nullptr, 0x00090000u, 0, 0, false, true, nullptr, "design_chamfer", L("Chamfer — pick two lines, set the distance"), false, CadAccess::Sketch, false,
     "{\"summary\":\"Bevel the corner of two lines of the open sketch, as the Chamfer tool does.\",\"detail"
     "s\":\"Both lines are cut back by `distance` from the corner and joined by a line bound coincident "
     "to them. Reply: entity (the new line), lines, released_constraints, entities, constraints, dof, "
     "solve_ok.\",\"params\":[{\"name\":\"entities\",\"type\":\"array\",\"default\":[],\"description\":\"the two line "
     "indices; omit for the selection\"},{\"name\":\"distance\",\"type\":\"number\",\"unit\":\"mm\",\"description\":\""
     "setback along each line\"}],\"examples\":[{\"title\":\"A rectangle's corner bevelled 2 mm back\",\"calls"
     "\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\""
     "p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_chamfer\",\"args\":{\"distance\":2,\"entities"
     "\":[1,2]}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sk_array", L_CONTEXT("Linear array", "Design"), 4, nullptr, "fly:design_array#0", nullptr, 0x000b0000u, 0, 0, false, true, L_CONTEXT("Array", "Design"), "design_array", L("Pick entities, drag the spacing handle, click the count; click empty to apply"), false, CadAccess::None, false,
     nullptr, "sketch_pattern", "{\"type\":\"linear\"}", nullptr, false},
    {"sk_array_polar", L_CONTEXT("Polar array", "Design"), 4, nullptr, "fly:design_array#1", nullptr, 0x000b0000u, 0, 0, false, true, L_CONTEXT("Array", "Design"), "design_polararray", L("Pick entities, drag the sweep handle, click the count; click empty to apply"), false, CadAccess::None, false,
     nullptr, "sketch_pattern", "{\"type\":\"circular\"}", nullptr, false},
    {"sketch_mirror", L_CONTEXT("Mirror", "Design"), 4, "M", "key:M", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_mirror", L("Mirror — pick axis, then entities"), false, CadAccess::Sketch, false,
     "{\"summary\":\"Add a mirrored copy of sketch entities about a line of the sketch, one of its axes, "
     "or the line through two points.\",\"details\":\"About axis, as the Mirror tool's Confirm: each copy "
     "is bound Symmetric to its source about the axis, unless that would move anything off its reflect"
     "ion, and constraints_added lists the constraints kept. axis_a and axis_b, two free points, bind "
     "nothing, and axis is refused beside either. Reply: first_index, added, constraints_added (as ske"
     "tch_list_constraints writes them), dof.\",\"params\":[{\"name\":\"axis\",\"type\":\"integer|string\",\"defau"
     "lt\":null,\"description\":\"a line of the sketch by its entity index, \\\"x_axis\\\" or \\\"y_axis\\\"; omit"
     " for axis_a and axis_b\"},{\"name\":\"axis_a\",\"type\":\"array\",\"default\":[0,0],\"description\":\"[x, y] i"
     "n the sketch plane, mm\"},{\"name\":\"axis_b\",\"type\":\"array\",\"default\":[0,1],\"description\":\"[x, y] i"
     "n the sketch plane, mm\"},{\"name\":\"entities\",\"type\":\"array\",\"default\":[],\"description\":\"entity in"
     "dices; omit for the selection, else every entity but the axis\"}],\"examples\":[{\"title\":\"A circle "
     "mirrored about the sketch's y axis, its copy bound to it\",\"calls\":[{\"tool\":\"sketch_create\",\"args"
     "\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"center\":[10,0],\"radius\":3,\"type\":\""
     "circle_center\"}]}},{\"tool\":\"sketch_mirror\",\"args\":{\"axis\":\"y_axis\"}}]},{\"title\":\"A line mirrored"
     " about a construction line of the sketch\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}"
     "},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"p0\":[5,0],\"p1\":[10,5],\"type\":\"line\"},{\"constructio"
     "n\":true,\"p0\":[20,-10],\"p1\":[20,10],\"type\":\"line\"}]}},{\"tool\":\"sketch_mirror\",\"args\":{\"axis\":1,\"e"
     "ntities\":[0]}}]},{\"title\":\"A circle mirrored across the line through two free points\",\"calls\":[{"
     "\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"center"
     "\":[10,0],\"radius\":3,\"type\":\"circle_center\"}]}},{\"tool\":\"sketch_mirror\",\"args\":{\"axis_a\":[0,0],\"a"
     "xis_b\":[1,1]}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sk_move", L_CONTEXT("Move", "Design"), 5, nullptr, "fly:design_move#0", nullptr, 0x000f0000u, 0, 0, false, true, L_CONTEXT("Move", "Design"), "design_move", L("Pick entities, then drag the handle or click the distance; click empty to apply"), false, CadAccess::None, false,
     nullptr, "sketch_transform", "{\"type\":\"move\"}", nullptr, false},
    {"sk_rotate", L_CONTEXT("Rotate", "Design"), 5, nullptr, "fly:design_move#1", nullptr, 0x000f0000u, 0, 0, false, true, L_CONTEXT("Move", "Design"), "design_rotate", L("Pick entities, then drag around the pivot or click the angle; click empty to apply"), false, CadAccess::None, false,
     nullptr, "sketch_transform", "{\"type\":\"rotate\"}", nullptr, false},
    {"sk_scale", L_CONTEXT("Scale", "Design"), 5, nullptr, "fly:design_move#2", nullptr, 0x000f0000u, 0, 0, false, true, L_CONTEXT("Move", "Design"), "design_scale", L("Pick entities, then drag the handle or click the factor; click empty to apply"), false, CadAccess::None, false,
     nullptr, "sketch_transform", "{\"type\":\"scale\"}", nullptr, false},
    {"sketch_dimension", L_CONTEXT("Dimension", "Design"), 6, "D", "key:D", nullptr, 0x000f8000u, 0, 0, false, true, nullptr, "design_dimension", L("Dimension — click 2 points or an entity"), false, CadAccess::Sketch, false,
     "{\"summary\":\"Add a dimension to the open sketch: a distance, an angle, a radius or a diameter.\",\""
     "details\":\"With `entities`, else the selection, the CONSTRAIN bar's rules choose the points, and "
     "the value is `value` or the current one. With `refs` the references are taken as written, as ske"
     "tch_constrain takes them, and `value` is required. distance has no button and takes refs. A dime"
     "nsion the sketch cannot solve with is rolled back and the call fails. Reply: added (indices), it"
     "ems, constraints, dof, solve_ok.\",\"params\":[{\"name\":\"type\",\"type\":\"string\",\"enum\":[\"distance\",\"d"
     "istance_x\",\"distance_y\",\"angle\",\"radius\",\"diameter\"]},{\"name\":\"entities\",\"type\":\"array\",\"default"
     "\":[],\"description\":\"1 or 2 entity indices; omit for the selection\"},{\"name\":\"refs\",\"type\":\"array"
     "\",\"default\":[],\"description\":\"[{entity, role?}], instead of entities\"},{\"name\":\"value\",\"type\":\"n"
     "umber\",\"default\":null,\"description\":\"mm, or degrees for an angle\"}],\"examples\":[{\"title\":\"A line"
     "'s ends held 25 mm apart, named by refs\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}}"
     ",{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"p0\":[0,0],\"p1\":[10,2],\"type\":\"line\"}]}},{\"tool\":\"sk"
     "etch_dimension\",\"args\":{\"refs\":[{\"entity\":0,\"role\":\"p0\"},{\"entity\":0,\"role\":\"p1\"}],\"type\":\"dista"
     "nce\",\"value\":25}}]},{\"title\":\"A circle's radius set to 5 mm\",\"calls\":[{\"tool\":\"sketch_create\",\"a"
     "rgs\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":3,\"type\""
     ":\"circle_center\"}]}},{\"tool\":\"sketch_dimension\",\"args\":{\"entities\":[0],\"type\":\"radius\",\"value\":5"
     "}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_constrain", L_CONTEXT("Constrain", "Design"), 6, "K", "key:K", nullptr, 0x000f0000u, 0, 0, false, true, nullptr, "design_constrain", L("Constrain the selected sketch entities to each other"), false, CadAccess::Sketch, false,
     "{\"summary\":\"Add a constraint to the open sketch, as a CONSTRAIN bar button does (sketch_set_valu"
     "e is the value field, V).\",\"details\":\"With `entities`, else the selection, the button's own rule"
     "s choose the points. With `refs` the references are taken as written, in sketch_list_constraints"
     "' form: {entity, role} for a point (role p0, p1 or center; entity also \\\"origin\\\") and {entity} "
     "for a whole entity (also \\\"x_axis\\\", \\\"y_axis\\\"). lock_x, lock_y, point_on_line and point_on_obj"
     "ect have no button and take refs. A distance, an angle, a radius or a diameter is sketch_dimensi"
     "on's. A constraint the sketch cannot solve with is rolled back and the call fails. Reply: added "
     "(indices), items, constraints, dof, solve_ok.\",\"params\":[{\"name\":\"type\",\"type\":\"string\",\"enum\":["
     "\"fix\",\"coincident\",\"horizontal\",\"vertical\",\"lock_x\",\"lock_y\",\"equal_length\",\"parallel\",\"perpendi"
     "cular\",\"concentric\",\"tangent\",\"midpoint\",\"symmetric\",\"point_on_line\",\"point_on_object\",\"equal_ra"
     "dius\",\"collinear\",\"symmetric_about_y\",\"symmetric_about_x\",\"equal\"],\"description\":\"\\\"equal\\\" is t"
     "he Equal button: equal length, or equal radius for two curves\"},{\"name\":\"entities\",\"type\":\"array"
     "\",\"default\":[],\"description\":\"1 to 3 entity indices; omit for the selection\"},{\"name\":\"refs\",\"ty"
     "pe\":\"array\",\"default\":[],\"description\":\"[{entity, role?}], instead of entities\"},{\"name\":\"value\""
     ",\"type\":\"number\",\"default\":null,\"description\":\"mm: the coordinate lock_x and lock_y hold, or poi"
     "nt_on_line's distance from the line; the other types take none\"}],\"examples\":[{\"title\":\"A slopin"
     "g line made horizontal\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_"
     "draw\",\"args\":{\"entities\":[{\"p0\":[0,0],\"p1\":[10,2],\"type\":\"line\"}]}},{\"tool\":\"sketch_constrain\",\""
     "args\":{\"entities\":[0],\"type\":\"horizontal\"}}]},{\"title\":\"Two circles given equal radii\",\"calls\":["
     "{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"cente"
     "r\":[0,0],\"radius\":4,\"type\":\"circle_center\"},{\"center\":[15,0],\"radius\":6,\"type\":\"circle_center\"}]"
     "}},{\"tool\":\"sketch_constrain\",\"args\":{\"entities\":[0,1],\"type\":\"equal\"}}]},{\"title\":\"A line's sta"
     "rt held at x = 5, named by refs\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\""
     ":\"sketch_draw\",\"args\":{\"entities\":[{\"p0\":[0,0],\"p1\":[10,2],\"type\":\"line\"}]}},{\"tool\":\"sketch_con"
     "strain\",\"args\":{\"refs\":[{\"entity\":0,\"role\":\"p0\"}],\"type\":\"lock_x\",\"value\":5}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    // Same verb, model-mode vocabulary: offered when a SKETCH is selected (bit 14, SkLoop), the
    // state a user is in right after finishing one. Without this item the only way in was the
    // toolbar icon, and constraints read as absent — see the Onshape-comparison report.
    // It sits in the Reference row, the address the sketch-mode Constrain has: one verb, one row.
    {"sketch_constrain_selected", L_CONTEXT("Constrain sketch", "Design"), 6, nullptr, "btn:constrain", L("Select a sketch to constrain it"), 0x00004000u, 0, 1, false, false, nullptr, "design_constrain", L("Add dimensions and relations (coincident, tangent, parallel...) to the selected sketch"), false, CadAccess::None, false,
     nullptr, nullptr, nullptr, "Menu items", true},
    {"sketch_toggle_construction", L_CONTEXT("Construction", "Design"), 6, "Q", "key:Q", nullptr, 0x000b8000u, 0, 0, false, true, nullptr, "design_construction", L("Toggle construction: geometry that guides but is never built"), false, CadAccess::Sketch, false,
     "{\"summary\":\"Toggle construction on sketch entities.\",\"details\":\"Reply: changed, how many, and ok"
     ", false when none did.\",\"params\":[{\"name\":\"entities\",\"type\":\"array\",\"default\":[],\"description\":\""
     "entity indices; omit for the selection\"}],\"examples\":[{\"title\":\"A centre line across a rectangle"
     ", made construction so it splits no loop\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}"
     "},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\""
     ":\"sketch_draw\",\"args\":{\"entities\":[{\"p0\":[0,-10],\"p1\":[0,10],\"type\":\"line\"}]}},{\"tool\":\"sketch_t"
     "oggle_construction\",\"args\":{\"entities\":[4]}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_extend", L_CONTEXT("Extend", "Design"), 7, "X", "key:X", nullptr, 0x000b0000u, 0, 0, false, true, nullptr, "design_extend", L("Extend — click a line/arc to extend it"), false, CadAccess::Sketch, false,
     "{\"summary\":\"Extend the open sketch's line or arc nearest `at` to the next entity, as an Extend c"
     "lick there does.\",\"details\":\"The line's or arc's end nearer `at` runs out to the first entity it"
     " meets. The entity's constraints are released. Reply: entity, geometry (its new fields), release"
     "d_constraints, entities, dof.\",\"params\":[{\"name\":\"at\",\"type\":\"array\",\"description\":\"[x, y] in th"
     "e sketch plane, mm: near the end to extend\"},{\"name\":\"tolerance\",\"type\":\"number\",\"unit\":\"mm\",\"de"
     "fault\":0.05,\"description\":\"how far from `at` the entity may be\"}],\"examples\":[{\"title\":\"A line e"
     "xtended down to the line below it\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"too"
     "l\":\"sketch_draw\",\"args\":{\"entities\":[{\"p0\":[-10,0],\"p1\":[10,0],\"type\":\"line\"},{\"p0\":[0,3],\"p1\":["
     "0,8],\"type\":\"line\"}]}},{\"tool\":\"sketch_extend\",\"args\":{\"at\":[0,3]}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_delete", L_CONTEXT("Delete", "Design"), 7, "Del", "btn:sk_delete", nullptr, 0x000f0000u, 0, 0, false, true, nullptr, "design_delete", L("Delete the selected sketch entities"), false, CadAccess::Sketch, false,
     "{\"summary\":\"Delete sketch entities.\",\"details\":\"Reply: removed, entities.\",\"params\":[{\"name\":\"en"
     "tities\",\"type\":\"array\",\"default\":[],\"description\":\"entity indices; omit for the selection\"}],\"ex"
     "amples\":[{\"title\":\"Delete a rectangle's top side\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plan"
     "e\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}}"
     ",{\"tool\":\"sketch_delete\",\"args\":{\"entities\":[2]}}]},{\"title\":\"Delete the selected sides\",\"calls\""
     ":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\"p"
     "1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_select\",\"args\":{\"entities\":[1,3]}},{\"too"
     "l\":\"sketch_delete\",\"args\":{\"silent\":true}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    // Typing the defining number of the element you pointed at. Three items rather than one so
    // each names the quantity in the drawing-office word for THAT element; all three land on
    // the same handler, because dimension_kind() already resolves the quantity from the
    // selection. Without these, an element's own numbers were reachable only by arming the
    // Dimension tool and re-picking geometry that was already selected.
    {"sk_length", L_CONTEXT("Length…", "Design"), 7, "V", "key:V", nullptr, 0x00010000u, 0, 0, false, true, nullptr, "design_dimension", L("Type the length of this line"), false, CadAccess::None, false,
     nullptr, "sketch_set_value", "{}", nullptr, false},
    {"sk_radius", L_CONTEXT("Radius / diameter…", "Design"), 7, "V", "key:V", nullptr, 0x00020000u, 0, 0, false, true, nullptr, "design_dimension", L("Type the radius of this arc, or the diameter of this circle"), false, CadAccess::None, false,
     nullptr, "sketch_set_value", "{}", nullptr, false},
    {"sk_angdist", L_CONTEXT("Angle / distance…", "Design"), 7, "V", "key:V", nullptr, 0x00080000u, 0, 0, false, true, nullptr, "design_dimension", L("Type the angle between two lines, or the distance between the two picks"), false, CadAccess::None, false,
     nullptr, "sketch_set_value", "{}", nullptr, false},
    {"describe_design", L_CONTEXT("Describe design", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"The Design document: features, variables with their values, bodies, undo_depth / red"
     "o_depth and generation.\",\"details\":\"A feature's bound lists the fields an expression drives; a b"
     "ody gives bbox, where the model has it, source_feature, visible, color, moved, and placement: wh"
     "ere the body stands for Commit to Plate and Export STEP (body_place, the Move gizmo, Place on Fa"
     "ce), null where the model has it, else three rows of four numbers, the rotation and the translat"
     "ion in mm, in the world frame. commit_mode is the Commit button's mode, assembly or bodies, whic"
     "h commit_to_plate uses when given none. selection is what the right-click menu would be built fo"
     "r: its kind, the selection_kind a tool's opening reply gives, and its id (none, face_planar, bod"
     "y_solid, sk_line and the like). picked_plane is the reference plane picked in the view (XY, XZ, "
     "YZ), which the next Sketch opens on, and feature_row the selected row of the Feature list, a fea"
     "ture index; each is null when there is none.\",\"params\":[],\"examples\":[{\"title\":\"An empty design\""
     ",\"calls\":[{\"tool\":\"describe_design\",\"args\":{}}]},{\"title\":\"A plate's two features and its body\","
     "\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"describe_desig"
     "n\",\"args\":{}}]}],\"ids\":false}", nullptr, nullptr, "Document", false},
    {"feature_describe", L_CONTEXT("Feature describe", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"One feature as its card shows it: params, the editable ones with their ranges, expr,"
     " refs, used_by and bodies_made.\",\"details\":\"expr holds its bindings, and refs the feature and bo"
     "dy indices and face and edge ids it uses, those feature_edit's refs takes; ids_of says which mod"
     "el those body indices and ids are read in: \\\"before\\\", the model just before the feature, whose "
     "bodies bodies_before lists ({index, name}) and body_describe {before: feature} reads, or \\\"finis"
     "hed\\\" for a Plane, an Axis and a CoordSys, read with body_describe without before. A sketch adds"
     " its plane, entities and constraints (angles in degrees). Imported Text or SVG art shows its reg"
     "ions (a count), region_loops (how many closed loops each region has, its outer loop plus its hol"
     "es; counts, not the loops' geometry) and region_areas (each region's filled area in mm² as place"
     "d, its outer loop less its holes).\",\"params\":[{\"name\":\"feature\",\"type\":\"integer\",\"description\":\""
     "feature index (describe_design)\"}],\"examples\":[{\"title\":\"A plate's extrude, with the fields its "
     "card edits\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"fe"
     "ature_describe\",\"args\":{\"feature\":1}}]},{\"title\":\"A committed sketch's plane, entities and const"
     "raints\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p"
     "0\":[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool"
     "\":\"feature_describe\",\"args\":{\"feature\":0}}]}],\"ids\":true}", nullptr, nullptr, "History", false},
    {"body_describe", L_CONTEXT("Body describe", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
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
     "default\":0},{\"name\":\"vertices\",\"type\":\"boolean\",\"default\":false,\"description\":\"also list every v"
     "ertex of the body as vertices [{id, p}], unfiltered and unpaged\"},{\"name\":\"before\",\"type\":\"integ"
     "er\",\"default\":null,\"description\":\"a feature index: the body as it stood just before that feature"
     ", whose ids are the ones feature_edit's refs takes for it; the reply says before, and its genera"
     "tion is the design's\"}],\"examples\":[{\"title\":\"Every face and edge of a 20 x 20 x 5 mm plate\",\"ca"
     "lls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"body_describe\",\"a"
     "rgs\":{\"body\":0}}]},{\"title\":\"A plate's top face\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5"
     ",\"height\":20,\"width\":20}},{\"tool\":\"body_describe\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}}]},{\"title"
     "\":\"A plate's vertical outside corners, the edges a fillet rounds\",\"calls\":[{\"tool\":\"extrude\",\"ar"
     "gs\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"body_describe\",\"args\":{\"body\":0,\"convex\":tru"
     "e,\"direction\":[0,0,1],\"edge_kinds\":[\"line\"]}}]}],\"ids\":true}", nullptr, nullptr, "Inspect", false},
    {"body_section", L_CONTEXT("Body section", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"Cross-section of a body by a base plane at an offset (sections-as-evidence).\",\"detai"
     "ls\":\"Returns ordered world contours, each flagged closed/open.\",\"params\":[{\"name\":\"body\",\"type\":"
     "\"integer\",\"default\":0},{\"name\":\"plane\",\"type\":\"string\",\"default\":\"XY\",\"enum\":[\"XY\",\"XZ\",\"YZ\"],\"d"
     "escription\":\"the base plane through the world origin, not through modeling_origin as the other c"
     "ommands' planes are\"},{\"name\":\"offset\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0,\"description\":\"al"
     "ong the plane's normal, from the world origin\"}],\"examples\":[{\"title\":\"A plate's cross-section h"
     "alfway up\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"bod"
     "y_section\",\"args\":{\"body\":0,\"offset\":2.5,\"plane\":\"XY\"}}]},{\"title\":\"The two contours through a p"
     "late with a bore\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\","
     "\"args\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_draw\",\"args\":{\""
     "entities\":[{\"center\":[0,0],\"radius\":4,\"type\":\"circle_center\"}]}},{\"tool\":\"sketch_finish\",\"args\":"
     "{}},{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"sketch\":0}},{\"tool\":\"body_section\",\"args\":{\"body\":0,"
     "\"offset\":2.5,\"plane\":\"XY\"}}]}],\"ids\":true}", nullptr, nullptr, "Inspect", false},
    {"body_compare", L_CONTEXT("Body compare", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"Compare a body with a reference: volume, bbox, centroid and surface deviation (the R"
     "E acceptance metric).\",\"details\":\"The reference is {step:path|body:id}; the surface deviation is"
     " max/mean/rms in mm.\",\"params\":[{\"name\":\"body\",\"type\":\"integer\",\"default\":0},{\"name\":\"reference\""
     ",\"type\":\"object\"}],\"examples\":[{\"title\":\"A 5 mm plate against a 6 mm one\",\"calls\":[{\"tool\":\"extr"
     "ude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"extrude\",\"args\":{\"distance\":6,\"heigh"
     "t\":20,\"operation\":\"new_body\",\"width\":20}},{\"tool\":\"body_compare\",\"args\":{\"body\":0,\"reference\":{\""
     "body\":1}}}]},{\"title\":\"A plate against a reference part's STEP file\",\"calls\":[{\"tool\":\"extrude\","
     "\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"body_compare\",\"args\":{\"body\":0,\"reference"
     "\":{\"step\":\"/path/to/reference.step\"}}}],\"needs\":\"a STEP file of the reference part\"}],\"ids\":fals"
     "e}", nullptr, nullptr, "Inspect", false},
    {"sketch_describe", L_CONTEXT("Sketch describe", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"The open sketch, or a committed one: entities, constraint count and its loops.\",\"det"
     "ails\":\"The loops are closed_loops {index, entities, holes, area}, open_ends, defects, buildable."
     " The open sketch adds dof, the armed tool, the selection (entity indices) and selected_constrain"
     "t, the index of the constraint picked by its badge or the Constraints list, null for none; a com"
     "mitted one adds its name and plane.\",\"params\":[{\"name\":\"sketch\",\"type\":\"integer\",\"default\":null,"
     "\"description\":\"a committed Sketch feature's index; omit for the open sketch\"}],\"examples\":[{\"tit"
     "le\":\"The open sketch: a rectangle with a circle inside it as a hole\",\"calls\":[{\"tool\":\"sketch_cr"
     "eate\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":\"r"
     "ectangle_2_point\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":4,\"type\":"
     "\"circle_center\"}]}},{\"tool\":\"sketch_describe\",\"args\":{}}]},{\"title\":\"A committed sketch's plane,"
     " entities and loop\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw"
     "\",\"args\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_finish\",\"args"
     "\":{}},{\"tool\":\"sketch_describe\",\"args\":{\"sketch\":0}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_validate", L_CONTEXT("Sketch validate", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"The open sketch's loops (closed_loops, open_ends, defects, buildable) and dof.\",\"det"
     "ails\":\"\",\"params\":[{\"name\":\"tolerance\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0.001,\"description\""
     ":\"reported back\"}],\"examples\":[{\"title\":\"A triangle with a 0.05 mm gap: two open ends, not build"
     "able\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"ent"
     "ities\":[{\"p0\":[0,0],\"p1\":[20,0],\"type\":\"line\"},{\"p0\":[20,0.05],\"p1\":[20,10],\"type\":\"line\"},{\"p0\""
     ":[20,10],\"p1\":[0,0],\"type\":\"line\"}]}},{\"tool\":\"sketch_validate\",\"args\":{}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_list_constraints", L_CONTEXT("Sketch list constraints", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"The constraints of the open sketch, or of a committed one: index, type, refs and val"
     "ue.\",\"details\":\"A ref is {entity, role} for a point and {entity} for a whole entity; value is mm"
     ", or degrees for an angle. Reply: scope (live or committed), items, and dof and solve_ok for the"
     " open sketch.\",\"params\":[{\"name\":\"sketch\",\"type\":\"integer\",\"default\":null,\"description\":\"a commi"
     "tted Sketch feature's index; omit for the open sketch\"}],\"examples\":[{\"title\":\"The coincident, h"
     "orizontal and vertical constraints a drawn rectangle gets\",\"calls\":[{\"tool\":\"sketch_create\",\"arg"
     "s\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2"
     "_point\"}},{\"tool\":\"sketch_list_constraints\",\"args\":{}}]},{\"title\":\"A committed sketch's constrai"
     "nts\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":"
     "[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\":\""
     "sketch_list_constraints\",\"args\":{\"sketch\":0}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"import_step", L_CONTEXT("Import step", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, true,
     "{\"summary\":\"Import a STEP file as native B-rep bodies (the reference part to measure).\",\"details"
     "\":\"\",\"params\":[{\"name\":\"path\",\"type\":\"string\",\"description\":\"absolute path\"}],\"examples\":[{\"titl"
     "e\":\"A part read from a STEP file as a body\",\"calls\":[{\"tool\":\"import_step\",\"args\":{\"path\":\"/path"
     "/to/part.step\"}}],\"needs\":\"a STEP file of the part\"}],\"ids\":false}", nullptr, nullptr, "Document", false},
    {"import_mesh", L_CONTEXT("Import mesh", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, true,
     "{\"summary\":\"Convert a triangle mesh (STL/OBJ) into an editable B-rep body.\",\"details\":\"Reports w"
     "hether the result is a real solid or an open shell, and why.\",\"params\":[{\"name\":\"path\",\"type\":\"s"
     "tring\",\"description\":\"absolute path\"},{\"name\":\"tolerance\",\"type\":\"number\",\"default\":0.01},{\"name"
     "\":\"merge_angle_deg\",\"type\":\"number\",\"default\":5.0}],\"examples\":[{\"title\":\"An STL mesh turned int"
     "o an editable body\",\"calls\":[{\"tool\":\"import_mesh\",\"args\":{\"path\":\"/path/to/part.stl\"}}],\"needs\""
     ":\"an STL or OBJ mesh file\"}],\"ids\":false}", nullptr, nullptr, "Document", false},
    {"sketch_blend_curve", L_CONTEXT("Sketch blend curve", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, true,
     "{\"summary\":\"Build a cubic-Bezier G1 bridge (BSpline) between two sketch-entity endpoints within "
     "a sketch feature.\",\"details\":\"\",\"params\":[{\"name\":\"sketch\",\"type\":\"integer\",\"description\":\"sketc"
     "h feature index\"},{\"name\":\"ent_a\",\"type\":\"integer\",\"description\":\"first entity index within the "
     "sketch\"},{\"name\":\"end_a\",\"type\":\"integer\",\"default\":1,\"enum\":[0,1],\"description\":\"0 = start/p0 s"
     "ide, 1 = end/p1 side\"},{\"name\":\"ent_b\",\"type\":\"integer\",\"description\":\"second entity index withi"
     "n the sketch\"},{\"name\":\"end_b\",\"type\":\"integer\",\"default\":0,\"enum\":[0,1],\"description\":\"0 = star"
     "t/p0 side, 1 = end/p1 side\"}],\"examples\":[{\"title\":\"A smooth curve joining the facing ends of tw"
     "o lines in a committed sketch\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\""
     "sketch_draw\",\"args\":{\"entities\":[{\"p0\":[-20,0],\"p1\":[-5,0],\"type\":\"line\"},{\"p0\":[5,10],\"p1\":[20,"
     "10],\"type\":\"line\"}]}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"sketch_blend_curve\",\"args\":{\"e"
     "nd_a\":1,\"end_b\":0,\"ent_a\":0,\"ent_b\":1,\"sketch\":0}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"set_variable", L_CONTEXT("Set variable", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, true,
     "{\"summary\":\"Set a design variable to an expression, or remove it with an empty expr.\",\"details\":"
     "\"Feature fields bound with feature_set_expression follow it.\",\"params\":[{\"name\":\"name\",\"type\":\"s"
     "tring\"},{\"name\":\"expr\",\"type\":\"string\",\"description\":\"e.g. \\\"20\\\" or \\\"width / 2 + 3\\\"; \\\"\\\" rem"
     "oves the variable\"}],\"examples\":[{\"title\":\"A variable w of 20\",\"calls\":[{\"tool\":\"set_variable\",\""
     "args\":{\"expr\":\"20\",\"name\":\"w\"}}]},{\"title\":\"A variable worked out from another: h = w / 2 + 3, w"
     "hich is 13\",\"calls\":[{\"tool\":\"set_variable\",\"args\":{\"expr\":\"20\",\"name\":\"w\"}},{\"tool\":\"set_variab"
     "le\",\"args\":{\"expr\":\"w / 2 + 3\",\"name\":\"h\"}}]},{\"title\":\"A variable removed with an empty express"
     "ion\",\"calls\":[{\"tool\":\"set_variable\",\"args\":{\"expr\":\"20\",\"name\":\"w\"}},{\"tool\":\"set_variable\",\"ar"
     "gs\":{\"expr\":\"\",\"name\":\"w\"}}]}],\"ids\":false}", nullptr, nullptr, "Document", false},
    {"feature_set_expression", L_CONTEXT("Feature set expression", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, true,
     "{\"summary\":\"Bind a feature's numeric field to an expression over the design variables, or unbind"
     " it with an empty expr.\",\"details\":\"Unbound, the field keeps its value.\",\"params\":[{\"name\":\"feat"
     "ure\",\"type\":\"integer\",\"description\":\"feature index (describe_design)\"},{\"name\":\"field\",\"type\":\"s"
     "tring\",\"description\":\"a numeric field of feature_describe's params, e.g. width, distance, hole_d"
     "iameter, thread_diameter\"},{\"name\":\"expr\",\"type\":\"string\",\"description\":\"\\\"\\\" removes the bindin"
     "g\"}],\"examples\":[{\"title\":\"A plate's thickness bound to the variable t\",\"calls\":[{\"tool\":\"extrud"
     "e\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"set_variable\",\"args\":{\"expr\":\"8\",\"name"
     "\":\"t\"}},{\"tool\":\"feature_set_expression\",\"args\":{\"expr\":\"t\",\"feature\":1,\"field\":\"distance\"}}]},{"
     "\"title\":\"A plate's thickness unbound, keeping the 8 mm the binding gave it\",\"calls\":[{\"tool\":\"ex"
     "trude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"set_variable\",\"args\":{\"expr\":\"8\",\""
     "name\":\"t\"}},{\"tool\":\"feature_set_expression\",\"args\":{\"expr\":\"t\",\"feature\":1,\"field\":\"distance\"}}"
     ",{\"tool\":\"feature_set_expression\",\"args\":{\"expr\":\"\",\"feature\":1,\"field\":\"distance\"}}]}],\"ids\":fa"
     "lse}", nullptr, nullptr, "History", false},
    {"feature_move", L_CONTEXT("Feature move", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Move a feature one place up or down the history, as Move up / Move down do.\",\"detail"
     "s\":\"A feature cannot come before one it uses. One undo step. Reply: feature (its new index), fro"
     "m.\",\"params\":[{\"name\":\"feature\",\"type\":\"integer\",\"description\":\"feature index (describe_design)\""
     "},{\"name\":\"delta\",\"type\":\"integer\",\"enum\":[-1,1],\"description\":\"-1 up, 1 down\"}],\"examples\":[{\"t"
     "itle\":\"A datum plane moved up the history, above the extrude before it\",\"calls\":[{\"tool\":\"extrud"
     "e\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"construct_plane\",\"args\":{\"base\":\"XY\",\""
     "offset\":20,\"type\":\"offset\"}},{\"tool\":\"feature_move\",\"args\":{\"delta\":-1,\"feature\":2}}]}],\"ids\":fa"
     "lse}", nullptr, nullptr, "History", false},
    {"feature_suppress", L_CONTEXT("Feature suppress", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Suppress a feature, or bring it back, as its row's eye hides or shows it.\",\"details\""
     ":\"A suppressed feature is left out of the rebuild and bodies may renumber. One undo step, none w"
     "hen it already was. Reply: feature, suppressed, changed, bodies.\",\"params\":[{\"name\":\"feature\",\"t"
     "ype\":\"integer\",\"description\":\"feature index (describe_design)\"},{\"name\":\"suppressed\",\"type\":\"boo"
     "lean\",\"description\":\"true leaves the feature out of the rebuild; false brings it back\"}],\"exampl"
     "es\":[{\"title\":\"A plate's hole hidden: the plate rebuilds without it\",\"calls\":[{\"tool\":\"extrude\","
     "\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"hole\",\"args\":{\"diameter\":5,\"through\":true"
     "}},{\"tool\":\"feature_suppress\",\"args\":{\"feature\":2,\"suppressed\":true}}]},{\"title\":\"A hidden hole "
     "shown again\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"h"
     "ole\",\"args\":{\"diameter\":5,\"through\":true}},{\"tool\":\"feature_suppress\",\"args\":{\"feature\":2,\"suppr"
     "essed\":true}},{\"tool\":\"feature_suppress\",\"args\":{\"feature\":2,\"suppressed\":false}}]}],\"ids\":false"
     "}", nullptr, nullptr, "History", false},
    {"feature_rename", L_CONTEXT("Feature rename", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Rename a feature as F2 on its row does.\",\"details\":\"Its own undo step. Reply: name, "
     "changed.\",\"params\":[{\"name\":\"feature\",\"type\":\"integer\",\"description\":\"feature index (describe_de"
     "sign)\"},{\"name\":\"name\",\"type\":\"string\",\"description\":\"trimmed; not empty\"}],\"examples\":[{\"title\""
     ":\"A plate's extrude renamed Base plate\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\""
     ":20,\"width\":20}},{\"tool\":\"feature_rename\",\"args\":{\"feature\":1,\"name\":\"Base plate\"}}]}],\"ids\":fal"
     "se}", nullptr, nullptr, "History", false},
    {"body_set", L_CONTEXT("Body set", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Rename, colour or show/hide a body, as its Bodies row and the Color button do; give "
     "one or more.\",\"details\":\"Not in the undo history. The name and colour are saved with the project"
     " and the name names its committed object; visibility is view state, and commit_to_plate leaves h"
     "idden bodies out. Reply: name, color, visible.\",\"params\":[{\"name\":\"body\",\"type\":\"integer\",\"descr"
     "iption\":\"body index (describe_design)\"},{\"name\":\"name\",\"type\":\"string\",\"default\":null,\"descripti"
     "on\":\"trimmed; not empty\"},{\"name\":\"color\",\"type\":\"string\",\"default\":null,\"description\":\"\\\"#RRGGB"
     "B\\\"\"},{\"name\":\"visible\",\"type\":\"boolean\",\"default\":null}],\"examples\":[{\"title\":\"A plate's body n"
     "amed Base and coloured blue\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\""
     ":20}},{\"tool\":\"body_set\",\"args\":{\"body\":0,\"color\":\"#3366CC\",\"name\":\"Base\"}}]},{\"title\":\"The seco"
     "nd of two bodies hidden\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}"
     "},{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"operation\":\"new_body\",\"profile\":[[20,-5],[30,-5],[30,"
     "5],[20,5]]}},{\"tool\":\"body_set\",\"args\":{\"body\":1,\"visible\":false}}]}],\"ids\":false}", nullptr, nullptr, "Bodies", false},
    {"commit_to_plate", L_CONTEXT("Commit to plate", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Commit to Plate: send the visible bodies to Prepare at their Move-gizmo positions an"
     "d show Prepare.\",\"details\":\"It is one Prepare undo step. A lone body is sent even when hidden. A"
     " body named in the Bodies list or with body_set names its object or part. Reply: objects (object"
     "_list index after replace, object_id, name, parts: how many), bodies_sent, bodies_hidden, replac"
     "ed.\",\"params\":[{\"name\":\"mode\",\"type\":\"string\",\"default\":\"\",\"enum\":[\"assembly\",\"bodies\"],\"descrip"
     "tion\":\"assembly: one object with a part per body; bodies: an object per body. Default: the Commi"
     "t button's mode, which this leaves as it is.\"},{\"name\":\"replace\",\"type\":\"array\",\"default\":[],\"de"
     "scription\":\"object_list indices to delete in the same undo step, such as an earlier commit. Thei"
     "r settings, modifiers and paint go with them.\"},{\"name\":\"replace_ids\",\"type\":\"array\",\"default\":["
     "],\"description\":\"replace by the objects' ids in object_list.\"}],\"examples\":[{\"title\":\"A plate se"
     "nt to Prepare as one object\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\""
     ":20}},{\"tool\":\"commit_to_plate\",\"args\":{}}]},{\"title\":\"Two bodies sent to Prepare as an object e"
     "ach\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"extrude\","
     "\"args\":{\"distance\":10,\"operation\":\"new_body\",\"profile\":[[20,-5],[30,-5],[30,5],[20,5]]}},{\"tool\""
     ":\"commit_to_plate\",\"args\":{\"mode\":\"bodies\"}}]}],\"ids\":false}", nullptr, nullptr, "Document", false},
    {"new_design", L_CONTEXT("New design", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"New Design: erase every feature, body and variable and the Design undo history.\",\"de"
     "tails\":\"Committed objects stay on the plate. Reply: cleared {features, bodies, variables}.\",\"par"
     "ams\":[{\"name\":\"discard_changes\",\"type\":\"boolean\",\"default\":false,\"description\":\"needed when the "
     "design is not empty\"}],\"examples\":[{\"title\":\"A plate's design erased\",\"calls\":[{\"tool\":\"extrude\""
     ",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"new_design\",\"args\":{\"discard_changes\":tr"
     "ue}}]},{\"title\":\"An empty design started over, which needs no discard_changes\",\"calls\":[{\"tool\":"
     "\"new_design\",\"args\":{}}]}],\"ids\":false}", nullptr, nullptr, "Document", false},
    {"export_step", L_CONTEXT("Export step", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Export STEP: write every body to a file as native B-rep, at its Move-gizmo position."
     "\",\"details\":\"Reply: path, bytes, bodies, moved_bodies.\",\"params\":[{\"name\":\"path\",\"type\":\"string\""
     ",\"description\":\"absolute path; its directory must exist\"},{\"name\":\"overwrite\",\"type\":\"boolean\",\""
     "default\":false,\"description\":\"replace an existing file\"}],\"examples\":[{\"title\":\"A plate written "
     "to a STEP file\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\""
     ":\"export_step\",\"args\":{\"path\":\"/path/to/part.step\"}}],\"needs\":\"a folder to write the STEP file i"
     "n\"}],\"ids\":false}", nullptr, nullptr, "Document", false},
    {"sketch_finish", L_CONTEXT("Sketch finish", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, true,
     "{\"summary\":\"Commit the open sketch as a Sketch feature, as the sketch's Confirm does, and leave "
     "sketch mode.\",\"details\":\"A new sketch left empty is refused and stays open. A sketch re-opened w"
     "ith sketch_edit is replaced in place, in one undo step, with what is built on it rebuilt, and th"
     "e reply adds replaced: true. A replace the rebuild refuses, or a re-opened sketch left empty, en"
     "ds the edit with the design as it was and replies ok false, replaced false, its sketch_index and"
     " error, which says why it was not replaced. Reply: sketch_index, name, features (the new count) "
     "and its loops (closed_loops with the index extrude's loop takes, open_ends, defects, buildable)."
     "\",\"params\":[],\"examples\":[{\"title\":\"A rectangle around a circle, committed as a sketch of two lo"
     "ops\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":"
     "[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\""
     "center\":[0,0],\"radius\":4,\"type\":\"circle_center\"}]}},{\"tool\":\"sketch_finish\",\"args\":{}}]}],\"ids\":"
     "false}", nullptr, nullptr, "Sketch", false},
    {"sketch_cancel", L_CONTEXT("Sketch cancel", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Discard the open sketch and leave sketch mode.\",\"details\":\"A sketch re-opened with s"
     "ketch_edit keeps what it had before the edit.\",\"params\":[],\"examples\":[{\"title\":\"Discard a sketc"
     "h with a rectangle drawn\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketc"
     "h_draw\",\"args\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_cancel\""
     ",\"args\":{}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_edit", L_CONTEXT("Sketch edit", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Re-open a committed sketch to change its entities and constraints, as Edit on its Fe"
     "ature tree row does.\",\"details\":\"The sketch_ commands then act on it; sketch_finish replaces it "
     "in place, in one undo step, and rebuilds what is built on it, and sketch_cancel drops the change"
     "s. An extrude of one loop keeps its own copy of that loop and does not follow the edit. Refused "
     "while a sketch is open, and for text, imported art, a Project sketch and a sketch not drawn from"
     " entities. Reply: sketch, entities and constraints (how many the edit starts with) and plane.\",\""
     "params\":[{\"name\":\"sketch\",\"type\":\"integer\",\"description\":\"a committed Sketch feature's index (de"
     "scribe_design's features[].index)\"}],\"examples\":[{\"title\":\"Re-open a committed rectangle\",\"calls"
     "\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\""
     "p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"sketch_edit"
     "\",\"args\":{\"sketch\":0}}]},{\"title\":\"Re-open the sketch a block is extruded from, which the block "
     "follows\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\""
     "p0\":[-10,-5],\"p1\":[10,5],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\""
     ":\"extrude\",\"args\":{\"distance\":5,\"sketch\":0}},{\"tool\":\"sketch_edit\",\"args\":{\"sketch\":0}}]}],\"ids\""
     ":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_draw", L_CONTEXT("Sketch draw", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Draw a shape in the open sketch from its fields beside `type`, or a list of shapes a"
     "s `entities`.\",\"details\":\"Each type, and the fields it takes: line {p0, p1}; polyline {points: 2"
     " or more, a loop when the last is the first}; rectangle_2_point {p0, p1: opposite corners}; rect"
     "angle_center {center, corner}; rectangle_3_point {p0, p1: one side; width: signed, to the left o"
     "f p0 to p1}; rectangle_rounded {p0, p1: opposite corners; radius: of the corners}; circle_center"
     " {center, radius}; circle_2_point {p0, p1: the ends of a diameter}; circle_3_point {p0, p1, p2: "
     "on the circle}; arc_3_point {p0: start; p1: end; on_arc}; arc_center {center, radius, start_angl"
     "e, end_angle}; arc_tangent {end; entity: the line or arc it leaves, the sketch's or one an item "
     "before it adds, at that one's end, along its direction there, left out the one added last}; slot"
     " {c0, c1, width}; slot_arc {center, start, end: its direction from the centre, width}; ellipse {"
     "center, radius (semi-major), rminor, rotation}; ellipse_arc {the ellipse's fields, start_angle, "
     "end_angle: parametric, counter-clockwise}; spline {ctrl: 2 or more poles}; polygon {center, side"
     "s, radius (to a vertex) or across_flats, angle (of that vertex, or of the first edge's midpoint)"
     "}; point {p}. A type refuses a field it does not take. Each is built as its drawing tool builds "
     "it: a rectangle, a slot or a polygon is one shape, which one undo of the sketch takes off whole,"
     " and a polyline is its lines, as its tool leaves them; the constraints are inferred exactly, so "
     "that none moves what is given. Each shape carries the constraints its tool gives it, at any slan"
     "t; a constraint between a shape and the sketch's axes or what is already in the sketch, or betwe"
     "en plain lines, is inferred only where it holds exactly, so a square on the axes is level only o"
     "n a side lying exactly on one, with right angles holding the others. Each item of entities may c"
     "arry its own construction; the top-level construction is the single-type form's, and beside enti"
     "ties each item's that gives none. Beside entities a shape's field, type among them, is refused: "
     "it belongs in its item. Points are [x, y] in the sketch plane (mm); angles are radians, or degre"
     "es as start_angle_deg and the like. A polygon is lines; its side count is not kept once committe"
     "d. Reply: first_index, added, shapes [{first, count}] per item, entities, dof.\",\"params\":[{\"name"
     "\":\"entities\",\"type\":\"array\",\"default\":[],\"description\":\"shapes, each an object with its type and"
     " fields, drawn in order\"},{\"name\":\"type\",\"type\":\"string\",\"default\":\"\",\"enum\":[\"line\",\"polyline\","
     "\"rectangle_2_point\",\"rectangle_center\",\"rectangle_3_point\",\"rectangle_rounded\",\"circle_center\",\""
     "circle_2_point\",\"circle_3_point\",\"arc_3_point\",\"arc_center\",\"arc_tangent\",\"slot\",\"slot_arc\",\"ell"
     "ipse\",\"ellipse_arc\",\"spline\",\"polygon\",\"point\"],\"description\":\"one shape, its fields beside it\"}"
     ",{\"name\":\"p0\",\"type\":\"array\",\"default\":null,\"description\":\"line: its start; rectangle_2_point, r"
     "ectangle_rounded: a corner; rectangle_3_point: one end of a side; circle_2_point: one end of a d"
     "iameter; circle_3_point: a point on it; arc_3_point: its start. [x, y] in the sketch plane, mm\"}"
     ",{\"name\":\"p1\",\"type\":\"array\",\"default\":null,\"description\":\"line: its end; rectangle_2_point, rec"
     "tangle_rounded: the opposite corner; rectangle_3_point: the side's other end; circle_2_point: th"
     "e diameter's other end; circle_3_point: a second point on it; arc_3_point: its end. [x, y], mm\"}"
     ",{\"name\":\"p2\",\"type\":\"array\",\"default\":null,\"description\":\"circle_3_point: a third point on it, "
     "[x, y], mm\"},{\"name\":\"on_arc\",\"type\":\"array\",\"default\":null,\"description\":\"arc_3_point: a point "
     "on it between its ends, [x, y], mm\"},{\"name\":\"points\",\"type\":\"array\",\"default\":null,\"description"
     "\":\"polyline: its points, 2 or more [x, y], mm; the last on the first closes a loop\"},{\"name\":\"co"
     "rner\",\"type\":\"array\",\"default\":null,\"description\":\"rectangle_center: a corner, [x, y], mm\"},{\"na"
     "me\":\"center\",\"type\":\"array\",\"default\":null,\"description\":\"rectangle_center, circle_center, arc_c"
     "enter, slot_arc, ellipse, ellipse_arc, polygon: the centre, [x, y], mm\"},{\"name\":\"radius\",\"type\""
     ":\"number\",\"unit\":\"mm\",\"default\":null,\"description\":\"rectangle_rounded: of its corners, less than"
     " half its shorter side; circle_center, arc_center: the radius; ellipse, ellipse_arc: the semi-ma"
     "jor axis; polygon: centre to a vertex, in place of across_flats\"},{\"name\":\"start_angle\",\"type\":\""
     "number\",\"unit\":\"rad\",\"default\":null,\"description\":\"arc_center: the start, counter-clockwise from"
     " the plane's x axis, 0 when left out; ellipse_arc: the parametric start, which it requires; or s"
     "tart_angle_deg\"},{\"name\":\"start_angle_deg\",\"type\":\"number\",\"unit\":\"deg\",\"default\":null,\"descript"
     "ion\":\"arc_center, ellipse_arc: start_angle in degrees\"},{\"name\":\"end_angle\",\"type\":\"number\",\"uni"
     "t\":\"rad\",\"default\":null,\"description\":\"arc_center: the end, 0 when left out; ellipse_arc: the pa"
     "rametric end, which it requires; or end_angle_deg\"},{\"name\":\"end_angle_deg\",\"type\":\"number\",\"uni"
     "t\":\"deg\",\"default\":null,\"description\":\"arc_center, ellipse_arc: end_angle in degrees\"},{\"name\":\""
     "end\",\"type\":\"array\",\"default\":null,\"description\":\"arc_tangent: where it ends; slot_arc: a point "
     "in the direction its centreline ends, from the centre. [x, y], mm\"},{\"name\":\"entity\",\"type\":\"int"
     "eger\",\"default\":null,\"min\":0,\"description\":\"arc_tangent: the index of the line or arc it leaves,"
     " one the sketch has or one an item before it adds, at that one's end along its direction there; "
     "left out, the entity added last\"},{\"name\":\"start\",\"type\":\"array\",\"default\":null,\"description\":\"s"
     "lot_arc: where its centreline starts, [x, y], mm\"},{\"name\":\"c0\",\"type\":\"array\",\"default\":null,\"d"
     "escription\":\"slot: one end of its centreline, [x, y], mm\"},{\"name\":\"c1\",\"type\":\"array\",\"default\""
     ":null,\"description\":\"slot: the other end of its centreline, [x, y], mm\"},{\"name\":\"width\",\"type\":"
     "\"number\",\"unit\":\"mm\",\"default\":null,\"description\":\"rectangle_3_point: the distance to the far si"
     "de, signed, to the left of p0 to p1; slot, slot_arc: its whole width\"},{\"name\":\"rminor\",\"type\":\""
     "number\",\"unit\":\"mm\",\"default\":null,\"description\":\"ellipse, ellipse_arc: the semi-minor axis, at "
     "most radius\"},{\"name\":\"rotation\",\"type\":\"number\",\"unit\":\"rad\",\"default\":null,\"description\":\"elli"
     "pse, ellipse_arc: the major axis's angle from the plane's x axis, 0 when left out; or rotation_d"
     "eg\"},{\"name\":\"rotation_deg\",\"type\":\"number\",\"unit\":\"deg\",\"default\":null,\"description\":\"ellipse, "
     "ellipse_arc: rotation in degrees\"},{\"name\":\"ctrl\",\"type\":\"array\",\"default\":null,\"description\":\"s"
     "pline: its poles, 2 or more [x, y], mm\"},{\"name\":\"sides\",\"type\":\"integer\",\"default\":null,\"min\":3"
     ",\"max\":1000,\"description\":\"polygon: its number of sides\"},{\"name\":\"across_flats\",\"type\":\"number\""
     ",\"unit\":\"mm\",\"default\":null,\"description\":\"polygon: the distance between opposite flats, in plac"
     "e of radius\"},{\"name\":\"angle\",\"type\":\"number\",\"unit\":\"rad\",\"default\":null,\"description\":\"polygon"
     ": the angle of the vertex radius reaches, or with across_flats of the first edge's midpoint, 0 w"
     "hen left out; or angle_deg\"},{\"name\":\"angle_deg\",\"type\":\"number\",\"unit\":\"deg\",\"default\":null,\"de"
     "scription\":\"polygon: angle in degrees\"},{\"name\":\"p\",\"type\":\"array\",\"default\":null,\"description\":"
     "\"point: where it is, [x, y], mm\"},{\"name\":\"construction\",\"type\":\"boolean\",\"default\":false,\"descr"
     "iption\":\"true draws construction geometry: the single-type form's shape, or each item of entitie"
     "s that gives no construction of its own\"}],\"examples\":[{\"title\":\"A 30 x 20 mm rectangle\",\"calls\""
     ":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\"p"
     "1\":[15,10],\"type\":\"rectangle_2_point\"}}]},{\"title\":\"A hexagon 10 mm across flats inside a circle"
     "\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entitie"
     "s\":[{\"center\":[0,0],\"radius\":12,\"type\":\"circle_center\"},{\"across_flats\":10,\"center\":[0,0],\"sides"
     "\":6,\"type\":\"polygon\"}]}}]},{\"title\":\"A quarter arc given in degrees, and a slot\",\"calls\":[{\"tool"
     "\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"center\":[0,"
     "0],\"end_angle_deg\":90,\"radius\":20,\"start_angle_deg\":0,\"type\":\"arc_center\"},{\"c0\":[-10,-20],\"c1\":"
     "[10,-20],\"type\":\"slot\",\"width\":6}]}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_select", L_CONTEXT("Sketch select", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Select sketch entities by index; an empty list clears the selection.\",\"details\":\"Rep"
     "ly: selected.\",\"params\":[{\"name\":\"entities\",\"type\":\"array\",\"default\":[],\"description\":\"entity in"
     "dices\"}],\"examples\":[{\"title\":\"Select a rectangle's two long sides\",\"calls\":[{\"tool\":\"sketch_cre"
     "ate\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":\"re"
     "ctangle_2_point\"}},{\"tool\":\"sketch_select\",\"args\":{\"entities\":[0,2]}}]},{\"title\":\"Clear the sele"
     "ction\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0"
     "\":[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"sketch_select\",\"args\":{\"entities\""
     ":[0]}},{\"tool\":\"sketch_select\",\"args\":{\"entities\":[]}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_repair", L_CONTEXT("Sketch repair", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Weld sketch endpoints that nearly meet and constrain them coincident, so loops are c"
     "losed by construction.\",\"details\":\"Reply: welded and the loops.\",\"params\":[{\"name\":\"tolerance\",\""
     "type\":\"number\",\"unit\":\"mm\",\"default\":0.001},{\"name\":\"ignore_construction\",\"type\":\"boolean\",\"defa"
     "ult\":true}],\"examples\":[{\"title\":\"Close a triangle's 0.05 mm gap\",\"calls\":[{\"tool\":\"sketch_creat"
     "e\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"p0\":[0,0],\"p1\":[20,0],\"typ"
     "e\":\"line\"},{\"p0\":[20,0.05],\"p1\":[20,10],\"type\":\"line\"},{\"p0\":[20,10],\"p1\":[0,0],\"type\":\"line\"}]}"
     "},{\"tool\":\"sketch_repair\",\"args\":{\"tolerance\":0.1}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_set_value", L_CONTEXT("Sketch set value", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Set the dimension of the selected sketch entities, as the value field does.\",\"detail"
     "s\":\"A line's length, a circle's diameter, an arc's radius, or the angle or distance between two."
     " Select them with sketch_select first. Reply: kind, before, value, dof and the loops.\",\"params\":"
     "[{\"name\":\"value\",\"type\":\"number\",\"description\":\"mm, or degrees for an angle\"}],\"examples\":[{\"tit"
     "le\":\"A rectangle's bottom side made 40 mm long\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\""
     ":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{"
     "\"tool\":\"sketch_select\",\"args\":{\"entities\":[0]}},{\"tool\":\"sketch_set_value\",\"args\":{\"value\":40}}]"
     "},{\"title\":\"A circle's diameter set to 12 mm\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\""
     "XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":4,\"type\":\"circle_center"
     "\"}]}},{\"tool\":\"sketch_select\",\"args\":{\"entities\":[0]}},{\"tool\":\"sketch_set_value\",\"args\":{\"value"
     "\":12}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_transform", L_CONTEXT("Sketch transform", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Move, rotate or scale entities of the open sketch, as the transform tools' Confirm d"
     "oes.\",\"details\":\"move {dx, dy}, rotate {angle, pivot} and scale {scale, pivot} change the entiti"
     "es and release the constraints that no longer hold. pivot defaults to the entities' centroid. An"
     "gles are radians, or degrees as angle_deg. Reply: type, moved, released_constraints, pivot, enti"
     "ties, dof.\",\"params\":[{\"name\":\"type\",\"type\":\"string\",\"enum\":[\"move\",\"rotate\",\"scale\"]},{\"name\":\""
     "entities\",\"type\":\"array\",\"default\":[],\"description\":\"entity indices; omit for the selection\"},{\""
     "name\":\"dx\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0},{\"name\":\"dy\",\"type\":\"number\",\"unit\":\"mm\",\"de"
     "fault\":0},{\"name\":\"angle\",\"type\":\"number\",\"unit\":\"rad\",\"default\":null,\"nullable\":false,\"descript"
     "ion\":\"rotate's angle, or angle_deg\"},{\"name\":\"angle_deg\",\"type\":\"number\",\"unit\":\"deg\",\"default\":"
     "null,\"nullable\":false,\"description\":\"rotate's angle in degrees, in place of angle\"},{\"name\":\"sca"
     "le\",\"type\":\"number\",\"default\":null,\"nullable\":false},{\"name\":\"pivot\",\"type\":\"array\",\"default\":nu"
     "ll,\"nullable\":false,\"description\":\"[x, y] in the sketch plane, mm\"}],\"examples\":[{\"title\":\"A cir"
     "cle moved 10 mm along x\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch"
     "_draw\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":3,\"type\":\"circle_center\"}]}},{\"tool\":\"sketch"
     "_transform\",\"args\":{\"dx\":10,\"entities\":[0],\"type\":\"move\"}}]},{\"title\":\"A line turned 30 degrees "
     "about its start\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\""
     "args\":{\"entities\":[{\"p0\":[0,0],\"p1\":[20,0],\"type\":\"line\"}]}},{\"tool\":\"sketch_transform\",\"args\":{"
     "\"angle_deg\":30,\"entities\":[0],\"pivot\":[0,0],\"type\":\"rotate\"}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_pattern", L_CONTEXT("Sketch pattern", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Repeat entities of the open sketch in a row or around a point, as the Linear and Pol"
     "ar array tools do.\",\"details\":\"linear adds copies stepped by dx, dy; circular adds copies every "
     "angle/count about pivot (angle is the whole sweep, default 360 degrees). count includes the orig"
     "inal, and each copy is bound to its source (equal length and parallel, or its radius). Angles ar"
     "e radians, or degrees as angle_deg. Reply: type, first_index, added, bound, pivot (circular), en"
     "tities, dof.\",\"params\":[{\"name\":\"type\",\"type\":\"string\",\"enum\":[\"linear\",\"circular\"]},{\"name\":\"en"
     "tities\",\"type\":\"array\",\"default\":[],\"description\":\"entity indices; omit for the selection\"},{\"na"
     "me\":\"count\",\"type\":\"integer\",\"default\":null,\"nullable\":false,\"min\":2,\"max\":1000,\"description\":\"i"
     "nstances, the original among them; required\"},{\"name\":\"dx\",\"type\":\"number\",\"unit\":\"mm\",\"default\""
     ":0,\"description\":\"linear: the step\"},{\"name\":\"dy\",\"type\":\"number\",\"unit\":\"mm\",\"default\":0,\"descr"
     "iption\":\"linear: the step\"},{\"name\":\"angle\",\"type\":\"number\",\"unit\":\"rad\",\"default\":null,\"nullabl"
     "e\":false,\"description\":\"circular: the whole sweep, or angle_deg\"},{\"name\":\"angle_deg\",\"type\":\"nu"
     "mber\",\"unit\":\"deg\",\"default\":null,\"nullable\":false,\"description\":\"circular: the whole sweep in d"
     "egrees, in place of angle\"},{\"name\":\"pivot\",\"type\":\"array\",\"default\":null,\"nullable\":false,\"desc"
     "ription\":\"circular, which requires it: the centre [x, y] in the sketch plane, mm\"}],\"examples\":["
     "{\"title\":\"Six holes on a 40 mm bolt circle\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY"
     "\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"center\":[20,0],\"radius\":3,\"type\":\"circle_center\""
     "}]}},{\"tool\":\"sketch_pattern\",\"args\":{\"angle_deg\":360,\"count\":6,\"entities\":[0],\"pivot\":[0,0],\"ty"
     "pe\":\"circular\"}}]},{\"title\":\"Three holes 10 mm apart along x\",\"calls\":[{\"tool\":\"sketch_create\",\""
     "args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"center\":[0,0],\"radius\":2,\"type"
     "\":\"circle_center\"}]}},{\"tool\":\"sketch_pattern\",\"args\":{\"count\":3,\"dx\":10,\"entities\":[0],\"type\":\""
     "linear\"}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"sketch_delete_constraint", L_CONTEXT("Sketch delete constraint", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Sketch, false,
     "{\"summary\":\"Delete a constraint of the open sketch, as its row's delete button does; the constra"
     "ints after it move down one index.\",\"details\":\"Reply: removed, constraints, dof, solve_ok.\",\"par"
     "ams\":[{\"name\":\"index\",\"type\":\"integer\",\"description\":\"sketch_list_constraints' items[].index\"}],"
     "\"examples\":[{\"title\":\"A rectangle's bottom side freed from horizontal\",\"calls\":[{\"tool\":\"sketch_"
     "create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":"
     "\"rectangle_2_point\"}},{\"tool\":\"sketch_list_constraints\",\"args\":{}},{\"tool\":\"sketch_delete_constr"
     "aint\",\"args\":{\"index\":4}}]}],\"ids\":false}", nullptr, nullptr, "Sketch", false},
    {"list_align_types", L_CONTEXT("Align types", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Read, false,
     "{\"summary\":\"Which align types fit a pair of coordinate systems, and why each other one does not."
     "\",\"details\":\"As the Mate card dims the types it does not offer. Reply: cs_a, cs_b and types, the"
     " five align types in order (fastened, planar, revolute, slider, cylindrical), each {type, fits, "
     "reason}, reason null where it fits.\",\"params\":[{\"name\":\"cs_a\",\"type\":\"integer\",\"description\":\"fe"
     "ature index of the fixed CoordSys, align's cs_a\"},{\"name\":\"cs_b\",\"type\":\"integer\",\"description\":"
     "\"feature index of the CoordSys on the body that moves, align's cs_b\"}],\"examples\":[{\"title\":\"The"
     " types that fit a plate's top-face connector and a block's bottom-face one\",\"calls\":[{\"tool\":\"ex"
     "trude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20}},{\"tool\":\"extrude\",\"args\":{\"distance\":10,\"op"
     "eration\":\"new_body\",\"profile\":[[30,-5],[40,-5],[40,5],[30,5]]}},{\"tool\":\"body_describe\",\"args\":{"
     "\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"construct_coordinate_system\",\"args\":{\"body\":0,\"face\":5,\"ge"
     "neration\":{\"from_call\":2,\"key\":\"generation\"},\"type\":\"face_and_direction\"}},{\"tool\":\"body_describ"
     "e\",\"args\":{\"at_extreme\":\"-z\",\"body\":1}},{\"tool\":\"construct_coordinate_system\",\"args\":{\"body\":1,\""
     "face\":4,\"generation\":{\"from_call\":4,\"key\":\"generation\"},\"type\":\"face_and_direction\"}},{\"tool\":\"l"
     "ist_align_types\",\"args\":{\"cs_a\":4,\"cs_b\":5}}]}],\"ids\":false}", nullptr, nullptr, "Solid", false},
    {"select_geometry", L_CONTEXT("Select geometry", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Select a body, a face, edges, a vertex, a reference plane, a sketch's loop or a feat"
     "ure by id, as a click selects it.\",\"details\":\"One form: body; body with face, edges (the last th"
     "e current edge, as in a Shift+click set) or vertex; plane; sketch with loop; feature; or nothing"
     ", which clears the selection. A body is selected as a click on its row of the Bodies list select"
     "s it, a face, edges or a vertex as a click in the view picks them, a loop as a click on it, whic"
     "h also selects its Sketch's row, and a feature as a click on its row of the Feature list. A plan"
     "e is picked only where a click can pick one: while the Origin row shows the planes, or while Ske"
     "tch waits for its plane (sketch_create {}), where picking it opens the sketch. A loop is numbere"
     "d as sketch_describe {sketch} numbers closed_loops and extrude {sketch, loop} takes it, and is s"
     "elected only while the view shows it: an extrude that takes the whole sketch hides the sketch, a"
     "nd one that takes a loop hides that loop. Whatever was selected goes first, of every kind. The m"
     "enu-item forms then act on the selection: feature_edit, rename_selected, feature_delete and sket"
     "ch_constrain_selected called with no arguments act on the selected row of the Feature list while"
     " nothing is picked in the view, and zoom_to_selection acts on what is picked in the view, so a s"
     "ketch is zoomed to through sketch with loop, not through feature. Refused, changing nothing, for"
     " an id the model does not have, and while a card, a sketch, Constrain mode, a body Move or the T"
     "ext dialog is open, where a click means something else. Reply: selection {kind, id}, plane and f"
     "eature, as describe_design reports selection, picked_plane and feature_row; for a vertex also po"
     "int, as body_describe gives it, and placed_point, where the view draws it (the same unless the b"
     "ody was moved).\",\"params\":[{\"name\":\"body\",\"type\":\"integer\",\"default\":null,\"min\":0,\"description\":"
     "\"body index (describe_design); alone, the whole body\"},{\"name\":\"face\",\"type\":\"integer\",\"default\""
     ":null,\"min\":0,\"description\":\"a face id of body (body_describe)\"},{\"name\":\"edges\",\"type\":\"array\","
     "\"default\":null,\"description\":\"edge ids of body (body_describe), one or more\"},{\"name\":\"vertex\",\""
     "type\":\"integer\",\"default\":null,\"min\":0,\"description\":\"a vertex id of body (body_describe {vertic"
     "es: true})\"},{\"name\":\"plane\",\"type\":\"string\",\"default\":null,\"enum\":[\"XY\",\"XZ\",\"YZ\"],\"description"
     "\":\"a reference plane\"},{\"name\":\"sketch\",\"type\":\"integer\",\"default\":null,\"min\":0,\"description\":\"f"
     "eature index of a committed sketch\"},{\"name\":\"loop\",\"type\":\"integer\",\"default\":null,\"min\":0,\"des"
     "cription\":\"a closed loop of sketch: its index in sketch_describe's closed_loops\"},{\"name\":\"featu"
     "re\",\"type\":\"integer\",\"default\":null,\"min\":0,\"description\":\"feature index (describe_design): its "
     "row of the Feature list\"}],\"examples\":[{\"title\":\"A plate selected whole, as a click on its row o"
     "f the Bodies list selects it\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width"
     "\":20}},{\"tool\":\"select_geometry\",\"args\":{\"body\":0}}]},{\"title\":\"A plate's top face selected, as "
     "a click on it selects it\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distance\":5,\"height\":20,\"width\":20"
     "}},{\"tool\":\"body_describe\",\"args\":{\"at_extreme\":\"+z\",\"body\":0}},{\"tool\":\"select_geometry\",\"args\""
     ":{\"body\":0,\"face\":5,\"generation\":{\"from_call\":1,\"key\":\"generation\"}}}]},{\"title\":\"A sketched squ"
     "are's loop selected, which selects its Sketch's row too\",\"calls\":[{\"tool\":\"sketch_create\",\"args\""
     ":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"p0\":[-10,-10],\"p1\":[10,10],\"type\":\"rectangle_2_p"
     "oint\"}},{\"tool\":\"sketch_finish\",\"args\":{}},{\"tool\":\"select_geometry\",\"args\":{\"loop\":0,\"sketch\":0"
     "}}]}],\"ids\":true}", nullptr, nullptr, "Document", false},
    {"confirm_tool", L_CONTEXT("Confirm tool", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Ui, false,
     "{\"summary\":\"Confirm what is open in the Design tab, as its Confirm button and Enter do.\",\"detail"
     "s\":\"What is open, in the order Confirm takes it: the Text dialog (text), a value field (value), "
     "a body Move (move), a text or art placement (insert), a feature card (card), a sketch (sketch), "
     "Constrain mode (constrain). The Text dialog takes it as its OK does: a new text goes into the de"
     "sign, its placement then open (insert), and an edited one keeps its new words. A card adds or ed"
     "its its feature in one undo step and closes; an add the card refuses, or whose rebuild fails, le"
     "aves no feature and no undo step and keeps the card open with the reason on the status line. A C"
     "onfirm that does not take, such as that add or a value that is no number, replies ok false. It r"
     "uns under what is open, which refuses the commands that change the document. Refused, changing n"
     "othing, when nothing is open, while Sketch waits for its plane (pick one with select_geometry {p"
     "lane}, or call cancel_tool), while the Text dialog's OK is off, with no text to insert, with the"
     " dialog's reason, and while Confirm is off, such as for a card whose preview fails, with the sta"
     "tus line as the reason. Reply: was, what was open; ok, whether it took the Confirm; feature, the"
     " index of the feature added or edited, or null; open, what is still open, or null; status, the s"
     "tatus line.\",\"params\":[],\"examples\":[{\"title\":\"A sketched rectangle committed, as the sketch's C"
     "onfirm commits it\",\"calls\":[{\"tool\":\"sketch_create\",\"args\":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\""
     ",\"args\":{\"p0\":[-15,-10],\"p1\":[15,10],\"type\":\"rectangle_2_point\"}},{\"tool\":\"confirm_tool\",\"args\":"
     "{}}]}],\"ids\":false}", nullptr, nullptr, "Document", false},
    {"cancel_tool", L_CONTEXT("Cancel tool", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Ui, false,
     "{\"summary\":\"Cancel what is open in the Design tab, as its Cancel button does.\",\"details\":\"What i"
     "s open, in the order Cancel takes it: the Text dialog (text), closed as its Cancel closes it, wi"
     "th a new text not added and an edited one as it was, a value field (value), a body Move (move), "
     "put back where it started, a text or art placement (insert), a feature card (card), with nothing"
     " added or edited, Sketch waiting for its plane (plane_pick), a sketch (sketch), discarded withou"
     "t the dialog a person gets, a re-opened one kept as it was, and Constrain mode (constrain). It r"
     "uns under what is open, which refuses the commands that change the document. Refused when nothin"
     "g is open. Reply: was, what was open, and status, the status line.\",\"params\":[],\"examples\":[{\"ti"
     "tle\":\"A sketch holding a line discarded, with no dialog\",\"calls\":[{\"tool\":\"sketch_create\",\"args\""
     ":{\"plane\":\"XY\"}},{\"tool\":\"sketch_draw\",\"args\":{\"entities\":[{\"p0\":[0,0],\"p1\":[10,0],\"type\":\"line\""
     "}]}},{\"tool\":\"cancel_tool\",\"args\":{}}]}],\"ids\":false}", nullptr, nullptr, "Document", false},
    {"body_place", L_CONTEXT("Place body", "Design"), -1, nullptr, nullptr, nullptr, 0x00000000u, 0, 0, false, false, nullptr, nullptr, nullptr, false, CadAccess::Change, false,
     "{\"summary\":\"Place a body where Commit to Plate and Export STEP send it: move it, turn it, lay a "
     "face on the bed, or put it back.\",\"details\":\"The Bodies row's Move and Place on Face, outside th"
     "e history: no undo step, and the project file does not keep it. One form: move and rotate, eithe"
     "r or both, added to where the body stands as the Move / Rotate card adds them, the turn about th"
     "e body's centre; face, which lays that face on the bed as Place on Face does; or reset: true, wh"
     "ich puts the body back where the model has it. The view draws and picks the body where it stands"
     ", and its placement follows it when the bodies are renumbered. Refused, changing nothing, for a "
     "body or face the model does not have, for none or more than one of the forms, and while a card, "
     "a sketch, Constrain mode, a body Move or the Text dialog is open. Reply: ok, body, moved and pla"
     "cement, as describe_design reports them.\",\"params\":[{\"name\":\"body\",\"type\":\"integer\",\"min\":0,\"des"
     "cription\":\"body index (describe_design)\"},{\"name\":\"move\",\"type\":\"array\",\"unit\":\"mm\",\"default\":nu"
     "ll,\"description\":\"[dx, dy, dz] in the world frame\"},{\"name\":\"rotate\",\"type\":\"object\",\"default\":n"
     "ull,\"description\":\"{\\\"axis\\\": \\\"x\\\", \\\"y\\\" or \\\"z\\\", \\\"angle\\\": degrees}: a turn about that worl"
     "d axis through the body's centre\"},{\"name\":\"face\",\"type\":\"integer\",\"default\":null,\"min\":0,\"descr"
     "iption\":\"a face id of body (body_describe), laid on the bed\"},{\"name\":\"reset\",\"type\":\"boolean\",\""
     "default\":false,\"description\":\"true: back where the model has it\"}],\"examples\":[{\"title\":\"A box m"
     "oved 10 mm along x and turned a quarter turn about z\",\"calls\":[{\"tool\":\"extrude\",\"args\":{\"distan"
     "ce\":10,\"height\":20,\"width\":30}},{\"tool\":\"body_place\",\"args\":{\"body\":0,\"move\":[10,0,0],\"rotate\":{"
     "\"angle\":90,\"axis\":\"z\"}}}]},{\"title\":\"A box laid on its side face whose normal is -y\",\"calls\":[{\""
     "tool\":\"extrude\",\"args\":{\"distance\":10,\"height\":20,\"width\":30}},{\"tool\":\"body_describe\",\"args\":{\""
     "at_extreme\":\"-y\",\"body\":0}},{\"tool\":\"body_place\",\"args\":{\"body\":0,\"face\":0,\"generation\":{\"from_c"
     "all\":1,\"key\":\"generation\"}}}]}],\"ids\":true}", nullptr, nullptr, "Bodies", false},
};
static const int kDesignToolCount = 127;

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_DesignOffer_hpp_
