#ifndef slic3r_GUI_PluginWebDialog_hpp_
#define slic3r_GUI_PluginWebDialog_hpp_

#include "Widgets/WebViewHostDialog.hpp"

#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>
#include <wx/webview.h>

namespace Slic3r { namespace GUI {

// A host-owned webview window that renders plugin-supplied raw HTML and bridges
// messages to/from the page through a small injected `window.orca` API.
//
// This class is deliberately Python-agnostic: it talks to the plugin layer only
// through std::function hooks. Those hooks must NOT capture bare pybind11
// objects, because the dialog can be destroyed on the main thread without the
// GIL held; the plugin layer wraps any Python callables in a GIL-safe holder.
//
// Usable both modally (ShowModal -> read result()) and modelessly (Show()).
class PluginWebDialog : public Slic3r::GUI::WebViewHostDialog
{
public:
    using MessageHandler = std::function<void(const nlohmann::json& data)>;
    using SubmitHandler  = std::function<void(const nlohmann::json& data)>;
    using CloseHandler   = std::function<void()>;

    // on_submit fires once for window.orca.submit(). on_close fires only on a
    // user/JS-initiated close (while the window is alive). on_destroyed runs from
    // the destructor on every path and must touch host-side state only (no Python
    // / no derived members).
    PluginWebDialog(wxWindow*          parent,
                    const wxString&    title,
                    const std::string& html,
                    const wxSize&      size,
                    MessageHandler     on_message,
                    SubmitHandler      on_submit,
                    CloseHandler       on_close,
                    CloseHandler       on_destroyed,
                    long               wx_style = wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxMAXIMIZE_BOX | wxRESIZE_BORDER);
    ~PluginWebDialog() override;

    static void post_message(PluginWebDialog* dialog, const nlohmann::json& data);
    static void request_close(PluginWebDialog* dialog);
    static void destroy_for_plugin(PluginWebDialog* dialog);

    // Push a payload to the page; delivered to handlers registered via
    // window.orca.onMessage(). MAIN-THREAD ONLY (the plugin layer marshals).
    void push_message(const nlohmann::json& data);

    bool is_open() const { return m_open; }

    // The payload submitted via window.orca.submit() (modal use), if any.
    const std::optional<nlohmann::json>& result() const { return m_result; }

protected:
    void on_script_message(const nlohmann::json& payload) override;
    // Plugin HTML is loaded as a raw string, not a localized resource URL.
    bool append_language_to_url() const override { return false; }
    void add_user_scripts() override;

private:
    void on_bootstrap_event(wxWebViewEvent& event);
    void load_plugin_content();
    void on_close_window(wxCloseEvent& event);
    void fire_submit(const nlohmann::json& data);
    void fire_close();
    void finish(bool submitted, const nlohmann::json& data);

    std::string                   m_html;
    bool                          m_content_loaded{false};
    bool                          m_open{true};
    bool                          m_close_fired{false};
    std::optional<nlohmann::json> m_result;
    MessageHandler                m_on_message;
    SubmitHandler                 m_on_submit;
    CloseHandler                  m_on_close;
    CloseHandler                  m_on_destroyed;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PluginWebDialog_hpp_
