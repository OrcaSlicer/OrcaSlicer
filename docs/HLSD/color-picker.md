# Color picker

The color picker is a local HTML dialog hosted by `ColorPickerDialog`, a
`WebViewHostDialog` subclass. Its header and implementation own the ordered
selection contract and shared filament conversion. Bridge validation, favorites
persistence, placement and gettext strings remain private to the implementation.
Its colors use the existing `ColorRGBA` utility.
It appears as a rounded, borderless floating panel. macOS rounds the native view
layer instead of calling `SetShape`, which synchronously resizes its window;
other platforms use a validated shape region with a reentry guard.
Like SpeedDial, the host disables browser zoom accelerators, restores native
WebView focus on activation and readiness, and synchronizes layout and the macOS
viewport before repainting the backend. Activation never dismisses this modal
dialog; focus restoration preserves an active editor and its partial input.
The initialized page reports
its intrinsic content height; native code validates the current page identifier
and bounded numeric height, converts CSS pixels to device-independent window
size, and fits within the display's work area. Identical sizes are ignored. The
page observes content rather than viewport height, so resizing does not create
a feedback loop; oversized content remains scrollable.
The panel snapshots the triggering color or More Colors button's screen rectangle.
It aligns below the button, flips above when needed, and clamps to that button's
display work area. Content and DPI changes reuse the same anchor and display;
later mouse movement does not affect placement. wxGTK clears the dialog's default
parent-centering constraint and reapplies its native position after showing,
bypassing wxGTK's requested-position cache. Absolute top-level placement remains
subject to the window manager; Wayland compositors control it. The official filament palette's
existing placement is independent of this custom panel.
A solid selection contains one color; a gradient contains exactly two ordered
endpoints. The bridge represents these as `{"type":"solid|gradient",
"colors":["#RRGGBBAA", ...]}`. RGB input is also accepted and becomes opaque.
Native validation rejects malformed hex and incorrect cardinality.

The project filament color action and the official filament picker's More Colors
action enable two-endpoint gradients and independent endpoint alpha. The preset
combo box reads project colors in their stored vector order locally and applies
changes through `sync_colour_config`,
retaining the existing RGB/RGBA profile/project formats and dirty marking.
Opaque colors use `#RRGGBB`; nonopaque colors use `#RRGGBBAA`, including fully
transparent alpha `00`. Initial selections, alpha-only edits and cancellation
retain stored alpha. Ordered
custom results bypass the official palette's `FilamentColor` set. Choosing an
official swatch retains its existing palette behavior.
The Linux sidebar's Change extruder color menu uses this same custom editor and
the filament swatch anchor, including alpha, ordered gradients and configuration syncing.

Striped colors and gradients with more than two endpoints cannot be represented
by this editor. A notice explains that the current selection is retained; the
editor starts from the first color, and unchanged confirmation is a no-op.
Cancellation is also a no-op. If the WebView backend cannot initialize, a local
unavailable panel offers Cancel and Escape, without opening a system color picker.

Generic color option fields use an ordinary color button on every platform and
open the same panel with gradients and alpha disabled. The panel is anchored to
the field button. Empty or invalid field values remain undefined (the empty
string), distinct from opaque black. Right-click resets to undefined. Cancel does
not notify the field; confirmation and reset notify only when its RGB string
changes. Programmatic assignments remain silent, and reading a field never
writes legacy favorites.

AMS material settings retain their fixed/AMS swatches and first-level popup. Only
its custom-color action opens the solid, opaque editor, anchored to that custom
control. The transient popup releases its grab while the modal is open and returns
to the same selection on cancellation. Confirmation uses the existing default-color
and packed RGBA event path. Device permission/status checks remain at the first-level
entry.

Texture import's color-mapping Add Material action opens a solid, opaque editor
anchored to the mapping row. The existing filament limit is checked before opening.
The first-level material popup closes as an action; its callback is copied before
dismissal, so popup destruction does not invalidate the modal's completion path.
Confirmation invokes the existing virtual-filament callback once; cancellation
adds no material.

Native gettext strings are installed at document start. Static page labels and
palette names use stable English keys; RAL codes, channel symbols, and numeric
labels remain data. The palette color values do not depend on the UI language.
The application locale also sets the document language. Numeric editor validation
uses the application's gettext messages, rather than browser language defaults;
missing translations fall back to the stable English keys.

The pure color/state regressions run with
`node --test resources/web/dialog/ColorPickerDialog/colorpicker.test.js`.
Optional real-browser regressions live beside them in
`colorpicker.browser.test.js`. They use an existing Playwright installation
(`ORCA_PLAYWRIGHT_MODULE` can name its module path) and an installed browser
(`ORCA_BROWSER_EXECUTABLE` can name its executable), without adding a production
dependency. They cover bridge initialization, capability combinations, invalid
editor recovery, ordered endpoint alpha, favorites, gettext keys, and live theme
changes. These browser checks complement native contract/persistence tests; they
do not establish the full native modal confirmation and favorites journey.

Gradient and alpha capabilities are independent and default off. Unsupported
gradients are rejected; disabled alpha normalizes the selected result to opaque.
Favorites retain both capabilities independently of the current caller. The page
stores alpha as a byte and only converts to percentage when displaying or editing
opacity, so switching endpoints does not change alpha precision.

The page starts disabled. Each document supplies a page identifier in its ready
message, receives native initialization, and acknowledges it before native code
accepts confirmation or favorite updates. Repeated initialization does not reset
edits, and messages from an obsolete document are ignored. Native WebView message
handling is deferred and guarded by a lifetime token. Only a validated confirmation
sets the optional result. Cancel, Escape and native window close leave it empty.

Favorites belong to application configuration, separately from the selected result,
in the editor configuration returned by `AppConfig::config_path()` under `data_dir()`.
The `color_picker` section has string `version` equal to `1` and a `favorites`
string containing a JSON array of typed selections. Native helpers normalize and
deduplicate this array in order and limit it to 24 entries. Updates replace the
entire section and immediately call `AppConfig::save()`, so accepted favorite
changes survive cancellation. Empty arrays are saved explicitly. Core `AppConfig`
only stores the raw section; color validation belongs to the GUI boundary.

When the section is absent, valid solid colors from `custom_color_list` are imported
once, without changing that legacy section. The legacy native picker saves decimal
`r,g,b,a` byte strings; hexadecimal RGB and RGBA strings are also supported. Migration
strictly validates all four decimal channels and preserves alpha. Gradient strings
and malformed values are ignored. The new version and an empty array are
saved even when there is nothing to import. A present section with an unknown or
missing version is left untouched and disables favorite writes. Malformed version
1 data is displayed as empty and remains on disk until the user explicitly saves
a valid replacement. Other configuration sections and project/profile color
formats are independent of this storage.

Interface colors consume the host's `data-orca-theme` and `--orca-*` contract through
shared styles. The picker opts into scoped global control roles. Application theme
changes update UI surfaces without reloading the page or changing color data.
