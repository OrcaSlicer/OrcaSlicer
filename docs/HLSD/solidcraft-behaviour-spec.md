# Design tab — interaction behaviour spec (from SolidCraft)

Normative rules for the Design tab's input behaviour. Each rule has a stable id (`F-`, `P-`,
`S-`, `D-`, `T-`, `U-`, `R-`, `N-`); work items and rig tests cite them. Origin: a code
read of SolidCraft 0.14 (egui); numbers marked *(egui)* are toolkit defaults rather than
SolidCraft choices. Status: **have** = already true here, **part** = partly, **new** = to build.
Plan: `~/.claude/plans/humble-strolling-shell.md` (P3).

## F — Focus and keyboard
| id | rule | status |
|---|---|---|
| F-01 | Shortcuts are evaluated once per event, before widgets, by one gate. | part (CHAR_HOOK) |
| F-02 | The gate is closed while any text field (wx or ImGui `WantTextInput`), the inline dimension editor, the command box, or a modal owns input. Letters typed into a box never fire tools (`D`, click line, type 40, Enter works). | part |
| F-03 | The viewport takes no keyboard focus of its own; text fields grab it on open and give it back on close. After the action bar hides, focus returns to the canvas. | have |
| F-04 | Esc peels exactly one layer per press: Select-Other list → active tool → open card → selection → (in a sketch) finish sketch. A widget that handles Esc marks it handled for the frame. | part (4-level ladder; add Select-Other layer) |
| F-05 | Reserved keys cannot be rebound: Esc, Delete, Backspace, Ctrl+Z/Y/S/N/O/C/X/V, arrows. Binding a key in use is refused unless confirmed; confirming unbinds the other. | new |
| F-06 | Enter applies a card from a value field, or when nothing is focused. Not for Sketch/Variables/Measure/ConfirmDelete cards. | part |
| F-07 | Command box ("S-box"): opens at the cursor clamped on screen; field focused **once** on open (not every frame — key-repeat bug); Up/Down move highlight, clamped; Enter runs; Esc or outside click closes. | have (Ctrl+K palette, `check-ui-palette.py`) |
| F-08 | S-box scoring: prefix > word-start > substring > letters-in-order; matches id/tab/group at a discount; enabled commands +150; empty query shows 12 recents; pinned commands as an icon row; row context menu; per-row pin. | new |
| F-09 | A sketch-tool key pressed outside a sketch starts a new sketch and arms the tool for after the plane pick. | new |
| F-10 | Dimension editor: Enter commits; focus loss cancels but only after ≥ 6 frames (the opening double-click must not cancel it); Esc cancels and clears the tool picks; a rejected value reopens with the error. | part |
| F-11 | Rename box: opens with all text selected; Enter commits, Esc cancels, outside click commits. | new |
| F-12 | A disabled command starts nothing and states why in the status line (never silent). | new |

