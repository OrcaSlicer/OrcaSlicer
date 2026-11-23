# CanvasUi

The panels drawn over the 3D scene — the navigator, the G-code panel, the legend, the print summary and the
statistics of all plates — are built from one toolkit, `CanvasUi`
(`src/slic3r/GUI/CanvasUi/`), on top of ImGui. It exists so that every panel gets the same surfaces, controls and popups from the design
rules instead of drawing its own: a card's shadow, outline and rounded corners, a menu row, a tooltip
delay or an icon are written once.

The toolkit is ImGui-only. It does not cover wxWidgets windows and dialogs.

## Theme roles

A component asks for a colour by its role, never for the colour itself: `card`, `card_border`, `foreground`,
`muted_foreground`, `inactive_foreground`, `disabled_foreground`, `border`, `input`, `secondary*`, `accent` (the
surface of a hovered or highlighted item), `primary` (the brand colour), `primary_strong`, `primary_foreground`,
`selection`, the tooltip's `tooltip*`, `toggle_pressed*`, `switch_thumb*`, `scrollbar_thumb*`, `tabs_list`,
`tab_hover`, `shadow_below`, `current_line`, `position_indicator*`, `contrast_outline`, `shadow`, and the alerts'
`warning`, `warning_surface` and `info`.

Some roles stand where another would do but for contrast:
- A default button is filled with `primary_strong`, not `primary`: `primary_foreground` on the brand colour
  itself falls under 4.5:1.
- A ghost button's text is `primary_strong`, not `foreground`: the reset of a panel's settings reads as
  an action in the brand colour.
- A toggle or a tab not selected is `inactive_foreground`: on the tracks of this design `muted_foreground`
  falls below 4.5:1.
- Secondary text on the tint of a hovered or selected item takes `foreground` (`muted_text()`), as
  `muted_foreground` falls below 4.5:1 there.

## Scales

Every size of the toolkit and its panels comes from a scale in `CanvasUi/Tokens.hpp`, in logical pixels.
Each scale is a type that only the scale makes, and `Metrics::px`/`whole_px` take only those types, so
a size off the scale does not compile. `Metrics` and the `Sizes` of each panel name what the steps are for.

| Scale | Steps |
|---|---|
| Spacing, `space<N>()` | any even N, and 1 |
| Radius, `Radius::sm/md/lg/xl` | base − 4, base − 2, base, base + 4; `RADIUS_BASE` = 8 |
| Stroke, `Stroke::sm/md` | 1, 1.5 |
| Icon, `icon_size<N>()` | 12, 14, 16, 18 |
| Font, `font_size<N>()` | 9, 11, 12, 13, 14, 15 (`INTERFACE_FONT_SIZE`) |
| Text role, `TextRole::caption/label/body/title/code` | 11/14, 13/18, 15/20, 15/20, 14/18 (size/line) |
| Elevation, `Elevation::card/overlay` | shadow spread 4 and 6, opacity ×1 and ×1.25 |
| Duration, `duration::tooltip/turn` | 500 ms, 250 ms |

A component height is the line of its text role plus padding on the spacing scale: a menu row and a
field are a body line and 8, a control a body line and 4, a row of code a code line. The
monospace font is loaded at `MONO_FONT_SIZES` (11, 12, 14). Colours stay with the theme roles.

## Terms

- **Component**: a basic control — button, toggle group, tabs, switch, field.
- **Panel**: what the layout places over the scene, such as the legend, the navigator or the G-code
  panel (`GCodePanel`, shown to users as the G-code window). A panel is built from components on a card.
- **Card**: the surface of a panel.
- **Popup**: a menu or a tooltip.
- **Tokens**: the colours of the light and the dark theme by role (`Theme`), the scales of sizes
  (`Tokens.hpp`) and the sizes of components taken from them (`Metrics`).
