#!/usr/bin/env python3
"""Emit the C++ offer table from docs/ux/tool_atlas.json.

    python3 docs/ux/mockups/gen_offer_table.py

The map exists ONCE. The mockups and the shipping menu read the same rows in the same
order from the same file, so a drawing and the product cannot drift apart — which is the
only way row constancy (charter 4.1) survives contact with a codebase.

Output: src/slic3r/GUI/DesignOffer.hpp, checked in and never hand-edited.
"""

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
UX = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(UX))
ATLAS = os.path.join(UX, "tool_atlas.json")
OUT = os.path.join(REPO, "src", "slic3r", "GUI", "DesignOffer.hpp")

# selection id -> C++ enumerator
ENUM = {
    "none": "None", "face_planar": "FacePlanar", "face_cyl": "FaceCyl",
    "face_other": "FaceOther", "edge_str": "EdgeStr", "edge_circ": "EdgeCirc",
    "vertex": "Vertex", "body_solid": "BodySolid", "body_sheet": "BodySheet",
    "bodies_2": "Bodies2", "datum_plane": "DatumPlane", "datum_axis": "DatumAxis",
    "coordsys": "CoordSys", "art": "Art", "sk_loop": "SkLoop", "sk_none": "SkNone",
    "sk_line": "SkLine", "sk_arc": "SkArc", "sk_point": "SkPoint", "sk_2ent": "Sk2Ent",
}


def cstr(s):
    if s is None:
        return "nullptr"
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    A = json.load(open(ATLAS, encoding="utf-8"))
    sels = [s["id"] for s in A["selections"]]
    assert all(s in ENUM for s in sels), [s for s in sels if s not in ENUM]
    slots = [s["id"] for s in A["slots"]]

    lines = [
        "// GENERATED FILE — DO NOT EDIT.",
        "// Source: docs/ux/tool_atlas.json   Generator: docs/ux/mockups/gen_offer_table.py",
        "//",
        "// The object-driven tool offer (charter 4.1): every verb has ONE row index, that index",
        "// is the same in every selection it appears in, and verbs that do not apply are shown",
        "// disabled in place with their reason rather than removed. Row order was ratified",
        "// 2026-07-31; changing an index is a breaking change to every user's muscle memory.",
        "#ifndef slic3r_GUI_DesignOffer_hpp_",
        "#define slic3r_GUI_DesignOffer_hpp_",
        "",
        "#include <cstdint>",
        "",
        "namespace Slic3r { namespace GUI {",
        "",
        "// What the viewport has selected. Ordered as in tool_atlas.json; the bitmask in",
        "// OfferVerb::accepts indexes these.",
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
        "// One row of the offer. `action` routes to the code that already implements the verb:",
        '//   "key:S+E"        -> m_keys_feature[SHIFT(\'E\')]',
        '//   "key:L"          -> m_keys_sketch[\'L\']',
        '//   "fly:material#4" -> row 4 of the "material" feature flyout',
        '//   "btn:delete"     -> a standalone toolbar button',
        "//   nullptr          -> kernel support exists, no GUI path yet (row shows disabled)",
        "struct OfferVerb {",
        "    const char* id;",
        "    const char* name;        // drawing-office word (L10); translated at use with wxGetTranslation",
        "    int         row;         // 0..7, the ratified index — NEVER reorder",
        "    const char* key;         // shortcut shown in the row, or nullptr",
        "    const char* action;",
        "    const char* refusal;     // why this row is greyed, in the product's own words",
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
        "};",
        "",
        "// Row labels, in ratified order.",
        "static const char* const kOfferRowNames[] = {",
    ]
    for s in A["slots"]:
        lines.append(f'    "{s["label"]}",')
    lines += [
        "};",
        f"static const int kOfferRowCount = {len(slots)};",
        "",
        "static const OfferVerb kOfferVerbs[] = {",
    ]
    for v in A["verbs"]:
        mask = 0
        for a in v["accepts"]:
            mask |= 1 << sels.index(a)
        n = v.get("needs") or {}
        lines.append(
            "    {%s, %s, %d, %s, %s, %s, 0x%08xu, %d, %d, %s, %s, %s}," % (
                cstr(v["id"]), cstr(v["name"]), slots.index(v["slot"]),
                cstr(v.get("key")), cstr(v.get("action")), cstr(v.get("refusal")),
                mask, n.get("bodies", 0), n.get("sketches", 0),
                "true" if n.get("sheet") else "false",
                "true" if v.get("mode") == "sketch" else "false",
                cstr(v.get("family"))))
    lines += [
        "};",
        f"static const int kOfferVerbCount = {len(A['verbs'])};",
        "",
        "}} // namespace Slic3r::GUI",
        "",
        "#endif // slic3r_GUI_DesignOffer_hpp_",
        "",
    ]
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    wired = sum(1 for v in A["verbs"] if v.get("action"))
    print(f"wrote {os.path.relpath(OUT, REPO)}: {len(A['verbs'])} verbs, "
          f"{len(slots)} rows, {wired} wired to existing actions")


if __name__ == "__main__":
    main()