## P — Pointer, picking, navigation
| id | rule | status |
|---|---|---|
| P-01 | Hit priority: sketch point (8 px, +2 for the active sketch) > sketch curve (6) > body vertex (7) > body edge (6) > origin/axis (5) > surface by depth (profiles slightly preferred over their face; planes pushed behind). | part |
| P-02 | Edges/vertices are pickable only if depth ≤ nearest face + 2 % of scene radius; off in wireframe. | new |
| P-03 | Only the nearest hit of each kind is kept unless all hits are requested (Select Other). | part |
| P-04 | Selection filter toggles: sketch, sketch curves, sketch points, vertices, edges, faces, bodies. "Bodies" turns a face hit into the whole body. Planes/axes always pickable. | part (faces / edges / vertices switches in the SELECT panel; no sketch or body switches) |
| P-05 | While a card is open only hits its active input accepts count; planar-only inputs accept planar faces only. | part |
| P-06 | Hover is recomputed every frame; suppressed during navigation, box select, outside the view. Sketch curve hover = 3 px redraw; point = 6 px ring; vertex = 4.5 px dot. Tree/list rows highlight the matching body/feature and vice versa. | part |
| P-07 | Click ownership order: active tool > pick-taking card > text/3D-sketch handle > dimension text/constraint glyph > click cycling > plain select. Ctrl/Shift = add/toggle. Click on empty space clears (not with add). | part |
| P-08 | Click cycling: a second click within 4 px with unchanged model/view/filter/card selects the next candidate under the cursor (not on a double-click, not with modifiers). Preference "click cycles: escalate (current) / candidates". | have (SELECT panel: "Click cycles candidates", `check-ui-select-panel.py`) |
| P-09 | Select Other: holding the left button still for 0.5 s opens a list of everything under the cursor, ≤ 20 per kind, order point, curve, vertex, edge, face, body, profile, plane, axis; row hover highlights in the viewport; the press is consumed (no click/drag follows). Also in the context menu ("Select Other…"). | have (hold 0.5 s) |
| P-10 | Double-click: dimension → edit in place; text → edit text; active-sketch curve → select the end-to-end chain; body edge → select the tangent loop (2°). Never with a tool or card open. | part (tangent loop done; dimension edit-in-place existing; text edit not checked) |
| P-11 | Box select by direction: left→right = window (wholly inside, solid stroke), right→left = crossing (touching, dashed); 12 % fill. In a sketch it takes curves, points, dimension texts, glyphs; otherwise per the card's accept mask. Starts only with no nav mode/tool/drag. | part (window / crossing by direction; the solid / dashed stroke and 12 % fill are not drawn) |
| P-12 | Right-click without movement (≤ 3 px) finishes an active tool; otherwise opens the context menu after selecting the hovered candidate if unselected. | part |
| P-13 | Cursor shapes: grab on handles/gizmo, grabbing while dragging, pointing hand on view gizmo; no custom crosshair for sketch tools. | new |
| N-01 | Zoom = exp(−scroll·0.0015) anchored at the model point under the cursor (surface hit, else target-depth plane); pinch likewise. | part |
| N-02 | Orbit pivot options: view centre / origin / point under cursor at drag start (fixed for the drag; empty space uses visible-geometry centre). Shift while orbiting snaps to the 26 standard directions with a damped spring (ω=16, damping 0.82, ≈ 0.33 s). | new |
| N-03 | Axis-gizmo: click a tip = look from that axis, click again reverses; drag orbits; Home button; fit/home animated. | part |
| N-04 | Exactly one place reads camera input; a source-scanning test rejects any other. | new |

## S — Sketch interaction
| id | rule | status |
|---|---|---|
| S-01 | Press-drag-release draws two-point shapes (rectangle, circle, polygon) as well as click-click. | have (every shape with two points, and the Line; `check-ui-sketch-draw.py`) |
| S-02 | Line is a chain: after each segment only the last point survives, tagged `.end` so the next segment joins coincidently; right-click/Esc ends it; a shape that errors pops its last point. | part |
| S-03 | Click point resolution: existing active-sketch point > plane point with refine-snap > plane point rounded to the grid (minor/10) when grid snap is on. | part |
| S-04 | Refine-snap (7 px) order: model vertex (projected) → line midpoint → tangent point (line tool) → on-curve (×0.8) → H/V alignment with previous point → parallel / perpendicular / extension (line tool). Dashed guide + label (`//`, `⊥`, `Ext`, `T`, `Mid`, `On`, `H`, `V`). | part |
| S-05 | Placing a snapped point adds its constraint unless redundant/conflicting. Typing an angle turns inference off. Ctrl or Alt bypass **all** snapping (draw and drag); Shift bypasses only the grid. | part (Ctrl / Alt / Shift bypass exist; an inferred constraint is skipped when redundant) |
| S-06 | Inline dimension boxes while drawing (line L+angle, rectangle W+H(+R), circle Ø, arc R+sweep, polygon R+N, slot L+W). First box focused with text selected; editing locks the value (tinted); Tab / Shift+Tab cycle with wrap; Enter places with the typed values; geometry + constraints = **one** undo step. | part (type-first for line, rectangle, circle; Tab / Shift+Tab go round the fields and wrap; the locked value is not tinted) |
| S-07 | Dimension tool, type-first: picking one curve or two entities opens the value box at once; the text follows the cursor until a click/Enter pins it. A dimension the sketch already implies becomes driven (reference) with a message. | new |
| S-08 | Preselection: if the selection exactly fits a constraint tool, apply at once ("Constraint applied") and clear; ambiguous → pick tool. `P` projects selected projectable geometry in one step. | part (every relation is a verb `con_*` that applies to the selection at once; `P` project not) |
| S-09 | Dragging active-sketch geometry re-solves live each frame (solve only), records **one** undo entry, and on release adds the constraint implied by the snap (point/midpoint/on-curve; alignment adds none). Lost release (focus loss) is cleaned up like a release. | part (live drag, one undo step, constraint on release for point / midpoint / on-curve / origin; no drag guides; `check-ui-drag-snap.py`) |
| S-10 | A tool persists until Esc / right-click / another command / Finish Sketch. A construction toggle is a mode, not a command. Starting a command clears the old tool and dimension editor. | part |
| S-11 | Edit Sketch rolls the timeline to just after the sketch, faces the camera, remembers marker + camera; Finish restores both; editing a second sketch finishes the first; lost attachment opens redefine. | new |
| S-12 | Entering a sketch switches to the sketch ribbon tab; leaving returns and drops the tool. | have |

