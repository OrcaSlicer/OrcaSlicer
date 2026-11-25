#!/usr/bin/env python3
"""Emit the Design tab's table from scripts/CAD/tool_atlas.json.

    python3 scripts/CAD/gen_offer_table.py            # rewrite the header
    python3 scripts/CAD/gen_offer_table.py --check     # verify the checked-in header

The map exists ONCE: the atlas is the only place a verb's row, key, icon, refusal string and
Design command are written down, so the menu the user sees, the Speed Dial's Design tab actions and
the commands automation calls cannot drift from the table the code is compiled against.

Output: src/slic3r/GUI/CAD/DesignOffer.hpp, checked in and never hand-edited.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))       # scripts/CAD
REPO = os.path.dirname(os.path.dirname(HERE))
ATLAS = os.path.join(HERE, "tool_atlas.json")
OUT = os.path.join(REPO, "src", "slic3r", "GUI", "CAD", "DesignOffer.hpp")

# selection id -> C++ enumerator
ENUM = {
    "none": "None", "face_planar": "FacePlanar", "face_cyl": "FaceCyl",
    "face_other": "FaceOther", "edge_str": "EdgeStr", "edge_circ": "EdgeCirc",
    "vertex": "Vertex", "body_solid": "BodySolid", "body_sheet": "BodySheet",
    "bodies_2": "Bodies2", "datum_plane": "DatumPlane", "datum_axis": "DatumAxis",
    "coordsys": "CoordSys", "art": "Art", "sk_loop": "SkLoop", "sk_none": "SkNone",
    "sk_line": "SkLine", "sk_arc": "SkArc", "sk_point": "SkPoint", "sk_2ent": "Sk2Ent",
}

# a tool block's access -> CadAccess enumerator
ACCESS = {"read": "Read", "change": "Change", "sketch": "Sketch", "ui": "Ui"}

# The headings of tool_list's index of the Design commands (Mcp::design_command_index()), in its order: every
# command's `section` is one of them. Menu items holds the commands that take no arguments and only do what their menu item
# does.
SECTIONS = ("Document", "Sketch", "Solid", "Surface", "Construct", "Inspect", "Bodies", "History", "Menu items")

# The keys of a tool block that its command's description, schema and examples are made from.
DOC_KEYS = ("summary", "details", "params", "examples", "ids")

# A doc is emitted as adjacent string literals of at most this many characters each, so every
# compiler takes it (MSVC refuses one literal over 16 KiB) and a change to it shows on its own line.
DOC_PIECE = 96


def cstr(s):
    if s is None:
        return "nullptr"
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def tstr(s):
    # User-facing text: wrapped in the no-op L() marker so xgettext puts it in the catalogue.
    # The DesignPanel translates it at use; without the marker it compiles and never translates.
    return "nullptr" if s is None else "L(" + cstr(s) + ")"


def tstr_design(s):
    # Verb, row and family names: same msgctxt as the Design panel's own tool labels.
    return "nullptr" if s is None else "L_CONTEXT(" + cstr(s) + ', "Design")'


def doc_text(block):
    """A tool block's summary, details, params, examples and ids as compact JSON text."""
    doc = {k: block[k] for k in DOC_KEYS}
    return json.dumps(doc, ensure_ascii=False, separators=(",", ":"))


def same(a, b):
    """Whether two JSON values are equal, a boolean apart from a number."""
    return json.dumps(a, sort_keys=True) == json.dumps(b, sort_keys=True)


def mode_of(v):
    return "sketch" if v.get("mode") == "sketch" else "model"


def doc_literal(block, indent):
    if block is None:
        return "nullptr"
    text = doc_text(block)
    pieces = [text[i:i + DOC_PIECE] for i in range(0, len(text), DOC_PIECE)]
    return ("\n" + indent).join(cstr(p) for p in pieces)


