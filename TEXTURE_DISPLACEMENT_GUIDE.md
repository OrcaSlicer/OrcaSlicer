# Texture Displacement — Feature & Controls Guide

Texture Displacement is a paint-style gizmo that stamps height-map textures onto a model's surface
and turns them into real relief — engraved or embossed detail — either as a live preview or baked
into actual mesh geometry. You paint where the texture applies, stack multiple textures as blended
layers, choose how each is projected onto the surface, and (for the unwrap projection) lay the result
out by hand in a dedicated 2D **UV Editor** pane.

This document describes every feature and control. For the internal architecture and algorithms, see
`TEXTURE_DISPLACEMENT.md`.

---

## Table of contents

1. [Quick start](#quick-start)
2. [Entering the tool](#entering-the-tool)
3. [Selection modes](#selection-modes)
4. [View modes](#view-modes)
5. [Auto update](#auto-update)
6. [Texture layers](#texture-layers)
7. [Per-layer settings](#per-layer-settings)
8. [Projection methods](#projection-methods)
9. [The UV Editor](#the-uv-editor)
10. [Seams](#seams)
11. [Adjust placement (on-model)](#adjust-placement-on-model)
12. [Preparing the mesh: Subdivide & Remesh](#preparing-the-mesh-subdivide--remesh)
13. [Baking & resetting](#baking--resetting)
14. [Controls reference](#controls-reference)
15. [Tips & limitations](#tips--limitations)

---

## Quick start

1. Select an object and open the **Texture displacement** gizmo from the left toolbar.
2. A texture layer is added automatically. Pick a texture from the layer's picker, or import your own.
3. **Paint** the area you want the texture to affect (or press **Select whole model**).
4. The relief appears live on the model. Tune **Depth**, **Tile size**, **Rotation**, etc.
5. If the model is low-poly, use **Subdivide** or **Remesh** so there are enough vertices for detail.
6. Press **Bake** to convert the preview into real geometry, or leave it as a live preview.

> The tool only ever affects the **painted** area. Everything you don't paint keeps its original
> surface, and bake blends the relief seamlessly into it.

---

## Entering the tool

The gizmo lives on the left gizmo toolbar (icon: `toolbar_texture_displacement.svg`). Its settings
panel opens beside the toolbar. You can **Dock panel / Undock panel** (top of the panel) to pin it or
float it freely over the 3D view, and **Close** at the bottom exits the gizmo.

When you first open the tool on a never-textured object it starts with **one texture layer already
added**, so you can paint straight away.

---

## Selection modes

Choose *how* you paint. All three write into the **active layer's** mask.

| Mode | What it does |
|------|--------------|
| **Brush** | Free-hand painting with a round brush. Shows a **Brush size** slider and a **Circle / Sphere** choice (circle = surface disc, sphere = 3D ball that also paints around curves). |
| **Face** | Click a single triangle to paint it. |
| **Connected area** | Click to flood-fill a region; the **Angle threshold** slider limits how far the fill spreads across changes in surface angle. |

- **Select whole model** — marks the entire model as painted for the active layer, instead of
  brushing it by hand.

---

## View modes

A row of icon buttons labelled **View** controls how the painted area is shown. The first four are a
radio group; **Wireframe** is an independent toggle. Hover any icon for its tooltip.

| View | Meaning |
|------|---------|
| **Normal** | The true displaced geometry — exactly what **Bake** produces. Rebuilt in the background. |
| **Fast** | A GPU bump-shaded approximation of the *active layer only*. No real geometry movement — quick to update, not exact. Best while tuning or dragging islands. |
| **Checker** | A test grid painted over the unwrap so you can see stretching (squares stay square where the map isn't distorted). |
| **Distortion** | A blue→green→red heatmap of how much each area is compressed or stretched in UV space. Needs the **Unwrap (LSCM)** projection. |
| **Wireframe** | Overlays the mesh edges (white). Independent of the view above; in **Normal** view it sits on the displaced surface. |

---

## Auto update

**Auto update** (on by default) rebuilds the true displaced geometry as soon as *anything* changes —
painting, swapping textures, moving sliders. Turn it off on very heavy models to only rebuild when you
release a slider (painting still updates on stroke end).

---

## Texture layers

You can stack up to **8** texture layers. Each has its own independent paint mask, its own texture,
and its own parameters, and they combine in slot order like layers in an image editor.

- **Add a layer** — the **＋ icon** to the right of the *Texture layers* heading (reuses the tool icon
  for now).
- **Remove** — the button on each layer's header row.
- **Active layer** — click a layer's header (or anywhere in its block) to make it active. The active
  layer is the one you paint into and the one whose block is tinted. Only one layer is active at a time.
- **Erase all** — clears the active layer's paint.

Each layer shows a texture **picker** (large preview + name). Open it to choose from the shipped
library or import your own image (any png/jpg/bmp; it's converted to an 8-bit grayscale height map and
copied into your user texture folder so app updates can't overwrite it).

---

## Per-layer settings

| Control | Range / options | What it does |
|---------|-----------------|--------------|
| **Depth (mm)** | 0.01–10 (log) | Maximum displacement along the surface normal. |
| **Tile size (mm)** | 0.2–200 (log) | Physical size of one texture tile on the surface. |
| **Rotation** | 0–360° | Rotates the texture on the surface. |
| **Midlevel** | 0–10 | The grey level that means "don't move". At 0 the texture only pushes outward; raise it and darker texels cut *inward* (one map both embosses and engraves). 0.5 makes mid-grey neutral. |
| **Smoothing** | 0–1 | Blurs the height texture before it displaces — rounds hard edges and removes speckle without needing a softer source image. |
| **Edge smoothing** | checkbox + **Edge amount** 0–1 | Fades the relief to flat toward the *edge of the painted area*, so it blends into the surrounding surface. A small amount softens only a thin band at the very edge; the maximum flattens the whole painted face. |
| **Invert** | checkbox | Flips the height map (peaks become valleys). |
| **Blend** | Add / Subtract / Multiply / Divide | How this layer combines with the layers **below** it where they overlap. Add/Subtract pile relief on or carve it away; Multiply/Divide scale the relief underneath (a mask). The lowest painted layer is the **Base** and always behaves additively. |
| **Tile** | checkbox + **Repeat / Mirrored repeat** | When off, the texture is placed once (a decal) instead of repeating. Mirrored repeat flips every other tile to hide seams. |
| **Projection** | see below | How the texture is mapped onto the painted surface. |

> **Midlevel warning:** cutting inward can fold the surface through itself in sharp concave corners or
> thin walls. Keep Depth small relative to the feature you're cutting into; the panel warns when a deep
> inward setting is risky.

---

## Projection methods

How the 2D texture is wrapped onto the 3D painted area.

| Method | Best for | Notes |
|--------|----------|-------|
| **Triplanar (blended)** | Patches wrapping around edges | Projects from all three axes at once and blends, so there's no seam across a sharp edge. |
| **Cylindrical** | Round, tube-like selections | Wraps the texture around the patch's own centre/axis. |
| **Spherical** | Ball-like selections | Longitude/latitude wrap around the patch centre. |
| **Unwrap (LSCM)** | Flat, controlled layout | A real conformal unwrap. Cuts the area into pieces at sharp edges (see **Seam angle**), flattens each, and lets you lay them out by hand in the **UV Editor**. Unlocks Checker/Distortion, seams, and island editing. |
| **From view** | Decals / slide-projector look | Projects straight onto the surface from the current camera direction. Use **Capture current view** to re-lay it from wherever you're looking. |

### LSCM-only controls

These appear when a layer uses **Unwrap (LSCM)**:

- **Seam angle** (5–90°) — edges sharper than this are cut so each piece lies flat. Lower cuts more
  (less stretching, more seams); raise to keep more in one piece. A box's 90° corners are cut by
  default. *Ignored once you've marked any seam by hand* (your seams then define the pieces).
- **Connect islands** (on by default) — lays the unwrap out as a **connected net**: pieces that share
  an edge are unfolded next to each other (a cube becomes a joined net instead of six loose squares).
  They stay separate islands, so you can still move any of them by hand. Turn off for the classic
  packed-grid layout.
- **Open UV editor** — shows the flattened unwrap in a side pane (see below). Opens *only* when you
  turn this on — it never pops up on its own.
- **Mark seams** / **Path** / **Clear seams** — see [Seams](#seams).
- An **Unwrap: N islands, F faces, V verts** read-out tells you what the unwrap actually produced.

---

## The UV Editor

A dockable 2D pane (enable **Open UV editor** on an LSCM layer) showing the flattened unwrap over the
height texture. Islands are the flattened pieces; you can rearrange them freely — nothing re-packs them
behind your back. Moving an island updates the model **live** (in Fast view it tracks the cursor
smoothly, via a shader uniform — no rebuild until you release).

### Navigation

| Action | Control |
|--------|---------|
| Pan | Middle-drag |
| Zoom | Mouse wheel (zooms about the cursor) |
| Frame everything | **Home** or **F**, or the **Frame** toolbar button |

### Editing an island

| Action | Control |
|--------|---------|
| Select | Left-click an island |
| Move | Left-drag |
| Rotate | Right-drag, or press **R** then move the mouse (click/Enter to confirm, Esc to cancel) |
| Rotate snapped | Hold **Shift** while rotating — snaps to **global** 15° marks (0/15/30…). A protractor dial with tick marks and the current angle is shown. |
| Scale | Press **S** then move the mouse (click/Enter to confirm, Esc to cancel) |
| Undo / Redo | **Ctrl+Z** / **Ctrl+Shift+Z** or **Ctrl+Y** |

The **selected** island gets a bold light-green outline and a brighter wireframe; unselected islands
are a translucent light-green wash. The texture underneath repeats exactly as it will when baked.
A **status line** along the bottom always names the current gesture and the shortcuts in play.

### Toolbar

| Button | Action |
|--------|--------|
| **Frame** | Frame all islands (same as Home). |
| **Snap** | Toggle magnetic snapping — a dragged island sticks its boundary to a neighbour's when they come close. |
| **Avg scale** | Give every island the same texel density (Blender's "Average Islands Scale"). |
| **Cut** | Split the selected island across its long axis (useful for very long islands). |
| **Join** | Unfold the selected island onto its nearest neighbour along their shared edge — keeps both as separate islands with their own borders. |
| **Unjoin** | Send the selected island back to its own packed position. |

> **Checker / Distortion in the UV editor:** selecting those View modes also colours the UV pane — a
> checker background, or a per-island distortion heatmap — so you can judge stretch in 2D as well as
> on the model.

---

## Seams

Seams are edges the unwrap is forced to cut along, on top of whatever the Seam angle cuts — the
Blender "mark seam" workflow. They let you control exactly where the unwrap splits.

Enable **Mark seams** on an active LSCM layer, then:

- **Click an edge** on the model to mark it (it turns **red**); click a red edge again to unmark it.
  The edge under the cursor is highlighted **yellow** so you can see what a click will toggle.
- **Path mode** (the **Path** checkbox) — for dense meshes where clicking each edge is tedious: click a
  start point, then an end point, and the whole **shortest path** between them is seamed at once. It
  chains (each click extends from the last point); the start vertex is shown in **green**.
- **Ctrl+drag** rotates/pans the camera while in seam mode.
- **Clear seams** removes them all.

Once any seam is marked, the automatic Seam-angle cutting is disabled so *your* seams define the
islands — pieces you leave un-seamed merge together.

---

## Adjust placement (on-model)

**Adjust placement** (on an active layer) lets you position the texture by dragging a handle on the
model instead of nudging the Rotation/offset numbers. The handle is a flat panel in the patch's
tangent plane (drag anywhere on it to move freely) plus U/V arrows for single-axis nudges. It's
anchored to the painted patch, so paint something first.

---

## Preparing the mesh: Subdivide & Remesh

Displacement can only move vertices that exist, so a coarse model needs more of them first.

### Subdivide

Splits every triangle into four, **1–5 times** (each step roughly quadruples the triangle count).

- **Subdivide steps** (1–5) — how many times to split.
- **Preview subdivision** — shows the result as a **cyan wireframe** without changing the model.
- **Apply** — commits the subdivision to the geometry.
- **Done** — ends the preview and leaves the model as it is.

### Remesh

Rebuilds the whole model with triangles close to a target edge length — evens out a mesh with wildly
varying triangle sizes (CGAL isotropic remeshing).

- **Target edge (mm)** — desired triangle edge length (seeded to the model's current average).
- **Remesh** — splits the big triangles and merges the small ones to that size.

> Both Subdivide-Apply and Remesh **replace the geometry** and clear any *not-yet-baked* paint on it
> (already-baked relief is kept). If you had the mesh **Wireframe** on before, it stays on afterward.

---

## Baking & resetting

- **Bake** — converts the current preview into real, permanent mesh geometry, restricted to the
  painted area. Runs in the background; the button shows *Baking…* while it works.
- **Erase all** — clears the active layer's paint.

Baking is the exact same algorithm as the **Normal** preview, so what you see is what you get.

---

## Controls reference

### Mouse — 3D view (while painting)

| Input | Action |
|-------|--------|
| Left-drag | Paint the active layer |
| Ctrl + drag | Rotate / pan the camera (works in seam mode too) |
| Wheel | Zoom |

### Mouse & keys — UV Editor

| Input | Action |
|-------|--------|
| Left-click | Select island |
| Left-drag | Move island |
| Right-drag | Rotate island |
| **R** / **S** | Modal rotate / scale (mouse drives it, click or Enter confirms, Esc cancels) |
| **Shift** (while rotating) | Snap to global 15° marks |
| Middle-drag | Pan |
| Wheel | Zoom about cursor |
| **Home** / **F** | Frame all islands |
| **Ctrl+Z** / **Ctrl+Shift+Z** / **Ctrl+Y** | Undo / redo |

### Seam mode

| Input | Action |
|-------|--------|
| Click edge | Mark / unmark a seam (yellow = hover, red = marked) |
| Click (Path mode) | Set start, then seam the shortest path to the next click |
| Ctrl + drag | Rotate / pan camera |

---

## Tips & limitations

- **Paint first, then bake.** The preview is free to explore; only Bake changes the real mesh.
- **Not enough detail?** Subdivide or Remesh before painting fine textures.
- **Inward cuts** (high Midlevel + big Depth) can self-intersect on thin walls or sharp concave
  corners — keep Depth modest there.
- **Fast vs Normal:** Fast preview shades a bump and shows only the active layer; use it for quick
  tuning and smooth UV dragging, but trust **Normal**/**Bake** for the exact result.
- **Topology changes drop unbaked paint.** Subdivide-Apply, Remesh, and Simplify replace the mesh, and
  texture-displacement paint isn't remapped across that change (already-baked relief is unaffected).
- **Island placements** are tied to the current unwrap. Re-painting or changing the Seam angle can
  re-segment the charts and renumber them, so a re-unwrap re-lays the connected net and discards
  hand placements made before it.
- **Connect islands** is on by default; turn it off (per layer) for the classic packed-grid layout, or
  if an unfold looks wrong on an unusual mesh.