## D — Cards (feature dialogs)
| id | rule | status |
|---|---|---|
| D-01 | Selected items at open move into the card's ordered inputs and the selection clears. A single input replaces and auto-advances; a multi input toggles; profiles from one sketch only; fillet "chain" expands tangent chains. | part |
| D-02 | ✓ enabled only when inputs produce commands and the preview has no error. While applying: spinner, inputs disabled, Cancel discards the result. Errors stay in the card until an input changes. | part |
| D-03 | Live preview on a scratch copy off the UI thread; one job at a time keyed by hash(commands, revision); last good preview stays while the next computes or an input fails; join/cut/intersect preview the tool body only (boolean runs once on ✓). | part |
| D-04 | Main value has an on-canvas arrow + floating value box that takes focus (select-all) when geometry is set. Drag snaps to 1/2/5×10ⁿ scaled to zoom; Alt/Ctrl = free; tip target 20 px, stem 6 px with offset kept; min length 40 px; hot = accent + ring. | part |
| D-05 | Move triad: X/Y/Z arrows, 3 plane squares, 3 rings, centre ball; constant screen size; distances snap to round steps, angles to 5°. | part |
| D-06 | Edit-in-place: roll to just before the feature, reopen the card pre-filled, restore the marker on ✓/✗. The marker move is view state, not an undo step. | new |

## T — Timeline / tree
| id | rule | status |
|---|---|---|
| T-01 | 24 px rows: icon, name, error/warning mark; suppressed = slashed icon + dim; rolled-back = dim; error row tinted. | part |
| T-02 | Click selects; Ctrl toggles; Shift range over visible order; Shift+Ctrl extends; folders participate. Double-click edits (sketch opens sketch edit). Right-click selects the row first if unselected. | part (on-canvas tree: Ctrl toggles, Shift extends, right-click keeps the selection; no folders; `check-ui-tree-multi.py`) |
| T-03 | Context menu: Edit, Edit Profile Sketch, Rename (F2), Suppress/Unsuppress, Roll Back Here, Copy/Paste, Delete. Tooltip: "Double-click to edit · drag to reorder · right-click for more". | part (Edit, Copy, Paste, Rename, Suppress, Move, Zoom, Delete over the selection; no Roll Back Here / Edit Profile Sketch) |
| T-04 | Rollback bar: 7 px accent line + dot; drag to roll back; dropping below the last row restores full history; new features insert at the bar; commit on release. | have (marker persisted in the recipe; a feature added while rolled back goes in at the bar) |
| T-05 | Drag-reorder drags the whole selection; nearest slot by Y; horizontal offset picks folder level; drop commits one block move; moves that would break a working feature are refused. | part (`move_feature`) |
| T-06 | Folders: group / ungroup / rename inline / move into; Delete on selected folders handled before body delete. | new |
| T-07 | Hovering a browser or timeline row highlights its bodies/feature in the viewport (and the reverse). | part |

## U — Undo
| id | rule | status |
|---|---|---|
| U-01 | One undo step per committed command; failed command leaves the document unchanged. | have (`check-undo.py`) |
| U-02 | Selection, camera and edit-marker moves are never undo steps. | have for selection, the bar and a cancelled card (`check-undo.py`); camera not checked |
| U-03 | A drag is one step (live frames collapsed). Esc during a drag restores the pre-drag checkpoint. | have (a drag is one step, `check-ui-sketch-draw.py`); Esc during a drag not checked |
| U-04 | Compound edits (typed-dimension draw, text dialog live edits + ✓) are one step. | part |
| U-05 | Undo clears the selection; stack capped at 200. | have |

## R — Registry
| id | rule | status |
|---|---|---|
| R-01 | Every behaviour is one registry entry: id, label, tab/group, icon, default key, params doc, `enabled()` returning a reason, `run(json)`, undoable flag. Ribbon, keys, S-box, MCP and scripts all go through it. | part (atlas verbs + `list_commands` / `execute`, constraint verbs, sheet-metal verbs) |
| R-02 | UI entry starts a tool/card; programmatic entry (`execute`) never opens a dialog. | part (`execute` of a tool never opens a dialog; a verb opens its card) |
| R-03 | Custom keymap overrides stored by command id; empty = unbound; export/import. | new |
| R-04 | Ribbon groups collapse by width: fit, then collapse from the last, then a "»" menu. | have (ImGui ribbon) |
