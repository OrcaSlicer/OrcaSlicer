#ifndef slic3r_GUI_PluginProgressDialog_hpp_
#define slic3r_GUI_PluginProgressDialog_hpp_

#include <functional>
#include <memory>

#include <wx/event.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/window.h>

#include "Widgets/ProgressDialog.hpp"

namespace Slic3r { namespace GUI {

// A host-owned progress dialog for Python plugins. This class is deliberately
// Python-agnostic; the plugin layer owns any pybind/GIL concerns and marshals
// all calls to the UI thread.
//
// It derives from OrcaSlicer's own Slic3r::GUI::ProgressDialog (a real wxDialog
// with themable wx children) rather than the native wxProgressDialog: on Windows
// wxProgressDialog is a comctl32 TaskDialog running on a worker thread with no
// recolorable wx surface, so it cannot follow OrcaSlicer's (OS-independent) dark
// theme. The base themes itself via UpdateDlgDarkUI(this) in its Create().
class PluginProgressDialog : public ProgressDialog
{
public:
    using CloseHandler = std::function<void()>;

    // on_destroyed runs from the destructor on every path and must touch
    // host-side state only (no Python / no derived members).
    PluginProgressDialog(wxWindow*       parent,
                         const wxString& title,
                         const wxString& message,
                         int             maximum,
                         int             style,
                         CloseHandler    on_destroyed = nullptr);
    ~PluginProgressDialog() override;

    // Convenience helpers for plugin-host callers. MAIN-THREAD ONLY.
    static PluginProgressDialog* create_dialog(wxWindow*       parent,
                                               const wxString& title,
                                               const wxString& message,
                                               int             maximum,
                                               int             style,
                                               CloseHandler    on_destroyed);
    static bool pulse(PluginProgressDialog* dialog, const wxString& message = wxEmptyString);
    static bool update(PluginProgressDialog* dialog, int value, const wxString& message = wxEmptyString);
    static void start_pulse(PluginProgressDialog* dialog, int interval_ms, const wxString& message = wxEmptyString);
    static void stop_pulse(PluginProgressDialog* dialog);
    static void request_close(PluginProgressDialog* dialog);

    // MAIN-THREAD ONLY.
    bool pulse(const wxString& message = wxEmptyString);
    bool update(int value, const wxString& message = wxEmptyString);
    void start_pulse(int interval_ms, const wxString& message = wxEmptyString);
    void stop_pulse();
    void close();
    bool is_open() const { return m_open; }

private:
    void on_timer(wxTimerEvent& event);

    bool                     m_open{true};
    wxString                 m_pulse_message;
    std::unique_ptr<wxTimer> m_timer;
    CloseHandler             m_on_destroyed;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PluginProgressDialog_hpp_
