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

The wx windows - the settings tabs, the sidebar, dialogs - have tooltips of the same kind, drawn by
`slic3r/GUI/IllustratedTipPopup.hpp` instead of ImGui; see [The wx tooltips](#the-wx-tooltips). The two
share the picture struct (`IllustratedTooltip::Media` is `TipMedia`), the decoder and the timing, so a
picture looks and plays the same in both.

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
and it pushes the tooltip colours itself (`ImGuiWrapper::push_tooltip_style()`), because a call from
inside a combo or popup would otherwise inherit that popup's light background under the tooltip's text.

The painting tools share one table of text and pictures, `GLGizmoPainterBase::painter_tooltip()`, so a
tool that appears in several of the four panels reads the same in all of them.

## Where the pictures live

`resources/images/texture_displacement/`, `resources/images/gizmo_toolbar/` and
`resources/images/gizmos/<panel>/`, as PNG, JPG, BMP or GIF; the folder is given when the
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

`slic3r/GUI/AnimatedTexture.hpp` decodes a file once (`decode_illustration()`, which the wx tooltips
use too) and uploads one GL texture per frame. The wx tooltips scale the decoded frames to the size they
are drawn at, in physical pixels, and play them on their own timer, rather than through
`wxAnimationCtrl`, which draws at the file's size, plays a native animation on GTK that does not
reliably restart from its first frame, and composites frames differently per platform; the last couple
of dozen decoded pictures are kept. Stills go
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

## The wx tooltips

`set_illustrated_tooltip(window, text, media)` replaces `SetToolTip()` for a wx control, and
`hover_illustrated_tooltip(window, item, text, media)` serves a window that draws several controls
itself and reports the one under the pointer on each mouse move: the option labels and undo icons of
`OG_CustomCtrl`, the cells of the object list, the items of a drop-down list. A tooltip without pictures
is drawn the same way, so the controls of a page all look alike. The `_deferred` variants take a
`TipMediaSource` instead of the pictures, which is asked when the tooltip opens; the settings use it,
since their pictures depend on the value chosen at that moment and looking them up is not free.

They follow the 3D view's rules: the same waits, nothing while a mouse button is down or an editable
text field in the control has the keyboard, and animations from their first frame each time. A click,
a key or the wheel hides the tooltip; it comes back, after the full wait, once the pointer moves again,
as the 3D view's does after a control has been let go. It also goes when the pointer leaves the control,
when the application is deactivated, and when the control is hidden, moved or destroyed. Text wraps at
the width of the 3D view's tooltips (300 DIP, its 20 em) and pictures are at most 150 DIP tall, laid out
side by side at equal widths with their captions under them. Colours are the dark mode palette of the
drop-down lists, in both themes, and the 3D view's tooltips use the same ones
(`ImGuiWrapper::push_tooltip_style()`): every tooltip in the application looks the same, and the
pictures, drawn on dark, sit the same way in all of them.

### Where it goes

Beside the control, never under the mouse and never on the control, and off the controls around it
where there is room, the way the texture-displacement panel's tooltips sit beside the panel:

- For a control inside a popup - an item of a drop-down list - beside the popup, level with the item:
  below the item it would cover the next items, which are what the user is choosing between.
- For a control in a column of controls - the sidebar with the settings pages, a side panel - beside
  the column, level with the control, so it covers the 3D view rather than the next rows of the page.
  The column is found by walking up from the control: the outermost window below the top-level one that
  is at least as tall as it is wide and at most three fifths of the top-level window's width.
- In a dialog, beside the dialog. Not on Wayland, where a tooltip is kept inside its window.
- On a page that spans its window - a settings page in its own window, on Wayland or when the window
  fills the screen - to the right of the page's column of controls, level with the control. The page is
  the first window above the control that is at least three fifths of the top-level window's width; the
  column ends at the right edge of the narrower windows in it, so a G-code editor that spans the page
  does not push the tooltip off the window. Never to the left of the column, which is over its labels.
- On Wayland, in a dialog with no room for it inside: beside the dialog after all. The screen is unknown
  there, so the popup leaves its position to the compositor with `gdk_window_move_to_rect()`: to the
  right of the dialog, level with the control, flipped to the left when the right has no room.
- Otherwise - a toolbar, a page as wide as the window - or when there is no room on either side: below
  the control, level with its left edge, and above it when there is no room below.

It is kept inside the work area of the control's display; on Wayland, where screen coordinates are
relative to the window and the displays are unknown, inside the window the control is in.

There is one popup window for all of them, a plain `wxPopupWindow` that draws everything itself and has
no children. Not a `wxPopupTransientWindow` (Orca's `PopupWindow`): that kind grabs the pointer on GTK,
captures it on macOS and dismisses itself on its own terms, and a tooltip must do none of that. It never
takes the focus or the activation. macOS orders it in without activating it; on GTK it is a popup
window, override-redirect on X11 and an `xdg_popup` without a grab of its own on Wayland. On Windows its
`Show()` uses `SW_SHOWNOACTIVATE` and the window has `WS_EX_NOACTIVATE`, because `wxPopupWindow::Show()`
would activate it, taking the activation from an open drop-down list, which closes when it loses it.
Its parent is the top-level window of the control, and it is recreated when the pointer moves to a
control in another window: that makes it the owned window on Windows and a popup of the right surface on
Wayland, where it is placed relative to that surface.

On Wayland a tooltip over an open drop-down list is a popup of the list, and GDK gives it the list's
grab, so the compositor hands it the keyboard while it is up and the main window reports a
deactivation. `DropDown::ShouldDismissOnTopWindowDeactivate()` ignores that deactivation while the
tooltip is shown, as `PopupWindow` already does for GTK's own tooltips, and the drop-down list hides the
tooltip before it hides itself, since GDK refuses to unmap the parent of a mapped popup.

### Following the pointer

The pointer is followed by binding the enter, motion and leave events of the window and of every window
inside it, not by a timer asking `wxFindWindowAtPoint()`, which needs the global pointer position that
Wayland does not give. The windows inside matter because Orca's composite widgets (`TextInput`,
`::ComboBox`, `SpinInput`) are a frame with a native text field in it, and the pointer is over the
field. Moving between the frame and the field produces a leave and an enter; a leave whose position is
still inside the control is ignored, and any other waits 40 ms for the pointer to come back. The
handlers are free functions that look the window up when an event arrives, and each window is bound
once however often its tooltip is set, so a window that dies while its tooltip waits or shows leaves
nothing dangling. A click, a key, the wheel and the application's deactivation are caught by an event
filter, which sees them before any control does.

A window with an illustrated tooltip shows no native one besides it. Some widgets set one of their own
later: `::ComboBox` and `PresetComboBox` show the selected value, and `TextInput` forwards its tooltip to
its text field. Some code adds to a setting's tooltip the plain way: the settings tabs and the object
table prepend why an option is disabled. So native tooltips are removed from the window and the windows
inside it whenever the pointer moves over it, and a native tooltip on the window itself that still
contains the text it was given is taken as the new text, while any other is dropped.

### Settings

The text of a print, filament or printer setting's tooltip is its definition in `PrintConfig.cpp`, as
`Field::get_tooltip_text()` formats it: the wiki link, the default value, the restrictions. The pictures
are kept apart, in `slic3r/GUI/SettingTips*.cpp`, one file per group of settings so the groups are
edited independently, and `setting_tip(key)` builds the table and looks a setting up in it. A setting
can have pictures shown whatever its value (`media`), and an enum one picture per value (`values`,
keyed by the value's name as stored). A filament's override of a printer setting (`filament_z_hop_types`
overrides `z_hop_types`, as listed by `is_filament_extruder_override_key()`) shows the pictures of the
setting it overrides unless it has its own, so the two tabs show the same ones. The table holds
translated captions, so it is rebuilt on each lookup; it is looked up when a tooltip opens, never on a
paint or a mouse move.

Every field of a settings page goes through `Field::set_tooltip()`, and the label beside it shows the
same pictures with the label's own text, whether the label is a `wxStaticText` or drawn by
`OG_CustomCtrl`. For an enum the tooltip shows the setting's pictures and then the picture of the value
chosen at that moment, captioned with the value's label unless it has a caption of its own, and it is
redrawn if the value changes while it is up.

### Values in the drop-down list

While a setting's drop-down list is open, the item under the pointer shows its value's picture beside
the list, once the pointer has rested on it. `DropDown` reports the item under the pointer to a handler
the field sets (`DropDown::SetItemHoverHandler()`), which passes it on as a `hover_illustrated_tooltip`
with the item's row; a value without a picture shows nothing. An item is matched to its value by its
text, not its index, since some lists leave values out or are filled elsewhere.

## Making one

### The toolbar

`scripts/gen_gizmo_tips.py` draws the toolbar's set (`scripts/gizmo_tips/toolbar.py`, the main toolbar's
tools included) and the gizmo
panels' sets (one module each in `scripts/gizmo_tips/`, except that `gizmos_anim.py` draws the loops of
several panels and names each by its panel's folder). Each picture is a scene - a model on the
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
