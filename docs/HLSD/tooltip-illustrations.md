# Tooltip illustrations

Three sets of tooltips carry a picture under their text: a still image, or a short animation that
loops while the tooltip is open.

- The texture-displacement panel's settings. Several of them describe a shape rather than a number -
  what Rotation turns, what Tile repeats, what Invert turns inside out - and a sentence about a shape
  is read twice and understood once.
- The tools of the toolbar over the 3D view: the gizmos, and the main and assemble view tools before
  them (Add, Add plate, Orient, Arrange, the instance and split tools, Variable layer height, Assembly
  view). A tool's name says little about what it does until it has been used once, and an icon even
  less; a few seconds of the tool at work says it outright.
- The controls in every other gizmo's panel: Move, Rotate and Scale, Cut, Mesh Boolean, the painting
  tools, Text and SVG, Measure and Assemble, Simplify and Brim Ears. Pictures go only to the controls
  that are a shape, a placement or a behaviour; a number or a plain on/off gets text alone.

Both go through `slic3r/GUI/IllustratedTooltip.hpp`, which keeps the part that has to stay the same
everywhere: when a tooltip appears, and how its pictures are decoded and played. Where it goes and how
wide it is are left to the owner.

## Where a tooltip is drawn

### The texture-displacement panel

`GLGizmoTextureDisplacement::panel_tooltip()` replaces `ImGuiWrapper::tooltip()` throughout the panel.
It differs in two ways.

It places the tooltip beside the panel instead of under the mouse. The panel is pinned to the right
edge of the canvas, so a tooltip following the mouse covers the control it is explaining, and one
carrying a picture covers most of the panel with it. The tooltip goes to the left of
`m_tooltip_anchor_min`/`_max` - the panel's own screen rectangle, or the texture picker's while that
popup is up and the tooltip comes from inside it - and to the right only when there is no room on the
left. This is the placement the picker popup itself already uses.

ImGui normally keeps a tooltip glued to the mouse and ignores `SetNextWindowPos()`. Orca's imgui
branch honours a position set for a tooltip (see the `ImGuiWindowFlags_Tooltip` branch of `Begin()` in
`deps_src/imgui/imgui.cpp`) and still clamps the result inside the canvas, which is what keeps a tall
tooltip near the bottom edge on screen.

It also gives the tooltip a fixed width rather than one derived from its text, because a picture has
to be laid out against something, and a column of tooltips that are all one width is steadier to read.

### The gizmo toolbar

`GLGizmosManager::render_toolbar_tooltip()` draws the tooltip of the toolbar icon under the pointer,
in place of the one-line name `GLCanvas3D`'s own tooltip used to show for it. It goes under the icon,
centred on it and kept inside the canvas, so reading along the toolbar never covers the next icons.
It shows the tool's name and shortcut in bold, a sentence on what the tool is for, and one picture -
three, captioned, for Mesh Boolean's three operations. When the tool cannot be opened yet, the reason
`get_name()` gives for that follows the name, dimmed.

The text and pictures for each tool are a table in `GLGizmosManager.cpp` (`toolbar_tip()`), keyed by
the gizmo type, next to the list of gizmos it describes.

The tools before the gizmos belong to `GLCanvas3D`'s main and assemble view toolbars, and
`GLCanvas3D::_render_toolbar_tooltip()` gives them the same tooltip, from a table keyed by the tool's
name (`toolbar_item_tip()`). The title is the tool's existing tooltip, shortcuts included, so its
wording and translations stay as they were. A tool whose menu is open under it (Arrange, Variable layer
height) keeps the plain tooltip, which would otherwise land on its own menu. From the moment the pointer
reaches a tool with a picture, the plain tooltip that follows the mouse is held back, also during the
wait, so the two never take turns.

### The gizmo panels

`GLGizmoBase::panel_tooltip()` is called right after the control it explains, on every frame, and
checks the hover itself, so a panel needs no `if (IsItemHovered())` around it. It goes through
`IllustratedTooltip::item_tooltip()`, which places the tooltip beside the panel's root window - to the
right, or to the left when there is no room - level with the control. `GizmoObjectManipulation`, which
draws the Move, Rotate and Scale panels but is not a gizmo, calls `item_tooltip()` directly.
`slider_with_tooltip()` covers ImGuiWrapper's sliders, whose last item is the edit button beside the
slider rather than the slider itself.

