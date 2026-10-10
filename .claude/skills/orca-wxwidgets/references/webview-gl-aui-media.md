# WebView, OpenGL canvas, AUI docking and media

How wxWebView, wxGLCanvas/wxGLContext, wxAuiManager/wxAuiToolBar and the camera view behave in the
wx 3.3.2 tree Orca builds, and the Orca wrappers built on them: `WebView`, `WebViewHostDialog` and
the web dialogs/panels, `OpenGLManager`, `GLCanvas3D` and its ImGui layer, the Plater docking
manager, `BBLTopbar`, `wxMediaCtrl3`. Read it before touching any browser view, GL canvas, docked
pane, the top bar or the camera view, or when debugging a blank web page, a black or garbled 3D
view, a crash from a JS message, or a lost window layout.

Contents: [Rules](#rules) · [WebView backends](#wxwebview-backends-and-build-options) ·
[Creation order](#wxwebview-creation-order-and-asynchronous-creation) ·
[Script messages](#script-messages-js--c) · [Deferral rule](#no-window-work-inside-a-script-message-handler) ·
[RunScript](#runscript-vs-runscriptasync) · [User scripts](#user-scripts) ·
[Navigation events and settings](#navigation-events-and-settings) ·
[Orca WebView wrapper](#orca-webview-wrapper-widgetswebview) ·
[Web dialogs and panels](#hosted-html-ui-webviewhostdialog-webdialog-webpanel-dockpanel) ·
[WebKitGTK on Linux](#webkitgtk-on-linux-sessions) · [wxGLCanvas/wxGLContext](#wxglcanvas-and-wxglcontext) ·
[EGL vs GLX](#egl-vs-glx) · [Orca OpenGL stack](#orca-opengl-stack-openglmanager-and-the-shared-context) ·
[GLCanvas3D rendering](#glcanvas3d-rendering) · [ImGui boundary](#the-imgui-layer-boundary) ·
[AUI docking](#wxauimanager-and-orcas-docking) · [wxAuiToolBar / BBLTopbar](#wxauitoolbar-and-bbltopbar) ·
[Camera and media](#camera-and-media)

Build facts used throughout: wx is built with `wxBUILD_DEBUG_LEVEL=0` and `libslic3r_gui` with
`wxDEBUG_LEVEL=0`, so every precondition wx enforces only with an assert or a `wxCHECK` (late
`PreferGLX`, `SetCurrent` on a hidden canvas, duplicate AUI pane name, `SetDataPath` after the context
exists, sync `RunScript` inside an Edge callback) fails silently in Orca: `wxASSERT`/`wxFAIL` are
compiled out and `wxCHECK_*` guards still return early without a message
(`include/wx/debug.h:229-231, 356-382`). Linux builds wx against GTK3 (`DEP_WX_GTK3` defaults ON,
`SLIC3R_GTK` is "3", the GUI links `webkit2gtk-4.1`), so the Linux web backend is WebKit2GTK and EGL is
available; GTK2/WebKit1 is an opt-out configuration the GUI does not support.

## Rules

1. Create browsers with `WebView::CreateWebView(parent, url)`. If you must call `wxWebView::New()`,
   null-check it (MSW without the WebView2 runtime returns nullptr) and check `Create()`'s bool.
   §[Creation order](#wxwebview-creation-order-and-asynchronous-creation)
2. Register scheme handlers (`RegisterHandler`) and set the user agent before `Create()` (macOS
   requires it; Edge accepts either order, and Orca follows the doc's "after" there); call
   `AddScriptMessageHandler`, `AddUserScript`, `EnableAccessToDevTools` after `Create()` (Edge queues
   them, WebKit needs them after). §[Creation order](#wxwebview-creation-order-and-asynchronous-creation)
3. A user script affects only documents loaded after it is added: add it before the first `LoadURL`
   (create with an empty URL, or reload). §[User scripts](#user-scripts)
4. Add a script message handler exactly once per view; track it with a per-view flag
   (`WebView::MarkScriptMessageHandlerAdded`), never with the return value.
   §[Script messages](#script-messages-js--c)
5. Never call `RemoveAllUserScripts()` to replace one script: it also deletes the `window.<name>`
   alias. Track your own scripts; if you must clear them, re-add only the alias.
   §[Script messages](#script-messages-js--c)
6. In a script-message handler, take the view from your own capture, route by payload (post JSON
   objects from JS), and never rely on `GetEventObject()` or `GetMessageHandler()`.
   §[Script messages](#script-messages-js--c)
7. Do no window work (create/show/raise/destroy, modal loops) on the stack of a script-message
   handler; defer with `CallAfter` and re-check liveness inside the lambda. In a `WebViewHostDialog`
   subclass, defer inside your own `on_script_message`. §[Deferral rule](#no-window-work-inside-a-script-message-handler)
8. Never call the synchronous `wxWebView::RunScript(js, &out)` from an event handler or a worker
   thread; use Orca's fire-and-forget `WebView::RunScript`, or `RunScriptAsync` when you need a value.
   §[RunScript](#runscript-vs-runscriptasync)
9. In `wxEVT_WEBVIEW_NEWWINDOW`, call `evt.Veto()` whenever you open the URL elsewhere.
   §[Navigation](#navigation-events-and-settings)
10. Do not depend on `NAVIGATED` arriving before `LOADED`. §[Navigation](#navigation-events-and-settings)
11. A web host that re-themes in place handles `EVT_WEBVIEW_RECREATED` without `Skip()`; one that
    needs a reload lets it through. §[Orca wrapper](#orca-webview-wrapper-widgetswebview)
12. Keep `WebViewWebKit`'s destructor removing the `"wx"` handler. §[Orca wrapper](#orca-webview-wrapper-widgetswebview)
13. Do not widen `WEBKIT_DISABLE_COMPOSITING_MODE` beyond Orca's default-path XWayland case, and never
    make a page's progress depend only on a C++→JS callback. §[WebKitGTK on Linux](#webkitgtk-on-linux-sessions)
14. GL attribute lists: legacy `int[]` lists end with `0` and spell out `WX_GL_RGBA` and
    `WX_GL_DOUBLEBUFFER`; `wxGLAttributes`/`wxGLContextAttrs` end with `EndList()`; MSAA is requested
    explicitly. §[wxGLCanvas](#wxglcanvas-and-wxglcontext)
15. A canvas that uses Orca's shared context is created with the exact attribute list of
    `OpenGLManager::create_wxglcanvas`, including the sample count.
    §[Orca OpenGL stack](#orca-opengl-stack-openglmanager-and-the-shared-context)
16. Check `IsShownOnScreen()` and the result of `SetCurrent()` before issuing GL; bail out on failure.
    §[wxGLCanvas](#wxglcanvas-and-wxglcontext)
17. One-time GL initialisation runs only when the canvas is shown and current; otherwise it is
    re-scheduled from idle, never continued. §[GLCanvas3D](#glcanvas3d-rendering)
18. `glViewport` and mouse coordinates are physical pixels: multiply logical sizes by
    `GetContentScaleFactor()` (Orca: `get_canvas_size()`, `get_scale()`). §[wxGLCanvas](#wxglcanvas-and-wxglcontext)
19. `wxGLCanvas::PreferGLX()` runs before any GL attribute, canvas or extension query.
    §[EGL vs GLX](#egl-vs-glx)
20. A GL canvas keeps `wxBG_STYLE_PAINT`; on MSW its paint handler renders, it does not only set a
    dirty flag; `SwapBuffers()` is called only when `IsShownOnScreen()`. §[GLCanvas3D](#glcanvas3d-rendering)
21. Never render one canvas from inside another canvas's render pass; mark it dirty and let its own
    paint draw. §[Orca OpenGL stack](#orca-opengl-stack-openglmanager-and-the-shared-context)
22. Do not place wx child windows over a GL canvas; draw overlays with ImGui or GL.
    §[ImGui boundary](#the-imgui-layer-boundary)
23. Alpha-blend into the default framebuffer with `glBlendFuncSeparate(..., GL_ONE,
    GL_ONE_MINUS_SRC_ALPHA)` so destination alpha stays 1. §[GLCanvas3D](#glcanvas3d-rendering)
24. ImGui text uses `_u8L`; ImGui sizes are physical pixels (scale by `GLCanvas3D::get_scale()`);
    a font atlas must fit `GL_MAX_TEXTURE_SIZE`. §[ImGui boundary](#the-imgui-layer-boundary)
25. After `AddPane` or any `wxAuiPaneInfo` change, call `wxAuiManager::Update()` once for the batch.
    Keep a docked pane hidden until the managed window has been laid out.
    §[AUI](#wxauimanager-and-orcas-docking)
26. Give every pane a unique, stable `Name()` free of layout delimiters (`|`, `;`, `=`, `\`).
    §[AUI](#wxauimanager-and-orcas-docking)
27. Handlers bound on the AUI-managed window (`wxEVT_SIZE`, `wxEVT_AUI_PANE_CLOSE`) call `Skip()`.
    §[AUI](#wxauimanager-and-orcas-docking)
28. `LoadPerspective` hides panes it does not know: re-establish pane visibility after loading and
    fall back to the default perspective when it returns false. §[AUI](#wxauimanager-and-orcas-docking)
29. `DetachPane` neither hides nor destroys: destroy the window yourself; never detach a pane whose
    `GetPane()` reference is captured; the manager must outlive floating frames.
    §[AUI](#wxauimanager-and-orcas-docking)
30. No floating panes on Wayland. §[AUI](#wxauimanager-and-orcas-docking)
31. `wxAuiToolBar`: `Realize()` after changing tools; pick one click model per tool (BBLTopbar acts
    on `wxEVT_AUITOOLBAR_TOOL_DROPDOWN`). §[wxAuiToolBar / BBLTopbar](#wxauitoolbar-and-bbltopbar)
32. A live view is an `IMediaController`; decoded frames are copied under a mutex on the worker and
    repaints are coalesced into one pending `CallAfter` on the view itself. §[Camera](#camera-and-media)

## wxWebView backends and build options

**Contract** (`interface/wx/webview.h:806-935`, backend descriptions):

| Backend | Platform | Documented limits |
|---|---|---|
| Edge (WebView2) | MSW | No custom schemes: a handler is served as the virtual host `https://<scheme>.wxsite` (`interface/wx/webview.h:838-840`, `1267-1271`); customise with `wxWebViewHandler::SetVirtualHost()`, which "has to be set **before** registering the handler" (`interface/wx/webview.h:719-734`, 3.3.0). Available when `WebView2Loader.dll` loads and Edge (Chromium) is installed; check with `wxWebView::IsBackendAvailable()` (`interface/wx/webview.h:857-859`, `1111-1123`). A fixed-version runtime needs `wxWebViewEdge::MSWSetBrowserExecutableDir()` "before using the Edge backend" (`interface/wx/webview.h:870-874`). `Reload()` flags ignored (`interface/wx/webview.h:1278`); `SetPage()` `baseUrl` unused (`interface/wx/webview.h:1295-1298`); `SetEditable()` not implemented (`interface/wx/webview.h:1287`); one script message handler only (`interface/wx/webview.h:1513-1514`); user scripts only `wxWEBVIEW_INJECT_AT_DOCUMENT_START` (`interface/wx/webview.h:1539-1540`). |
| WKWebView | macOS | Full custom-scheme/VFS support on 10.13+, but "two-step creation has to be used and RegisterHandler() has to be called before Create()" (`interface/wx/webview.h:912-914`). Since macOS 10.11 / iOS 9, insecure connections (HTTP, unverified HTTPS) need `NSAppTransportSecurity` keys in Info.plist (`interface/wx/webview.h:916-919`). `RunScript` limited to 10 MiB / 10 s (`interface/wx/webview.h:1432-1433`). `SetEditable()` not implemented. |
| WebKit2GTK | GTK3 | "All features are supported except for clearing and enabling / disabling the history" (`interface/wx/webview.h:894-904`). Same `wxWebViewBackendWebKit` name. |
| WebKit v1 | GTK2 | Min WebKitGTK 1.3.1; custom schemes and virtual file systems supported, but embedded resources load via `data://`; `RunScript` cannot return results (`interface/wx/webview.h:881-892`, `1427-1430`). Not an Orca configuration. |
| Chromium (CEF) | all | Build-time opt-in. X11-only on GTK: set `GDK_BACKEND=x11` or call `gdk_set_allowed_backends("x11")`, else under Wayland `IsBackendAvailable()` is false and creation fails. The browser exists only once the window is shown; bind `wxEVT_CREATE` to know when it is usable (`interface/wx/webview.h:925-931`, `interface/wx/webview_chromium.h:133-150`). Not built in Orca. |
| IE | MSW | Not built in Orca. |

Other backend-limited calls: `ClearBrowsingData()` only Edge, WebKit2GTK and macOS
(`interface/wx/webview.h:1382`); `wxWebViewHandler::StartRequest()` only macOS, Chromium and Edge (`interface/wx/webview.h:788`);
`SetSecurityURL()` only IE (`interface/wx/webview.h:708`).

**Orca's build** (`deps/wxWidgets/wxWidgets.cmake`): `wxUSE_WEBVIEW=ON`; `wxUSE_WEBVIEW_EDGE` ON only
for MSVC; `wxUSE_WEBVIEW_IE=OFF`; Chromium left at its default OFF (`build/cmake/options.cmake:303`),
so the Chromium notes never apply; `wxUSE_WEBVIEW_EDGE_STATIC` is off, so the top-level
`CMakeLists.txt` ships `WebView2Loader.dll` from `deps/WebView2/lib/win-<arch>`. On macOS,
`cmake/modules/MacOSXBundleInfo.plist.in` sets `NSAllowsArbitraryLoads` and
`NSAllowsArbitraryLoadsInWebContent`, which is what the ATS note requires.

**Default backend** [source]: `wxWebViewBackendDefault` is `""` on MSW and `"wxWebViewWebKit"`
elsewhere (`src/common/webview.cpp:41-45`). On MSW with IE disabled, `FindFactory("")` returns Edge
only if `IsAvailable()`, otherwise the IE entry, which is absent, so **`wxWebView::New()` returns
nullptr on Windows without the WebView2 runtime** (`src/common/webview.cpp:494-515`), matching the
documented "@NULL if the requested backend is not available" (`interface/wx/webview.h:1086-1087`).

## wxWebView creation order and asynchronous creation

**Contract.**
- `New(backend)` / `New(config)` are two-step factories; call `Create()` on the result
  (`interface/wx/webview.h:1050-1066`). `New(parent, …)` returns NULL if the backend is unavailable.
- `Create()` "may be asynchronous when using some backends (e.g., wxWebViewEdge or wxWebViewChromium)
  and the object is not really created until wxEVT_WEBVIEW_CREATED event is received, so any
  non-trivial calls to its member functions should be delayed until then" (`interface/wx/webview.h:1033-1040`;
  event at `961-966`, 3.3.0).
- `SetUserAgent()`: "If your first request should already use the custom user agent please use two
  step creation and call SetUserAgent() before Create()"; not implemented for IE (`interface/wx/webview.h:1317-1328`, 3.1.5).
- `RegisterHandler()`: "On macOS … RegisterHandler() has to be called before Create(). With the other
  backends it has to be called after Create()" (`interface/wx/webview.h:1262-1266`).

**Source facts** [source]:
- `wxWebViewEdge::Create` returns `false` before `wxControl::Create` when the loader or runtime is
  missing (`src/msw/webview_edge.cpp:1136-1146`): you hold an uncreated window.
- `wxEVT_WEBVIEW_CREATED` is always queued (`NotifyWebViewCreated` → `QueueEvent`,
  `src/common/webview.cpp:448-453`): macOS and GTK queue it from inside `Create()`
  (`src/osx/webview_webkit.mm:342`, `src/gtk/webview_webkit2.cpp:962`), Edge only once the WebView2
  controller exists (`src/msw/webview_edge.cpp:864-891`). Binding it right after `Create()` is safe everywhere.
- Edge hides the WebView2 controller while the view's top-level parent is iconized
  (`wxWebViewEdge::OnTopLevelParentIconized`, `src/msw/webview_edge.cpp:1178-1183`).
- What each backend accepts before `Create()` / before CREATED:

| Call | Edge | WKWebView | WebKit2GTK |
|---|---|---|---|
| `RegisterHandler` | stored; filters added at creation (`src/msw/webview_edge.cpp:1694-1710`, `945-956`) | **must** be before; applied in `Create` (`src/osx/webview_webkit.mm:259-271`) | registered on the view's own context; works before (`src/gtk/webview_webkit2.cpp:1781-1788`) |
| `SetUserAgent` | queued, applied before the pending URL loads (`1473-1482`, `978-982`) | stored, applied in `Create` before the first load (`500-512`, `281-282`) | stored, applied in `Create` (`1058-1068`, `925-926`) |
| `AddScriptMessageHandler`, `AddUserScript`, `EnableContextMenu`, `EnableAccessToDevTools` | queued until creation (`1389-1420`, `1661-1680`, `960-1001`) | no `WKWebView` exists until `Create` (`src/osx/webview_webkit.mm:273`): after `Create` only | `AddScriptMessageHandler` returns false without a view (`1640-1641`); `AddUserScript`/`EnableAccessToDevTools` act on a null `m_web_view`: after `Create` only |
| `RunScriptAsync` | reports failure immediately ("TODO: postpone execution", `1593-1600`) | after `Create` | after `Create` |

(Bare line numbers in the Edge, WKWebView and WebKit2GTK columns of this and the following tables are
in `src/msw/webview_edge.cpp`, `src/osx/webview_webkit.mm` and `src/gtk/webview_webkit2.cpp`.)

So "handlers and UA before `Create`, everything else after" is the one order that works on all three.
Edge also defers the initial `Navigate` of `Create(url)` until creation, so a UA or user script set
right after a one-step `Create(url)` still applies to the first load there (`src/msw/webview_edge.cpp:978-982`,
`1015-1019`); on macOS and GTK it applies only to later requests.

**Usage** (portable raw-wx shape; Orca code uses the wrapper below):
```cpp
wxWebView* wv = wxWebView::New();                       // nullptr on MSW without WebView2
if (!wv) { /* fallback UI */ return; }
wv->RegisterHandler(handler);                           // before Create: required on macOS
wv->SetUserAgent(ua);                                   // before Create if the first request needs it
if (!wv->Create(parent, wxID_ANY, wxString(), wxDefaultPosition, wxDefaultSize, wxBORDER_NONE))
    { /* Edge runtime missing */ }
wv->AddScriptMessageHandler("wx");                      // after Create (Edge queues it)
wv->AddUserScript(js);                                  // before the first LoadURL
wv->Bind(wxEVT_WEBVIEW_CREATED, ...);                   // queued on every backend
wv->LoadURL(url);
```

**`wxWebViewConfiguration`** (3.3.0): "All settings **must** be set before creating a new web view
with wxWebView::New()" and "Additional instances of wxWebView must be created using the same
wxWebViewConfiguration instance" (`interface/wx/webview.h:299-312`). `GetNativeConfiguration()` is a
`WKWebViewConfiguration*` (macOS), `ICoreWebView2EnvironmentOptions*` (Edge) or `WebKitWebContext*`
(WebKitGTK). `SetDataPath()` is used only by Edge, WebKit2GTK and Chromium (`interface/wx/webview.h:365`);
`EnablePersistentStorage()` only by Edge, WebKit2GTK and macOS (`interface/wx/webview.h:393`). [source] A
default-constructed GTK `wxWebViewWebKit` (what `New()` returns) gets a fresh configuration
(`src/gtk/webview_webkit2.cpp:852-858`), which on WebKitGTK ≥ 2.10 creates its own `WebKitWebContext` and
website-data manager (`GetOrCreateContext`, `src/gtk/webview_webkit2.cpp:765-802`): every GTK view has separate cookies, storage
and URI-scheme registrations unless you share one configuration. `SetDataPath` after the context
exists only asserts (`src/gtk/webview_webkit2.cpp:723-727`) — silent in Orca.

**Custom schemes.** On Edge, registering before `Create` also works [source], and `scheme:` navigations
are rewritten to the virtual host (`src/msw/webview_edge.cpp:571-595`, `1185-1211`).

**OrcaSlicer.** `WebView::CreateWebView` follows the documented order per backend (handlers after
`Create` on Edge, before it elsewhere) and sets the UA before `Create` on Edge, after it elsewhere;
see [Orca wrapper](#orca-webview-wrapper-widgetswebview). On macOS and GTK the first request of
`CreateWebView(parent, url)` therefore goes out without the Orca UA; hosts that call `WebView::LoadUrl`
again after creation (as `WebViewHostDialog::create_webview` does) send it from that load on. Orca never
shares a `wxWebViewConfiguration` between views.

**Pitfalls.**
- **Rule:** Create browsers with `WebView::CreateWebView` and run scripts with `WebView::RunScript`;
  never chain `wxWebView::New()->Create(...)` or use `New(parent, …)` without a null check.
  **Why:** on MSW without the WebView2 runtime the factory returns nullptr, and Edge `Create()` can
  return false and leave an uncreated window. A raw view also misses the `"wx"` handler bookkeeping (a
  later add can throw on macOS), is not themed by `RecreateAll`, and invites the yielding `RunScript`.
  ```cpp
  // Wrong: m_webview = wxWebView::New(this, wxID_ANY, url); m_webview->AddScriptMessageHandler("x");
  // Right: m_webview = WebView::CreateWebView(this, url);   // never null; see the wrapper section
  ```
  Cite: `src/common/webview.cpp` `wxWebView::FindFactory`; `WebView::CreateWebView`.
- **Rule:** Call `AddScriptMessageHandler`/`AddUserScript` after `Create()` on macOS and GTK.
  **Why:** before `Create` macOS has no `WKWebView` and GTK returns false or acts on a null view.
- **Rule:** On macOS and GTK, set the UA before `Create(url)` (or `Create` with an empty URL, then
  `LoadURL`) when the first request must carry it.

## Script messages (JS → C++)

**Contract.** JS calls `window.<name>.postMessage(body)`; C++ receives
`wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED` with `GetString()` = body and `GetMessageHandler()` = name.
`AddScriptMessageHandler` returns "true if the handler could be added". "The Edge backend only supports
a single message handler and the Chromium and IE backends do not support script message handlers"
(`interface/wx/webview.h:1484-1518`, 3.1.5). `RemoveScriptMessageHandler()` returns a bool likewise (`interface/wx/webview.h:1520-1530`).

**Per-backend behaviour the docs do not state** [source]:

| | Edge | WKWebView | WebKit2GTK |
|---|---|---|---|
| `Add…` return | `false` if *any* handler already exists, whatever its name (`src/msw/webview_edge.cpp:1635-1645`) | always `true` (`src/osx/webview_webkit.mm:620-629`); a duplicate name makes WebKit raise an NSException that cannot be caught and kills the app ([external]: Apple behaviour, recorded in the `WebView::CreateWebView` comment) | `false` on a duplicate or with no view, **but** `g_signal_connect` runs before registration, so a duplicate add leaves a second connection (`src/gtk/webview_webkit2.cpp:1638-1657`) |
| `Remove…` | clears the single name (`1647-1652`) | removes the native handler (`632-636`) | unregisters but **does not disconnect** the signal (`1659-1664`): Remove + Add delivers every later message **twice** |
| `window.<name>` alias | user script `window.<name> = window.chrome.webview` + `ExecuteScript` (`1030-1046`) | `AddUserScript` + **synchronous** `RunScript` (`626-628`) | `AddUserScript` + **synchronous** `RunScript` (`1650-1654`) |
| Delivery | queued with `AddPendingEvent` (`785-813`); [documented] "Queue Edge wxWebView events" (`docs/changes.txt:525`) | `ProcessWindowEvent` **inside** the `WKScriptMessageHandler` delegate (`1324-1361`) | `HandleWindowEvent` **inside** the GTK signal (`397-409`) |
| Event fields | handler name and event object set | name set, **no event object** | **empty `GetMessageHandler()`, no event object** |
| Body | `get_WebMessageAsJson`, JSON-decoded when it is a string (`798-809`) | NSString as is; dict/array → JSON; NSNumber → `stringValue` (booleans arrive as `1`/`0`); NSNull → `"null"` | object → JSON string, otherwise `ToString` (`64-94`) |

Consequences:
- The return value is not a duplicate check you can rely on on any backend; track registration per view.
- `RemoveAllUserScripts()` removes the `window.<name>` alias on all three backends, because the alias
  is a user script everywhere (`src/osx/webview_webkit.mm:651-654`; GTK
  `webkit_user_content_manager_remove_all_scripts`; `src/msw/webview_edge.cpp:1682-1692`). The native handler survives and `window.webkit.messageHandlers.<name>` (WebKit) /
  `window.chrome.webview` (Edge) still work, but pages using `window.<name>.postMessage` lose it after
  the next load. On macOS it also removes wx's internal fullscreen shim (`src/osx/webview_webkit.mm:300-337`).
- Post JSON objects from JS; the body normalisation differs per backend for everything else.
- `~wxWebViewWebKit` (macOS) does not remove script message handlers, and the registered
  `WebViewScriptMessageHandler` holds a raw `wxWebViewWebKit*` (`src/osx/webview_webkit.mm:351-359`, `620-623`).

**OrcaSlicer.** The `"wx"` handler is added once per view by `WebView::CreateWebView`, guarded by
`WebViewRef::m_script_handler_added`; any other code that adds `"wx"` calls
`WebView::MarkScriptMessageHandlerAdded(view)` so the deferred add skips. `WebViewWebKit::~WebViewWebKit`
calls `RemoveScriptMessageHandler("wx")` because wx does not. `WebViewHostDialog` forbids
`RemoveAllUserScripts()` (comment in `register_theme_user_scripts`).

**Pitfalls.**
- **Rule:** Register a script message handler exactly once per web view; skip a second registration
  using a per-view flag.
  **Why:** on WKWebView a duplicate name throws an uncatchable NSException — this crashed startup
  whenever a printer had a print host set, because both `PrinterWebView::SendAPIKey()` and
  `CreateWebView`'s deferred hook added `"wx"`. The return value is no protection: macOS returns
  true unconditionally (after the duplicate has already thrown), GTK returns false only after
  connecting a second signal handler, Edge returns false once any handler exists.
  ```cpp
  // Wrong: unconditional second add throws on WKWebView
  m_browser->AddScriptMessageHandler("wx");
  // Right: record it so CreateWebView's deferred add skips
  if (m_browser->AddScriptMessageHandler("wx"))
      WebView::MarkScriptMessageHandlerAdded(m_browser);
  ```
  Cite: 857adad293 (`Widgets/WebView.cpp/.hpp`, `PrinterWebView.cpp`).
- **Rule:** Do not use `RemoveAllUserScripts()` to swap one script; if scripts must be cleared, re-add
  only the alias rather than Remove + Add of the handler.
  **Why:** clearing drops the `window.wx` alias on every backend. Remove + Add of the handler is clean
  on macOS and Edge, but on GTK leaves two signal connections, so every message is handled twice; a
  blind Add without Remove throws on macOS.
  ```cpp
  // Wrong (GTK double delivery): RemoveAllUserScripts(); RemoveScriptMessageHandler("wx"); AddScriptMessageHandler("wx");
  // Right: m_browser->RemoveAllUserScripts();
  //        m_browser->AddUserScript("window.wx = window.webkit.messageHandlers.wx;");  // WebKit
  //        // Edge: "window.wx = window.chrome.webview;"
  ```
  Cite: `src/gtk/webview_webkit2.cpp` `wxWebViewWebKit::AddScriptMessageHandler`/`RemoveScriptMessageHandler`.
- **Rule:** Never filter on `GetMessageHandler()` or use `GetEventObject()` in a script-message handler.
  **Why:** GTK sets neither, macOS sets no event object.
  ```cpp
  // Wrong: auto* wv = static_cast<wxWebView*>(evt.GetEventObject()); if (evt.GetMessageHandler() == "wx") ...
  // Right: m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, [this](wxWebViewEvent& e) {
  //            auto j = nlohmann::json::parse(e.GetString().utf8_string(), nullptr, false); ... m_browser ... });
  ```
- **Rule:** Keep the macOS `WebViewWebKit` subclass and its destructor's `RemoveScriptMessageHandler("wx")`.
  **Why:** WebKit's handler object keeps a raw pointer to the destroyed wx view; routing macOS through
  plain `wxWebView::New()` bypassed the destructor and reintroduced the teardown bug.
  Cite: d62aa42e61 (`Widgets/WebView.cpp` `WebViewWebKit`).

## No window work inside a script-message handler

**Rule.** Do no window work — create, show, raise, destroy, `ShowModal`/`EndModal` — on the stack of a
`wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED` handler (or any other native network/JS callback). Post the
whole operation to a clean main-loop iteration with `CallAfter` and re-check liveness inside the lambda.

**Why.** WebKitGTK and WKWebView deliver script messages synchronously inside the native signal or
delegate frame (table above). Creating and presenting a second web-view window there crashed on Linux:
`gtk_window_present` ran while GTK's deferred `Show()` map was still pending behind the X11
frame-extents handshake (`src/gtk/toplevel.cpp:1146-1167`, `Raise()` is `gtk_window_present`,
`1301-1310`), and it is the same crash class on macOS. Edge already queues the event, so deferring
gives every platform the same delivery model.

**OrcaSlicer.** The base `WebViewHostDialog::on_script_message_event` parses the JSON and calls the
subclass's `on_script_message` synchronously — nothing is deferred for you, and
`handle_common_script_command` (`close_page` → `EndModal`/`Close`) also runs on that stack. Each
subclass that does window work defers in its own `on_script_message`; `PluginsDialog::on_script_message`
is the model:
```cpp
// Wrong: runs on the webview delegate stack
void on_script_message(const json& payload) { handle_web_command(payload); }

// Right: defer at the single entry point; every handler is off that stack by construction
void on_script_message(const json& payload) {
    if (handle_common_script_command(payload)) return;
    wxGetApp().CallAfter([this, alive = m_alive, payload]() {   // m_alive: shared_ptr<atomic<bool>>,
        if (alive->load(std::memory_order_acquire))              // set false in the destructor
            handle_web_command(payload);
    });
}
```
The model keeps `handle_common_script_command` synchronous, so `close_page` still hides the dialog on
the delegate stack (`EndModal` ends with `Show(false)`, `src/gtk/dialog.cpp:187-205`; `Close()` runs the
close handlers). Move it into the lambda when a close path does more than hide.
Other deferral shapes in the tree: `ui_create_window` (`src/slic3r/plugin/host/PluginHostUi.cpp`)
defers the whole `WebDialog` creation to `wxGetApp().CallAfter`, pre-binds a `UiRegistry` id so the
returned handle is live at once, re-checks `UiRegistry::is_open` inside the lambda, and does not
`Raise()`; `DockPanel::request_close` (a page-requested close) destroys via `CallAfter` on the panel with
a `wxWeakRef<DockPanel>`. A `CallAfter` queued on the view or dialog itself is dropped if that object
dies first; one queued on `wxGetApp()` always runs and must re-check liveness itself (see
`references/events.md §CallAfter`). `WebViewHostDialog::call_web_handler` queues `run_script` on
`wxGetApp()` capturing a bare `this`, so call it only from code that cannot destroy the dialog before
the next event-loop pass.

Cite: b779a7bfed (`ui_create_window` in `PluginHostUi.cpp`; `WebDialog`, named `PluginWebDialog` in that commit),
f2ccbfc8b5 (`GUI_App` terminal dialog), 0a0d59b76b (`PluginsDialog::on_script_message`, `m_alive`).

## RunScript vs RunScriptAsync

**Contract.** "Because of various potential issues it's recommended to use RunScriptAsync() instead of
this method. This is especially true if you plan to run code from a webview event"
(`interface/wx/webview.h:1395-1398`). Limits: WebKit1/GTK2 and Chromium cannot return results; macOS caps
execution at "10MiB of memory and 10 seconds"; IE only after `LOADED`, needs a `<script>` tag, and
has JSON-conversion caveats (`interface/wx/webview.h:1425-1451`). `RunScriptAsync(js, clientData)` (3.1.6) reports through
`wxEVT_WEBVIEW_SCRIPT_RESULT` (`GetString()`, `IsError()`, the `clientData` pointer); IE does not
support it (`interface/wx/webview.h:1463-1481`).

**Source** [source]:
- No backend overrides `RunScript` except Edge, which only adds a guard. The base implementation is
  `RunScriptAsync(js); while (m_syncScriptResult == -1) wxYield();` (`src/common/webview.cpp:366-379`):
  a **nested event loop** in which timers, idle handlers, `CallAfter`s and user input all run
  re-entrantly (see `references/threads-timers-app.md` §Yields and nested event loops).
- `YieldFor` returns immediately off the main thread (`src/common/evtloopcmn.cpp:128-135`), so a sync
  `RunScript` from a worker spins without dispatching anything and can hang (the native completion that
  ends the loop is never delivered to that thread).
- Edge rejects sync `RunScript` inside its NAVIGATING and NEWWINDOW callbacks (`m_inEventCallback`,
  `src/msw/webview_edge.cpp:436-438`, `1586-1591`; a `wxCHECK`, silent in Orca).
- `AddScriptMessageHandler` on macOS and GTK calls sync `RunScript` internally, so it yields too.
- `SendScriptResult` dispatches `wxEVT_WEBVIEW_SCRIPT_RESULT` synchronously from the native completion
  handler (`src/common/webview.cpp:387-410`).

**OrcaSlicer.** `WebView::RunScript(view, js)` is fire-and-forget native execution:
`ICoreWebView2::ExecuteScript` (Windows), `WKWebView_evaluateJavaScript` (macOS),
`webkit_web_view_run_javascript` (Linux). It never yields and returns no value; on Windows it returns
false and drops the script until Edge is created (`GetNativeBackend()` is null). Use
`wxWebView::RunScriptAsync` only when you need a result.

**Pitfall.**
- **Rule:** Never call `wxWebView::RunScript(js, &out)` from an event handler, a script-message handler
  or a worker thread.
  **Why:** it spins `wxYield`, re-entering any handler (including the one you are in), hangs off the
  main thread, and silently fails inside Edge navigation callbacks.
  ```cpp
  // Wrong: m_webview->RunScript("update(" + json + ")");          // yields inside the handler
  // Right: WebView::RunScript(m_webview, "update(" + json + ")");  // or RunScriptAsync + SCRIPT_RESULT
  ```

## User scripts

**Contract.** `AddUserScript(js, injectionTime = wxWEBVIEW_INJECT_AT_DOCUMENT_START)`: unsupported by
IE; "the Edge backend does only support wxWEBVIEW_INJECT_AT_DOCUMENT_START" (`interface/wx/webview.h:1532-1547`);
Edge returns false for `AT_DOCUMENT_END` (`src/msw/webview_edge.cpp:1664-1666`).

**Source** [source]:
- On WebKit scripts run in **all frames**, cross-origin iframes included (GTK
  `WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES`, `src/gtk/webview_webkit2.cpp:1669-1675`; macOS
  `forMainFrameOnly:NO`, `src/osx/webview_webkit.mm:641-645`); WebView2 also injects into child frames,
  cross-origin ones included ([external], the `WebViewHostDialog::document_start_injector` comment). Guard
  page-global state with `if (window.top !== window.self) return;` as
  `WebViewHostDialog::document_start_injector` does.
- A document-start script on WebView2 can run before `<html>` exists (`document.head` and
  `document.documentElement` both null); `document_start_injector` retries through a
  `MutationObserver`.
- Scripts apply to documents loaded after they are added. Repeated `AddUserScript` calls accumulate
  duplicates; register once.

**OrcaSlicer.** `WebViewHostDialog::register_theme_user_scripts` adds the theme script and then the
subclass's `add_user_scripts()` exactly once, at creation; live re-theme goes through
`apply_theme_live()` (a `RunScript`), never a re-registration.

**Pitfall.**
- **Rule:** Add user scripts before the first load of the document they must affect.
  **Why:** after `Create(url)` the first load is already running on WebKit, so the script misses it.
  ```cpp
  // Wrong: wv = CreateWebView(this, url); wv->AddUserScript(js);           // first page has no js on WebKit
  // Right: wv = CreateWebView(this, url); wv->AddUserScript(js); WebView::LoadUrl(wv, url);  // reload
  //    or: create on a bootstrap page and SetPage() the real content after its first LOADED (WebPanel)
  ```

## Navigation events and settings

| Event | Doc | Edge [source] | macOS / GTK [source] |
|---|---|---|---|
| `NAVIGATING` | vetoable; one per frame (`interface/wx/webview.h:967-971`) | sync `HandleWindowEvent`; `SetInt(1)` for the main frame (`571-605`) | sync (`src/osx/webview_webkit.mm:1030-1050`; `src/gtk/webview_webkit2.cpp:210-245`) |
| `NAVIGATED` | "may not be vetoed"; per frame (`interface/wx/webview.h:972-976`) | queued after `NavigationCompleted`, i.e. **after LOADED** (DOMContentLoaded); no event object (`634-705`, `760-767`) | on commit, before LOADED (`src/osx/webview_webkit.mm:883-895`; GTK `WEBKIT_LOAD_COMMITTED`, `179-206`) |
| `LOADED` | per frame (`interface/wx/webview.h:977-980`) | DOMContentLoaded, queued | `didFinishNavigation` / `WEBKIT_LOAD_FINISHED` |
| `ERROR` | `GetInt()` is a `wxWebViewNavigationError` (`interface/wx/webview.h:981-985`) | a cancellation caused by your own `Veto` is suppressed (`674-677`) | — |
| `NEWWINDOW` | "If a wxEVT_WEBVIEW_NEWWINDOW is not vetoed, a wxEVT_WEBVIEW_NEWWINDOW_FEATURES event will be sent … A new child web view will be available via GetChildWebView()" (`interface/wx/webview.h:193-200`) | builds a child `wxWebViewEdge` and a deferral (`709-741`) | builds a child view and returns its native view to WebKit as the page's new window (`src/osx/webview_webkit.mm:1196-1232`; `src/gtk/webview_webkit2.cpp:537-555`) |

`IsTargetMainFrame()` is "only available with the macOS and the Edge backend" (`interface/wx/webview.h:2111-2117`).
`wxWebViewWindowFeatures` deletes an unused child view only if `GetChildWebView()` was never called
(`src/common/webview.cpp:89-105`).

Settings:
- `EnableContextMenu`: enabled by default (`interface/wx/webview.h:1596-1604`).
- `EnableAccessToDevTools`: "Dev tools are disabled by default" (`interface/wx/webview.h:1614-1624`); Edge forces
  the disabled state at creation (`src/msw/webview_edge.cpp:475-476`, `966-970`). `ShowDevTools` (3.3.0)
  is implemented only by Edge, WebKit2GTK and Chromium — not IE, WebKit1 or macOS WKWebView, where the
  base returns false (`interface/wx/webview.h:1653-1654`; `include/wx/webview.h:255`); on WebKit2GTK it
  also enables context-menu access to the dev tools.
- `EnableBrowserAcceleratorKeys`: "only implemented for the Edge backend" (`interface/wx/webview.h:1660-1674`).
- On GTK, `Create` forces `wxHSCROLL|wxVSCROLL` (`src/gtk/webview_webkit2.cpp:899-901`).

**OrcaSlicer.** `PrinterWebView` vetoes `NEWWINDOW` and opens the URL with `wxLaunchDefaultBrowser`;
`DockPanel` vetoes it outright. `WebDialog` and `WebPanel` re-apply `SetPage` content on a reload by
watching `NAVIGATED` + `LOADED` for `web_hosting::content_base_url()`: WebKit reloads the base URL,
while Edge ignores the base URL and restores `SetPage` content itself.

**Pitfalls.**
- **Rule:** Call `evt.Veto()` in `NEWWINDOW` when you open the URL externally or in the same view.
  **Why:** otherwise wx 3.3 constructs a child web view (on macOS and GTK handed to WebKit as the page's
  new window) that you never use.
  ```cpp
  // Wrong: void OnNewWindow(wxWebViewEvent& e) { wxLaunchDefaultBrowser(e.GetURL()); }
  // Right: void OnNewWindow(wxWebViewEvent& e) { wxLaunchDefaultBrowser(e.GetURL()); e.Veto(); }
  ```
- **Rule:** Key logic on `LOADED` or on a message the page posts, not on `NAVIGATED` ordering.
  **Why:** on Edge `LOADED` arrives first.

## Orca WebView wrapper (`Widgets/WebView`)

`WebView` (`src/slic3r/GUI/Widgets/WebView.hpp`, global namespace, static members only) is the
sanctioned way to make and drive a browser.

**`WebView::CreateWebView(parent, url)`:**
- On MSVC builds, an `edge_fixed/` folder next to the executable triggers
  `wxWebViewEdge::MSWSetBrowserExecutableDir`.
- Normalises the URL (backslashes → `/` on Windows, then `wxURI(url).BuildURI()`); `WebView::LoadUrl`
  applies the same normalisation.
- Picks the backend:

| Platform | Object | Order |
|---|---|---|
| Windows | `new WebViewEdge` (subclass) | `SetUserAgent` → `Create(url)` → `RegisterHandler("bbl" archive, "memory" FS)`, the documented "after Create" order |
| macOS | `new WebViewWebKit` (constructed from `wxWebView::NewConfiguration(wxWebViewBackendWebKit)`, a fresh config per view) | `RegisterHandler("wxfs", "memory")` → `Create(url)` → `SetUserAgent`; then `WKWebView_setTransparentBackground` |
| Linux | `wxWebView::New()` | as macOS, without the transparency call |

- The UA is `… BBL-Slicer/v<version> (dark|light) BBL-Language/<lang>`.
- Calls `EnableContextMenu(true)`.
- Adds `"wx"` once per view: immediately on Windows (Edge queues it); on macOS/Linux inside
  `webView->CallAfter` (after `Create`), because the add yields through the sync `RunScript`. While
  one add is yielding (`GUI_App::is_adding_script_handler()`), other views wait in `g_delay_webviews`
  (`wxWeakRef<wxWebView>`, so a view destroyed meanwhile is skipped); `GUI_App`'s idle handler holds
  `post_init` back while an add is in progress, and on macOS `GUI_App::run_wizard` retries on a 200 ms
  one-shot `wxTimer` instead of opening the wizard during the add.
- Attaches a `WebViewRef` (`wxObjectRefData`) that registers the view in `g_webviews` and holds
  `m_script_handler_added`.
- Never returns null: when the factory returns null it substitutes a `FakeWebView` stub. It ignores
  `Create()`'s result, so on Windows without the runtime you get an uncreated `WebViewEdge`;
  `GUI_App::init_webview_runtime` (`post_init`, MSVC) checks the runtime with
  `WebView::CheckWebViewRuntime` (`wxWebViewFactoryEdge::GetVersionInfo`; the concrete factory's
  override takes `wxVersionContext` with no default argument, `include/wx/msw/webview_edge.h:150`, so
  pass `wxVersionContext::RunTime`) and offers
  `WebView::DownloadAndInstallWebViewRuntime` before the first web view (the setup wizard).

**Scheme-handler caveat** [source]. The non-Windows registration sits behind a function-static
`s_schemes_registered` (its comment calls WebKit2GTK schemes global). In this wx they are not: wx
registers on the view's own `WKWebViewConfiguration` (applied in `Create`) or the view's own
`WebKitWebContext`, so only the **first** web view of the process on macOS and Linux gets the `wxfs:`
and `memory:` schemes. Do not rely on them in a second view; register what you need on your view.

**`WebView::RecreateAll()`** runs on a colour-mode change (`MainFrame::on_sys_color_changed`; a
language switch rebuilds the main window, and its web views with it). For every registered view it
re-sets the UA (with the current language token) — the macOS-format string on every platform, and
`WebViewEdge` applies the whole string, so after a colour-mode change Windows views send a Macintosh UA (Edge also
derives its colour scheme from the `dark` token) — and sends `EVT_WEBVIEW_RECREATED` through the view's
handler; if nothing processes it (not bound, or the handler calls `Skip()`), the view is `Reload()`ed.
It is a `wxCommandEvent`, so an unhandled one propagates to the view's parents [source]: a parent
binding it without `Skip()` also suppresses the reload. Hosts that re-theme in place
(`WebViewHostDialog::on_webview_recreated`, `WebPanel::on_webview_recreated`) bind it on the browser
and handle it without `Skip()`. Views not made by `CreateWebView` are not in `g_webviews` and get no
theming.

**`WebViewEdge`** overrides `SetUserAgent` (derives `ICoreWebView2Profile::PreferredColorScheme`
from a `dark` token in the UA) and adds `SetColorScheme`. Both store pending values while Edge is not
created and apply them in `DoGetClientSize`, because the backend initialises asynchronously. The
override does not call the base, so wx's own pending-UA queue stays unused; the pending values are
applied when `OnWebViewCreated` calls `UpdateBounds()` → `GetClientRect()`, before the pending URL loads
[source] (`src/msw/webview_edge.cpp:553-559`, `886-891`, `1015-1019`).

## Hosted HTML UI: WebViewHostDialog, WebDialog, WebPanel, DockPanel

**`Widgets/WebViewHostDialog`** (a `DPIDialog`) is the shell for local HTML dialogs; derive from it for
a new one (subclasses include `PluginsDialog`, `PluginsConfigDialog`, `SpeedDialWebDialog`,
`WebDialog`). `create_webview(resource_path, title, size, min_size)`:
1. builds the `file://` resource URL (plus `?lang=` unless `append_language_to_url()` is false) and calls
   `WebView::CreateWebView(this, url)`;
2. binds `wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED` (by the browser's id) → `on_script_message_event`;
3. calls `register_theme_user_scripts()` once (theme script, then the subclass hook `add_user_scripts()`);
4. binds `EVT_WEBVIEW_RECREATED` → `apply_theme_live()` (a `RunScript`, no reload, no `Skip()`);
5. calls `load_url(url)` again so the document-start scripts (and, on macOS/GTK, the UA) apply to the
   first page on WebKit.

Messaging: JS → C++ through `window.wx.postMessage(JSON)`; `on_script_message_event` parses it with
`nlohmann::json` and calls the pure-virtual `on_script_message(payload)` (parse errors go to
`on_script_message_parse_error`). C++ → JS through `call_web_handler(payload, "HandleStudio")`.
`handle_common_script_command` implements `close_page` (`EndModal(close_return_code)` when modal, else
`Close()`).

**`WebDialog`** hosts plugin HTML: it loads the bundled bootstrap page (`web_hosting::BOOTSTRAP_PAGE`),
then swaps in the plugin HTML with `SetPage(html, web_hosting::content_base_url())` on the first
`LOADED` or `ERROR` (so a blocked bootstrap still triggers it). Its user scripts (element defaults, the
`orca` bridge) are registered before that `SetPage`, so they apply to the real content.
**`WebPanel`** (a `wxPanel`; `page_html()`, `on_page_message(kind, json)`, `post_to_page`) is the
non-dialog host with the same bootstrap + `SetPage` design; **`DockPanel : WebPanel`** is a plugin pane
docked in the Plater AUI (see [AUI](#wxauimanager-and-orcas-docking)). A frameless
`WebViewHostDialog` used as a popup is covered in `references/popups-menus.md §frameless-dialog-as-popup`.

**Other web users.** The Home page `WebViewPanel` (`WebViewDialog.hpp`) uses the same
`window.wx.postMessage` → `wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED` bridge. `PrinterWebView` (embedded printer UIs) binds `ERROR`, `LOADED`, `NEWWINDOW`,
`SCRIPT_MESSAGE`, calls `EnableAccessToDevTools(developer_mode)`, and on Linux injects a WebKitGTK
vue-resize workaround script and sets a persistent SQLite cookie store on the view's own
`WebKitWebContext` (`webkit_web_view_get_context`). The camera's web controller is in
[Camera](#camera-and-media).

**Pitfalls** (deferring window work in a subclass: [Deferral rule](#no-window-work-inside-a-script-message-handler)).
- **Rule:** Never call `RemoveAllUserScripts()` in a host, and never call `register_theme_user_scripts()`
  twice. **Why:** the first drops `window.wx`; the second appends duplicate scripts.
- **Rule:** Bind `EVT_WEBVIEW_RECREATED` without `Skip()` only if your handler re-themes the page.
  **Why:** a handled event suppresses `RecreateAll`'s reload.

## WebKitGTK on Linux sessions

**Mechanism.** `CLI::run` (`src/OrcaSlicer.cpp`, under `#ifdef __WXGTK__`, before GTK initialises)
branches on `GDK_BACKEND`:
- **Default path** (`GDK_BACKEND` unset or not starting with `x11`): if wx lacks EGL (`!wxHAS_EGL`) and
  `WAYLAND_DISPLAY` is set, it forces `GDK_BACKEND=x11`; it sets `WEBKIT_DISABLE_COMPOSITING_MODE=1`
  non-replacing **only** when both `DISPLAY` and `WAYLAND_DISPLAY` are non-empty (a Wayland session with
  XWayland available — the session, not the backend GTK will pick); `XInitThreads()` runs only when
  `DISPLAY` is set.
- **`GDK_BACKEND=x11…` opt-in path**: sets `DRI_PRIME=1` (and the NVIDIA PRIME variables when
  `/proc/driver/nvidia/version` exists), calls `XInitThreads()`, and deliberately does **not** disable
  compositing (the comment: the WebKit2GTK XWayland bug appears fixed in WebKit2GTK ≥ 2.42, and leaving
  compositing on keeps hardware acceleration for Device / Setup Wizard / login pages).

Runtime backend detection elsewhere uses Orca's `is_running_on_wayland()` / `is_running_on_x11()`
(see `references/platforms.md`), not these environment variables.

**Pitfall.**
- **Rule:** Set `WEBKIT_DISABLE_COMPOSITING_MODE` only on the default path in Wayland sessions,
  non-replacing (`setenv(..., false)`, so a value the user already exported wins), before GTK
  initialises; never unconditionally. Keep JS-side fallbacks so a page never
  waits forever for a C++→JS call.
  **Why:** WebKit2GTK accelerated compositing could fail silently under XWayland, leaving web views
  blank (Setup Wizard stuck on "Loading..."); disabling it everywhere needlessly drops hardware
  acceleration; without the JS timeout the user is stuck when the signal is lost.
  ```cpp
  // Wrong: unconditional, kills HW acceleration everywhere
  ::setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", false);
  // Right (default path only): non-replacing, before GTK init, Wayland session with XWayland
  if (display && *display && wayland && *wayland)
      ::setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", /*replace*/ false);
  ```
  Cite: c12912e0df (`src/OrcaSlicer.cpp`; `resources/web/guide/0/load.js` `OnInit`
  `setTimeout("JumpToTarget()", …)`), 9446030079 (the `GDK_BACKEND=x11` opt-in branch).

## wxGLCanvas and wxGLContext

**Attributes.**
- `wxGLAttributes` (pixel format) and `wxGLContextAttrs` (context) replace the legacy `int attribList[]`
  (since 3.1.0). "Notice that EndList() must be used as the last attribute. Not adding it will likely
  result in nothing displayed at all" (`interface/wx/glcanvas.h:127-128`, `290-291`). Extra attributes go
  through `AddAttribute()`/`AddAttribBits()`; "While WGL_/GLX_/NS attributes can be added, PFD_ (for MSW)
  can not" (`interface/wx/glcanvas.h:130-133`). `PlatformDefaults()` sets "typically needed attributes. E.g.
  full-acceleration on MSW" (`254-258`).
- A legacy list is zero-terminated, and "if you do specify some attributes you also need to explicitly
  include these two default attributes" (`WX_GL_RGBA`, `WX_GL_DOUBLEBUFFER`) (`interface/wx/glcanvas.h:963-973`).
  With a NULL list wx uses RGBA, 16-bit depth, double buffering (`interface/wx/glcanvas.h:261-264`;
  `src/common/glcmn.cpp:150-163`).
- **No MSAA by default since 3.3**: "wxGLCanvas doesn't use multi-sampling by default any longer"
  (`docs/changes.txt:21-23`). The entry says `Samplers(1).SampleBuffers(4)`, which swaps the names: per
  `interface/wx/glcanvas.h:232-246` it is `SampleBuffers(1).Samplers(4)` (or `WX_GL_SAMPLE_BUFFERS, 1, WX_GL_SAMPLES, 4`).
- `IsDisplaySupported()` "only applies for visual attributes, not rendering context attributes"
  (`interface/wx/glcanvas.h:1047-1070`); stereo, aux buffers, alpha and accumulation buffers are not supported
  everywhere — check with it (`626-629`).
- `MajorVersion`/`MinorVersion`/`OGLVersion`/`ForwardCompatible` (and `WX_GL_MAJOR_VERSION`) have no
  effect on macOS, where `CoreProfile()` yields at least 3.2 (`interface/wx/glcanvas.h:313-363`,
  `701-703`); a core-profile request is ignored when the requested version is below 3.2 (`304-309`,
  `720-725`).

**Context.**
- `wxGLContext(win, other = nullptr, ctxAttrs = nullptr)`: `win` is needed only during construction;
  pass `other` to share resources (`interface/wx/glcanvas.h:500-523`).
- "The best way of knowing if your OpenGL environment supports a specific type of context is creating a
  wxGLContext instance and checking wxGLContext::IsOK(). If it returns false, then simply delete that
  instance and create a new one with other attributes" (`interface/wx/glcanvas.h:483-486`).
- "Binding (making current) a rendering context with another instance of a wxGLCanvas however works
  only if the both wxGLCanvas instances were created with the same attributes" (`interface/wx/glcanvas.h:466-468`).
- `wxGLContext::GetProcAddress<T>()` (3.3.2): `wglGetProcAddress` on MSW, where pointers "may be specific
  to the currently active context" (do not share them between contexts), `glXGetProcAddress`/
  `eglGetProcAddress` on Unix, always NULL on macOS (`interface/wx/glcanvas.h:559-598`).
  `wxGLContext::ClearCurrent()` is new in 3.3.2 (`interface/wx/glcanvas.h:551-559`).

**SetCurrent preconditions.**
- `SetCurrent` "may only be called when the window is shown on screen, in particular it can't usually
  be called from the constructor" (`interface/wx/glcanvas.h:1118-1120`); `wxGLContext::SetCurrent` "may be called from
  any thread … provided that the context is not used by any other thread" (`546-548`).
- [source] `wxGLCanvasBase::SetCurrent` only asserts `IsShown()`, with the comment "although on MSW it
  works even if the window is still hidden, it doesn't work in other ports (notably X11-based ones)"
  (`src/common/glcmn.cpp:75-83`); the assert is compiled out in Orca, so a hidden canvas fails silently.
- [source] macOS `SetCurrent` returns true whenever the context exists and calls `setView:` + `update`
  on every main-thread call (`src/osx/cocoa/glcanvas.mm:241-256`).
- [source] On Unix `wxGLCanvasUnix::IsShownOnScreen()` is `m_impl->HasWindow() && base`
  (`src/unix/glcanvas.cpp:306-309`): false until a native drawable exists. On Wayland that drawable is a
  `wl_subsurface` created only after map and a frame callback (`src/unix/glegl.cpp:710-738`, `982-996`).
  This is the only `IsShownOnScreen()` override; for every other window it only checks the
  `IsShown()` chain and says nothing about occlusion, minimisation or workspaces.

**Pixels.** "wxGLContext always uses physical pixels … multiply the result returned by
wxWindow::GetClientSize() by wxGLCanvas::GetContentScaleFactor() before passing it to glViewport().
Same considerations apply … notably those retrieved from wxMouseEvent" (`interface/wx/glcanvas.h:829-836`;
`docs/doxygen/overviews/high_dpi.md:100-119`, which also covers `ToPhys`/`FromPhys`). The factor is
always 1 on MSW; GTK returns the integer `gtk_widget_get_scale_factor` (`src/gtk/window.cpp:4934-4944`);
on macOS the canvas view requests a full-resolution surface (`setWantsBestResolutionOpenGLSurface:YES`,
`src/osx/cocoa/glcanvas.mm:168`) [source], so the drawable is the backing-store size.

**Paint.** `wxGLCanvasBase` sets `wxBG_STYLE_PAINT` in its constructor (`src/common/glcmn.cpp:68-73`). The paint
handler "must create a wxPaintDC" (`interface/wx/event.h:2274`); [source] wxMSW 3.3 still validates the
window through `DefWindowProc` when none was created (`src/msw/window.cpp:5396-5431`).

**Swap** [source]:
- EGL `SwapBuffers` returns false without drawing when the window is hidden (X11) or the Wayland surface
  is not ready (`src/unix/glegl.cpp:898-947`).
- `SetSwapInterval` (3.3.2, `interface/wx/glcanvas.h:1126-1154`): 0 disables VSync, negative requests adaptive VSync,
  `DefaultSwapInterval` "means to turn off automatically setting it to 0 by default"; it returns
  `SwapInterval::NonAdaptive` when adaptive VSync is unsupported but standard VSync was enabled, and
  `GetSwapInterval()` may differ from the requested value (clamping). On Linux (EGL and
  GLX) wx sets interval **0** at the first swap unless the app called `SetSwapInterval`, so that an
  occluded or unmapped window cannot block (`include/wx/unix/private/glcanvas.h:96`; `src/unix/glegl.cpp:898-915`;
  `src/unix/glx11.cpp:943-949`). Called before the native window exists, `SetSwapInterval` is deferred to
  the first swap and optimistically reports `Set` (`include/wx/unix/private/glcanvas.h:49-61`). macOS and
  MSW leave the driver default; MSW needs
  `WGL_EXT_swap_control` (`src/msw/glcanvas.cpp:797-822`). EGL has no adaptive VSync and
  `GetSwapInterval` returns `DefaultSwapInterval` (`src/unix/glegl.cpp:949-980`). Orca does not call it; frame
  pacing is `GLCanvas3D`'s own FPS cap.

**Pitfalls.**
- **Rule:** Build the viewport from physical pixels.
  ```cpp
  // Wrong: glViewport(0, 0, w, h);                         // GetClientSize(): logical
  // Right: const double s = GetContentScaleFactor();       // 1 on MSW
  //        glViewport(0, 0, int(w * s), int(h * s));        // Orca: GLCanvas3D::get_canvas_size()
  ```
  **Why:** on macOS Retina and GTK3 HiDPI a logical viewport covers only the bottom-left 1/scale of the
  drawable. Cite: `UVEditorCanvas.cpp` `gl_drawable_size`, `TextureImportDialog.cpp` `gl_viewport_size`.
- **Rule:** Check `IsShownOnScreen()` and `SetCurrent()`'s result before any GL call.
  **Why:** after a failed `SetCurrent` every GL call lands in whatever context is current — usually the
  3D view mid-frame.
  ```cpp
  // Wrong: SetCurrent(*m_context); glClear(...);
  // Right: if (m_context == nullptr || !IsShownOnScreen() || !SetCurrent(*m_context)) return;
  ```
  Cite: `UVEditorCanvas` paint path.
- **Rule:** Request MSAA explicitly. **Why:** canvases built from a NULL list or `Defaults()` lost
  multisampling in 3.3.

## EGL vs GLX

**Contract.** `wxGLCanvas::PreferGLX()` (3.3.2) "must be called before using any OpenGL functionality,
which includes not only creating wxGLCanvas but also checking for extension support or creating
attributes objects, doing it afterwards will trigger an assert failure and have no other effect"; "when
using wxGTK with Wayland, calling this function will have no effect anyhow as only EGL is supported";
the environment variable `wx_opengl_egl=0` does the same; it exists only when `wxHAS_GLX` is defined
(wxGTK/wxX11 on Unix) (`interface/wx/glcanvas.h:1082-1102`). `GetEGLVersion(major, minor)` (3.3.2)
returns true only when EGL is in use (false when `wxHAS_EGL` is undefined); `GetGLXVersion()` returns 0
when GLX is not in use; both exist only when `wxHAS_GLX` is defined (`interface/wx/glcanvas.h:999-1030`).
`wxGLCanvas::CreateSurface()` was removed "as exposing it doesn't make sense when wxGLCanvas may use
either EGL or GLX" (`docs/changes.txt:250-252`); "Allow using GLX and EGL in same program (#26023)" is
an upstream 3.3.2 wxGTK change (`docs/changes.txt:290-292`).

**Source** [source]:
- When both backends are built, **EGL is the default even on X11** unless `PreferGLX()` or the
  `opengl.egl` system option says otherwise (`src/unix/glcanvas.cpp:218-246`); GTK3 on Wayland always
  returns EGL. A late `PreferGLX` only asserts (`208-214`) — silent in Orca.
- `wxUSE_GLCANVAS_EGL` (`build/cmake/options.cmake:165`; configure `--disable-glcanvasegl`,
  `docs/gtk/install.md:203-204`; the EGL development package is a build requirement, `docs/gtk/install.md:77`)
  is forced OFF unless GTK3 and EGL are found
  (`build/cmake/init.cmake:559-561`); `wxHAS_EGL` is set at `build/cmake/init.cmake:531-540`.
- wxGTK's `wxGLCanvas` refuses to run (fatal message) on Wayland when built without EGL
  (`src/gtk/glcanvas.cpp:105-134`).
- Without an explicit alpha request, the EGL backend deliberately prefers an alpha-less config
  (`src/unix/glegl.cpp:801-860`).
- The EGL-based `wxGLCanvas` for Wayland dates from 3.1.5 (`docs/changes_32.txt:497`), so it predates
  Orca's upgrade; its
  high-DPI scale under EGL/Wayland was fixed in 3.3.0 (`docs/changes.txt:538`).

**OrcaSlicer.**
- `deps/wxWidgets/wxWidgets.cmake` passes `-DwxUSE_GLCANVAS_EGL=ON` (also in the Flatpak manifest).
- `GUI_App::on_init_inner`, under `#if defined(__WXGTK__) && wxHAS_EGL`, calls `wxGLCanvas::PreferGLX()`
  when `is_running_on_x11()`, before any canvas exists, for driver compatibility; Wayland uses EGL.
- `CLI::run` forces `GDK_BACKEND=x11` on a Wayland session when wx lacks EGL (see
  [WebKitGTK on Linux](#webkitgtk-on-linux-sessions)).
- `OpenGLManager::init_gl` loads GLAD after the context is current: `gladLoadGL(eglGetProcAddress)` on
  Wayland (`gladLoaderLoadGL()` would `dlopen`/`dlclose` `libGL` under EGL), `gladLoaderLoadGL()`
  everywhere else. It does not use `wxGLContext::GetProcAddress`.
- `OpenGLManager::detect_multisample` skips detection on Wayland without EGL, because
  `wxGLCanvas::IsDisplaySupported()` there would go through GLX and crash touching a non-existent X11
  display.

**Pitfall.**
- **Rule:** Call `PreferGLX()` before the first `wxGLAttributes`, canvas or extension query.
  **Why:** once the backend singleton exists the call is ignored, silently in Orca.

## Orca OpenGL stack: OpenGLManager and the shared context

**`OpenGLManager::create_wxglcanvas(parent)`** builds every main canvas from the legacy list:
`WX_GL_RGBA`, `WX_GL_DOUBLEBUFFER`, min RGB 8/8/8 and **alpha 8** (NVIDIA's `glReadPixels` does not
return alpha unless requested at context creation), depth 24, stencil 8 (outlines), and explicit
`WX_GL_SAMPLE_BUFFERS`/`WX_GL_SAMPLES` from `SETTING_OPENGL_AA_SAMPLES` (0/2/4/8/16, default 4). Because
MSAA is explicit, the 3.3 default change does not touch the 3D views. `detect_multisample` probes once
with `wxGLCanvas::IsDisplaySupported(attribList)`, only for wx ≥ 3.0.3, never on ChromeOS
(`PlatformFlavor::LinuxOnChromium` swaps red/blue with multisampling) and not on Wayland without EGL;
when it fails the sample entries are zeroed. The canvas gets `wxWANTS_CHARS` (keys reach the canvas and
ImGui) and an explicit `SetBackgroundStyle(wxBG_STYLE_PAINT)` (redundant with the wx base, harmless).

**`OpenGLManager::init_glcontext(canvas, …)`** creates **one** `wxGLContext` for the whole app:
when no version is requested, a descending loop over core versions with
`wxGLContextAttrs().PlatformDefaults().MajorVersion(m).MinorVersion(n).CoreProfile().ForwardCompatible()`,
each checked with `IsOK()` under `wxLogNull`; otherwise a compatibility profile or the requested core
version; finally `PlatformDefaults()` alone, without an `IsOK()` check. View3D, Preview and
AssembleView (`GUI_Preview.cpp`), the CAD `DesignCanvas` (all built by `create_wxglcanvas`) and
`UVEditorCanvas` call `wxGetApp().init_glcontext(*canvas)` and receive that context, so shaders and
`GLModel`s registered app-wide work on all of them.

**Secondary canvases.**
- `UVEditorCanvas` shares the context, so `gl_attrib_list()` mirrors `create_wxglcanvas` exactly,
  including the sample count (read from `OpenGLManager::can_multisample()`): WGL requires the HDC passed
  to `wglMakeCurrent` to have the pixel format the context was created with, and a different sample
  count is a different pixel format. It bails out when `SetCurrent` fails, sets and restores the GL
  state it depends on (cull, scissor, stencil) because the 3D view leaves its own behind, and only marks
  itself dirty when called from the gizmo's ImGui panel — i.e. from inside the 3D canvas's render pass.
- Self-contained previews (`TexturePreviewCanvas` in `TextureImportDialog`, `SkipPartCanvas` in
  `PartSkipDialog`) build `wxGLAttributes(...).EndList()` lists, own a private `wxGLContext(this)`, and
  share nothing with the main context; they have no MSAA (built from `PlatformDefaults()`/`Defaults()`).

**Touch.** `GLCanvas3D` binds `wxEVT_GESTURE_PAN/ZOOM/ROTATE`, calls
`EnableTouchEvents(wxTOUCH_ZOOM_GESTURE | wxTOUCH_ROTATE_GESTURE)`, and on macOS installs native
recognisers with `initGestures(m_canvas->GetHandle(), m_canvas)` (`Utils/MacDarkMode.mm`), cleared with
`nullptr` on unbind.

**Pitfalls.**
- **Rule:** A canvas that binds the shared context copies the main attribute list, MSAA included.
  **Why:** a mismatched pixel format makes `SetCurrent` fail silently on WGL; the canvas then shows
  whatever was in its back buffer — nothing. Cite: `UVEditorCanvas.cpp` `gl_attrib_list`.
- **Rule:** Never render a canvas from code that runs inside another canvas's frame; mark it dirty and
  `Refresh()`.
  **Why:** making the second surface current mid-frame corrupts the 3D view's frame, and when the
  second canvas is hidden its `SetCurrent` fails and the GL calls land on the 3D canvas.
  Cite: `UVEditorCanvas::set_islands` comment.

## GLCanvas3D rendering

`GLCanvas3D` is not a window: it wraps the `wxGLCanvas* m_canvas` created by `OpenGLManager`, binds its
events in `bind_event_handlers` and must unbind them (`GLCanvas3D::unbind_event_handlers`, reached at
shutdown through `Plater::unbind_canvas_event_handlers`) before teardown (`references/orca-architecture.md`).

**Render loop.**
- Handlers mark state dirty (`set_as_dirty()`, `request_extra_frame()`, `schedule_extra_frame(ms)`).
- `on_idle` collects dirty and overlay-dirty state, applies the 3D-mouse queue only when shown, enforces
  the FPS cap (`schedule_extra_frame` + `RequestMore()`), and renders via `_refresh_if_shown_on_screen()`.
- `on_paint`: before `m_initialized` it calls `render()` directly (initialisation); afterwards it renders
  immediately on MSW and only sets `m_dirty` elsewhere. It creates no `wxPaintDC`, which wxMSW 3.3
  tolerates (see [Paint](#wxglcanvas-and-wxglcontext)).
- `_render_frame` is re-entrancy-guarded (`m_in_render`; a nested call only sets `m_dirty`) and calls
  `SwapBuffers()` only when `m_canvas->IsShownOnScreen()`, because EGL swaps can block or fail on hidden
  or occluded canvases.
- `_set_current()` is `m_context != nullptr && m_canvas->SetCurrent(*m_context)`;
  `make_current_for_postinit()` forwards to it.
- `get_canvas_size()` multiplies `GetSize()` by the Retina helper's factor when `ENABLE_RETINA_GL`
  (`__APPLE__ || __WXGTK3__`): the backing scale on macOS, `GetContentScaleFactor()` on GTK3.
  `get_scale()` returns that factor (1 elsewhere); `on_mouse` rescales event coordinates the same way.

**Initialisation.**
- `GUI_App::post_init` (first idle) initialises GL only if the Prepare canvas `IsShownOnScreen()` **and**
  `make_current_for_postinit()` succeeds. Otherwise it re-enables rendering and marks the canvas dirty;
  on Linux it also clears `m_post_initialized` and returns, so the app idle handler calls `post_init`
  again (the map, size and paint events of the window being shown provide the next idle — without
  `RequestMore()` wx idles passively, `interface/wx/event.h:4476-4478`). On MSW and macOS the not-ready
  branch falls through and GL is initialised by the first paint. When the start page is not Prepare,
  `post_init` skips this block and GL loads at idle instead.
- `MainFrame::GLResourcesPrebuild::build_step` loads GL resources on the **hidden** Prepare canvas at
  idle, deliberately outside the "SetCurrent only when shown" doc: a hidden Windows child window keeps
  its device context, macOS attaches the context to a hidden view, and on GTK it calls
  `gtk_widget_realize` first because wx creates the canvas's GL surface when the widget is realised. If
  the context cannot be made current, the canvas's first render loads everything instead. Loading on a
  shown canvas under `Freeze()` would hold back the start page's paint, and on GTK `Freeze()` cannot hide
  the canvas (a native child window or a Wayland subsurface drawn outside GTK). Design:
  `docs/HLSD/deferred-page-construction.md` §"The 3D view's GL resources".

**Pitfalls.**
- **Rule:** Do one-time GL initialisation only when the canvas is shown and current; if either check
  fails, mark it not done and let idle retry — never fall through to GL entry points.
  **Why:** on Wayland/EGL the surface may not be committed when `post_init` runs; continuing left GLAD's
  function pointers null and the first Preview focus crashed in `Camera::apply_viewport`. Marking the
  canvas dirty alone never retries `post_init`.
  ```cpp
  // Wrong: log and continue with uninitialised GL
  if (canvas->IsShownOnScreen() && make_current_for_postinit()) init_gl();
  else canvas->set_as_dirty();                     // never retried on Wayland
  // Right
  if (!canvas->IsShownOnScreen() || !make_current_for_postinit()) {
      canvas->enable_render(true); canvas->set_as_dirty();
      m_post_initialized = false;                  // idle handler calls post_init again (Linux)
      return;
  }
  init_gl();
  ```
  Cite: d2c24fdabb (`GUI_App::post_init`).
- **Rule:** On MSW render from the GL canvas's paint handler, not only from idle; keep
  `wxBG_STYLE_PAINT`; call `SwapBuffers()` only when `IsShownOnScreen()`.
  **Why:** wx idle events come only from wx's own loop (`src/common/evtloopcmn.cpp:115-117`), and
  interactive resizing runs inside Windows' modal size/move loop, which wxMSW turns into
  `wxEVT_MOVE_START`/`END` only (`src/msw/window.cpp:5704-5719`); `WM_PAINT` still arrives. A
  dirty-flag-only `on_paint` left the canvas blank for the whole drag. A non-PAINT background style
  makes wx erase before every paint (flash); `wxGLCanvasBase` sets PAINT already — never reset it.
  EGL swaps block (Wayland) or fail on hidden or occluded canvases, hence the swap guard.
  ```cpp
  // on_paint — Wrong: m_dirty = true;   (idle never runs while resizing on MSW)
  #ifdef __WXMSW__
      _refresh_if_shown_on_screen(); m_dirty = false;
  #else
      m_dirty = true;
  #endif
  ```
  Cite: c06a0223a7 (`GLCanvas3D::on_paint`, `OpenGLManager::create_wxglcanvas`). For plain
  custom-painted windows see `references/painting-custom-widgets.md`.
- **Rule:** When alpha-blending into the default framebuffer, use
  `glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`, not
  `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`.
  **Why:** the framebuffer has alpha (Orca requests `WX_GL_MIN_ALPHA 8`), and Wayland compositors treat
  it as window transparency [external], so reduced destination alpha at anti-aliased edges made overlay
  icons show the desktop through the window. RGB blending is unchanged, so X11, Windows and macOS are
  unaffected. Cite: d8369e5f75 (`GLTexture::render_sub_texture`).

## The ImGui layer boundary

**What is ImGui.** Drawn inside `GLCanvas3D`, not wx windows — wx theming, `DPIDialog`, sizers and
`Widgets/` do not apply: gizmo panels and their controls (`GLGizmoBase::on_render_input_window`,
`GLGizmosManager`, CAD sketch tools), notifications (`NotificationManager`, `HintNotification`,
`SlicingProgressNotification`), the preview layer and move sliders (`IMSlider`), the G-code legend
(`GCodeViewer`), plate labels (`PartPlate`), `IMToolbar`, `GLToolbar` item option windows, the
variable-layer-height dialog, the 3D navigator, the debug camera window. Everything outside the canvas
(sidebar, tabs, dialogs, top bar, Home/Device pages) is wx.

**Input.** ImGui sees only what `GLCanvas3D::on_mouse`/`on_char`/`on_key` forward through
`ImGuiWrapper::update_mouse_data`/`update_key_data`; if ImGui wants the event (and no gizmo, object or
rectangle drag is active) it stops there. ImGui drags call `CaptureMouse()` and release it on button-up. Text
entry therefore needs canvas focus. ImGui text is UTF-8: use `_u8L`.

**Coordinates and scale.** ImGui works in physical pixels: `ImGuiWrapper::set_display_size` gets the
canvas size from `get_canvas_size()` and sets `DisplayFramebufferScale` to 1. Scale hard-coded sizes
by `GLCanvas3D::get_scale()`. `GLCanvas3D::set_imgui_scaling` passes `em_unit() * 1.5` as the font size
(on Windows the 1.5 is dropped above 30 px, using ImGui font scaling instead) plus the Retina factor
(`ENABLE_RETINA_GL`) or `GetContentScaleFactor()`.

**Overlays over GL.** On Wayland, EGL draws into a desynchronised `wl_subsurface` placed over the canvas
widget (`src/unix/glegl.cpp:710-738`) [source]; on GTK a child window over a `wxGLCanvas` is a native window that
does not reliably stack over GL (`docs/HLSD/design-tab.md`). Put overlays in ImGui or GL, or place wx
widgets beside the canvas (a sibling, not a child over it), or in a popup.

**Pitfall.**
- **Rule:** After building a large GL texture such as the ImGui font atlas, check its height against
  `GL_MAX_TEXTURE_SIZE` and re-pack with a doubled `TexDesiredWidth` (bounded retries) until it fits,
  stopping once `2 × width` would exceed the limit (both dimensions share it); start with
  `TexDesiredWidth = 0` so small glyph sets stay minimal.
  **Why:** XWayland on Intel exposes a smaller `GL_MAX_TEXTURE_SIZE`; an over-tall atlas fails to
  upload and every ImGui overlay renders as unreadable garbage.
  Cite: 22e121f4e4, 05b3c9053e (`ImGuiWrapper::init_font`).

## wxAuiManager and Orca's docking

**Contract and source.**
- **Managed window.** `SetManagedWindow(w)`: "wxAuiManager handles many events for the managed window,
  including ::wxEVT_SIZE, so any application-defined handlers for this window should take care to call
  wxEvent::Skip()" (`interface/wx/aui/framemanager.h:685-690`).
- **Update.** "Update() must be invoked after AddPane() or InsertPane() … any number of changes may be
  made to wxAuiPaneInfo structures (retrieved with wxAuiManager::GetPane), but to realize the changes,
  Update() must be called" (`interface/wx/aui/framemanager.h:747-756`). Batch, then `Update()` once.
- **Dock size** [source]. The docs limit "any new dock" to a `SetDockSizeConstraint` fraction of the
  window (`interface/wx/aui/framemanager.h:660-672`). The source applies it to the managed window's client
  size at the `Update()` that creates the dock, sizing the dock from its panes' `best_size` and then
  raising it to their `min_size` (`src/aui/framemanager.cpp:2748-2848`); a dock with a resizable pane
  keeps that size (`2674-2677`), and resizing the managed window only re-lays the docks out
  (`4542-4546`). A pane first shown before its managed window is laid out — a `wxDefaultSize` window is
  20×20 until its first sizer layout (`references/sizers-layout.md`) — docks at that width or its
  `min_size` and stays there. Hiding a dock's last shown pane empties it, and the next `Update()`
  removes it (`2733-2738`) with any `dock_size` a `LoadPerspective` restored, so the pane comes back
  at its `best_size`.
- **Names** [source]. A duplicate `Name()` hits `wxFAIL` (silent in Orca); duplicate and empty names
  get a random identifier (`src/aui/framemanager.cpp:949-1010`), which `LoadPerspective` can never
  match. `AddPane` rejects a null window, but its "already managed" check looks up `paneInfo.window`
  (`946-947`), which is null in a freshly built `wxAuiPaneInfo`, so adding a managed window again is not
  rejected: check `GetPane(window).IsOk()` first.
- **Pane references** [source]. `GetPane()` returns a reference into a `wxBaseObjectArray<wxAuiPaneInfo>`
  (`include/wx/aui/framemanager.h:425`) whose elements are heap-allocated: references survive
  `AddPane`, dangle after `DetachPane`.
- **`DetachPane`** neither destroys nor hides the window; it reparents a floating one back to the
  managed window and destroys its floating frame (`src/aui/framemanager.cpp:1325-1360`).
- **Perspectives.** 3.3 writes `"layout3|"` (`src/aui/framemanager.cpp:1869`) and accepts layout2/layout3
  (`1898-1904`; the source comment dates layout3 to 3.3.1); older wx rejects layout3 (a downgrade with the
  same app config loses the layout). The
  doc says panes missing from the string "remain unchanged" (`interface/wx/aui/framemanager.h:559-566`), but the source
  first **hides every pane and docks every dockable one** (`src/aui/framemanager.cpp:1906-1912`), so a pane
  missing from an old saved string ends up hidden. Perspective sizes are pixels;
  `SaveLayout`/`LoadLayout` (3.3.0) store DIPs (`src/aui/framemanager.cpp:1988-1991`).
- **`UnInit()`** is "unnecessary" since 3.1.4: it runs on the managed window's destruction and in the
  manager's destructor (`interface/wx/aui/framemanager.h:737-745`; `src/aui/framemanager.cpp:567-570`, `4490-4505`). It does not
  destroy floating frames (`812-833`), and `~wxAuiFloatingFrame` dereferences its owner manager
  (`src/aui/floatpane.cpp:69-78`): the manager must outlive every floating frame.
- **Close.** `wxEVT_AUI_PANE_CLOSE` goes to the managed window first and is vetoable (`ProcessMgrEvent`,
  `src/aui/framemanager.cpp:910-920`; `OnPaneButton`); `DestroyOnClose` panes are detached and `Destroy()`ed
  (`ClosePane`, `1431-1467`). With several handlers bound, each must `Skip()`.
- **3.3 changes.** `wxAUI_MGR_HINT_FADE` left the default flags (`interface/wx/aui/framemanager.h:43-50`;
  `docs/changes.txt:132`); `wxAUI_MGR_LIVE_RESIZE` joined them (`interface/wx/aui/framemanager.h:59-62`); `AlwaysUsesLiveResize()` "always
  returns false" as of 3.3.0 (`interface/wx/aui/framemanager.h:345`; `src/aui/framemanager.cpp:711-714`), non-live resize being
  restored (`docs/changes.txt:490`) — the older "live resize is always used on wxOSX and GTK3/Wayland"
  statement (`interface/wx/aui/framemanager.h:336-340`) is obsolete. Docking hints for floating frames use
  `::wxGetMousePosition()` (`src/aui/framemanager.cpp:4139-4148`, `4242-4251`), which Wayland cannot supply.
- `wxAuiNotebook` (not used by Orca): page indices are logical; use `GetPagePosition()` for on-screen
  order (`docs/changes.txt:121-126`); the default tab art changed (`35-37`).

**OrcaSlicer.** Orca's docks use `AuiMgr` (`AuiMgr.hpp`), a `wxAuiManager` whose `CreateFloatingFrame`
returns a `wxAuiFloatingFrame` that applies `UpdateFrameDarkUI`. `init(window)` calls
`SetManagedWindow(window)` and `SetDockSizeConstraint(1, 1)`, clears `wxAUI_MGR_ALLOW_FLOATING` on
Wayland and sets the dock-art metrics and colours; `apply_color_mode()` re-colours captions, sashes and
borders and is called from the owner's theme-switch handler. `Plater::priv::m_aui_mgr` manages the
Plater (Prepare and Preview); `DesignPanel::m_aui` manages the Design tab's body below its toolbar.
- Panes: `"sidebar"` (left, no close button, not top/bottom dockable, `BestSize` in em units),
  `"uv_editor"` (right, hidden; forced hidden again after a layout load), `"main"`
  (`CenterPane().PaneBorder(false)`, holding `panel_3d` with View3D, Preview and AssembleView in one
  sizer). Every `wxEVT_AUI_PANE_CLOSE` handler bound on `q` calls `Skip()`.
- Persistence: `m_default_window_layout = SavePerspective()` right after the panes are added; the saved
  `window_layout` app-config string goes through `sanitize_window_layout_for_wayland` (strips
  floating/floatable bits on Wayland) and `LoadPerspective(layout, false)`, falling back to the default
  on failure; then one `Update()`. `Plater::priv::reset` saves `SavePerspective()` back to
  `window_layout`. `AuiMgr::track_docked_size` copies a docked pane's size into `BestSize` on idle
  (wxAUI does not record a dragged sash there), so the width persists. `Plater::priv::reset_window_layout` is the model for
  re-establishing panes after a load: it loads the default perspective, then re-shows each plugin pane
  the perspective does not list.
- Plugin panes: `Plater::add_dock_pane(window, name, caption, dock, size, on_close)` (window must be a
  Plater child) uniquifies the name (`name#2`, …), sets `DestroyOnClose(true)`, restores a saved
  per-pane entry with `LoadPaneInfo`, forces `Dock().Floatable(false)` when floating is disabled, tracks
  the docked size on idle, and calls `Update()`. `remove_dock_pane` detaches, `Update()`s and
  `Destroy()`s without running `on_close`; `show_dock_pane` toggles and updates. Pane names come from
  `plugin_pane_name` (`DockPanel.cpp`), which replaces `|`, `;`, `=`, `\` — the perspective-string
  delimiters.
- Other pages: a page outside the Plater docks through its own `AuiMgr` member, since the Plater's
  manages only the Plater. Its managed window is a body panel below the page's toolbar; a sidebar
  takes `AuiMgr::sidebar_pane_info()`; the layout has its own app-config key, is reset from
  `Plater::reset_window_layout`, and is saved by a `shutdown()` called from `MainFrame::shutdown`,
  which also detaches a floating pane (Lifetime below). Where floating is disabled it forces
  `Dock().Floatable(false)` after loading, as plugin panes do. A `LazyPage` builds its panel at 20×20
  and lays it out only as the tab shows, so the sidebar stays hidden until the tab is first shown (Dock
  size above). `DesignPanel` is the model
  (`docs/HLSD/design-tab.md`). A `GLCanvas3D` beside such a sidebar gets that sidebar's collapse
  button with `set_collapse_toolbar`; without it the canvas falls back to the Plater's button, which
  collapses Prepare's sidebar.
- Lifetime: no explicit `UnInit`. `m_aui_mgr` is a `Plater::priv` member, so it is destroyed in
  `~Plater`, before `~wxWindow` destroys the Plater's children (`wxWindowBase::DestroyChildren`,
  `src/common/wincmn.cpp:586-609`); a floating frame is such a child (`CreateFloatingFrame(m_frame, …)`,
  `src/aui/framemanager.cpp:3151`), so a pane still floating at that point outlives the manager [source].
  Plugin panes are removed earlier (`MainFrame::shutdown` → `Plater::remove_dock_panes`). A handler
  that can outlive its pane looks the pane up when it runs, as `track_docked_size` does: an idle
  handler bound on the pane's window outlives the `DetachPane` in `DesignPanel::shutdown`. Never detach
  a pane whose `GetPane()` reference is captured (Pane references above). `DesignPanel::shutdown` is
  the model for a member manager: it detaches a
  floating sidebar, which `Destroy()`s the frame into the pending-delete list, and the main frame's
  deletion deletes pending top-level children before its own children (`~wxTopLevelWindowBase`,
  `src/common/toplvcmn.cpp:67-93`), while the panel and its manager are still alive [source].

**Pitfalls.**
- **Rule:** Call `Update()` once after a batch of `AddPane`/`wxAuiPaneInfo` changes.
  **Why:** pane edits are inert until then.
  ```cpp
  // Wrong: m_aui_mgr.GetPane(w).Show();            // nothing happens
  // Right: m_aui_mgr.GetPane(w).Show(); m_aui_mgr.Update();
  ```
- **Rule:** Give each pane a unique, stable `Name()` without layout delimiters.
  **Why:** random or colliding names can never be matched by a saved perspective.
- **Rule:** After `LoadPerspective`, re-establish panes the saved string may not know, and handle a
  `false` return by loading the default perspective.
  **Why:** it hides every pane it does not find, contrary to its doc.
- **Rule:** Handlers on the managed window for `wxEVT_SIZE` and `wxEVT_AUI_PANE_CLOSE` call `Skip()`.
  **Why:** AUI lays out from the size event, and other close handlers (including AUI's own) must run.
- **Rule:** After `DetachPane`, destroy (or re-parent) the window yourself.
  ```cpp
  // Wrong: m_aui_mgr.DetachPane(w); m_aui_mgr.Update();      // window lingers as a stray child
  // Right: if (m_aui_mgr.DetachPane(w)) m_aui_mgr.Update(); w->Destroy();  // Plater::priv::remove_dock_pane
  ```
- **Rule:** Disable floating on Wayland (clear `wxAUI_MGR_ALLOW_FLOATING`, `Floatable(false)`, strip
  floating state from saved layouts).
  **Why:** AUI's floating and docking-hint logic needs global pointer positions and top-level window
  moves (`wxGetMousePosition`, `wxEVT_MOVE`), which Wayland does not provide.

## wxAuiToolBar and BBLTopbar

**Contract and source.**
- Call `Realize()` after adding or changing tools: it measures the tools and sets the bar's size
  [source] (`src/aui/auibar.cpp:1952-1982`; declared without a doc comment at
  `interface/wx/aui/auibar.h:1073`).
- [source] Left-down on **any** enabled tool sends `wxEVT_AUITOOLBAR_TOOL_DROPDOWN`;
  `IsDropDownClicked()` tells whether the arrow was hit. Only if that event is unprocessed or skipped
  does the bar capture the mouse — and the `wxEVT_MENU` tool click is sent on left-up only while the bar
  holds capture (`src/aui/auibar.cpp:2715-2745`, `2755-2805`). A DROPDOWN handler that consumes the
  event therefore suppresses the click.
- `DeleteTool` does not delete a control added with `AddControl`; use `DestroyTool` (`interface/wx/aui/auibar.h:1105-1130`).
- `wxAuiDefaultToolBarArt` is a **macro**: `wxAuiMSWToolBarArt` on MSW with uxtheme,
  `wxAuiGenericToolBarArt` elsewhere (`include/wx/aui/auibar.h:799-807`), so a subclass has a different
  base on Windows.

**OrcaSlicer.** `BBLTopbar` (`BBLTopbar.hpp/.cpp`) is a standalone
`wxAuiToolBar(…, wxAUI_TB_TEXT | wxAUI_TB_HORZ_TEXT)` used as the custom title bar in `MainFrame`'s
sizer on Windows and Linux (macOS uses the native menu bar — see `references/popups-menus.md`); it is
not an AUI pane. It draws with `BBLTopbarArt : wxAuiDefaultToolBarArt`, calls `Realize()` after
building and fixes its height to `FromDIP(30)`, and `BBLTopbar::Rescale()` rebuilds bitmaps and
re-`Realize()`s on DPI change. **Every** button — menus, undo/redo, open/save, iconize/maximize/close —
acts on `wxEVT_AUITOOLBAR_TOOL_DROPDOWN` (mouse-down, consumed). Window dragging uses
`ClientToScreen(event.GetPosition())` instead of `wxGetMousePosition()` and, on GTK,
`gtk_window_begin_move_drag` (`BBLTopbar::OnMouseLeftDown`).

**Pitfalls.**
- **Rule:** Pick one click model per tool; on `BBLTopbar` act on mouse-down through
  `wxEVT_AUITOOLBAR_TOOL_DROPDOWN` like every other topbar button.
  **Why:** a DROPDOWN handler that consumes the event leaves the bar without capture, so left-up sends
  no `wxEVT_MENU`; mixing the two models on one tool silently loses the click.
  ```cpp
  // Wrong: DROPDOWN consumed (e.g. for feedback) and the action bound to the click
  Bind(wxEVT_AUITOOLBAR_TOOL_DROPDOWN, [](wxAuiToolBarEvent&) { /* highlight */ }, wxID_SAVE);
  Bind(wxEVT_MENU, [this](wxCommandEvent&) { /* save */ }, wxID_SAVE);   // never arrives
  // Right (topbar convention)
  this->Bind(wxEVT_AUITOOLBAR_TOOL_DROPDOWN, &BBLTopbar::OnSaveProject, this, wxID_SAVE);
  ```
- **Rule:** Test custom toolbar art on Windows and on another OS.
  **Why:** `wxAuiDefaultToolBarArt` is MSW themed art on Windows and generic art elsewhere.

## Camera and media

**Contract.** `wxMediaCtrl`: "Call Load() …, catch the EVT_MEDIA_LOADED event, and then call Play()"
(`interface/wx/mediactrl.h:107-109`); state changes are asynchronous — "the media may not actually be in
the wxMEDIASTATE_PLAYING … catch the event relevant to the state" (`interface/wx/mediactrl.h:147-150`).

**OrcaSlicer.** Orca builds `wxUSE_MEDIACTRL=ON` but instantiates no `wxMediaCtrl`; `wx/mediactrl.h` is
included for `wxMediaState`, so keep the option on. The camera view is driven by `MediaPlayCtrl`
through `IMediaController` (`Load(wxURI)`, `Play`, `Stop`, `GetState` → `wxMediaState`,
`GetLastError`, `GetVideoSize`), with two implementations:
- **`wxMediaCtrl3`** — a plain `wxWindow` (plus `BambuLib`) created in `StatusBasePanel::create_monitoring_page`. A worker
  thread (`PlayThread`/`PlayFfmpeg`) decodes frames; the frame (a `wxBitmap` on MSW, a `wxImage`
  elsewhere) is copied into `m_frame` under `m_mutex`; repaint requests are coalesced with
  `std::atomic_bool m_refresh_pending` + `CallAfter([this]{ m_refresh_pending.store(false); Refresh(); })`;
  `paintEvent` draws the frame under the same mutex; state changes go out as `EVT_MEDIA_CTRL_STAT`. The
  `CallAfter` is queued on the control itself, so it is discarded if the control dies first; posting to
  the control from the worker is safe only because `~wxMediaCtrl3` joins the decoding thread before the
  window's base destructors run (the joined-thread exception in `references/threads-timers-app.md`
  §Marshaling to the GUI thread).
- **`WebMediaController`** — drives a web view made with `WebView::CreateWebView` for camera streams
  rendered in a browser; attached with `MediaPlayCtrl::SetWebMediaController`.

There is no per-platform native player (Win32 / GStreamer / AVFoundation) behind the camera view: new
camera sources implement `IMediaController` rather than wrapping a native media control (7e3724b5f3,
97955dbab8 removed the native-player paths).

**Pitfall.**
- **Rule:** From a decoding thread, touch only mutex-protected frame state and post at most one pending
  repaint, queued on the view itself.
  **Why:** wx windows are main-thread only; an unthrottled `CallAfter` per frame floods the event queue;
  a `CallAfter` on `wxGetApp()` would outlive the view. Frames cross threads only through `m_frame`, which
  the worker and `paintEvent` touch under `m_mutex` (wx image objects have a non-atomic ref-count), and
  queuing on the view from the worker relies on the view's destructor joining the thread first; a worker
  that is not joined that way posts to the app with an alive flag.
  ```cpp
  // Wrong (worker): m_frame = frame; Refresh();
  // Right (worker): { std::lock_guard lk(m_mutex); m_frame = frame; }
  //                 if (!m_refresh_pending.exchange(true))
  //                     CallAfter([this]{ m_refresh_pending.store(false); Refresh(); });
  ```
  Cite: `wxMediaCtrl3::PlayFfmpeg`, `wxMediaCtrl3::~wxMediaCtrl3`; `references/threads-timers-app.md` §Marshaling to the GUI thread.
