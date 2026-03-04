Double Title Bar Issue — Summary

  Problem

  OrcaSlicer shows two title bars on Linux:
  1. Window manager (KDE/KWin) title bar — "Untitled" with system min/max/close buttons
  2. OrcaSlicer's custom BBLTopbar — File menu, toolbar icons, and its own min/max/close buttons

  Root Cause

  OrcaSlicer was designed with a custom title bar (BBLTopbar at src/slic3r/GUI/BBLTopbar.cpp). On Windows, the system title bar is removed by intercepting WM_NCCALCSIZE in
  MainFrame::MSWWindowProc() (line ~732). On macOS, it uses a simple panel instead. On Linux/GTK, there is no equivalent code to remove window manager decorations.

  The frame style at MainFrame.cpp:164:
  #define BORDERLESS_FRAME_STYLE (wxRESIZE_BORDER | wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxCLOSE_BOX)
  Omits wxCAPTION, but wxGTK3 still draws WM decorations because the button flags imply a standard decorated window.

  wxGTK internals: During Create(), wxGTK computes m_gdkDecor and m_gdkFunc from the style flags.
  Later, GTKHandleRealized() calls gdk_window_set_decorations(window, m_gdkDecor) and
  gdk_window_set_functions(window, m_gdkFunc), overriding any earlier gtk_window_set_decorated() call.
  Both m_gdkDecor and m_gdkFunc are public members of wxTopLevelWindowGTK.

  Attempts & Results

  Attempt 1: gtk_window_set_decorated(FALSE) in constructor (original code at line 200)
  Result: FAILED — wxGTK's GTKHandleRealized() re-applies decorations afterward.

  Attempt 2: wxBORDER_NONE in BORDERLESS_FRAME_STYLE + gtk_window_set_decorated(FALSE) +
             g_signal_connect_after realize handler + wxEVT_SHOW fallback
  Changes:
    - Added wxBORDER_NONE to BORDERLESS_FRAME_STYLE for __WXGTK__
    - Called gtk_window_set_decorated(FALSE) in constructor
    - Added g_signal_connect_after on "realize" to call gdk_window_set_decorations(0)
    - Added GTK decoration removal in wxEVT_SHOW handler
  Result: Title bar REMOVED successfully, but RESIZE BROKEN — wxBORDER_NONE causes wxGTK
          to set m_gdkFunc=0 (disabling all window functions including resize).

  Attempt 3: Selective Motif hints — gdk_window_set_decorations(GDK_DECOR_BORDER | GDK_DECOR_RESIZEH)
             via g_signal_connect_after realize handler (no wxBORDER_NONE, no gtk_window_set_decorated)
  Changes:
    - Reverted BORDERLESS_FRAME_STYLE to original (no wxBORDER_NONE)
    - Used gdk_window_set_decorations with only GDK_DECOR_BORDER | GDK_DECOR_RESIZEH
    - Applied via g_signal_connect_after on "realize" + wxEVT_SHOW fallback
  Result: FAILED — Title bar came back. WM (KDE/KWin) ignored the selective hints,
          or our realize handler didn't run after wxGTK's.

  Attempt 4: GDK_DECOR_ALL XOR — gdk_window_set_decorations(GDK_DECOR_ALL | GDK_DECOR_TITLE | GDK_DECOR_MENU)
  Changes:
    - Same as attempt 3 but used GDK_DECOR_ALL with exclusion flags
    - With GDK_DECOR_ALL set, additional flags indicate decorations to REMOVE
  Result: FAILED — Title bar still there. Motif hint approach not working on KDE/KWin,
          or g_signal_connect_after not firing after wxGTK's handler.

  Attempt 5: Set m_gdkDecor = 0 directly
  Changes:
    - Set m_gdkDecor = 0 in constructor, before realization
    - wxGTK's own GTKHandleRealized() then applies zero decorations
    - m_gdkFunc left intact to preserve resize/minimize/maximize/close functions
  Result: Title bar REMOVED successfully, but RESIZE BROKEN — KDE/KWin does not provide
          resize handles for windows with zero decorations.

  Attempt 6: CSD titlebar + app-level resize handler + WM-integrated drag (CURRENT)
  Changes:
    MainFrame.cpp:
      - Added #include <gtk/gtk.h> under __WXGTK__ guard
      - Kept m_gdkDecor = 0 to prevent wxGTK from re-adding WM decorations
      - Added GTK CSD titlebar: gtk_window_set_titlebar() with a zero-height GtkBox widget
        to enter GTK Client-Side Decoration mode (CSD provides invisible resize handles)
      - Added CSS provider to remove CSD shadow/margin around the window
      - Added GtkResizeBorderHandler class (wxEventFilter) as app-level resize fallback:
        * Detects mouse within 8px of window edges
        * Shows resize cursors (GDK_TOP_SIDE, GDK_BOTTOM_LEFT_CORNER, etc.)
        * On left-click near edge, calls gtk_window_begin_resize_drag() to hand off to WM
        * Skips when maximized or fullscreen
        * Registered via wxEvtHandler::AddFilter(), cleaned up in shutdown()
    MainFrame.hpp:
      - Added forward declaration and member: GtkResizeBorderHandler* m_resize_border_handler
    BBLTopbar.cpp:
      - Added #include <gtk/gtk.h> under __WXGTK__ guard
      - Replaced manual CaptureMouse()+Move() drag with gtk_window_begin_move_drag()
        for WM-integrated window dragging on Linux

  Result: PARTIALLY WORKING
    - Title bar: REMOVED (only BBLTopbar visible) ✓
    - Resize from edges: WORKS, but with delay — resize cursors/handles don't respond
      immediately after app start, takes some time before they become active ✗
    - Menu bar: Same delayed responsiveness issue ✗
    - Maximize: Works ✓
    - Restore from maximized: BROKEN — window does not restore to previous size ✗
    - Window drag via BBLTopbar: Works (using gtk_window_begin_move_drag) ✓

  Remaining Issues

  1. Delayed resize/menu responsiveness: The GtkResizeBorderHandler (wxEventFilter) and
     possibly the CSD resize handles don't activate immediately on app start. May be related
     to GTK widget realization timing — the CSD titlebar or GDK window may not be fully set
     up when the filter starts intercepting events. Possible fixes:
       - Defer CSD setup to after window realization (g_signal_connect on "realize")
       - Check gtk_widget_get_realized() in the filter before processing
       - The delay may also affect the menu bar if CSD mode changes event routing

  2. Restore from maximized broken: After maximizing, clicking the restore button doesn't
     return the window to its previous size. This could be caused by:
       - CSD mode interfering with wxWidgets' maximize/restore logic
       - The zero-height CSD titlebar confusing the WM's unmaximize geometry calculation
       - m_gdkDecor = 0 conflicting with CSD mode's decoration management
     Possible fixes:
       - Save/restore window geometry manually in BBLTopbar maximize/restore handlers
       - Try without m_gdkDecor = 0 (CSD titlebar alone may be sufficient to hide WM bar)
       - Use gtk_window_unmaximize() directly instead of wxWidgets' Restore()

  Key Insight

  On KDE/KWin, gdk_window_set_decorations(0) removes ALL WM chrome including invisible
  resize edges. The resize function hint (m_gdkFunc) only controls whether resize is
  *allowed*, not whether the WM draws resize handles. Without any decoration hint, KWin
  provides no resize affordance at all.

  GTK CSD mode (via gtk_window_set_titlebar) provides its own invisible resize handles,
  but may have timing/interaction issues with wxWidgets' window management.

  Key Files

  | File | Purpose |
  |------|---------|
  | src/slic3r/GUI/MainFrame.cpp:162-167 | BORDERLESS_FRAME_STYLE definition |
  | src/slic3r/GUI/MainFrame.cpp:182 | Frame construction with style |
  | src/slic3r/GUI/MainFrame.cpp:101-211 | GtkResizeBorderHandler class definition |
  | src/slic3r/GUI/MainFrame.cpp:307-338 | CSD titlebar setup + handler instantiation |
  | src/slic3r/GUI/MainFrame.cpp:1085-1088 | Handler cleanup in shutdown() |
  | src/slic3r/GUI/MainFrame.hpp:427-430 | GtkResizeBorderHandler member declaration |
  | src/slic3r/GUI/MainFrame.cpp:717-799 | Windows WM_NCCALCSIZE handling (reference) |
  | src/slic3r/GUI/BBLTopbar.cpp:622-647 | Linux WM-integrated drag (gtk_window_begin_move_drag) |
  | src/slic3r/GUI/BBLTopbar.hpp | BBLTopbar class definition |
  | deps/.../wx/gtk/toplevel.h:110-111 | m_gdkDecor, m_gdkFunc declarations (public) |

  Current State

  Attempt 6 is active. Title bar removed, resize works but with startup delay, restore from
  maximized is broken. Next steps should focus on fixing the restore issue and the delayed
  responsiveness.