The hover test allows disabled items, since a control that cannot be used is the one whose tooltip
has to say why. For the same reason the tooltip resets the style alpha that `disabled_begin()` halves,
and it sets its own background colour, because a call from inside a combo or popup would otherwise
inherit that popup's light background under the tooltip's white text.

The painting tools share one table of text and pictures, `GLGizmoPainterBase::painter_tooltip()`, so a
tool that appears in several of the four panels reads the same in all of them.

### The 3D view and Preview

The canvas's own ImGui controls - the view menu, the section view and variable layer height panels,
the Preview legend, layer slider and plate strip, the assembly view controls and the notification
buttons - call `GLCanvas3D::view_tooltip()` right after the control, which goes through one
`IllustratedTooltip` for all of them, since only one tooltip is up at a time. GCodeViewer, IMSlider and
the notifications reach it through the current canvas. A tooltip is a window of its own rather than a
popup, so one on an item of the view menu or of a combo does not close it.

## Where the pictures live

`resources/images/texture_displacement/`, `resources/images/gizmo_toolbar/`,
`resources/images/viewport/` and `resources/images/gizmos/<panel>/`, as PNG, JPG, BMP or GIF; the folder is given when the
`IllustratedTooltip` is made, and the panels name their files with the subfolder (`"cut/connector_plug.png"`). A tooltip names them by file:

```cpp
hover_tip(_u8L("Repeats the texture across the painted area. ..."),
          { { "tile_off.png", _L("Off") }, { "tile_on.png", _L("On") } });
```

Several in one tooltip are laid out side by side at equal widths, which is how a setting is shown as a
comparison - the same model at two values of it. The caption under each is translated; the file name
is not.

A file that is missing or will not decode is skipped, and a tooltip whose pictures are all missing is
exactly the text tooltip it was before. So the art for a tooltip can land after the tooltip does, and
a half-finished set never breaks the panel.

## When a tooltip appears

A tooltip waits for the pointer to rest on its control for 0.45 s. The panel is dense and the tooltip
lands on the model beside it, so with no wait a sweep from the top of the panel down to Bake flashes a
dozen large panels over the scene. Once one has just been up, the wait drops to 0.1 s for 0.7 s
afterwards, so reading several settings in a row stays quick - but not to zero, or that same sweep
would flash again the moment one tooltip had been seen.

No tooltip appears at all while a control is being used - a slider dragged, a value typed into one, a
combo open, a layer card being dragged. The pointer sits on the very thing being edited, so a tooltip
there covers the model exactly while the user is watching it change. The full wait comes back
afterwards rather than the short one: having just let go of a slider is not a reason to be shown a
picture of it.

A pointer resting on a control produces no events, and the canvas only renders when something asks it
to, so the wait has to ask for its own frames, and so does an animation once one is on screen. So does
a tooltip that has just appeared, animated or not: ImGui hides a window on the first frame it is
submitted while it measures it, and does so every time a tooltip reappears because those windows are
recycled, so the frame a tooltip is decided on is never the frame it is seen on. Both use
`GLCanvas3D::request_extra_frame()`, which ends `on_idle()` with `evt.RequestMore()` and so obliges wx
to deliver the next idle. `schedule_extra_frame()` is not enough on its own: it only arranges a timer
whose event an idle event is assumed to follow, and on_idle renders nothing unless the canvas was
already dirty when it ran. Asking for frames as fast as they come is safe here because which frame is
drawn comes from the clock, not from the frame count, so a redraw that arrives early simply draws the
same frame again.

## Decoding and playback

`slic3r/GUI/AnimatedTexture.hpp` decodes a file once and uploads one GL texture per frame. Stills go
through `wxImage`; GIFs go through `wxGIFDecoder`, whose raw palette indices are composited here frame
by frame, replaying the disposal method each frame asks for. That replay is what makes an animation
look continuous: a GIF frame is a sub-rectangle drawn over what came before it, so an animation that
only re-sends the pixels that changed draws as a flicker of disconnected patches without it.

