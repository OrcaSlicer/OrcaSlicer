# Painting and custom widgets

How OrcaSlicer windows draw themselves: the wx paint contract, background styles, buffering,
memory DCs and `wxGCDC`, DC coordinates under DPI, text measurement and invalidation, then how
to write a custom control on the wx side and how to author an Orca widget on the
`StaticBox`/`StateHandler` foundation. Read it before writing or reviewing any `wxEVT_PAINT`
handler, `render`/`doRender` method, `messureSize`, or a new class under `src/slic3r/GUI/Widgets/`.

wx cites are relative to the pinned wx 3.3.2 tree (located as in `SKILL.md` §Ground truth).
Orca builds wx with `wxBUILD_DEBUG_LEVEL=0` and `libslic3r_gui` with `wxDEBUG_LEVEL=0`: every
wx assert below is compiled out and `wxCHECK*` returns silently, so paint misuse shows up only as
wrong, missing or stale pixels, never as an assert dialog.

Contents: [Rules](#rules) · [Paint handler contract](#the-paint-handler-contract) ·
[Background styles and erasing](#background-styles-and-erasing) ·
[Buffered painting](#buffered-painting-and-double-buffering) ·
[Alpha, anti-aliasing, wxGCDC](#alpha-anti-aliasing-wxgcdc-and-wxgraphicscontext) ·
[Memory DCs and bitmaps](#memory-dcs-and-bitmaps) ·
[DC coordinates under DPI](#dc-coordinates-and-scale-under-dpi) ·
[Measuring text](#measuring-text-without-painting) ·
[Refresh, RefreshRect, Update](#invalidation-refresh-refreshrect-update) ·
[Writing a custom control](#writing-a-custom-control-wx-side) ·
[StaticBox and StateHandler](#the-orca-foundation-staticbox-and-statehandler) ·
[Authoring an Orca widget](#authoring-an-orca-widget)

## Rules

1. In every `wxEVT_PAINT` handler create a `wxPaintDC` (or a buffered paint DC) first, before any
   early return; never create one outside a paint handler, and never create a `wxClientDC`/`wxWindowDC`
   inside one. → [Paint handler contract](#the-paint-handler-contract)
2. Draw only from the paint handler. Change state, then `Refresh()`/`RefreshRect()`; never draw
   with `wxClientDC` from timers, mouse or idle handlers (no effect on macOS or Wayland).
   → [Paint handler contract](#the-paint-handler-contract)
3. Never call `Refresh()` — or a setter that calls it — from inside the paint path.
   → [Paint handler contract](#the-paint-handler-contract)
4. Set `wxBG_STYLE_PAINT` only when the handler covers every pixel (clear first); otherwise keep
   the default `wxBG_STYLE_ERASE` with a background colour. → [Background styles](#background-styles-and-erasing)
5. `wxBG_STYLE_TRANSPARENT` must be set between the default ctor and `Create()`; never use
   `wxBG_STYLE_COLOUR`. → [Background styles](#background-styles-and-erasing)
6. `wxAutoBufferedPaintDC`/`wxBufferedPaintDC` require `SetBackgroundStyle(wxBG_STYLE_PAINT)` in
   the ctor and a full `dc.Clear()` (the buffer is shared and stale). → [Buffered painting](#buffered-painting-and-double-buffering)
7. `SetDoubleBuffered(true)` only after creation, only on MSW, only when a buffered DC is not
   enough. → [Buffered painting](#buffered-painting-and-double-buffering)
8. Rounded shapes, alpha and anti-aliasing on MSW need `wxGCDC`/`wxGraphicsContext`; plain
   `wxPaintDC` there is GDI (no alpha, no AA). → [Alpha, AA](#alpha-anti-aliasing-wxgcdc-and-wxgraphicscontext)
9. Set font, text colour, pen and brush explicitly on every `wxGCDC` and memory DC before drawing.
   → [Alpha, AA](#alpha-anti-aliasing-wxgcdc-and-wxgraphicscontext)
10. Back buffers: `bmp.CreateWithLogicalSize(GetClientSize(), GetDPIScaleFactor())`; a raw
    `wxBitmap(w, h)` sized from `GetSize()` is only correct inside `#ifdef __WXMSW__`, and there
    only for shapes: text drawn into its scale-1 bitmap is sized for 96 DPI.
    → [Memory DCs](#memory-dcs-and-bitmaps), [Alpha, AA](#alpha-anti-aliasing-wxgcdc-and-wxgraphicscontext)
11. Lay out bitmaps with `GetLogicalSize()` (`ScalableBitmap::GetBmpSize()` for Orca bitmaps), never
    `GetWidth()/dc.GetContentScaleFactor()`. → [DC coordinates](#dc-coordinates-and-scale-under-dpi)
12. Convert DIPs with the window's `FromDIP`, not the DC's, and do not derive pixel snapping from
    `dc.GetContentScaleFactor()` in code that can receive different DC types.
    → [DC coordinates](#dc-coordinates-and-scale-under-dpi)
13. Measure text with `wxWindow::GetTextExtent(s, …, &font)` or `wxInfoDC`, not a new `wxClientDC`.
    → [Measuring text](#measuring-text-without-painting)
14. Never rely on `Update()` (no-op on Wayland) or `Refresh(); Update();` loops; return to the event loop.
    → [Invalidation](#invalidation-refresh-refreshrect-update)
15. A renderer whose output must keep up with an interactive resize on MSW renders inside the
    paint handler, not from idle. → [Invalidation](#invalidation-refresh-refreshrect-update)
16. Size-dependent drawing needs `wxFULL_REPAINT_ON_RESIZE` or `wxEVT_SIZE` → `Refresh(); e.Skip();`.
    → [Custom control](#writing-a-custom-control-wx-side)
17. A composite that hosts focusable children derives from `wxNavigationEnabled<Base>`; display-only
    widgets return `false` from `AcceptsFocus()`. → [Custom control](#writing-a-custom-control-wx-side)
18. Background colour is never inherited: read the parent's colour at create time and re-apply it
    on theme/DPI change. → [Custom control](#writing-a-custom-control-wx-side), [StaticBox](#the-orca-foundation-staticbox-and-statehandler)
19. Ancestor enable/disable never calls a child's `Enable()` override; disable Orca widgets
    individually. → [Custom control](#writing-a-custom-control-wx-side), [StateHandler](#statehandler)
20. A subclass's dynamic `Bind(wxEVT_PAINT, …)` pre-empts the base class's paint handler; skip it
    only if the base must paint too. → [Custom control](#writing-a-custom-control-wx-side)
21. Do not paint over native controls (`wxButton`, …); `LabeledStaticBox` is the guarded exception.
    → [Paint handler contract](#the-paint-handler-contract)
22. New framed, state-coloured widgets derive `StaticBox` (or `wxNavigationEnabled<StaticBox>`),
    attach every `StateColor` to `state_handler` and call `update_binds()` after each replacement.
    → [Authoring](#authoring-an-orca-widget)
23. Every `Enable()` override emits `EVT_ENABLE_CHANGED` exactly once per real change.
    → [StateHandler](#statehandler)
24. `remove_child()` an `attach_child()`ed window before destroying it separately.
    → [StateHandler](#statehandler)
25. Every new widget provides `Rescale()` (re-rasterise named bitmaps, re-measure, `Refresh()`) and
    its owner calls it (existing widgets without one: `references/orca-widgets.md §Shared lifecycle
    rules`). → [Authoring](#authoring-an-orca-widget)
26. Make a whole card clickable by calling the action (or emitting a semantic `wxEVT_BUTTON` with
    `ProcessWindowEvent`) from child handlers, never by synthesising `wxCommandEvent(wxEVT_LEFT_DOWN)`.
    → [Authoring](#clickable-card-panels-wxpanel--wxbg_style_paint)

## The paint handler contract

**Contract.**
- "If you have an EVT_PAINT() handler, you @e must create a wxPaintDC object within it even if
  you don't actually use it" (`interface/wx/dcclient.h:14-15`).
- "you must @e not create other kinds of wxDC (e.g. wxClientDC or wxWindowDC) in EVT_PAINT
  handlers and also don't create wxPaintDC outside of this event handlers" (`interface/wx/event.h:2285-2287`).
- `wxPaintDC` "automatically sets the clipping area to the damaged area of the window" and "is
  initialized to use the same font and colours as the window" (`interface/wx/dcclient.h:17-22`); it
  also inherits the window background colour, so `dc.Clear()` fills with `GetBackgroundColour()`
  [source] (`src/common/dcbase.cpp` `wxDCImpl::InheritAttributes`).
- `GetUpdateRegion()` "Should only be called within a wxPaintEvent handler" (`interface/wx/window.h:2354`).
- Painting standard controls: "it is impossible to change the drawing of a standard control (such
  as wxButton) … inherently not portable and won't work everywhere" (`interface/wx/event.h:2327-2330`).
- `ClearBackground()` uses a client DC and "shouldn't be used from EVT_PAINT handlers"; use
  `dc.Clear()` on the paint DC (`interface/wx/window.h:2190-2200`).

**Usage.**
```cpp
Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
    wxPaintDC dc(this);                 // always first, even when nothing is drawn
    if (GetSize().x <= 0) return;
    render(dc);
});
```

**Platforms** [source]:

| Situation | MSW | macOS | GTK3 (X11 and Wayland) | GTK2 (opt-out build) |
|---|---|---|---|---|
| Handler creates no `wxPaintDC` | `wxWindowMSW::HandlePaint` sees no DC and lets `DefWindowProc` validate the region: no `WM_PAINT` storm in 3.3.2, but drawing is unclipped and the contract is broken (`src/msw/window.cpp` `HandlePaint`) | no error; the window shows only what the background step left (undetermined under `wxBG_STYLE_PAINT`) | same | same |
| Handler creates a DC, then `Skip()`s | default `OnPaint` (native painting) runs as well | — | — | — |
| `wxPaintDC` outside a paint event | `wxCHECK_RET(!paintStack.empty(), "wxPaintDC can't be created outside wxEVT_PAINT handler")` (also for a window other than the one being repainted): the DC gets no HDC and every draw call is a silent no-op (`src/msw/dcclient.cpp` `wxPaintDCImpl::wxPaintDCImpl`) | no visible effect | `wxCHECK_RET(cr, "using wxPaintDC without being in a native paint event")`, every draw call is a silent no-op (`src/gtk/dc.cpp:518`) | works (GDK) |
| `wxClientDC` drawing | works on non-composited windows; the doc lists "wxMSW when using double buffering" (`WS_EX_COMPOSITED`, see below) as non-working (`interface/wx/dcclient.h:82-84`), yet `CanBeUsedForDrawing` returns `true` unconditionally (`include/wx/msw/dcclient.h:60`), so it cannot detect that case | never; `CanBeUsedForDrawing` is `false` (`include/wx/osx/dcclient.h:58`) | X11 works; Wayland has no effect, `CanBeUsedForDrawing` is `false` on `wxDisplayWayland` (`src/gtk/dc.cpp` `wxClientDCImpl::CanBeUsedForDrawing`) | works |

`wxClientDC` "simply doesn't have any effect" on wxOSX and GTK3/Wayland and is deprecated
(`interface/wx/dcclient.h:41-58`). To repaint part of a window, `RefreshRect()` it and check
`GetUpdateRegion()` in the handler.

**OrcaSlicer.** `StaticBox::paintEvent`, `Button::paintEvent`, `TextInput::paintEvent` and the
AMS/device widgets all have the one-line shape `wxPaintDC dc(this); render(dc);`. Comments above
some `render()` methods say they work "with wxPaintDC or wxClientDC"; ignore them and call
`render()` only from the paint handler. `LabeledStaticBox` paints over a native
`wxStaticBox` and is guarded per platform (see [StaticBox](#the-orca-foundation-staticbox-and-statehandler)).

**Pitfalls.**
- **Rule:** Create the paint DC before any early return.
  **Why:** documented requirement. Without the DC, wxMSW falls back to `DefWindowProc` to validate
  the region; on every port a `wxBG_STYLE_PAINT` window that returned early shows undetermined
  pixels (clear before returning, see [Background styles](#background-styles-and-erasing)).
  ```cpp
  // Wrong: if (!m_ready) return; wxPaintDC dc(this);
  // Right: wxPaintDC dc(this); if (!m_ready) return;
  ```
  Cite: `interface/wx/dcclient.h:14-15`.
- **Rule:** Draw only in `wxEVT_PAINT`; elsewhere update state and invalidate.
  **Why:** `wxClientDC` drawing has no effect on macOS and Wayland; it races the next paint on the
  others. `ClearBackground()` is a client-DC call too.
  ```cpp
  // Wrong (timer/mouse handler): wxClientDC dc(this); dc.DrawRectangle(r);
  // Right:                        m_highlight = r; RefreshRect(r);
  ```
  Cite: `interface/wx/dcclient.h:41-58`.
- **Rule:** No `Refresh()` from the paint path — including Orca setters that call `Refresh()`
  themselves (`SetLabel`, `SetFont`, `SetBackgroundColor`, `SetCornerRadius`, …) and `messureSize()`
  paths that `SetSize()` (`TextInput::messureSize` → `EVT_SIZE` → `StaticBox::sizeEvent` → `Refresh()`).
  **Why:** `Refresh()` invalidates for "the next event loop iteration" (`interface/wx/window.h:2376-2390`);
  doing it while painting schedules another paint, so the window repaints continuously and burns CPU.
  Measure and set sizes in setters, `Rescale()` or size handlers, never in `render()`.
- **Rule:** Do not handle paint events of native controls.
  **Why:** non-portable by contract; on MSW `wxStaticBox` even installs its own `OnPaint` with
  `wxBG_STYLE_PAINT` when not double-buffered (`src/msw/statbox.cpp` `wxStaticBox::UseCustomPaint`,
  disabled by system option `msw.staticbox.optimized-paint=0`, `interface/wx/sysopt.h:74-78`).
  Use an owner-drawn `wxWindow`/`StaticBox` instead.

## Background styles and erasing

**Contract** (`interface/wx/defs.h:488-544`). The default is `wxBG_STYLE_ERASE`
(`src/common/wincmn.cpp:306`).

| Style | Meaning |
|---|---|
| `wxBG_STYLE_ERASE` (default) | Background "may be erased in the user-defined EVT_ERASE_BACKGROUND handler"; with no handler (or a skipping one) it behaves like `SYSTEM`; an empty non-skipping handler behaves like `PAINT`. "The only background style value for which erase background events are generated at all." |
| `wxBG_STYLE_SYSTEM` | System/theme background, or the window's non-default background colour. No erase events. |
| `wxBG_STYLE_PAINT` | "only erased in the user-defined EVT_PAINT handler … must not be used however if the paint handler leaves any parts of the window unpainted as their contents is then undetermined." No erase events. |
| `wxBG_STYLE_COLOUR` | "deprecated and doesn't do anything" (`interface/wx/defs.h:533`); [source] MSW and GTK treat it as ERASE (`src/msw/window.cpp` `HandleEraseBkgnd`, `src/gtk/window.cpp` `GTKSendPaintEvents`), macOS as SYSTEM (`src/osx/window_osx.cpp` `MacDoRedraw`). |
| `wxBG_STYLE_TRANSPARENT` | Parent shows through; only wxOSX, wxGTK with compositing, and wxMSW since 3.3.0. |
| `wxBG_STYLE_CUSTOM` | Alias of `PAINT` (`include/wx/defs.h:1737`). |

`SetBackgroundStyle` (`interface/wx/window.h:2466-2511`): use `PAINT` "if you define an
EVT_PAINT handler which completely overwrites the window background … Do ensure that the
background is entirely erased by your EVT_PAINT handler in this case however as otherwise garbage
may be left on screen"; an empty erase handler is the obsolete way to do the same. For
`TRANSPARENT`: "it must be called before Create()", with the default-ctor → `SetBackgroundStyle`
→ `Create` pattern shown in the doc.

Erase event: "You must use the device context returned by GetDC() to draw on, don't create a
wxPaintDC in the event handler"; `GetDC()` is never null; on GTK the event "is simulated (simply
generated just before the paint event)" (`interface/wx/event.h:3361-3395`).

**What actually happens without `PAINT`** [source]:
- MSW `wxWindowMSW::HandleEraseBkgnd`: ERASE/COLOUR send `wxEraseEvent`; if unhandled, fall through
  to SYSTEM, which fills with the background brush; PAINT/TRANSPARENT do nothing.
- GTK3 `GTKSendPaintEvents` (`src/gtk/window.cpp`): ERASE sends the erase event, then SYSTEM renders
  the theme background, or `m_backgroundColour` when the theme background is off (GTK ≥ 3.20).
- macOS `wxWindowMac::MacDoRedraw` (`src/osx/window_osx.cpp`): ERASE/SYSTEM create a `wxWindowDC`;
  ERASE sends the erase event and `Clear()`s if it is unhandled; both then fill with the background
  colour when one was set.

So on every port, a window that keeps `ERASE` and has a background colour gets that colour under
whatever its paint handler leaves unpainted.

**Platforms.** `wxBG_STYLE_TRANSPARENT`: MSW implements it as `WS_EX_TRANSPARENT` on non-TLW
children and `IsTransparentBackgroundSupported()` is always `true` there; GTK needs a compositing
manager (`src/gtk/window.cpp` `IsTransparentBackgroundSupported`); macOS supports it.
[source] `src/common/wincmn.cpp:1616-1625`: setting it on a created window, or unsetting it, is a
`wxCHECK_MSG` that returns `false` — silent in Orca.

**OrcaSlicer.**
- `StaticBox` deliberately keeps `ERASE` and sets its wx background colour to the parent's
  (`StaticBox::Create`), so the erase pass paints the area outside the rounded rect in the parent
  colour. Its `eraseEvent` (a parent-`wxClientDC` blit) is commented out of the event table as
  "not work". This is an intentional deviation from "custom-painted windows use PAINT": `doRender`
  draws a rounded rect and leaves the corners to the erase.
- Owner-drawn panels that fill everything use `PAINT`: the card panels (`PurgeModeBtnPanel`,
  `CapsuleButton`), `DropDown`, and `LabeledStaticBox` (off macOS, with a full `dc.Clear()` first).
- `wxGLCanvasBase`'s ctor already sets `wxBG_STYLE_PAINT` (`src/common/glcmn.cpp:68-73`);
  `OpenGLManager::create_wxglcanvas` sets it again (redundant, harmless). Never reset it on a GL
  canvas, and keep `PAINT` on any window you paint entirely yourself: a non-`PAINT` style erases
  before every paint, which shows as a blank flash (c06a0223a7). GL specifics:
  `references/webview-gl-aui-media.md`.

**Pitfalls.**
- **Rule:** `PAINT` only with full coverage; clear first.
  **Why:** unpainted pixels are "undetermined" (`interface/wx/defs.h:524-526`); with a buffered DC they hold
  another window's pixels (see below); with the AMS `Blit` pattern they would be read back as garbage.
  ```cpp
  // Wrong: SetBackgroundStyle(wxBG_STYLE_PAINT); … dc.DrawRoundedRectangle(rc, r);   // corners unpainted
  // Right: dc.SetBackground(wxBrush(StaticBox::GetParentBackgroundColor(GetParent())));
  //        dc.Clear(); dc.DrawRoundedRectangle(rc, r);
  ```
  Same for early returns: a `PAINT` handler that returns early when it has nothing to draw (no
  items, no selection) must still clear.
- **Rule:** `TRANSPARENT` between the default ctor and `Create()`.
  ```cpp
  // Wrong: MyPanel(wxWindow* p) : wxPanel(p) { SetBackgroundStyle(wxBG_STYLE_TRANSPARENT); }  // returns false, ignored
  // Right: MyPanel(wxWindow* p) { SetBackgroundStyle(wxBG_STYLE_TRANSPARENT); Create(p); }
  ```
  Cite: `interface/wx/window.h:2495-2511`; `src/common/wincmn.cpp:1616-1625`.
- **Rule:** Never `wxBG_STYLE_COLOUR`; use `SetBackgroundColour(c)` with ERASE or SYSTEM.
  Cite: `interface/wx/defs.h:533`.

## Buffered painting and double buffering

**Contract.**
- `wxAutoBufferedPaintDC` "is simply a typedef of wxPaintDC on platforms that have native
  double-buffering, otherwise … wxBufferedPaintDC" (`interface/wx/dcbuffer.h:154-160`). [source]
  The choice is compile-time: `// Only wxMSW doesn't use double buffering` →
  `wxALWAYS_NATIVE_DOUBLE_BUFFER 0` on MSW (`include/wx/dcbuffer.h:18-23`). On MSW it is always a
  `wxBufferedPaintDC`, whatever `IsDoubleBuffered()` says; `wxAutoBufferedPaintDCFactory(win)` is
  the runtime variant that checks `IsDoubleBuffered()`.
- Both buffered paint DCs require `SetBackgroundStyle(wxBG_STYLE_PAINT)` "somewhere in the class
  initialization code" (`interface/wx/dcbuffer.h:149-151, 189-192`); the `wxAutoBufferedPaintDC` ctor asserts it
  (`include/wx/dcbuffer.h:227-231`), compiled out in Orca.
- With `wxScrolled`, do not call `PrepareDC()` on a `wxBufferedPaintDC`; it already does
  (`interface/wx/dcbuffer.h:193-196`).
- [source] Without an explicit bitmap the buffer is one process-wide bitmap
  (`src/common/dcbufcmn.cpp` `wxSharedDCBufferManager`), created with
  `CreateWithLogicalSize(size, dc->GetContentScaleFactor())` (DPI-correct on every port; 24 bpp on
  MSW) and reused by every window. It holds the last user's pixels. The buffered DC copies the paint
  DC's attributes (`wxBufferedDC::UseBuffer` → `CopyAttributes`), so `dc.Clear()` uses the window
  background colour.
- `wxGraphicsContext::Create(autoBufferedDC)` resolves to the `wxMemoryDC` overload on MSW and the
  `wxWindowDC` overload elsewhere; both work. Delete the context before the buffered DC goes out of
  scope (its destructor blits); Direct2D/Cairo may defer drawing until `Flush()` or destruction
  (`interface/wx/graphics.h:488-497`).

**Usage.**
```cpp
SetBackgroundStyle(wxBG_STYLE_PAINT);                    // ctor, before the first paint
void MyCard::OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(StaticBox::GetParentBackgroundColor(GetParent())));
    dc.Clear();                                          // mandatory: shared, stale buffer
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    gc->SetBrush(wxBrush(StateColor::darkModeColorFor(m_fill)));
    gc->SetPen(wxPen(StateColor::darkModeColorFor(m_border), 1));
    gc->DrawRoundedRectangle(1, 1, GetClientSize().x - 2, GetClientSize().y - 2, FromDIP(8));
}                                                        // gc destroyed before dc
```

**Native double buffering (`SetDoubleBuffered`).**
- MSW [source]: `IsDoubleBuffered()` is true only if `WS_EX_COMPOSITED` is set on the window or an
  ancestor up to the TLW (`src/msw/window.cpp` `wxWindowMSW::IsDoubleBuffered`);
  `SetDoubleBuffered(on)` toggles that style; `MSWDisableComposited()` removes it up the chain.
  `WS_EX_COMPOSITED` makes the whole subtree paint bottom-up into one buffer.
- wx 3.3.0 enabled double buffering for all MSW windows (#22851, `docs/changes.txt:557`) and 3.3.2
  reverted it (#25808, `docs/changes.txt:308`). In 3.3.2 nothing in wx sets `WS_EX_COMPOSITED`
  unless the app calls `SetDoubleBuffered(true)`: MSW windows are not double-buffered by default,
  exactly as in 3.2.
- macOS: always double-buffered, `IsDoubleBuffered()` returns `true` (`include/wx/osx/window.h:161`);
  `SetDoubleBuffered` is the base no-op (`include/wx/window.h:1169`). GTK: native buffering;
  `SetDoubleBuffered` maps to the deprecated `gtk_widget_set_double_buffered`.
- [source] Before `Create()` it is a silent no-op on every port: MSW updates a null HWND
  (`include/wx/msw/private/winstyle.h`), GTK hits `wxCHECK_RET(m_widget)` (`src/gtk/window.cpp:5991`),
  macOS is the base no-op.

**OrcaSlicer.** `DropDown` (a `PopupWindow`) uses `SetBackgroundStyle(wxBG_STYLE_PAINT)` and an
explicit `wxBufferedPaintDC` on all ports — redundant on GTK3/macOS but correct, since the shared
buffer is DPI-aware. `DropDown::render` does not clear: it covers every pixel only while `radius` is
0 (the default) and the item list is non-empty (it returns before drawing when there are no items),
so a `SetCornerRadius` or an empty-list path would show stale shared-buffer pixels — clear first if
you change either. `PurgeModeBtnPanel::OnPaint` and `CapsuleButton::OnPaint` are the card shape
(`wxAutoBufferedPaintDC` + `wxGraphicsContext` + `dc.Clear()`); see
[clickable cards](#clickable-card-panels-wxpanel--wxbg_style_paint) for what not to copy from them.

**Pitfalls.**
- **Rule:** Buffered DC ⇒ `PAINT` style set in the ctor.
  **Why:** documented requirement; the assert is compiled out, so the symptom is flicker (erase,
  then blit) and nothing else.
- **Rule:** Clear the whole buffer first.
  **Why:** the shared buffer holds pixels of the last window that used it (`src/common/dcbufcmn.cpp`).
- **Rule:** `SetDoubleBuffered(true)` after `Create()`, MSW only, and only when a buffered DC in the
  widget itself does not fix the flicker.
  **Why:** before creation it does nothing; `WS_EX_COMPOSITED` changes painting of the whole subtree,
  which is why wx itself backed it out in 3.3.2.
  ```cpp
  // Wrong: MyWidget() { SetDoubleBuffered(true); wxWindow::Create(parent, …); }
  // Right: MyWidget() { wxWindow::Create(parent, …);
  //        #ifdef __WXMSW__
  //            SetDoubleBuffered(true);
  //        #endif
  //        }
  ```

## Alpha, anti-aliasing, wxGCDC and wxGraphicsContext

**Contract.**
- "In general wxDC methods don't support alpha transparency and the alpha component of wxColour is
  simply ignored … under macOS and GTK+ 3 colours with alpha channel are supported in all the normal
  wxDC-derived classes as they use wxGraphicsContext internally" (`interface/wx/dc.h:820-830`).
- `wxGCDC` offers the wxDC API on a `wxGraphicsContext` (`interface/wx/dc.h:767-770`). Not
  implemented: `GetPixel`, `FloodFill`; `SetLogicalFunction` and the `Blit` raster op only work with
  `wxCOPY/wxOR/wxNO_OP/wxCLEAR/wxXOR`; Direct2D supports TrueType fonts only (`interface/wx/dcgraph.h:12-32`).
- `wxGCDC(wxGraphicsContext*)` takes ownership, and the context "will continue using the same
  font, pen and brush" until `SetFont/SetPen/SetBrush` (`interface/wx/dcgraph.h:59-73`). [source] A `wxGCDC` built
  from a DC starts with pen = black, brush = white, font = `*wxNORMAL_FONT` — not the window font
  (`src/common/dcgraph.cpp` `wxGCDCImpl::Init`).
- Default renderer: Core Graphics on macOS, GDI+ on MSW, Cairo on GTK (`interface/wx/graphics.h:1810-1814`).
  Orca never selects Direct2D.
- `SetInterpolationQuality` is "Not implemented in the Cairo backend" (`interface/wx/graphics.h:1291`).
  [source] `wxGCDC::SetLogicalFunction(wxXOR)` silently switches anti-aliasing off (`src/common/dcgraph.cpp`
  `wxGCDCImpl::SetLogicalFunction`).
- `dc.GetGraphicsContext()` gives the context for paths, arcs and transforms on a `wxGCDC` or a
  GC-backed paint DC; it can be `nullptr` for a plain GDI `wxPaintDC` on MSW (`interface/wx/dc.h:1943-1948`).

**Platforms.**

| | MSW | macOS | GTK3 X11 / Wayland | GTK2 (opt-out) |
|---|---|---|---|---|
| Plain `wxPaintDC` alpha / AA | no / no (GDI) | yes / yes | yes / yes | no / no (GDK) |
| `wxPaintDC` implementation [source] | GDI | `wxWindowDCImpl : wxGCDCImpl` (`src/osx/carbon/dcclient.cpp`) | `wxGTKCairoDCImpl` (`src/gtk/dc.cpp`) | GDK |
| Default GC renderer | GDI+ | Core Graphics | Cairo | Cairo |

So a rounded rect or circle drawn with a plain `wxPaintDC` is smooth on macOS and Linux and
jagged on Windows, and a semi-transparent colour is honoured on macOS and Linux and painted opaque
on Windows.

**OrcaSlicer — the MSW anti-aliasing pattern.** Orca widgets get AA on MSW by drawing through a
`wxGCDC` over a memory DC and blitting the result; elsewhere they draw straight into the paint DC.
Two variants exist:

```cpp
// StaticBox::render (radius != 0) and LabeledStaticBox::PickDC: fill with the window colour
#ifdef __WXMSW__
    wxMemoryDC memdc(&dc);
    wxBitmap bmp(size.x, size.y);                 // physical == logical on MSW only
    memdc.SelectObject(bmp);
    memdc.SetBackground(wxBrush(GetBackgroundColour()));
    memdc.Clear();
    { wxGCDC dc2(memdc); doRender(dc2); }         // AA drawing; dc2 dies before deselect
    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    doRender(dc);
#endif

// AMS/device widgets (AMSItem.cpp, FanControl.cpp, …): copy the already-erased window pixels
    memdc.Blit({0, 0}, size, &dc, {0, 0});        // instead of SetBackground + Clear
```

- The purpose is **anti-aliasing and alpha** on MSW, where the paint DC is GDI; reduced flicker
  (one blit) is a side effect, not the reason.
- The `Blit` variant reads back the pixels the erase pass painted. It works only because these
  widgets keep the default `ERASE` style and a GDI paint DC is readable; under `PAINT` it would read
  undetermined pixels, and on GC-backed ports it is meaningless — hence the `#ifdef`.
- The raw `wxBitmap(size.x, size.y)` is valid only because the block is MSW-only, and only for
  geometry: its scale factor is 1, so text `doRender` draws through `dc2` is sized for 96 DPI —
  too small on a 125–200 % monitor. [source] the GDI+ context of a memory DC takes its DPI from
  the selected bitmap's scale factor and has no window (`src/msw/graphics.cpp`
  `wxGDIPlusRenderer::CreateContext(const wxMemoryDC&)`, `wxGDIPlusContext::GetDPI`), the same rule
  as for the memory DC itself (`references/dpi-bitmaps-fonts.md §wxFont`).
- Text split: `Button::render` calls `StaticBox::render(dc)` for the frame, then draws the icon and
  label directly on the original paint DC (window font, native GDI text rendering);
  `SideButton::paintEvent` makes the same split explicitly — shapes through `wxGCDC` on MSW, text
  through the raw `wxPaintDC`. Both centre the label from its measured extent on every port; the
  macOS-only vertical offsets they once applied were removed with the wx 3.3 upgrade (80c958d98b, in
  8248b06337), so do not re-add per-OS baseline nudges.
- `AxisCtrlButton::paintEvent` wraps the paint DC in `wxGCDC gcdc(dc)` on all ports and uses
  `dc.GetGraphicsContext()` for paths and arcs.
- `doRender` methods call `dc.SetFont(...)` and resolve colours themselves, as `wxGCDC` requires.

**Pitfalls.**
- **Rule:** Semi-transparent colours and smooth curves on MSW go through `wxGCDC`/`wxGraphicsContext`.
  **Why:** GDI ignores alpha (`interface/wx/dc.h:820-830`). Corollary: a `StateColor` with no matching entry
  returns `wxColour(0, 0, 0, 0)`, which paints **black** through a GDI DC on MSW (a plain `wxPaintDC`,
  StaticBox's radius-0 path) and nothing through a `wxGCDC` or on macOS/GTK3.
  ```cpp
  // Wrong (MSW): wxPaintDC dc(this); dc.SetBrush(wxColour(0, 150, 136, 80)); dc.DrawRoundedRectangle(rc, 6);
  // Right:       wxPaintDC dc(this); wxGCDC gdc(dc); gdc.SetBrush(wxColour(0, 150, 136, 80)); gdc.DrawRoundedRectangle(rc, 6);
  ```
- **Rule:** Set font and text colour on every `wxGCDC` before drawing text.
  ```cpp
  // Wrong: wxGCDC gdc(dc); gdc.DrawText(label, pt);                      // wxNORMAL_FONT, black
  // Right: wxGCDC gdc(dc); gdc.SetFont(GetFont());
  //        gdc.SetTextForeground(text_color.colorForStates(states)); gdc.DrawText(label, pt);
  ```
  Cite: `src/common/dcgraph.cpp` `wxGCDCImpl::Init`.
- **Rule:** Null-check `GetGraphicsContext()` unless the DC is a `wxGCDC`.

## Memory DCs and bitmaps

**Contract** (`interface/wx/dcmemory.h`).
- "A bitmap must be selected into the new memory DC before it may be used"; select it out
  (`SelectObject(wxNullBitmap)` or destroy the DC) before using the bitmap elsewhere; a bitmap cannot
  be selected into two DCs (`interface/wx/dcmemory.h:16-39, 112-114`).
- "the scaling factor of the bitmap determines the scaling factor used by this device context"
  (`interface/wx/dcmemory.h:41-58`); `SelectObject` "changes the scale factor of this device context … to be the
  same as the bitmap scale factor" (`interface/wx/dcmemory.h:116-118`).
- `wxMemoryDC(wxDC*)`: compatible with that DC "in wxMSW, the argument is ignored in the other
  ports"; "the DPI scaling factor is @e not inherited from @a dc" (`interface/wx/dcmemory.h:81-88`).
- Bitmap sizes: ctor sizes and `GetWidth/GetHeight` are **physical** pixels
  (`docs/doxygen/overviews/high_dpi.md:170-178`). `CreateWithLogicalSize(size, scale)` gives physical =
  size on MSW and size × scale on GTK3/macOS (`interface/wx/bitmap.h:524-556`); `GetLogicalSize()`
  is what you lay out with (`interface/wx/bitmap.h:686-706`). The scale factor "is only used in the ports where
  logical pixels are not the same as physical ones" and `GetScaleFactor()` "always returns 1 under
  the other platforms" (`interface/wx/bitmap.h:745-757`); [source] wxMSW still stores it,
  `GetScaleFactor()` returns it, and it drives `GetDIPSize()`, the memory DC's
  `GetContentScaleFactor()` and its text size — not its coordinates
  (`src/msw/gdiimage.cpp` `wxGDIImage::GetScaleFactor`, `src/msw/dcmemory.cpp`; details in
  `references/dpi-bitmaps-fonts.md §wxBitmap`), while `GetLogicalSize()` stays the physical size there.
- MSW `wxBitmap::Create(size, dc)` no longer multiplies by the DC content scale; the size is
  physical (`docs/changes.txt:94-96`).

**Usage — portable back buffer.**
```cpp
wxBitmap bmp;
bmp.CreateWithLogicalSize(GetClientSize(), GetDPIScaleFactor());
wxMemoryDC mdc(bmp);                             // DC scale = bitmap scale
mdc.SetBackground(wxBrush(GetBackgroundColour())); mdc.Clear();
mdc.SetFont(GetFont());                          // memory DCs use system defaults
… draw in logical pixels …
mdc.SelectObject(wxNullBitmap);
dc.DrawBitmap(bmp, 0, 0);
```

**OrcaSlicer — `SwitchButton::Rescale` (labelled switches).** Pre-renders the on/off track
bitmaps per platform:
- macOS: measures with the font scaled by `Slic3r::GUI::mac_max_scaling_factor()`, draws into a
  `wxImage`-backed bitmap at that physical size with alpha zeroed (no `wxGCDC`: the macOS memory DC
  is already Core Graphics), then re-wraps it as `wxBitmap(img, -1, scale)` so its logical size is
  physical / scale.
- MSW: an opaque scale-1 bitmap cleared to the background colour, drawn through `wxGCDC` with the
  font `Scaled(GetDPIScaleFactor())` (compensating the scale-1 memory DC's 96-DPI text), then
  `bmp.SetScaleFactor(GetDPIScaleFactor())`.
- GTK: the `wxImage` path at scale 1 (the GTK3 memory DC is Cairo, already AA).
- `mac_max_scaling_factor()` (`src/slic3r/Utils/MacDarkMode.mm`, `__APPLE__` only; also used by
  `BitmapCache` for the SVG raster scale): despite the name, its loop reads `objectAtIndex:0` on
  every iteration, so it returns the **first** screen's `backingScaleFactor` (at least 1), not the
  maximum over screens.
- `CreateWithLogicalSize` + `wxMemoryDC` is the portable equivalent; prefer it in new code.

**Pitfalls.**
- **Rule:** Size back buffers in logical pixels with the window's DPI factor.
  **Why:** the ctor size is physical, so a `GetSize()`-sized bitmap is half-resolution (blurry) on
  Retina and GTK3 HiDPI.
  ```cpp
  // Wrong (all platforms): wxBitmap bmp(GetSize().x, GetSize().y);
  // Right:                 wxBitmap bmp; bmp.CreateWithLogicalSize(GetClientSize(), GetDPIScaleFactor());
  ```
  Cite: `docs/doxygen/overviews/high_dpi.md:170-178`; `interface/wx/dcmemory.h:41-58`.
- **Rule:** Destroy the `wxGCDC` and deselect the bitmap before blitting or storing it.
  **Why:** a bitmap still selected (or still being drawn by a deferred GC) is incomplete or unusable.

## DC coordinates and scale under DPI

**Contract.**
- Window, `wxDC` and `wxGraphicsContext` coordinates are logical pixels
  (`docs/doxygen/overviews/high_dpi.md:95-97`); `wxGLCanvas` drawing is in physical pixels (`docs/doxygen/overviews/high_dpi.md:100-106`, see
  `references/webview-gl-aui-media.md`). Convert design sizes with the window's `FromDIP`
  (`references/dpi-bitmaps-fonts.md`).
- `wxDC::GetContentScaleFactor()` "Returns the same value as wxWindow::GetDPIScaleFactor() for the
  device contexts associated with a window and … wxBitmap::GetScaleFactor() … for wxMemoryDC";
  "since wxWidgets 3.1.6, this function does _not_ return the same value as
  wxWindow::GetContentScaleFactor()" (`interface/wx/dc.h:153-168`), which is "always 1.0" on MSW
  (`interface/wx/window.h:1587-1592`).

[source] What `dc.GetContentScaleFactor()` returns depends on the DC type:

| DC | MSW | macOS / GTK3 |
|---|---|---|
| Window/paint DC | window DPI factor, e.g. 1.5 (`src/msw/dc.cpp:254`) | backing/GDK scale, e.g. 2 |
| `wxMemoryDC` | bitmap scale; 1.0 for `wxBitmap(w, h)` (`src/msw/dcmemory.cpp:139`) | bitmap scale |
| user-created `wxGCDC` | always 1.0: `wxGCDCImpl` never sets `m_contentScaleFactor` (`include/wx/dc.h:682`) | always 1.0 |

The same drawing code therefore sees 1.5 through StaticBox's MSW radius-0 path and 1.0 through
its MSW `wxGCDC` path. `wxDC::FromDIP` (`interface/wx/dc.h:196-242`) scales by the DC's `GetPPI()`
on MSW [source] (`src/common/dcbase.cpp`) and is identity on GTK3/macOS; on an MSW memory DC with a
scale-1 bitmap it does not scale.

**OrcaSlicer.** `StaticBox::doRender` snaps the border rectangle with `dc.GetContentScaleFactor()`,
so the snapping differs between its MSW radius-0 path (paint DC) and radius ≠ 0 path (`wxGCDC`).
In widget drawing code use the window's `FromDIP()`/`GetDPIScaleFactor()`, not DC-derived scales.
`ScalableBitmap::GetBmpSize()` returns `GetScaledSize()` on Apple and the physical `GetSize()`
elsewhere — consistent with Orca's bitmap pipeline, not a general wx rule; for bitmaps you create,
use `GetLogicalSize()`.

**Scrolled windows.** A `wxScrolled<>` window either overrides `OnDraw(wxDC&)`, which the default
paint handler calls with a DC already prepared by `DoPrepareDC()` (device origin at the scroll
position, `SetScale()` applied), or handles `wxEVT_PAINT` itself and must call `DoPrepareDC(dc)` on
its paint DC (`interface/wx/scrolwin.h:73-80`, `:282-300`, `:482-491`). `DoPrepareDC` takes a
`wxPaintDC`, not a `wxInfoDC`; for coordinate maths outside painting (mouse positions) use
`DoPrepareReadOnlyDC(dc)` on a `wxInfoDC` (since 3.3.0, `:302-336`). A `wxBufferedPaintDC` is
already prepared ([Buffered painting](#buffered-painting-and-double-buffering)).

**Pitfalls.**
- **Rule:** Never divide a bitmap size by `dc.GetContentScaleFactor()`.
  **Why:** on MSW a window DC reports the DPI factor while `DrawBitmap` draws the bitmap at its
  physical size, so the computed position is off by the factor.
  ```cpp
  // Wrong: pos.x += (gap - bmp.GetWidth() / dc.GetContentScaleFactor()) / 2;
  // Right: pos.x += (gap - bmp.GetLogicalSize().x) / 2;        // or ScalableBitmap::GetBmpSize()
  ```
  Cite: `interface/wx/dc.h:153-168`.

## Measuring text without painting

**Contract.**
- `wxClientDC` is deprecated: "should not be used any longer, please use wxInfoDC instead for
  obtaining information" (`interface/wx/dcclient.h:41-46`); `wxClientDC`, `wxWindowDC` and
  `wxScreenDC` "are deprecated and don't work on all platforms any longer" (`interface/wx/dc.h:776-780`).
- `wxInfoDC`: "Unlike wxPaintDC, objects of this class can be created at any time"
  (`interface/wx/dc.h:1966`). The docs declare it `: public wxReadOnlyDC`; [source] the real header
  makes it a standalone wrapper around a `wxClientDC` with implicit conversions to `wxReadOnlyDC&`.
  Either way it does **not** derive from `wxDC` (`include/wx/dcclient.h:65-233`, `wxHAS_INFO_DC`). It works with `const wxReadOnlyDC&`
  APIs such as `wxControl::Ellipsize(label, const wxReadOnlyDC&, …)` (`interface/wx/control.h:425`),
  not with helpers taking `wxDC&`.
- Simplest: no DC at all. `wxWindow::GetTextExtent(str, &w, &h, &descent, &externalLeading, &font)`
  — "Font to use instead of the current window font (optional)" (`interface/wx/window.h:2316-2350`);
  `wxSize GetTextExtent(str)` uses the window font.
- Semantics (`interface/wx/dc.h`): `GetTextExtent` "only works with single-line strings" and gives
  0×0 for an empty string (`interface/wx/dc.h:683-687`); `GetMultiLineTextExtent` handles `\n`, and an empty
  string gets one line's height, which "differs from that of GetTextExtent()" (`interface/wx/dc.h:614-621`);
  `GetPartialTextExtents` returns cumulative per-character widths (`interface/wx/dc.h:653-669`); `GetFontMetrics()`
  is correct "only under wxMSW" (`interface/wx/dc.h:595-600`).
- Only `wxPaintDC` starts with the window font; other DCs "use system-default values so you always
  must set the appropriate fonts and colours" (`interface/wx/dc.h:775-780`). `GetFont()` returns `wxNullFont` until
  `SetFont()` is called (`interface/wx/dc.h:715-720`).
- wx 3.3 moved the non-drawing members to `wxReadOnlyDC` and several overridable measuring virtuals
  now take it (`docs/changes.txt:228-231`); see `references/wx-33-changes.md`.

**OrcaSlicer.** Orca widgets measure in `messureSize()`. A `messureSize()` that builds a
`wxClientDC dc(this)` (`Button::messureSize`, `TextInput::messureSize`) measures correctly on every
port but uses the deprecated class. New code: use the window `GetTextExtent` with an explicit font,
as `LabeledStaticBox::Create` does
(`GetTextExtent(label, &w, &h, &descent, &externalLeading, &m_font)`), or `wxInfoDC`.
`Label::split_lines(wxDC&, …)` takes a `wxDC&`, so it cannot be called with a `wxInfoDC` — call it
from the paint path (as `Button::render` does) or give it a `wxReadOnlyDC&` parameter.

**Pitfalls.**
- **Rule:** No new `wxClientDC` for measuring.
  ```cpp
  // Wrong: wxClientDC dc(this); dc.SetFont(f); int w = dc.GetTextExtent(s).x;
  // Right: int w, h; GetTextExtent(s, &w, &h, nullptr, nullptr, &f);
  // Right: wxInfoDC dc(this); dc.SetFont(f); wxSize sz = dc.GetTextExtent(s);
  ```
  **Why:** deprecated for this use; switching to `wxInfoDC` turns helpers that take `wxDC&` into
  compile errors (convert them to `const wxReadOnlyDC&`).
- **Rule:** Overrides of wx 3.3 measuring virtuals use `wxReadOnlyDC&` and `override`.
  **Why:** without `override` a stale `wxDC&` signature silently becomes an overload that wx never calls.

## Invalidation: Refresh, RefreshRect, Update

**Contract** (`interface/wx/window.h:2376-2425`).
- `Refresh()` "Causes this window, and all of its children recursively, to be repainted … only
  during the next event loop iteration". `eraseBackground`: "in non-MSW ports background is always
  erased".
- `RefreshRect(rect)` invalidates only that rectangle; pair it with `GetUpdateRegion()` in the
  handler (`interface/wx/dcclient.h:52-58`).
- `Update()` repaints the invalidated area immediately; it "doesn't do anything in wxGTK port when
  using Wayland", is not recommended ("perform all time consuming operations in background threads"),
  and "doesn't invalidate any area … Use Refresh() first".
- `Freeze()`/`Thaw()` and `wxWindowUpdateLocker`: `references/sizers-layout.md`.

**Platforms** [source]:

| | MSW | macOS | GTK3 X11 | GTK3 Wayland |
|---|---|---|---|---|
| `Refresh()` | `RedrawWindow(RDW_INVALIDATE \| RDW_ALLCHILDREN [\| RDW_ERASE])`; `eraseBackground=false` honoured (`src/msw/window.cpp:1679`) | dropped while `!IsShownOnScreen()` or `IsFrozen()`; `Thaw` re-marks the view (`src/osx/window_osx.cpp` `wxWindowMac::Refresh`) | invalidates only when mapped | same |
| `Update()` | `UpdateWindow` + `GdiFlush` | updates the whole TLW | processes pending updates | **no-op** (`src/gtk/window.cpp` `wxWindowGTK::Update`, it broke later updates, #25036) |
| Paint at all | — | only when `IsShownOnScreen()` (`MacDoRedraw`) | — | — |

**MSW modal size/move loop.** wx idle events are generated only by wx's own event loop
(`src/common/evtloopcmn.cpp` `ProcessIdle`); during an interactive resize or move Windows runs its
own modal loop, and wxMSW turns `WM_ENTERSIZEMOVE`/`WM_EXITSIZEMOVE` into `wxEVT_MOVE_START/END`
only. `WM_PAINT` still arrives, and pending events (`CallAfter`, `QueueEvent`) still run through a
message hook, but `wxEVT_IDLE` does not. A renderer that only sets a dirty flag in `on_paint` and
renders from idle stays blank for the whole drag.

```cpp
void GLCanvas3D::on_paint(wxPaintEvent&) {          // shape from c06a0223a7
#ifdef __WXMSW__
    _refresh_if_shown_on_screen(); m_dirty = false;  // render now: idle never runs while resizing
#else
    m_dirty = true;                                  // idle-driven rendering elsewhere
#endif
}
```
Cite: c06a0223a7 (`GLCanvas3D::on_paint`, `OpenGLManager::create_wxglcanvas`). The GL canvas
side (`wxGLCanvas` paint, `SetCurrent`, `wxBG_STYLE_PAINT`) is in `references/webview-gl-aui-media.md`.

**Pitfalls.**
- **Rule:** No `Refresh(); Update();` to force progress or animation inside a long handler or loop.
  **Why:** `Update()` is a no-op on Wayland, and on macOS the `Refresh()` before it is dropped while
  the window is hidden or frozen; the UI stays frozen. Return to the event loop (`CallAfter`, `wxTimer`, worker thread —
  `references/threads-timers-app.md`).
- **Rule:** Animate with a `wxTimer` that updates state and calls `Refresh()`; stop it when hidden.

## Writing a custom control (wx side)

**Base class.** The overview template derives from `wxControl` or `wxWindow` and overrides
`DoGetBestSize()` plus `OnPaint` (`docs/doxygen/overviews/customwidgets.h:51-114`). Differences:
- `wxControl::ShouldInheritColours()` returns `true`; `wxWindow` returns `false`
  (`interface/wx/window.h:2646-2653`, `include/wx/control.h:98`).
- `wxControl` adds label/mnemonic handling and validators; [source] its MSW `Create` is just
  `wxWindow::Create` + `SetValidator` (`src/msw/control.cpp`).
- `wxPanel` is `wxNavigationEnabled<wxWindow>` (`include/wx/panel.h:27`).
- Two-step creation (default ctor, then `Create(...)`) is required whenever something must precede
  window creation (`wxBG_STYLE_TRANSPARENT`, an initially disabled window — `interface/wx/window.h:3115-3127`).

**Focus.**
- `AcceptsFocus()`: return `false` for display-only widgets. `AcceptsFocusFromKeyboard()` (or
  `DisableFocusFromKeyboard()`): stay out of TAB order while remaining clickable
  (`interface/wx/window.h:472-488`). [source] A plain `wxWindow` accepts focus by default
  (`include/wx/window.h:727`).
- When `AcceptsFocus()`'s answer changes at runtime, call `SetCanFocus()` ("call this when the return
  value of AcceptsFocus() changes", `include/wx/window.h:768-769`). It is a hint "only implemented by
  ports which have support for native TAB traversal" and does not stop a programmatic `SetFocus()`
  (`interface/wx/window.h:538-548`); [source] only wxGTK implements it (`src/gtk/window.cpp`
  `wxWindowGTK::SetCanFocus`), elsewhere the base is a no-op — so also override `AcceptsFocus()`.
- `wxNavigationEnabled<W>` gives TAB navigation among children; it has only a default ctor, so call
  `W::Create()` (`interface/wx/containr.h:10-66`). [source] The container makes itself unfocusable
  while it has focusable children (`SetCanFocus(acceptsSelf && !children)`, `src/common/containr.cpp`
  `UpdateParentCanFocus`) and turns on `wxTAB_TRAVERSAL` when such a child is added
  (`include/wx/containr.h:223-233`, needed on MSW).

**Attributes.** `InheritAttributes()` "is (or should be, in case of custom controls) called during
window creation"; only attributes "explicitly … changed" on the parent are taken
(`interface/wx/window.h:4053-4075`). [source] It runs once, at create time; foreground colour is
inherited only if `ShouldInheritColours()`; background colour is **never** inherited — the code is
`#if 0` with "inheriting (solid) background colour is wrong" (`src/common/wincmn.cpp:1543-1552`).
Override `GetDefaultAttributes()`/`GetClassDefaultAttributes()` (`interface/wx/window.h:2262-2281`)
when the class has a fixed palette.

**Best size.** `GetBestSize()` is non-virtual: "Override virtual DoGetBestSize() or, better …
DoGetBestClientSize()" (`interface/wx/window.h:1338-1340`; `docs/doxygen/overviews/windowsizing.h`),
and call `InvalidateBestSize()` when content changes. Sizer interplay: `references/sizers-layout.md`.

**Enabling.** "when a parent window is disabled, all of its children are disabled as well"
(`interface/wx/window.h:3115-3127`). [source] Propagation never calls the child's virtual
`Enable()`: on MSW and macOS it calls the protected virtual `DoEnable()` on each child, recursively,
skipping children disabled on their own (`IsThisEnabled()` false) and not descending at all when a
top-level window is disabled, as during a modal dialog
(`src/common/wincmn.cpp` `wxWindowBase::NotifyWindowOnEnableChange`); on GTK
(`wxHAS_NATIVE_ENABLED_MANAGEMENT`) wx relies on native sensitivity and calls nothing on the
children. `IsEnabled()` is correct either way; `IsThisEnabled()` is the window's own flag.

**Resize repaint.** `wxFULL_REPAINT_ON_RESIZE` "applies on GTK+ 2 and Windows only, and full
repainting is always done on other platforms" (`interface/wx/window.h:253-262`); [source] contrary to
that, GTK3 also wires it to `gtk_widget_set_redraw_on_allocate`, so GTK3 does not fully repaint
without it (`src/gtk/window.cpp:3200`). Without it, growing a size-dependent drawing (centred text, a border at
the edge) leaves stale strips.

**Handler order.** Dynamic `Bind` handlers run before event-table entries and "in reverse order of
their registration" (`docs/doxygen/overviews/eventhandling.h:474-482`). A subclass's
`Bind(wxEVT_PAINT, …)` therefore shadows a base `EVT_PAINT` table entry (and a base `Bind` made
earlier) unless it calls `Skip()`; static entries are searched derived class first. Full rules:
`references/events.md`.

**`wxRendererNative`.** Generic controls draw check boxes, tree buttons, selection rectangles
through it (`interface/wx/renderer.h:280-287`); native controls (native `wxDataViewCtrl` on
GTK/macOS) do not. The `win` parameter "should only be used as a generic wxWindow" and "each drawing
function restores the wxDC attributes if it changes them" (`interface/wx/renderer.h:290-307`) — overrides must do
the same (`wxDCPenChanger`/`wxDCBrushChanger`, `interface/wx/dc.h:2038-2080`). `Get()` is the current
renderer, `GetDefault()` the native one, `GetGeneric()` the generic one (`interface/wx/renderer.h:556-572`);
`GetCheckBoxSize(win, …)` needs a non-null `win` (`interface/wx/renderer.h:575-585`). `Set()` replaces the
renderer process-wide and returns the previous one (`interface/wx/renderer.h:651-657`). Orca: the `ObjectList`
ctor installs a `wxDelegateRendererNative` subclass (`GUI_ObjectList.cpp` class `wxRenderer`) on MSW
only, so it affects every generic control on Windows, including the generic MSW `wxDataViewCtrl`.
Its overrides set pen and brush without restoring them; when extending it, use the changers above so
other generic controls do not inherit its pen and brush.

**Pitfalls.**
- **Rule:** React to disabling in `DoEnable(bool)` too, or read `IsEnabled()` at paint time.
  ```cpp
  // Wrong: bool Enable(bool e) override { m_grey = !e; Refresh(); return wxWindow::Enable(e); }
  // Right: void DoEnable(bool e) override { wxWindow::DoEnable(e); Refresh(); }   // + paint from IsEnabled()
  ```
  **Why:** ancestors reach children through `DoEnable` (MSW/macOS) or not at all (GTK).
- **Rule:** Read the parent's background colour explicitly at create time and again on theme or
  DPI change; do not expect a custom `wxWindow` to pick it up.
- **Rule:** A frame + inner `wxTextCtrl` composite derives from `wxNavigationEnabled<Base>`.
  **Why:** otherwise TAB lands on the frame (focusable by default) and keystrokes go nowhere.
- **Rule:** Size-dependent drawing: `wxFULL_REPAINT_ON_RESIZE`, or
  `Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(); e.Skip(); })`.
- **Rule:** Do not commit sizes or wrap results computed from a size that has not been laid out yet.
  **Why:** [source] a plain `wxWindow`/`wxPanel` child created with `wxDefaultSize` is 20×20 on every
  port until the first sizer `Layout()` (`include/wx/window.h` `WidthDefault`/`HeightDefault`, used by
  each port's `Create`; native controls take their best size instead), so
  a paint or measure pass can run with a bogus tiny width. Guard it the way
  `TroubleshootDialog.hpp` `CenteredMultiLinePanel::OnPaint` and `UpdateMinSize` do: clear, then
  return while the client width is implausibly small (5ede9711f5). Wrapping:
  `references/sizers-layout.md`.
- **Rule:** Do not `Skip()` a paint event after painting unless the base class must paint as well.
  **Why:** the base handler then runs too (and on MSW a skipped, painted event also runs native
  `OnPaint`, `src/msw/window.cpp` `HandlePaint`).

## The Orca foundation: StaticBox and StateHandler

### StaticBox

`StaticBox` (`src/slic3r/GUI/Widgets/StaticBox.{hpp,cpp}`) is the `wxWindow` base of most Orca
widgets: an owner-drawn rectangle or rounded rectangle with a state-coloured fill and border on a
plain window.

- **API:** `SetCornerRadius(double)`, `SetBorderWidth(int)`, `SetBorderColor(StateColor)`,
  `SetBorderColorNormal(wxColor)`, `SetBorderStyle(wxPenStyle)`, `SetBackgroundColor(StateColor)`,
  `SetBackgroundColorNormal(wxColor)`, `SetBackgroundColor2(StateColor)` (vertical gradient),
  `SetTopMargin(int)` (Orca: leave room above the box, LabeledStaticBox-style), `ShowBadge(bool)`,
  static `GetParentBackgroundColor(wxWindow*)`. Protected members: `radius`, `border_width`,
  `top_margin`, `border_style`, `state_handler`, `border_color`, `background_color`,
  `background_color2`, `badge`.
- **`SetBackgroundColor` ≠ wx `SetBackgroundColour`.** The former sets the `StateColor` fill that
  `doRender` paints; the latter is the wx window colour that the erase pass uses outside the
  rounded rect. They are different things on purpose.
- **Create:** `StaticBox::Create` maps `wxBORDER_NONE` to `border_width = 0`, calls
  `wxWindow::Create`, attaches `border_color`, `background_color`, `background_color2` to the state
  handler, `update_binds()`, then `SetBackgroundColour(GetParentBackgroundColor(parent))` — a
  **snapshot** of the parent background, taken once.
- **`GetParentBackgroundColor(parent)`** returns, for a `StaticBox` parent with a fill, its
  `background_color.defaultColor()` (the midpoint of the two defaults for a gradient); otherwise
  `parent->GetBackgroundColour()`; white without a parent. `defaultColor()` is
  `colorForStates(0)`, and state 0 has the `Enabled` bit clear, so a `Disabled` entry listed before
  the `Normal` one is what children pick up. Colours come back dark-mapped
  (`references/colours-dark-mode.md`).
- **Paint pipeline:** static event table `EVT_SIZE` → `sizeEvent` (`Refresh(); Skip();` — this
  replaces `wxFULL_REPAINT_ON_RESIZE`) and `EVT_PAINT` → `paintEvent` (`wxPaintDC dc(this);
  render(dc);`). `render(wxDC&)` is non-virtual: on MSW with `radius != 0` it uses the memory-DC +
  `wxGCDC` block cleared with `GetBackgroundColour()`
  ([pattern](#alpha-anti-aliasing-wxgcdc-and-wxgraphicscontext)); otherwise it calls `doRender(dc)`
  directly.
- **`virtual void doRender(wxDC&)`** fills the rect/rounded rect with
  `background_color.colorForStates(states)`, strokes `border_color`, draws the gradient when
  `background_color2` is set, honours `top_margin`, and draws the badge. Subclasses either call
  `StaticBox::doRender(dc)` first to keep the box and then draw on top (`StepCtrl::doRender`), or
  replace it entirely (`ModeSwitchButton::doRender`).
- **Background style:** stays `ERASE` by design (see [Background styles](#background-styles-and-erasing)).

**`LabeledStaticBox`** (`Widgets/LabeledStaticBox.cpp`) is the one widget that paints over a native
control: a real `wxStaticBox` subclass (so it can be a `wxStaticBoxSizer` box) with an owner-drawn
rounded border and the label in the gap. Its guards are the model for that situation:
- `SetBackgroundStyle(wxBG_STYLE_PAINT)` everywhere except `__WXOSX__`, and
  `DrawBorderAndLabel` starts with a full `dc.Clear()`.
- A dynamic `Bind(wxEVT_PAINT, …)` → `wxPaintDC` → `PickDC`, which uses the MSW memory-DC +
  `wxGCDC` block cleared with `GetBackgroundColour()`. Being bound after `wxStaticBox::Create`, it
  runs before, and without `Skip()` instead of, the `wxStaticBox::OnPaint` that wxMSW binds there
  (`src/msw/statbox.cpp` `wxStaticBox::UseCustomPaint`).
- macOS: `staticbox_remove_margin` and a `GetBordersForSizer` override so sizer padding matches
  the other ports.
- The label is measured with the window `GetTextExtent(…, &m_font)`; the widget calls
  `SetCanFocus(false)` and `DisableFocusFromKeyboard()`.

### StateHandler

`StateHandler` (`src/slic3r/GUI/Widgets/StateHandler.{hpp,cpp}`) tracks a widget's state bits —
`Enabled 1, Checked 2, Focused 4, Hovered 8, Pressed 16`, negations the same bits `<< 16` — across
the widget **and attached children**, and refreshes the owner when they change.

- **Event chain.** It is a `wxEvtHandler` member constructed with the owner; its ctor calls
  `owner->PushEventHandler(this)` (it sees events before the window) and seeds `Enabled`/`Focused`
  from the owner; its handler always `Skip()`s so the widget's own handlers still run; its dtor
  calls `RemoveEventHandler`. As a member of `StaticBox` it is destroyed before `~wxWindowBase`,
  which is what the pushed-handler rule needs (`references/events.md`).
- **Binding.** `attach({&colorA, &colorB}); update_binds();` binds only the events for states that
  some attached `StateColor` distinguishes: `EVT_ENABLE_CHANGED` (toggles Enabled),
  `wxEVT_CHECKBOX` (toggles Checked), `wxEVT_SET_FOCUS`/`KILL_FOCUS`, `wxEVT_ENTER_WINDOW`/`LEAVE_WINDOW`,
  `wxEVT_LEFT_DOWN`/`LEFT_UP`. Re-run `update_binds()` whenever an attached colour is replaced.
  When a state stops being distinguished, it unbinds the `KILL_FOCUS`/`LEAVE_WINDOW`/`LEFT_UP`
  partners from `owner_` although they were bound on the handler itself, so those `Unbind`s are
  silent no-ops and the partners stay bound (harmless only because clearing a bit is idempotent).
- **Refresh.** Whenever the combined mask (`states_ | states2_`) changes, it calls
  `owner->Refresh()` (or notifies its parent handler). Colours are resolved at paint time with
  `colorForStates(state_handler.states())`.
- **Children.** `attach_child(win)` pushes a child handler on a sub-window (e.g. `TextInput`'s
  inner `wxTextCtrl`) and ORs its bits into `states2_`, because enter/leave/focus events do not
  propagate to parents. `remove_child(win)` detaches it.
- **Programmatic state.** `set_state(state, mask)` (used by `Button::SetValue` for Checked).
- **Enabled state** is driven only by the custom `EVT_ENABLE_CHANGED`, which each widget's
  `Enable()` override must emit (`Button::Enable`, `TextInput::Enable`, `SpinInput::Enable`,
  `ModeSwitchButton::Enable`, `LabeledStaticBox::Enable`, …). A bare `StaticBox` does not emit it. Because the
  event *toggles* the bit, emit it exactly once per real change (use `wxWindow::Enable`'s return
  value). Disabling an ancestor never reaches these overrides (see
  [Enabling](#writing-a-custom-control-wx-side)), so the widget keeps its enabled colours while
  `IsEnabled()` is false: disable Orca widgets individually.
- `StateColor` semantics (first match wins, `Normal` last, `Not*` negation, no match → transparent
  black, the dark map applied on every `colorForStates()` call): `references/colours-dark-mode.md`.

**Pitfalls.**
- **Rule:** `remove_child(child)` before destroying an attached child on its own.
  **Why:** the child handler is pushed on the child; `~wxWindowBase`'s "pushed handlers must have
  been removed" assert is compiled out (`src/common/wincmn.cpp:468-472`), and the later
  `~StateHandler` calls `RemoveEventHandler` on a freed window — a crash, not an assert.
- **Rule:** Every setter that replaces an attached `StateColor` calls `update_binds()` and `Refresh()`.
  **Why:** a colour that distinguishes a new state (Hovered, Pressed) never sees its events otherwise.
- **Rule:** Emit `EVT_ENABLE_CHANGED` through `GetEventHandler()->ProcessEvent(e)` (or
  `ProcessWindowEvent`), not `ProcessEvent` on the window.
  **Why:** the state handler is pushed on the handler stack; `wxWindow::ProcessEvent` bypasses it
  (`interface/wx/window.h:2737-2744`).

## Authoring an Orca widget

A reusable control goes in `src/slic3r/GUI/Widgets/`, in the global namespace like `StaticBox`
and `Button`, registered in `SLIC3R_GUI_SOURCES` (`references/orca-architecture.md`); the catalog
of existing widgets and which raw control each replaces is `references/orca-widgets.md`.

**Choose the base.**
- `StaticBox` for a framed, state-coloured widget.
- `wxNavigationEnabled<StaticBox>` if it hosts focusable children; call `StaticBox::Create(...)`
  (`TextInput`, `SpinInput`, `TempInput`).
- Plain `wxWindow` for pure drawing; own a `StateHandler state_handler{this};` member if colours
  depend on state (`SideButton`).
- `wxPanel` + `wxBG_STYLE_PAINT` for a clickable card that lays out native children
  ([below](#clickable-card-panels-wxpanel--wxbg_style_paint)).

**Skeleton** (example class; every call below exists on the real base classes):
```cpp
class ToggleChip : public StaticBox                    // global namespace
{
public:
    ToggleChip(wxWindow* parent, const wxString& text);
    bool Enable(bool enable = true) override;
    bool SetFont(const wxFont& font) override;
    void Rescale();                                    // called by the owner's on_dpi_changed
protected:
    void doRender(wxDC& dc) override;
private:
    void messureSize();
    StateColor     text_color;
    ScalableBitmap icon;
};

ToggleChip::ToggleChip(wxWindow* parent, const wxString& text)
    : text_color(std::make_pair(0x6B6B6B, (int) StateColor::Disabled),
                 std::make_pair(0x262E30, (int) StateColor::Normal))      // Normal last
{
    radius = 4;                                        // StaticBox members: set before Create
    background_color = StateColor(std::make_pair(0xF0F0F1, (int) StateColor::Disabled),
                                  std::make_pair(0xDBDBDB, (int) StateColor::Hovered),
                                  std::make_pair(*wxWHITE, (int) StateColor::Normal));
    StaticBox::Create(parent, wxID_ANY);               // snapshots the parent background
    state_handler.attach({&text_color});
    state_handler.update_binds();
    icon = ScalableBitmap(this, "edit", 16);           // icon name, real window
    wxWindow::SetLabel(text);
    SetFont(Label::Body_14);                           // measures via the override
}

void ToggleChip::doRender(wxDC& dc)
{
    StaticBox::doRender(dc);                           // fill + border from the StateColors
    const int    states = state_handler.states();
    const wxSize size   = GetSize();
    int x = FromDIP(8);
    if (icon.bmp().IsOk()) {
        const wxSize bs = icon.GetBmpSize();
        dc.DrawBitmap(icon.bmp(), x, (size.y - bs.y) / 2);
        x += bs.x + FromDIP(4);
    }
    dc.SetFont(GetFont());                             // MSW passes a wxGCDC: no window font
                                                       // (and 96-DPI text there: checklist 2)
    dc.SetTextForeground(text_color.colorForStates(states));
    const wxSize ts = dc.GetTextExtent(GetLabel());
    dc.DrawText(GetLabel(), x, (size.y - ts.y) / 2);
}

void ToggleChip::messureSize()                         // Orca convention: measure → SetMinSize
{
    wxSize sz = GetTextExtent(GetLabel());             // window font, no DC
    sz.x += FromDIP(16);
    sz.y  = std::max(sz.y, FromDIP(16)) + FromDIP(8);
    if (icon.bmp().IsOk())
        sz.x += icon.GetBmpSize().x + FromDIP(4);
    SetMinSize(sz);
}

bool ToggleChip::SetFont(const wxFont& font)
{
    bool r = StaticBox::SetFont(font);
    messureSize();
    Refresh();
    return r;
}

bool ToggleChip::Enable(bool enable)
{
    bool changed = StaticBox::Enable(enable);          // wxWindow::Enable: true only on a change
    if (changed) {
        wxCommandEvent e(EVT_ENABLE_CHANGED);
        e.SetEventObject(this);
        GetEventHandler()->ProcessEvent(e);            // toggles the Enabled bit: once per change
    }
    return changed;
}

void ToggleChip::Rescale()
{
    if (!icon.name().empty())
        icon.msw_rescale();                            // re-rasterises at the new DPI and theme
    messureSize();
    Refresh();
}
```

**Checklist.**
1. **Colours:** initialise the `StateColor` members in the ctor before `Create`, attach each one,
   `update_binds()`; setters that replace one call `update_binds()` and `Refresh()`. Resolve them in
   `doRender` with `colorForStates(state_handler.states())`; keep palette colours so the dark map
   applies (`references/colours-dark-mode.md`).
2. **Paint:** either override `doRender` (inherit `StaticBox::paintEvent`/`render` and its MSW AA
   path), or bind `wxEVT_PAINT` → `wxPaintDC` → `render()` with the MSW memory-DC + `wxGCDC` block.
   On MSW with `radius != 0` the `doRender` route hands text a `wxGCDC` over a scale-1 memory
   bitmap (GDI+ text, sized for 96 DPI — [MSW pattern](#alpha-anti-aliasing-wxgcdc-and-wxgraphicscontext));
   to keep native GDI text at the window's DPI, draw the label on the paint DC after
   `StaticBox::render(dc)` from the widget's own paint handler, as `Button::paintEvent`/`Button::render`
   do. Always set font and text colour.
3. **Size:** Orca widgets do not override `DoGetBestSize`; `messureSize()` measures and calls
   `SetMinSize(...)`, and is called from `Create`, `SetLabel`, `SetFont`, `SetIcon`, `Rescale` and
   style setters (`Button::messureSize`, `TextInput::messureSize`). The owner must `Layout()` after
   content changes. `DoGetBestSize`/`DoGetBestClientSize` + `InvalidateBestSize()` is the wx protocol
   and fine for new widgets that are not in the StaticBox family (`Preferences.cpp`
   `WikiLabel::DoGetBestSize`, `BBLTopbar.cpp` `CenteredTitle::DoGetBestSize`). Resize repaint comes from StaticBox's
   `EVT_SIZE` → `Refresh()`; a plain `wxWindow` binds it itself.
4. **Enable:** `bool Enable(bool) override` emitting `EVT_ENABLE_CHANGED` as above. Native inner
   controls do not follow `StateColor`: re-apply their colours after the state change, as
   `TextInput::Enable` does for its `wxTextCtrl`.
5. **Focus:** display-only → `bool AcceptsFocus() const override { return false; }`; out of TAB
   order → `DisableFocusFromKeyboard()` (`LabeledStaticBox::Create` also calls `SetCanFocus(false)`
   for GTK; `SpinInput`'s arrow buttons, `StaticLine`); switchable → keep a flag in sync from a
   `SetCanFocus` override and return it from `AcceptsFocus()` (`Button::SetCanFocus`,
   `Button::AcceptsFocus`). Composites with an inner control use `wxNavigationEnabled<StaticBox>`.
6. **Rescale:** `msw_rescale()` every named `ScalableBitmap`, re-measure, re-apply `FromDIP` sizes,
   `Refresh()` (`Button::Rescale`); make sure the owning `DPIDialog`/panel calls it
   (`references/dpi-bitmaps-fonts.md`). A bitmap set from a raw `wxBitmap` has no name and cannot be
   re-rasterised — the caller must set it again.
7. **Theme:** painted `StateColor`s follow a theme toggle on the next `Refresh()`; the wx background
   colour snapshotted at `Create` does not (an `Update*DarkUI` walk re-maps it only when it is a
   `gDarkColors` key and the walk reaches the widget, `references/colours-dark-mode.md`). Re-apply
   `SetBackgroundColour(StaticBox::GetParentBackgroundColor(GetParent()))` on theme change, as
   `SwitchButton::Rescale` does, and create widgets only after the container's background is set.
8. **Events:** emit the native event type (`wxEVT_BUTTON`, `wxEVT_TOGGLEBUTTON`, …) with id and
   event object through `GetEventHandler()->ProcessEvent` so it propagates like a native command
   event; programmatic setters stay silent (`references/events.md`). Capture the mouse only with
   `HasCapture()` guards and handle `wxEVT_MOUSE_CAPTURE_LOST` (`Button::mouseDown`,
   `Button::mouseCaptureLost`, `references/mouse-keyboard-focus.md`).
9. **GTK:** wrapping a native control → `Slic3r::GUI::RemoveInputBorder`/`RemoveButtonBorder`
   under `__WXGTK__` (`references/platforms.md`).

**Pitfalls.**
- **Rule:** Check any paint handler you copy against the Rules list before reusing it. Reject on
  sight: `wxClientDC` drawing with no paint DC, a paint DC created only on some branches,
  `Refresh(); Update();` loops, `wxBG_STYLE_COLOUR`, and bitmap sizes
  divided by the DC scale.
- **Rule:** In shared `doRender` code compute pixel snapping and DIP sizes from the window
  (`FromDIP`, `GetDPIScaleFactor()`), not from the DC.
  **Why:** `doRender` receives a paint DC on one path and a scale-1 `wxGCDC` on another.

### Clickable card panels (wxPanel + wxBG_STYLE_PAINT)

A card is a `wxPanel` that lays out native children (`wxStaticBitmap`, `wxStaticText`, `Label`)
and paints its own rounded background and border; the whole card is clickable and highlights on
hover. `PurgeModeBtnPanel` (`PurgeModeDialog.cpp`) and `CapsuleButton` (`CapsuleButton.cpp`) have
this shape.

- **Paint:** `SetBackgroundStyle(wxBG_STYLE_PAINT)` in the ctor; `wxEVT_PAINT` →
  `wxAutoBufferedPaintDC` → `dc.Clear()` → `wxGraphicsContext` rounded rect with dark-mapped colours
  → delete the context (the [buffered painting](#buffered-painting-and-double-buffering) usage
  shape). The full clear is mandatory: on MSW the buffer is shared and holds stale pixels.
- **Children:** child controls paint their own background and do not show the card's painted
  fill; give each child the card's fill colour explicitly and update it with the card state, as
  `PurgeModeBtnPanel::UpdateStatus` does.
- **Hover:** `wxEVT_ENTER_WINDOW`/`wxEVT_LEAVE_WINDOW` on the card. Enter/leave fire per window, so
  the card can get a leave when the pointer moves onto one of its children: bind enter/leave on the
  children too and treat the card as hovered while any of them is. Do not resolve it with
  `ScreenToClient(wxGetMousePosition())` — there is no global pointer position on Wayland
  (`references/mouse-keyboard-focus.md`).
- **Clicks:** bind `wxEVT_LEFT_DOWN` on the card and on each child and call the same action.
  - **Rule:** Do not re-dispatch a child click as `wxCommandEvent(wxEVT_LEFT_DOWN)` through
    `this->ProcessEvent`.
    **Why:** the handlers expect a `wxMouseEvent` and receive a `wxCommandEvent` (undefined
    behaviour once they read mouse fields), the command event propagates to every ancestor like a
    click notification, and `wxWindow::ProcessEvent` skips pushed handlers.
    ```cpp
    // Wrong:
    auto fwd = [this](wxMouseEvent&) { wxCommandEvent e(wxEVT_LEFT_DOWN, GetId());
                                       e.SetEventObject(this); ProcessEvent(e); };
    // Right: call the action directly …
    for (wxWindow* w : std::initializer_list<wxWindow*>{this, m_label, m_icon})
        w->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { on_card_clicked(); });
    // … or emit a semantic event the owner binds on the card
    auto notify = [this](wxMouseEvent&) { wxCommandEvent e(wxEVT_BUTTON, GetId());
                                          e.SetEventObject(this); ProcessWindowEvent(e); };
    ```
    Cite: `interface/wx/window.h:2737-2744`; `references/events.md` (event class/type mismatch).