- **Corner**: every rounded surface of the toolkit has the same shape of corner, a circular arc
  stretched into a curve of continuous curvature by `CORNER_SMOOTHING` = 0.6, so that a corner
  meets its sides without a visible kink. A surface inside a rounded one, inset from its edge, has the outer
  radius less the inset (`nested_radius`).

The wx controls under `GUI/Widgets/` are unrelated.

## Layers

The frame services of the last item are the ground every other layer stands on; otherwise each layer uses
only the ones above it.

1. **Tokens.** `Theme` holds the colours by role: panel, text, divider, hover, accent, the thumb of a
   switch, the shadow and so on. A component asks for a role, never for a colour. `Metrics` holds the
   sizes, steps of the scales multiplied by the interface scale: radii, paddings, row heights, the gap
   between stacked components, icon sizes. Heights and vertical gaps are whole pixels, because ImGui
   floors the cursor after each item; outlines stay one physical pixel. Neither depends on ImGui.
   Points, sizes, rectangles and colours are the toolkit's own `Types`; colours are mixed and faded in
   whole steps of a colour byte, so a colour worked out from others lands on the byte it would have
   as written out.
2. **Measuring.** `Measure` works out the size of every component without drawing: a button from its
   label, a toggle group side by side or stacked, a field with its control beside or under the
   label, a switch whose label wraps, and the height of components stacked in a card. The components
   draw through the same helpers, such as the width of a toggle, so a size is worked out in one place.
   The items of a toggle group are measured by `ChoiceMeasure`, the wider of the regular and the
   bold face, because the selected one is drawn bold. A row lays out slots side by side, the inline gap
   apart: growing slots share the room the others leave, and when the row is overfull the slots allowed
   to shrink, such as a title, give up width so the rest stays inside. A panel places its header and
   toolbars with it instead of offsetting one part from another by hand. `Menu` does the
   same for a menu — it keeps the column of indicators only when one of its items has an indicator, so that
   the text of a menu without indicators is inset alike on both sides, and comes out a whole number of pixels across and down — and places it: on the preferred side of its anchor, on the other side when only
   that fits, then pushed inside the screen, on a whole pixel. Text is measured through `TextMeasure`, which
   `FontMeasure` implements on a `Font` and the tests on a monospaced stand-in. A `Font` is a face of
   the interface's fonts at a size, as the context hands it out: it measures text, gives the height of its baseline, and tells whether it
   has every character of it, so a panel can fall back to the regular face before drawing. The components
   draw to these sizes, and panels lay out by them before drawing, so a panel never waits a frame to
   learn how tall its content came out.
3. **Components** (`Button`, `ToggleGroup`, `Tabs`, `Field`, `NumberField`, `PositionField`, `Zone`, on the
   shared `ComponentParts`): the button — default, secondary or ghost, in the default, small or icon
   size, with an icon before the label when asked, pressed and marked as opening a popup when it is a
   toolbar button — the toggle group, folder tabs, fields with a toggle group or a switch, the number
   field, and the position field, "current / total" of a long list
   that turns into a number field to jump with when clicked. Each is an immediate-mode function that takes an id, its
   value when it has one, and a struct of options, and returns whether the user changed or pressed it; the tabs bar
   returns what happened to it. A pressed button has the selection tint and a `primary` outline: the tint alone
   differs from the card too little to tell.
   A component's `Availability` tells whether it can be used and, when not, why: a disabled component ignores
   the mouse and shows its reason, when it has one, in a tooltip. Its
   text and marks are `disabled_foreground`. A `zone` is the hit target of an area a panel paints by hand: it
   reports hovering, a press, the hold, the release and a double click only while the pointer is seen over it — not
   under a popup or past the clip of a scroll area — and the panel finds what lies under the pointer itself. Folder tabs stand on the bottom of a row: the selected one opens onto
   the content under the row and covers the row's divider; like the toggles, they are measured by `ChoiceMeasure`
   and label the chosen one in bold (`choice_label()`). `tabs_bar` is the row with them: the tabs list's surface
   and its divider, the tabs, and at the end the changed count and a reset button on the line of the tab labels,
   as the settings of a panel use it. A labelled button and an icon button both show their tooltip.
   `Select` is a field with the current choice and a chevron that opens a menu under it,
   its border in the primary colour while open. `Table` gives the rows of a table and their header: columns
   of fixed width laid out by `table_columns()`, a name column that takes the rest and is cut with an ellipsis
   (`ellipsize()`), a swatch, a selected and a muted style for something hidden; a row reports a click, an
   Alt+click and a right click, and a row that is no action still shows its tooltip. `Swatch` paints the colour a line
   type or a filament is drawn in — hollow for something hidden, numbered, over a checkerboard when
   see-through — with a ring when it stands too close to what is under it. `Alert` is an
   info or a warning callout with a title, wrapped text and small action buttons, measured by `alert_height()`.