def arg_faults(vid, args, into):
    """Why `args`, the arguments that choose entry `vid`'s item, are not arguments `into`'s block takes."""
    if not isinstance(args, dict):
        return [f"{vid}: args must be an object of the arguments that choose its item, not {args!r}"]
    params = {p.get("name"): p for p in into["tool"].get("params", [])}
    out = []
    for name, value in args.items():
        p = params.get(name)
        if p is None:
            out.append(f"{vid}: args name {name!r}, which {into['id']} does not take")
        elif "enum" in p and not any(same(value, e) for e in p["enum"]):
            out.append(f"{vid}: args give {name} {value!r}, which is not one of {p['enum']}")
    return out


def faults(A):
    """Every fault of the atlas A, each naming its entry: what the header cannot represent, and what would leave
    a call of a command unable to tell which of its items it opens.

    An unknown slot or selection would surface as a bare ValueError from .index() below, and a
    duplicate verb id would not surface at all: a verb is looked up by id (DesignPanel::mcp_run_verb,
    the catalog's command keys) and the first match taken, so the second item would be unreachable.
    An entry with no menu item, no handler (`action`) and no tool block is nothing; an item with neither is
    greyed (Align to, until it has a GUI path), and no command.

    A folded item (`fold`) is a way of calling the command `into` names with `args`: it keeps its row and
    its handler and has no block of its own, and its command is an entry with a block that is no folded
    item. An entry with a handler and a block may name the `args` that choose its own item. The arguments
    that choose an item are ones its command takes, and choose that item only (Mcp::opened_item()): no two
    items of one command with equal args and different handlers, and no folded item with the args its
    command's own item takes, which a call with none also gives. A command's items are in one mode.

    Every command, an entry with a handler or a block that is no folded item, has a `section`, its heading in
    tool_list's index (SECTIONS); a folded or greyed item, which is no command, has none.

    `feature_row` marks an item whose handler acts on the selected row of the Feature list, which a call of its
    command takes as its target when nothing is picked in the view (Mcp::open_refusal()): only a command with a menu item
    of its own has one, an entry with a handler that is no folded item.
    """
    slot_ids = {s["id"] for s in A["slots"]}
    sel_ids = {s["id"] for s in A["selections"]}
    errs, seen = [], set()
    for v in A["verbs"]:
        vid = v["id"]
        if v["slot"] is None:
            # A command only: no menu item, so no handler the menu could run either.
            if v.get("action"):
                errs.append(f"{vid}: a handler (action) needs a slot")
        elif v["slot"] not in slot_ids:
            errs.append(f'{vid}: unknown slot {v["slot"]!r}')
        for a in v.get("accepts", []):
            if a not in sel_ids:
                errs.append(f"{vid}: accepts unknown selection {a!r}")
        if vid in seen:
            errs.append(f"{vid}: duplicate verb id")
        seen.add(vid)
        block = v.get("tool")
        if v["slot"] is None and not v.get("action") and block is None:
            errs.append(f"{vid}: neither a menu item, a handler (action) nor a tool block")
        if v.get("modal") and not v.get("action"):
            errs.append(f"{vid}: modal without a handler (action)")
        command = (v.get("action") or block is not None) and "fold" not in v
        if not command and "section" in v:
            errs.append(f"{vid}: a folded or greyed item is no command and has no section")
        elif command and "section" not in v:
            errs.append(f"{vid}: a command needs a section, its heading in tool_list's index: one of {list(SECTIONS)}")
        elif command and v["section"] not in SECTIONS:
            errs.append(f'{vid}: section {v["section"]!r} is not one of {list(SECTIONS)}')
        if v.get("feature_row") and (not v.get("action") or "fold" in v):
            errs.append(f"{vid}: feature_row needs a menu item of its own: a handler (action), and no fold")
        # By the key, not its value: an "operations": null names nothing, and is refused as a wrong value.
        if "operations" in v:
            ops = v["operations"]
            if not command:
                errs.append(f"{vid}: operations names what a command reaches, and a folded or greyed item is no command")
            elif not (isinstance(ops, list) and ops and all(isinstance(o, str) and o and "," not in o for o in ops)):
                errs.append(f"{vid}: operations is a list of the names of the operations its command reaches")
        if block is not None:
            if block.get("access") not in ACCESS:
                errs.append(f'{vid}: tool access {block.get("access")!r} is not one of {sorted(ACCESS)}')
            missing = [k for k in DOC_KEYS + ("ok_false",) if k not in block]
            if missing:
                errs.append(f"{vid}: tool block lacks {missing}")
            elif len(doc_text(block).encode("utf-8")) > 60000:
                errs.append(f"{vid}: tool block over 60000 bytes")
            # nullable false keeps a param's type from admitting null, which only a null default gives it.
            for p in block.get("params", []):
                if "nullable" in p and ("default" not in p or p["default"] is not None):
                    errs.append(f'{vid}: param {p.get("name")!r} has nullable but no null default')
        if "args" in v:
            if not v.get("action") or block is None:
                errs.append(f"{vid}: own args need a handler (action) and a tool block")
            elif same(v["args"], {}):
                errs.append(f"{vid}: own args {{}} are what a call with none gives")
            else:
                errs += arg_faults(vid, v["args"], v)

    by_id = {}
    for v in A["verbs"]:
        by_id.setdefault(v["id"], v)
    folded = {}   # a command's id -> the items folded into it so far
    for v in A["verbs"]:
        if "fold" not in v:
            continue
        vid, fold = v["id"], v["fold"]
        if not isinstance(fold, dict):
            errs.append(f"{vid}: fold must be an object {{into, args}}, not {fold!r}")
            continue
        if not v.get("action"):
            errs.append(f"{vid}: a fold needs a handler (action), the item's own")
        if v.get("tool") is not None:
            errs.append(f"{vid}: a folded item has no tool block: its command has it")
        into = by_id.get(fold.get("into"))
        if into is None:
            errs.append(f"{vid}: fold's into {fold.get('into')!r} names no entry")
            continue
        if "fold" in into:
            errs.append(f"{vid}: fold's into {into['id']} is itself a folded item")
            continue
        if into.get("tool") is None:
            errs.append(f"{vid}: fold's into {into['id']} has no tool block")
            continue
        args = fold.get("args")
        bad = arg_faults(vid, args, into)
        errs += bad
        if bad:
            continue
        if into.get("action") and (same(args, {}) or ("args" in into and same(args, into["args"]))):
            errs.append(f"{vid}: its args are the args {into['id']}'s own item takes")
        if into["slot"] is not None and mode_of(v) != mode_of(into):
            errs.append(f"{vid}: in another mode than {into['id']}'s own item")
        for other in folded.get(into["id"], []):
            if mode_of(other) != mode_of(v):
                errs.append(f"{vid}: in another mode than {other['id']}, folded into {into['id']} too")
            if same(other["fold"]["args"], args) and other.get("action") != v.get("action"):
                errs.append(f"{vid}: the same args as {other['id']}, with another handler")
        folded.setdefault(into["id"], []).append(v)
    for s in sel_ids - set(ENUM):
        errs.append(f"selection {s!r} has no OfferSel enumerator")
    return errs


