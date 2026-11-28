# Tooltip illustrations

The texture-displacement panel's tooltips can carry a picture under their text: a still image, or a
short animation that loops while the tooltip is open. Several settings in that panel describe a shape
rather than a number - what Rotation turns, what Tile repeats, what Invert turns inside out - and a
sentence about a shape is read twice and understood once.

## Where a tooltip is drawn

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

## Where the pictures live

`resources/images/texture_displacement/`, as PNG, JPG, BMP or GIF. A tooltip names them by file:

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