4. **Surfaces, painting and popups.** `Card` opens a panel as one scope: the opaque fill, the outline, the
   rounding, the padding, the text colour, the gap between stacked components and an ImGui id scope.
   The card draws its fill before the content and its outline after it, so fills across the card never
   hide the outline. What a panel draws by hand — the rows of a list, the cube of the navigator — it
   paints with the card's `Painter`: rectangles, lines, polylines, convex fills with an optional soft
   edge, circles, text at a point, wrapped at a width, or from the start of a band and centred on its height, text laid in a plane,
   a divider, gradients between two colours or through stops, and icons, in colours given as values, with
   `Clip` and `SharpFills` scopes for clipping and seamless fills. `Painter::background()` paints under
   every panel, as the navigator's cube is when it stands on the scene above the toolbar's card.
   A card does not scroll with the wheel. A zero width or height fits that side to the content.
   Rounded rectangles are drawn from one outline, `rounded_rect_outline()`, with the same number of
   points whatever the radius and the corners, so a shadow joins it point by point; ImGui's own
   rounding is left only to the scrollbar and the frame of the number field. Row backgrounds go through
   the card, which fills its own inner shape cut to the row, so rows near a rounded corner follow it and meet
   without notches; ImGui cannot clip to a rounded rectangle, so this is where the corners are kept whole. `ScrollArea` takes the height of its content and
   scrolls it up and down with the wheel and a thin scrollbar, never sideways; it gives the origin of
   its content, which moves as it scrolls, and a painter of its own. Shadows of cards
   and menus are drawn in the window's own draw list, only outside its rounded rectangle, at the
   elevation of the surface, so a menu's shadow lies over the card under it and stands out more. They fade from
   half their strength at the edge, as a blur does, so that they do not darken the outline into a second line; the
   outline holds the edge, which is why `card_border` is darker in the light theme than a border on a white page
   needs: the card stands on the grey scene. A menu
   and a tooltip draw their surface themselves, as a card does, past the clip ImGui insets a window by
   (`fill_window()`, `stroke_window()`), so that the corners are the toolkit's. Settings a panel opens over its own content cast a
   shadow strip down onto that content with `Painter::shadow_below()`, drawn after the content so it lies on
   it without moving or recolouring it. `Tooltip` gives the delayed tooltip of an item, or of a zone
   drawn by hand under an id of its own, its text a step under the menu's. ImGui's other tooltips open in
   the same window, so it puts the window's font scale back before it closes. ImGui hides a tooltip or a popup in the frame it appears, so
   both ask for the next frame then. `MenuPopup` gives the menu, built from `MenuItem`s (items with a
   key, checkbox and radio items, group labels in the style of a panel's section titles, separators) and placed by a `Positioner`; its items are built only while it is open, and
   it returns the id of the item picked. An item can also carry a value on the right where a key goes, the tint of the
   current choice and a muted label for a choice with little to show.
   `Popover` is a popup with a title over content the caller draws while it is open, sized beforehand by
   `popover_size()` and placed by a `Positioner` like a menu. A click outside closes it, and so does Esc, also
   while a field in it holds the keyboard: the field drops its edit and the popover closes with it. Settings
   a panel opens under its header are `Collapsible`: the panel keeps whether they are open,
   `collapsible_trigger()` is the button pressed in while they are, and `CollapsiblePanel` draws them as a
   scroll area across the card under a bar of tabs, counter and reset, with a divider at its bottom and the
   shadow below on the content under it. The panel is drawn after that content, so the shadow lies on it. Esc
   closes it unless a field took the key or a popup was open, as the canvas closes the popup on top before
   the frame that sees the key. `SettingsForm` draws a panel's settings in it from data: each field a switch
   or a choice among options, on a tab when the form has tabs, with its availability read as the field is
   drawn, after the reset and the fields above it, so a field shows the state a reset in the same frame
   left. The form counts the settings changed from their defaults; the count is shown and the reset can be
   pressed only while it is above zero. `settings_form_height()` gives its height before it is drawn.
5. **Frame services** (`Context`, `CanvasFrame`, `Layout`, `Input`): one context per canvas frame carries the
   theme, the metrics, the time, the number of the frame among those of every canvas, the tooltip timer, the
   icon cache and the fonts. The regular face is the interface font as it is; the bold face and the monospaced
   ones — regular, bold and italic in each size of `MONO_FONT_SIZES` (`Tokens.hpp`), the one table of monospaced
   sizes, picked by `MonoSize` — are loaded by `ImGuiWrapper` (`canvas_ui_fonts()`). The canvas side of a frame
   is `CanvasFrame`: it starts the frame with all of these, ends it and reads the frames and the cursor the
   components asked for. Panels see only the context, and may not include `CanvasFrame.hpp`, so they cannot restart the frame
   they draw in.
   `Layout` places the next component and groups and scopes the ids of components; `Input` reads the
   mouse of the frame and Esc, tells whether an item holds the keyboard, keeps the mouse from the
   scene (`capture_mouse()`, for the panels of the frame) and copies to the clipboard. A canvas makes its context current for
   the frame through `ScopedContext`, which gives the previous one back, so a canvas drawn inside
   another's frame does not leave the outer one without its context. It is in `CanvasFrame.hpp` too, so a
   panel cannot make a context of its own current.

## Frames on demand

The canvas draws only on events and when it is marked dirty; its render timer only wakes the idle
loop. A component that animates, or waits for a tooltip, asks the context for its next frame: now, or
at a time; a component clicked asks for the next one too (`hit()` in `ComponentParts.cpp`), so the
change it makes shows at once. After its ImGui frame the canvas turns "now" into an overlay frame and "at a time" into the
deferred overlay frame `on_idle()` draws when the time comes; the time is taken afresh from every
frame, so a tooltip nothing waits for any more asks for nothing. An overlay frame does not draw the scene
again from its objects: the scene comes from its cache unless the camera moved, so a camera the
navigator turns still redraws it. A wheel over an ImGui window is an overlay frame too, unless a layer
or move slider takes it and changes what the scene shows. The tooltip timer is shared, so every tooltip
waits the same 500 ms after its item is first hovered, and moving to another item starts the wait again.

## Icons

Icons are one-colour SVG files on a 24 unit grid, kept in `resources/images/` with the other images, with round caps and
joins. The line is `Stroke::md` wide at any icon size: the width is set on the parsed file before it
is rasterized, not scaled with it, so a small icon is not paler than a large one. `draw_icon()`
rasterizes a file with nanosvg once per size and line width in pixels, places it on the pixel grid and tints it with the colour of its
state. All canvases share one OpenGL context, so the textures live in one `IconCache` that
`OpenGLManager` owns and frees before it deletes the context. Icons are not drawn as ImGui
polylines, which give square joins and butt caps, a stroke that does not scale, lines falling between
pixels, crossings that darken, and inner gaps faked with the background colour.

## Writing a panel

A panel names its own sizes in one place, its `Sizes`, built from the toolkit's metrics, rather than scattering
numbers through its drawing code. A panel of a column stands on `PanelCard`: a card `Metrics::panel_width` wide
unless the panel sets its own width (`PanelCardOptions::width`), a header (`panel_header`) with its collapse button,
and a body that scrolls in the room the column leaves and is measured as it is drawn, so that the next frame fits
the card to it. The body is written on `PanelBody` from its top down: a cursor components are placed at, which
moves down past each, the gap between sections, a section title with a note such as a unit, the titles of a
table's columns, and the height with the body's padding. `PanelCollapse` keeps whether the panel is collapsed,
takes a press of the collapse button at the start of the next frame, before anything is laid out for the old
state, and saves each change through a callback its owner sets. Another panel opens a `Card` at the place its layout gives, sized by the measuring functions, places
components with `place()` and `Group`, paints the rest with the card's `Painter`, describes menus and settings as data, and asks the context for
frames instead of keeping its own timers.

A panel over the scene goes through the toolkit only: it calls no ImGui function and holds no ImGui
object or value. The toolkit's public headers do not include ImGui; its own sources and tests reach it
through `ImGuiBridge`, which turns the toolkit's types into ImGui's and back, and `ComponentParts`, the parts
the components share. A panel that called
ImGui would have to include it itself, and the build stops when a panel's file does: `cmake/modules/CheckCanvasPanels.cmake`
reads the files listed in `SLIC3R_CANVAS_PANEL_SOURCES`, past their comments and string literals, and fails on an
ImGui or `ImGuiWrapper` include, a toolkit-internal header (`ComponentParts.hpp`, `ImGuiBridge.hpp`, `CanvasFrame.hpp`) or an
ImGui name (`ImGui…`, `Im[A-Z]…`, `IM_…`), and on a GUI file in any subfolder outside the toolkit that includes a toolkit
header but is not on the list. Only the files that hold the toolkit's frame, fonts and icons or lay out beside the panels
while drawing through `ImGuiWrapper` are exempt, by name, each with its reason in the check: the canvas, the ImGui
wrapper, the OpenGL manager, and the preview with its rails. The context keeps
to the toolkit what panels must not reach: the double click it counts for `Pointer` and `zone`, the icons, and the making of
fonts from ImGui's faces. It checks itself first on the examples in `cmake/modules/canvas_panels_check/`, and runs at build
time rather than in a test, because the test run has only the built tests, not the sources. What a panel needs and the toolkit lacks
is added to the toolkit first, with its size function and a test. Keeping ImGui behind the toolkit is
what lets one place decide how every panel paints, measures and reacts, and lets the corners, the
shadows or ImGui itself change without touching the panels. The navigator, the G-code panel, the legend, the
print summary and the statistics of all plates follow this rule, and so do their geometry, text and layout files and
the files that tie them to the preview; the rails and the move panel are drawn through `ImGuiWrapper`. Interface colours are theme roles, among them the bar of a
list's current row, a position indicator, the ring around a mark that contrasts too little, the text
of an option or a tab not selected and of a disabled component, the tabs list and the shadow below
opened settings. Colours
a panel needs beyond them belong to what it shows, not to the interface around it — the cube of the
navigator and the syntax colours of the G-code panel keep their own.

## Limits

- All canvases draw into one ImGui context, so ImGui windows are shared by name across them: a
  panel shown on two canvases keeps one position, scroll and open popup.
- The ImGui widgets of `ImGuiWrapper` draw the gizmos, the notifications, the rails and the move panel;
  they do not take the toolkit's theme or metrics.
- A press or a release over any ImGui window draws the scene again: widgets of `ImGuiWrapper`
  change the scene without asking for a frame, so the canvas cannot tell a toolkit click from theirs.
- The toolkit holds only components a panel uses. Checkbox, radio, visibility toggle, text field and dialog
  are not in it; the panel that first needs one adds it.
