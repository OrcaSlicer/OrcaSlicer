# IDEAS — surface stitch / patch / extend, joint motion (Orca-CAD MCP)
mode: lite | cycle: 1/3 | lenses used: tailoring (seam), welding, sheet-metal fold | provider diversity: no (main context only, no blind workers: every candidate is a known CAD technique) | human seed: none (user said "proceed autonomously")

Goal: stitch, patch and extend surfaces, and drive joints, over MCP, editable, with no new GUI card per feature.
Hard: stays in the feature history (editable, undoable, saved in the 3MF); no new serialized field unless needed.
Baseline: a new CadFeatureType per op + a DesignPanel card each (~25 touch points, see SurfaceOffset).
Function: join two edge-adjacent sheets into one watertight-or-open shell; grow a sheet past its free edge; fill a hole bounded by edges; vary a mate parameter and observe.

## Claim
C1 (lead): every gap is an existing feature composition plus ONE missing primitive — sewing inside the Boolean feature when both operands are sheets.
C2: stitch needs its own feature type (Boolean semantics for sheets differ too much).
C3: stitch belongs inside the consumers (thicken, offset) as an implicit sew, no user-visible op.

## Arms
A1 stitch = `boolean` Add on two sheet bodies → BRepBuilderAPI_Sewing (Boolean already has a card, an MCP tool, persistence, remap).
A2 extend = project the free edge to a plane normal to the extension direction + `surface_extrude` + stitch (straight edges, linear extension; curved-surface extension stays out).
A3 patch = project the boundary edges + `surface_fill` + stitch.
A4 joint motion = mate kinds already include Revolute/Slider; mate_angle/mate_offset are bindable fields; `sweep_variable` already steps a variable and reports.

## Candidates   id | line | provenance | gate | novelty | utility
K1 | Boolean-on-sheets sews | known (Fusion/Onshape "stitch") | pass | n/a | high, ~10 lines
K2 | extend macro = project + surface_extrude + stitch | new-combination of existing features | pass (straight edge) | not searched | medium
K3 | patch macro = project + surface_fill + stitch | new-combination | pass | not searched | medium
K4 | motion = mate + bound variable + sweep_variable (no code) | known, already present | pass | n/a | high, 0 lines

## Predictions
P1: BRepAlgoAPI_Fuse on two edge-adjacent sheets returns >1 shell or leaves the shared edge unsewn (so today's Boolean cannot stitch) | check: rig, two surface_extrude sheets, boolean union, count shells/faces/free edges | separates: C1 vs C3 | result: REFUTED. Exactly adjacent: 2 faces / 7 edges (4+4-1, shared edge merged). With a 0.004 mm gap: tolerance 0 -> 8 edges (not stitched), tolerance 0.01 -> 7 edges (stitched). Boolean union + tolerance IS the stitch; C1 'one missing primitive' was wrong, nothing is missing
P2: with a Sewing branch for sheet+sheet the same call yields exactly one shell with the shared edge merged | check: same rig script after the patch | separates: C1 vs C2 | result: VOID (P1 refuted, no Sewing branch written)
P3: a revolute mate with mate_angle bound to a variable moves body B when sweep_variable steps the variable | check: rig script, bbox of B per step | separates: C1 vs "motion needs new code" | result: CONFIRMED. ang 0 -> B x[170,180]; ang 90 -> B x[130,140] y[210,220]; sweep_variable ran 0/45/90 with per-step size and interference counts. Motion exists; only the recipe was undocumented
P4: extend (K2) gives a sheet whose area grew by edge_length x distance and which is one shell | check: rig | result: CONFIRMED. 10 mm sheet, edge at z=10, distance 5 -> bbox z 0..15, direction (0,0,1) computed from the face, 1 body, 2 faces / 7 edges (shared edge merged). K3 patch also confirmed: open 4-face tube + 4 rim edges -> 5 faces, 12 edges, 1 body; 3 of the 4 rim edges -> refused ('do not chain into one loop'). Limitation seen: a round rim becomes 48 chords (within the stitch tolerance), not a true circle.

## Refuted → constraints
R1 (P1): do not add a Sewing branch or a SurfaceStitch type; stitch = Boolean union with tolerance. New constraint: the MCP offers a `stitch` macro over boolean (default tolerance 0.05), nothing more.

Surprise count: 1 (P1). Frame holds for extend/patch (composition), not for stitch (already present) — the claim shrinks to 'the gaps are discoverability, plus extend and patch compositions'.

Outcome: shipped as MCP macros stitch / surface_extend / surface_patch over Boolean + Sketch3D + SurfaceFill + SurfaceExtrude (one kernel line: SurfaceFill accepts a Sketch3D boundary). No new feature type, no GUI card. Joint motion needs no code: mate (kind 2/3) + set_feature_expr mate_angle|mate_offset + sweep_variable. Not covered: extend along a curved edge or over a curved face (needs a sweep / GeomLib extension); a true circular patch rim.