A frame delay of 0 or 1 hundredths of a second is read as 100 ms, the convention browsers settle on,
so an animation authored against that plays at the speed it was authored at.

The animation restarts when its tooltip opens, rather than running off a free-running clock: these are
short loops of one setting going from one value to another, and joining one halfway through shows the
end of a change the user never saw the start of.

Which frame is drawn is measured on a real clock, not `ImGui::GetTime()`. Nothing in OrcaSlicer ever sets
`io.DeltaTime`, so it keeps imgui's default of 1/60 per frame and imgui's clock counts frames rather
than seconds. On a canvas that renders only when asked, that is not a clock at all: an animation driven
by it crawls while the canvas is idle and races while a moving mouse is rendering it flat out.

## Making one

### The toolbar

`scripts/gen_gizmo_tips.py` draws the toolbar's set (`scripts/gizmo_tips/toolbar.py`, the main toolbar's
tools included) and the gizmo
panels' sets (one module each in `scripts/gizmo_tips/`). Each picture is a scene - a model on the
print bed with the tool's handles, the pointer and the brush drawn over it - rendered by a small ray
marcher from signed distance functions, which makes a cut, a boolean operation or an embossed letter a
line of arithmetic each. The view is the plater's default, from the front right and above, so the
pictures look like the view the user is in. The model is grey; the one accent colour marks what the
tool made or acts on: a painted area, a cut face, a selected face, the second part.

The animations either loop with no step (Move, Rotate, Scale, Simplify and the embossing ones swing
from one end of the change to the other and back) or show an action once and hold its result before
starting over (the painting tools, Cut, Lay on Face, Assemble, Brim Ears). Frames that repeat are merged
into one longer frame by the GIF encoder.

### The texture-displacement panel

`scripts/gen_texture_displacement_tips.py` draws the lot, in four languages, picked by what the
setting actually is:

- **A shaded relief**, for the settings that are about a pattern: Depth, Tile, Tile size, Rotation,
  Repeat/Mirror, Smoothing, Edge fade, Invert, Blend and the Normal, Fast, Checker and Distortion
  views. One motif - a ring inside a square frame - runs through all of them, because using one shape
  everywhere is what lets two pictures be compared: whatever differs between them is the setting,
  since nothing else is.
- **A flat mesh of triangles**, for the settings that are about the mesh or about a tool: the paint
  tools, brush size, the connected-area fill, subdivision, feature-adaptive refinement, remeshing,
  hard edges, slivers and the wireframe view. These carry the one accent colour in the set, meaning
  "painted", because that is the only thing in such a picture that is not geometry.
- **A section**, for Midlevel and for smoothing the geometry. The reason generalises: shading reads
  slopes, so a height field moved up or down as a whole shades identically to where it started, and
  any setting whose meaning is *where the surface sits relative to where it was* is invisible from
  above and has to be drawn from the side.
- **A flat ramp of greys**, for Mix filaments, Mixed colors and Denoise. Those are about which
  filament a spot ends up as; lighting one would only add a second thing changing between two
  pictures meant to differ in one.

The five Mapping pictures are one ball with a checker laid on it five ways, which is the whole of what
choosing a projection does - except Unwrap, which is not a projection onto the model at all and so is
drawn as the flat layout it produces.

Captures of the real tool are still the first choice for anything involving a real model or real
filament colour: Bake, the mixed-colour count on a photograph, Adjust placement, the frame window and
the UV editor's seam tool have no picture yet for that reason.

Either way:

- Square, around 256 px (192 for an animation, where every frame ships). The tooltip is about 20 em
  wide, so that is roughly native size on a 100% display and downscales cleanly above it. A single
  picture is capped at 10 em tall and centred, two side by side take half the width each.
- Neutral grey. These say what a shape does; colour in one reads as a third thing the setting does,
  and has to work against both themes.
- Animations loop seamlessly and stay short - 16 frames at 100 ms, under about 150 KB. They sit in
  `resources/`, which ships with every copy of the application. A sweep driven by a cosine over the
  frame count loops with no step, since it starts and ends at the same value and the same rate.
- No text in the picture. Captions are translated; pixels are not.