def validate(A):
    """Refuse an atlas the header cannot represent, naming every fault at once (faults())."""
    errs = faults(A)
    if errs:
        sys.exit("tool_atlas.json is inconsistent:\n  " + "\n  ".join(errs))


def main():
    A = json.load(open(ATLAS, encoding="utf-8"))
    validate(A)
    sels = [s["id"] for s in A["selections"]]
    slots = [s["id"] for s in A["slots"]]

    lines = [
        "// GENERATED FILE — DO NOT EDIT.",
        "// Source: scripts/CAD/tool_atlas.json   Generator: scripts/CAD/gen_offer_table.py",
        "//",
        "// The Design tab's table. Its verbs are the object-driven tool offer (charter 4.1): every verb",
        "// has ONE row index, that index is the same in every selection it appears in, and verbs that do",
        "// not apply are shown disabled in place with their reason rather than removed. Row order was",
        "// ratified 2026-07-31; changing an index is a breaking change to every user's muscle memory.",
        "// An entry with a handler or a tool block, and no fold_into, is a command of the catalog's group Design",
        "// (Mcp::register_design_tools(), Mcp::is_design_command()), under its section in tool_list's index. A",
        "// call whose arguments are exactly those that choose one of its items opens that item's tool for a person",
        "// (Mcp::opened_item()): none choose the entry's own item where it has a handler, else the items folded into it",
        "// with {}; a folded item's args, or the entry's own, choose that item. Any other call runs its tool block's",
        "// silent form. A folded item (fold_into) is no command: it is a way of calling the command it names. An item",
        "// with neither a handler nor a tool block is greyed, and no command. An entry with no menu item is a command only.",
        "#ifndef slic3r_GUI_DesignOffer_hpp_",
        "#define slic3r_GUI_DesignOffer_hpp_",
        "",
        "#include <cstdint>",
        "",
        "#ifndef L",
        "#define L(s) s   // gettext marker, as in slic3r/GUI/I18N.hpp",
        "#endif",
        "#ifndef L_CONTEXT",
        "#define L_CONTEXT(s, context) s",
        "#endif",
        "",
        "namespace Slic3r { namespace GUI {",
        "",
        "// What the viewport has selected. Ordered as in tool_atlas.json; the bitmask in",
        "// DesignTool::accepts indexes these.",
        "enum class OfferSel : int {",
    ]
    for i, s in enumerate(sels):
        lines.append(f"    {ENUM[s]} = {i},")
    lines += [
        f"    Count = {len(sels)}",
        "};",
        "",
        "inline uint32_t offer_bit(OfferSel s) { return 1u << int(s); }",
        "",
        "// The atlas's id of each selection, in OfferSel's order.",
        "static const char* const kDesignSelectionIds[] = {",
    ]
    for s in sels:
        lines.append(f"    {cstr(s)},")
    lines += [
        "};",
        "",
        "// What a Design command's silent form does to the document, which decides when the Design tab lets",
        "// it run (DesignPanel::mcp_busy()). Read only looks, so it runs under anything but a rebuild. Sketch",
        "// drives the live sketch session, and Change edits the feature list or variables, or sets what is",
        "// selected in the model; both are refused under an open card, the Text dialog, Constrain mode or a body",
        "// Move, and Change under a sketch session too, which is about to commit into the feature list and in",
        "// which a click means something else. Ui acts on what the tab has open (its Confirm and Cancel), so only a",
        "// rebuild refuses it, though it changes the document. None: an entry with no silent form, and the opening",
        "// of a tool, which only a rebuild refuses, as the Design tab's keys and menu do.",
        "enum class CadAccess { None, Read, Change, Sketch, Ui };",
        "",
        "// One entry of the table. Its handler (`action`) routes to the code that already implements the verb:",
        '//   "key:S+E"        -> m_keys_feature[SHIFT(\'E\')]',
        '//   "key:L"          -> m_keys_sketch[\'L\']',
        '//   "fly:material#4" -> row 4 of the "material" feature flyout',
        '//   "btn:delete"     -> a standalone toolbar button',
        "//   nullptr          -> no GUI path (the item, if it has one, shows disabled)",
        "struct DesignTool {",
        "    const char* id;          // also the key of its Design command",
        "    const char* name;        // drawing-office word (L10); msgctxt \"Design\", translated at use",
        "    int         row;         // index into kOfferRowNames, the ratified address — NEVER reorder;",
        "                             // -1: no menu item, a command only",
        "    const char* key;         // shortcut shown in the item, or nullptr",
        "    const char* action;",
        "    const char* refusal;     // why this item is greyed, in the product's own words",
        "    uint32_t    accepts;     // bitmask over OfferSel",
        "    int         need_bodies;",
        "    int         need_sketches;",
        "    bool        need_sheet;",
        "    bool        sketch_mode; // belongs to the sketch-mode vocabulary, not the model one",
        "    // Second level INSIDE a row, for tools that come in variants: \"Rectangle\" holds corner,",
        "    // centre, oblique and rounded. nullptr = sits directly in the row. Keeps the row's own",
        "    // address fixed (L4.1) while the variants hang one level below it, mirroring the toolbar's",
        "    // grouping instead of flattening 19 create tools into one wall.",
        "    const char* family;",
        "    const char* icon;         // resources/images name, or nullptr — the offer draws it beside the item",
        "    const char* hint;         // what the verb does / what to click; shown on hover",
        "    bool        modal;        // its handler opens a dialog that waits for an answer: a modal one, which holds",
        "                              // the call until it is answered, or Text's, which is modeless and does not",
        "    CadAccess   access;       // its silent form's, None without one",
        "    bool        ok_false;     // its silent form reports a rejected rebuild in its reply (ok false)",
        "    // Its silent form's summary, details, params, examples and ids as JSON text, the parameter",
        "    // schema of record; nullptr for an entry whose command only does what its menu item does.",
        "    const char* doc;",
        "    const char* fold_into;    // the id of the command this item is a variant of, or nullptr",
        "    // The arguments that choose this item, as JSON text: a folded item's, or those that choose the own item of",
        "    // an entry with a handler and a tool block as a call with none does; nullptr for none.",
        "    const char* args;",
        "    // The heading its command is listed under in tool_list's index (kDesignSections,",
        "    // Mcp::design_command_index()); nullptr for an entry that is no command.",
        "    const char* section;",
        "    // Its handler acts on the selected row of the Feature list, which a call of its command, not a person's",
        "    // pick, takes as what the item is for while nothing is picked in the view (Mcp::open_refusal()).",
        "    bool        feature_row;",
        "    // The operations of GUI/Operations its command reaches, comma-separated, which the catalog lists as the",
        "    // command's (Mcp::reaching()); nullptr for none, as a command that changes only the Design document has.",
        "    const char* operations;",
        "};",
        "",
        "// The headings of tool_list's index of the Design commands, in its order. Menu items holds the commands",
        "// that take no arguments and only do what their menu item does.",
        "static const char* const kDesignSections[] = {",
    ]
    for s in SECTIONS:
        lines.append(f"    {cstr(s)},")
    lines += [
        "};",
        "",
        "// Row labels, in ratified order.",
        "static const char* const kOfferRowNames[] = {",
    ]
    # A flat row's label is never shown, so it is not marked for translation.
    for s in A["slots"]:
        lines.append(f'    {cstr(s["label"]) if s.get("flat") else tstr_design(s["label"])},')
    lines += [
        "};",
        f"static const int kOfferRowCount = {len(slots)};",
        "// A flat row is not a family: its verbs come first, at the top level of the offer, each",
        "// an item of its own.",
        "static const bool kOfferRowFlat[] = {",
    ]
    for s in A["slots"]:
        lines.append(f'    {"true" if s.get("flat") else "false"},')
    lines += [
        "};",
        "",
        "static const DesignTool kDesignTools[] = {",
    ]
    # A verb the user can PICK must say what it does. Fail loudly rather than ship a
    # bare name — the offer is now the only door to these tools.
    blind = [v["id"] for v in A["verbs"] if v.get("action") and not v.get("hint")]
    assert not blind, f"wired verbs with no hint: {blind}"
    for v in A["verbs"]:
        # A verb may carry a NOTE: the reason it exists, emitted as a C++ comment above its row.
        # Without somewhere to put it, a rationale written into the generated header is deleted by
        # the next regeneration — which is how the model-mode "Constrain sketch" row came to exist
        # in the header and not in the atlas at all (ziam). The map exists once; so does
        # the explanation.
        for ln in ([v["note"]] if isinstance(v.get("note"), str) else v.get("note") or []):
            lines.append(f"    // {ln}")
        mask = 0
        for a in v.get("accepts", []):
            mask |= 1 << sels.index(a)
        n = v.get("needs") or {}
        block = v.get("tool")
        in_menu = v["slot"] is not None
        # Every name is marked, an entry's with no menu item too: it is its command's title, which a person
        # sees translated in the Design context in the Action Replay window and a recording's caption,
        # and on the Speed Dial action of an entry with a menu item. A table with an unmarked name is
        # refused, written or checked, since nothing at run time can tell (L_CONTEXT compiles to the bare
        # string).
        name = tstr_design(v["name"])
        if not (name.startswith('L_CONTEXT("') and name.endswith('", "Design")')):
            sys.exit(f'entry {v["id"]}: its name is not marked for translation in the Design context: {name}')
        fold = v.get("fold")
        args = fold["args"] if fold else v.get("args")
        lines.append(
            "    {%s, %s, %d, %s, %s, %s, 0x%08xu, %d, %d, %s, %s, %s, %s, %s, %s, CadAccess::%s, %s,\n     %s, %s, %s, %s, %s, %s}," % (
                cstr(v["id"]), name, slots.index(v["slot"]) if in_menu else -1,
                cstr(v.get("key")), cstr(v.get("action")), tstr(v.get("refusal")),
                mask, n.get("bodies", 0), n.get("sketches", 0),
                "true" if n.get("sheet") else "false",
                "true" if v.get("mode") == "sketch" else "false",
                tstr_design(v.get("family")), cstr(v.get("icon")), tstr(v.get("hint")),
                "true" if v.get("modal") else "false",
                ACCESS[block["access"]] if block else "None",
                "true" if block and block["ok_false"] else "false",
                doc_literal(block, "     "), cstr(fold["into"]) if fold else "nullptr",
                "nullptr" if args is None else cstr(json.dumps(args, ensure_ascii=False, separators=(",", ":"))),
                cstr(v.get("section")), "true" if v.get("feature_row") else "false",
                cstr(",".join(v["operations"]) if v.get("operations") else None)))
    lines += [
        "};",
        f"static const int kDesignToolCount = {len(A['verbs'])};",
        "",
        "}} // namespace Slic3r::GUI",
        "",
        "#endif // slic3r_GUI_DesignOffer_hpp_",
        "",
    ]
    text = "\n".join(lines)
    # --check: prove the checked-in header IS what this generator produces, and change nothing.
    # The header calls itself GENERATED; a claim like that is only worth having if something
    # enforces it, so the check_cad_offer_table.yml workflow runs this on every push and pull
    # request that touches the atlas, this generator or the header.
    if "--check" in sys.argv:
        have = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if have == text:
            print(f"{os.path.relpath(OUT, REPO)} matches tool_atlas.json")
            return 0
        import difflib
        d = list(difflib.unified_diff(have.splitlines(), text.splitlines(),
                                      "checked-in", "generated", lineterm="", n=1))
        print(f"{os.path.relpath(OUT, REPO)} DIFFERS from tool_atlas.json:")
        print("\n".join(d[:60]))
        return 1
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    in_menu = [v for v in A["verbs"] if v["slot"] is not None]
    wired = sum(1 for v in A["verbs"] if v.get("action"))
    blocks = sum(1 for v in A["verbs"] if v.get("tool"))
    folded = sum(1 for v in A["verbs"] if v.get("fold"))
    print(f"wrote {os.path.relpath(OUT, REPO)}: {len(A['verbs'])} entries, {len(in_menu)} in "
          f"{len(slots)} rows, {wired} with a handler, {blocks} with a silent form, "
          f"{folded} folded into their commands")
    return 0


if __name__ == "__main__":
    sys.exit(main())
