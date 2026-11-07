---
name: orca-cad-checking
description: Use after building or changing a part in Orca-CAD over MCP, to verify it against hand-computed numbers, the oracle corpus, and the socket's own safety checks. Triggers - "verify the part", "check the model", "run the oracle", "is the MCP socket robust".
---

# Orca-CAD checking

Orca-CAD reports a feature as OK when OCCT built a shape, not when the shape is the one you meant. Check numbers.

## Per part
1. Before building, write the expected volume, bbox and centre of mass by hand (analytic where the shape allows).
2. Build with one `batch`; then `inspect_design`. Compare volume (rel 1e-3), bbox and COM (1e-3 of the bbox diagonal). The frame is shifted by `modeling_origin`: subtract it from bbox/COM before comparing with recipe coordinates.
3. A removal that misses its target is refused (`removed no material`); a cut that splits the body now yields separate bodies - check `bodies`.
4. Sketches: `sketch_describe` must show DoF 0 and `solved:true` before committing; a profile left at DoF > 0 can solve to another branch after an edit.
5. Look at it: `screenshot {path:/tmp/x.png, view:"iso", fit:true}` (X11 only; the Design tab is brought forward).

## Oracle corpus (82 cases, vendored under `tests/cad_oracle/`, MIT, from SolidCraft)
`python3 scripts/CAD/oracle_runner.py --sock <socket> [--filter=NN-] [--report r.json]` replays each recipe over MCP and compares to `expected.json`. Statuses: PASS, FAIL (exit 1), XFAIL (known OCCT/solver limit, reason in `XFAIL`), XPASS (remove from XFAIL), BLOCKED (the MCP surface cannot express it yet; the reason is the burn-down list). Never turn a FAIL into BLOCKED to make it green.

## Socket safety
`python3 scripts/CAD/check-mcp-hostile.py <socket>` feeds every tool junk arguments, refusals, paths outside scope, three concurrent clients, malformed and 100k-deep JSON; exit 0 means the app survived and refused with an owner. Run it on a scratch app after touching `McpControl.cpp`.

## GUI behaviour
`ui.inspect` reports who has keyboard focus and what ImGui wants; `ui.click/drag/key/text` drive the real event path; use them for focus-after-click and shortcut tests, and `ui.worldToScreen` to aim at geometry. After a non-canvas click, `ui.text` must still reach an ImGui text field.
