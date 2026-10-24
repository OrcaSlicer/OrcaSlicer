---
name: orca-cad-ai-assisted-design
description: Use when an agent designs or edits a parametric part in the Orca-CAD Design tab over its MCP socket, with every dimension bound to a variable (set_variable + set_feature_expr). Companion to orca-cad-modeling, which lists the verbs; this is the parametric workflow and its traps.
---

# Designing a parametric part over MCP

Verbs and parameters: `skills/orca-cad-modeling/SKILL.md` and the app's own `describe_tools` (the authority). This is the method.

## The rule
A part is parametric or it is not finished: every dimension is a variable (`set_variable`), and every sketch dimension and feature field is bound to it with `set_feature_expr` (the fx of the card fields). No fixed numbers in geometry.

## Reaching the app
- The socket is on by default when the Design tab is enabled: `/tmp/orca-cad-mcp.sock`, mode 0600, never taken from a process that still answers on it. `ORCA_CAD_MCP=<path>` moves it, `ORCA_CAD_MCP=0` (or `off`) disables it. `ORCA_CAD_MCP_TOKEN`: every request carries `token`. `ORCA_CAD_MCP_ALLOW`: roots for `save`/`export`/`open` (default `$HOME`, `/tmp`).
- JSON-RPC 2.0, one request per line; `tools/orca_cad_mcp_bridge.py` serves the same tools to an MCP client. Do not restart the user's running app unasked.

## Loop
1. Understand: `describe_scene` (tree, `modeling_origin`, `generation`), `inspect_design` (bodies, variables, volume, `error_feature`).
2. Measure: `query_topology`, `measure`, `slice_body`, `mass_properties`.
3. Build: each call returns the state after it (`ok`, `error`, created indexes).
4. Validate: `inspect_design` (`is_solid`, volume, bbox, `error_feature == -1`), then a sensitivity test: change three or four variables with `set_parameter` (it reports how many features rebuilt), check the solid stays valid and the dimensions follow, put them back.
5. Deliver: `save` (.3mf, the recipe inside), `export` (step | iges | stl | obj), absolute paths under the allowed roots.

## Traps
- **Indexes**: sketches and features each take one, in creation order (`sketch_index`, `extrude_index`, `pattern_index`...).
- **`extrude` without `sketch`** ignores the sketch you committed and builds a default 20 x 20 one. Pass `sketch: <index>`.
- **Ids go stale** on every rebuild. A call that takes a face or edge id is refused with `-32010 stale face/edge ids` when the model changed since this connection last read `query_topology` or `describe_scene`: re-read, then choose by geometry (normal, centroid, radius, length), not by number. A client that never read is not checked; passing `generation` explicitly checks against that value.
- **Coordinates** are relative to `modeling_origin` (read it; it changes between sessions). `sketch_begin {face, body}` answers `plane: "face:<id>"` with `origin`, `normal`, `x_axis` of the sketch plane.
- **Constraints**: `sketch_add` infers joints and H/V; `sketch_constrain` with `atomic:false` keeps the consistent ones and lists each rejected one as `{index, kind, error, conflicts}`. `kind` is `redundant_or_inconsistent`, `did_not_converge`, `too_many_unknowns` or `malformed`; `conflicts` names the sketch's existing constraints and this request's in the same notation you sent (`symmetric_y a=0.start b=0.end`). Aim for `dof == 0`. `symmetric` = points a, b about the LINE c; `symmetric_x` / `symmetric_y` = mirror images across the sketch X / Y axis; no symmetry about a point. To centre on an axis use `vertical_points` / `horizontal_points` between `origin` and the centre.
- **Binding**: `sketch_dimensions {sketch}` lists the dimensions; the field is `dim:<index>` (indexes are not contiguous). `set_feature_expr {feature, field, expr}`.
- **Bindable fields** are an allow-list: `docs/CAD/mcp_bindable_fields.md` (generated from the app) and the `set_feature_expr` description. Names to mind: Fillet and Chamfer share `dressup_size`; a pattern uses `pattern_spacing`, `pattern_count` and, for the second grid direction, `pattern_spacing2`, `pattern_count2`; Extrude uses `distance`, `distance2`. Anything else answers `unknown parameter: <field>` and leaves the document unchanged.
- **Patterns**: `pattern {features:[idx], axis, count, spacing, axis2, count2, spacing2}` (axes in world coordinates, `origin` in model coordinates, at most 64 features, `count2` 1..500); `mirror:true` with `axis`, `origin` mirrors the features instead (needs `features`).
- **Extrude** `end`: blind | symmetric | two_sided (`distance2`) | through_all | up_to_face (`up_to_face: <id>`); `boolean`: new | union | subtract | intersect. A cut that misses the body is refused.
- **`batch`** (up to 10,000 calls, `rollback:true`) undoes features and variables on failure but not an open sketch session: `sketch_commit` inside the batch.

## Hand-off
Say how many features and variables the part has, where the .3mf and .step are, and what was not checked on screen.
