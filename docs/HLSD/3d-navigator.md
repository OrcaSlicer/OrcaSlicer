# 3D navigator

The bottom-left corner of every 3D canvas holds the navigator: a cube standing on a printer bed
that follows the orientation of the camera and turns it, and under it a toolbar of camera buttons
with the Views and Display menus. Beside them stands the section view button (see
[section-view.md](section-view.md)), which is not part of the navigator.

The code is split in two:

- `NavigatorGeometry` (`src/slic3r/GUI/NavigatorGeometry.{hpp,cpp}`) holds what can be tested
  without a GUI: the cube zones, the bed outline, the projection, hit testing and the corner layout.
  Its tests are in `tests/slic3rutils/test_navigator_geometry.cpp`.
- `Navigator3D` (`src/slic3r/GUI/Navigator3D.{hpp,cpp}`) draws into the ImGui frame through the
  toolkit and handles the input. It reads no config: `GLCanvas3D::_render_3d_navigator()`
  (`src/slic3r/GUI/GLCanvas3DNavigator.cpp`) fills a `NavigatorFrame` each frame with the layout,
  whether another drag holds the mouse, the home view, whether the projection switches automatically,
  the shortcuts, the Display menu's toggles in titled groups (`NavigatorToggleGroup`), each with its
  availability, and the actions only the canvas
  can take: fit the view, frame the bed, set the projection, toggle its automatic switching and set
  the home view. The theme and the scale come from the toolkit context.

## The cube and the bed

The cube is [−1, 1]³ cut by 26 half-spaces: its 6 faces, a chamfer on each of the 12 edges
(`|a| + |b| ≤ 1.75`) and a cut on each of the 8 corners (`|x| + |y| + |z| ≤ 2.25`). The corner cut
is deeper than the chamfer, so each corner becomes a hexagon of about the same size as an edge
chamfer, and every face, edge and corner is a target large enough to click. The zone polygons are
built once, by intersecting the planes three at a time.

The bed is a fixed plate with a tab on its front edge, the same for every printer: the shape of the
real bed would make the cube tiny on a long bed and put it off the plate of a delta. It only shows
where the bed and the front of the printer are. The scene's own axes show the origin, so the
navigator shows no axis lines: each edge chamfer is tinted with the colour of the axis it runs
along, and each face carries a badge naming the axis of its normal (`+X` … `−Z`).

The navigator is drawn with a slight perspective, its eye 7 half edges from the centre of the cube,
whatever the projection of the main camera. In a straight orthographic view the cube would collapse
into a rectangle and the bed into a line. Because of the perspective, whether a zone faces the eye
is decided from the zone itself, not from one view direction (`NavProjection::toward_camera()`), and
so is the fading of a face label. Labels and badges are laid in the plane of their face by an affine
map taken at its centre, never turned to the screen, and fade out as the face turns away.

Everything is flat-shaded polygons painted with the toolkit's painter. There is no depth buffer, so the order is
fixed: from above, the bed, the cube's shadow, then the cube; from below, the cube, then the bed.
From below the bed fades with the angle, down to a quarter of its opacity 25° under its plane, and
lets the clicks through, so that the bottom face, edges and corners can be seen and picked.

## Input

Without a background the cube is painted under every panel (`CanvasUi::Painter::background()`), not in
a window, so the scene keeps the clicks around it; with one it is painted in the card it shares
with the toolbar, and takes the clicks over the card outside the toolbar. While the mouse is over
the cube or the bed, or holds a press that started there, the navigator keeps the mouse from the
scene (`CanvasUi::capture_mouse()`), and the canvas routes the events to ImGui.
`Navigator3D::holding()` keeps the canvas capturing the mouse until the release, and the canvas
drops the press when it loses the capture.

- A click on a zone turns the camera to the view of that zone over 250 ms, by `Camera::set_rotation()`
  around the camera target. A face asks for the orthographic projection and an edge or a corner for
  the perspective one, through `Camera::auto_type()`, which only acts when automatic switching is on.
- Dragging the cube turns the camera freely; dragging the bed turns it about the world Z axis only.
- The rotate buttons turn the view by 90° about Z from where the current turn will end, so that
  quick clicks add up.
- A turn stops where it is when something else turns the camera, or when other canvases drew frames
  since its last step, as they do while its own is hidden on another tab. A slow frame of its own
  canvas does not stop it: the turn ends at its target. The frame count comes from the toolkit context,
  which counts the frames of every canvas sharing the ImGui context.

While a turn runs or a drag goes on, the navigator asks the toolkit context for the next frame; its
tooltips wait on the toolkit's shared timer ([canvas-ui.md](canvas-ui.md)). Its toolbar, menus and tooltips are
toolkit components; the cube and the bed are drawn by the navigator itself, with the toolkit's painter —
the card's when the cube shares the card with the toolbar, the background's otherwise — and its mouse is
read through the toolkit too.

## Layout

`navigator_layout()` places the corner from the bottom edge of the scene, which the preview raises
above its bottom band (`GLCanvas3D::preview_corner_offset()`). Without a background the toolbar is a
card of its own, as wide as its row of buttons plus a 4 px padding on each side, with the cube over
it on the scene; with one, the cube and the toolbar share a card (`NavLayout::toolbar`, `NavLayout::card`).
The right edge and the height of the corner (`get_canvas_toolbar_right()`,
`navigator_corner_height()`) keep the preview overlays and the plate lists clear of it. The navigator
cannot be hidden: its toolbar is the only place on the canvas for the display toggles and for fitting
the view, so no menu item or setting hides it.

## Settings

All in `AppConfig`, none in the project; the canvas reads and writes them, and passes them to the
navigator through its frame:

| Key | Meaning |
|---|---|
| `navigator_background` | Put the cube and the toolbar in one card. |
| `navigator_home_view` | The home view as a quaternion; empty means Orca's default view. |
| `use_perspective_camera`, `auto_perspective` | The projection and its automatic switching, shared with the View menu. |
