#include "PluginHostUi.hpp"

#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp" // PythonGILState

#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/MainFrame.hpp>
#include <slic3r/GUI/MsgDialog.hpp>
#include <slic3r/GUI/PluginProgressDialog.hpp>
#include <slic3r/GUI/PluginWebDialog.hpp>

#include <nlohmann/json.hpp>
#include <pybind11/pybind11.h>

#include <boost/log/trivial.hpp>

#include <wx/app.h>
#include <wx/defs.h>
#include <wx/window.h>

#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace py = pybind11;
using json   = nlohmann::json;

namespace Slic3r {
namespace {

// --------------------------------------------------------------------------
// JSON <-> Python conversion (caller must hold the GIL).
// --------------------------------------------------------------------------
py::object json_to_py(const json& j)
{
    switch (j.type()) {
    case json::value_t::null:            return py::none();
    case json::value_t::boolean:         return py::bool_(j.get<bool>());
    case json::value_t::number_integer:  return py::int_(j.get<std::int64_t>());
    case json::value_t::number_unsigned: return py::int_(j.get<std::uint64_t>());
    case json::value_t::number_float:    return py::float_(j.get<double>());
    case json::value_t::string:          return py::str(j.get<std::string>());
    case json::value_t::array: {
        py::list lst;
        for (const auto& e : j)
            lst.append(json_to_py(e));
        return lst;
    }
    case json::value_t::object: {
        py::dict d;
        for (auto it = j.begin(); it != j.end(); ++it)
            d[py::str(it.key())] = json_to_py(it.value());
        return d;
    }
    default: return py::none();
    }
}

json py_to_json(const py::handle& o)
{
    if (o.is_none())
        return json(nullptr);
    if (py::isinstance<py::bool_>(o)) // bool before int (bool subclasses int in Python)
        return o.cast<bool>();
    if (py::isinstance<py::int_>(o))
        return o.cast<std::int64_t>();
    if (py::isinstance<py::float_>(o))
        return o.cast<double>();
    if (py::isinstance<py::str>(o))
        return o.cast<std::string>();
    if (py::isinstance<py::bytes>(o))
        return o.cast<std::string>();
    if (py::isinstance<py::dict>(o)) {
        json obj = json::object();
        for (auto item : py::reinterpret_borrow<py::dict>(o))
            obj[py::str(item.first).cast<std::string>()] = py_to_json(item.second);
        return obj;
    }
    if (py::isinstance<py::list>(o) || py::isinstance<py::tuple>(o)) {
        json arr = json::array();
        for (auto e : o)
            arr.push_back(py_to_json(e));
        return arr;
    }
    return py::str(o).cast<std::string>(); // fallback: str()
}

// --------------------------------------------------------------------------
// GIL-safe holder for a Python callable. A std::function that captured a bare
// py::object could be destroyed on the main thread without the GIL (a dialog
// teardown), which would Py_DECREF unsafely. Wrapping the callable here means
// the GIL is acquired exactly when the last reference is released, on any thread.
// --------------------------------------------------------------------------
struct GilSafeCallable
{
    py::object fn;
    explicit GilSafeCallable(py::object f) : fn(std::move(f)) {}
    ~GilSafeCallable()
    {
        if (fn) {
            PythonGILState gil;
            if (gil)
                fn = py::object();
            else
                (void) fn.release();
        }
    }
};
using CallablePtr = std::shared_ptr<GilSafeCallable>;

CallablePtr make_holder(py::object obj)
{
    if (!obj || obj.is_none())
        return nullptr;
    return std::make_shared<GilSafeCallable>(std::move(obj));
}

// Adapt a Python callable to a GUI message handler that acquires the GIL and
// swallows/logs exceptions (a raising handler must not escape into wx events).
GUI::PluginWebDialog::MessageHandler make_message_adapter(py::object on_message)
{
    CallablePtr holder = make_holder(std::move(on_message));
    if (!holder)
        return nullptr;
    return [holder](const json& data) {
        PythonGILState gil;
        if (!gil)
            return;
        try {
            holder->fn(json_to_py(data));
        } catch (py::error_already_set& e) {
            BOOST_LOG_TRIVIAL(error) << "orca.host.ui on_message handler raised: " << e.what();
            PyErr_Clear();
        }
    };
}

// --------------------------------------------------------------------------
// Registry of live plugin UI resources. Keyed by an opaque id; tracks the
// owning plugin so all of a plugin's UI can be torn down on unload.
// --------------------------------------------------------------------------
class UiRegistry
{
public:
    static UiRegistry& instance()
    {
        static UiRegistry r;
        return r;
    }

    int reserve_id()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_next_id++;
    }
    void bind(int id, wxWindow* window, const std::string& plugin_key)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_resources[id] = window;
        m_owners[id]    = plugin_key;
    }
    void remove(int id)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_resources.erase(id);
        m_owners.erase(id);
    }

    template<typename T>
    T* get_as(int id)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_resources.find(id);
        if (it == m_resources.end())
            return nullptr;
        return dynamic_cast<T*>(it->second);
    }
    bool is_open(int id)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_resources.count(id) > 0; // presence only; no pointer deref -> thread-safe
    }
    std::vector<wxWindow*> take_for_plugin(const std::string& plugin_key)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        std::vector<wxWindow*> out;
        for (auto it = m_owners.begin(); it != m_owners.end();) {
            if (it->second == plugin_key) {
                auto rit = m_resources.find(it->first);
                if (rit != m_resources.end()) {
                    out.push_back(rit->second);
                    m_resources.erase(rit);
                }
                it = m_owners.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }

private:
    std::mutex                           m_mtx;
    std::unordered_map<int, wxWindow*>   m_resources;
    std::unordered_map<int, std::string> m_owners;
    int                                  m_next_id{1};
};

// --------------------------------------------------------------------------
// Run a (pure C++/wx) callable on the main/UI thread, blocking the caller until
// it completes, with the GIL released across the wait. If already on the main
// thread, run inline (also with the GIL released so other Python threads run).
// --------------------------------------------------------------------------
template<typename Fn>
auto run_on_ui_blocking(Fn&& fn) -> std::invoke_result_t<Fn&>
{
    using R = std::invoke_result_t<Fn&>;
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    if (wxIsMainThread()) {
        py::gil_scoped_release nogil;
        return fn();
    }

    std::promise<R> prom;
    std::future<R>  fut = prom.get_future();

    py::gil_scoped_release nogil;
    GUI::wxGetApp().CallAfter([&prom, &fn]() {
        try {
            if constexpr (std::is_void_v<R>) {
                fn();
                prom.set_value();
            } else {
                prom.set_value(fn());
            }
        } catch (...) {
            prom.set_exception(std::current_exception());
        }
    });
    return fut.get();
}

wxWindow* ui_parent()
{
    return wxTheApp == nullptr ? nullptr : dynamic_cast<wxWindow*>(GUI::wxGetApp().mainframe);
}

// --------------------------------------------------------------------------
// orca.host.ui.message
// --------------------------------------------------------------------------
long message_style(const std::string& buttons, const std::string& icon)
{
    long style = wxOK;
    if (buttons == "ok_cancel")
        style = wxOK | wxCANCEL;
    else if (buttons == "yes_no")
        style = wxYES_NO;
    else if (buttons == "yes_no_cancel")
        style = wxYES_NO | wxCANCEL;

    if (icon == "warning")
        style |= wxICON_WARNING;
    else if (icon == "error")
        style |= wxICON_ERROR;
    else if (icon == "question")
        style |= wxICON_QUESTION;
    else
        style |= wxICON_INFORMATION;
    return style;
}

std::string button_to_string(int rc)
{
    switch (rc) {
    case wxID_OK:  return "ok";
    case wxID_YES: return "yes";
    case wxID_NO:  return "no";
    default:       return "cancel";
    }
}

std::string ui_message(const std::string& text, const std::string& title,
                       const std::string& buttons, const std::string& icon)
{
    const long style = message_style(buttons, icon);
    return run_on_ui_blocking([&]() -> std::string {
        GUI::MessageDialog dlg(nullptr, wxString::FromUTF8(text), wxString::FromUTF8(title), style);
        return button_to_string(dlg.ShowModal());
    });
}

// --------------------------------------------------------------------------
// orca.host.ui.show_dialog (modal)
// --------------------------------------------------------------------------
py::object ui_show_dialog(const std::string& html, const std::string& title,
                          int width, int height, py::object on_message)
{
    auto      handler = make_message_adapter(std::move(on_message));
    const int w       = width > 0 ? width : 820;
    const int h       = height > 0 ? height : 600;

    std::optional<json> result = run_on_ui_blocking([&]() -> std::optional<json> {
        return GUI::PluginWebDialog::show_modal_dialog(ui_parent(), wxString::FromUTF8(title), html, wxSize(w, h),
                                                       std::move(handler));
    });

    if (!result.has_value())
        return py::none();
    return json_to_py(*result); // GIL held in the binding body
}

// --------------------------------------------------------------------------
// orca.host.ui.create_window (non-modal) + UiWindow handle
// --------------------------------------------------------------------------
struct UiWindowHandle
{
    int id{0};
};

struct UiProgressHandle
{
    int id{0};
};

py::object ui_create_window(const std::string& html, const std::string& title, int width, int height,
                            py::object on_message, py::object on_close)
{
    auto              msg_adapter  = make_message_adapter(std::move(on_message));
    CallablePtr       close_holder = make_holder(std::move(on_close));
    const std::string plugin_key   = PluginAuditManager::instance().current_plugin();
    const int         w            = width > 0 ? width : 820;
    const int         h            = height > 0 ? height : 600;

    const int id = run_on_ui_blocking([&]() -> int {
        const int new_id = UiRegistry::instance().reserve_id();

        // Plugin's on_close: fired only on a user/JS-initiated close (not forced
        // teardown), while the dialog is alive. Empty if the plugin passed None.
        GUI::PluginWebDialog::CloseHandler on_close;
        if (close_holder) {
            on_close = [close_holder]() {
                PythonGILState gil;
                if (!gil)
                    return;
                try {
                    close_holder->fn();
                } catch (py::error_already_set& e) {
                    BOOST_LOG_TRIVIAL(error) << "orca.host.ui on_close handler raised: " << e.what();
                    PyErr_Clear();
                }
            };
        }
        // Registry cleanup: GIL-free, runs from the dialog destructor on every path.
        auto on_destroyed = [new_id]() { UiRegistry::instance().remove(new_id); };

        auto* dlg = GUI::PluginWebDialog::create_modeless_dialog(ui_parent(), wxString::FromUTF8(title), html,
                                                                 wxSize(w, h), std::move(msg_adapter),
                                                                 std::move(on_close), std::move(on_destroyed));
        UiRegistry::instance().bind(new_id, dlg, plugin_key);
        GUI::PluginWebDialog::show_modeless_dialog(dlg);
        return new_id;
    });

    return py::cast(UiWindowHandle{id});
}

void handle_post(int id, py::object data)
{
    if (wxTheApp == nullptr)
        return;
    json j = py_to_json(data); // GIL held (binding body)
    GUI::wxGetApp().CallAfter([id, j = std::move(j)]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginWebDialog>(id);
        GUI::PluginWebDialog::post_message(d, j);
    });
}

void handle_close(int id)
{
    if (wxTheApp == nullptr)
        return;
    GUI::wxGetApp().CallAfter([id]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginWebDialog>(id);
        GUI::PluginWebDialog::request_close(d);
    });
}

UiProgressHandle ui_create_progress_dialog(const std::string& title, const std::string& message, int maximum, int style)
{
    const std::string plugin_key = PluginAuditManager::instance().current_plugin();
    const int         max_value  = maximum > 0 ? maximum : 100;

    return run_on_ui_blocking([&]() -> UiProgressHandle {
        const int new_id = UiRegistry::instance().reserve_id();

        // Registry cleanup: GIL-free, runs from the dialog destructor on every path.
        auto on_destroyed = [new_id]() { UiRegistry::instance().remove(new_id); };

        auto* dlg = GUI::PluginProgressDialog::create_dialog(ui_parent(), wxString::FromUTF8(title),
                                                             wxString::FromUTF8(message), max_value, style,
                                                             std::move(on_destroyed));
        UiRegistry::instance().bind(new_id, dlg, plugin_key);
        return UiProgressHandle{new_id};
    });
}

UiProgressHandle* new_progress_dialog(const std::string& title, const std::string& message, int maximum, int style)
{
    return new UiProgressHandle(ui_create_progress_dialog(title, message, maximum, style));
}

bool progress_is_open(int id)
{
    return UiRegistry::instance().is_open(id);
}

bool progress_pulse(int id, const std::string& message)
{
    return run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        return GUI::PluginProgressDialog::pulse(d, wxString::FromUTF8(message));
    });
}

bool progress_update(int id, int value, const std::string& message)
{
    return run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        return GUI::PluginProgressDialog::update(d, value, wxString::FromUTF8(message));
    });
}

void progress_start_pulse(int id, int interval_ms, const std::string& message)
{
    run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        GUI::PluginProgressDialog::start_pulse(d, interval_ms, wxString::FromUTF8(message));
    });
}

void progress_stop_pulse(int id)
{
    run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        GUI::PluginProgressDialog::stop_pulse(d);
    });
}

void progress_close(int id)
{
    run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        GUI::PluginProgressDialog::request_close(d);
    });
}

} // namespace

void PluginHostUi::RegisterBindings(pybind11::module_& host)
{
    auto ui = host.def_submodule(
        "ui",
        "Host UI: native dialogs and interactive HTML windows. Calls run on the main/UI "
        "thread (marshaled from the plugin thread). See the plugin docs for the window.orca bridge. "
        "Do not call these from a slicing pipeline hook (SlicingPipeline capability): that hook runs "
        "on the slicing worker thread, which the UI thread can itself be blocked waiting on, so a "
        "marshaled UI call from there can deadlock the application.");

    ui.def("message", &ui_message, py::arg("text"), py::arg("title") = "OrcaSlicer", py::arg("buttons") = "ok",
           py::arg("icon") = "info",
           "Show a native modal message box; returns the clicked button id "
           "(\"ok\"/\"cancel\"/\"yes\"/\"no\"). buttons: \"ok\"|\"ok_cancel\"|\"yes_no\"|\"yes_no_cancel\"; "
           "icon: \"info\"|\"warning\"|\"error\"|\"question\".");

    ui.def("show_dialog", &ui_show_dialog, py::arg("html"), py::arg("title") = "OrcaSlicer", py::arg("width") = 820,
           py::arg("height") = 600, py::arg("on_message") = py::none(),
           "Show a modal dialog rendering the given raw HTML. The page talks to the plugin via "
           "window.orca (postMessage/onMessage/submit/close). Blocks until closed; returns the "
           "orca.submit() payload as a dict, or None.");

    ui.attr("PD_APP_MODAL")      = py::int_(wxPD_APP_MODAL);
    ui.attr("PD_AUTO_HIDE")      = py::int_(wxPD_AUTO_HIDE);
    ui.attr("PD_CAN_ABORT")      = py::int_(wxPD_CAN_ABORT);
    ui.attr("PD_CAN_SKIP")       = py::int_(wxPD_CAN_SKIP);
    ui.attr("PD_ELAPSED_TIME")   = py::int_(wxPD_ELAPSED_TIME);
    ui.attr("PD_ESTIMATED_TIME") = py::int_(wxPD_ESTIMATED_TIME);
    ui.attr("PD_REMAINING_TIME") = py::int_(wxPD_REMAINING_TIME);

    py::class_<UiWindowHandle>(ui, "UiWindow", "Handle to a non-modal plugin window created by create_window().")
        .def_property_readonly("id", [](const UiWindowHandle& h) { return h.id; })
        .def(
            "post", [](const UiWindowHandle& h, py::object data) { handle_post(h.id, std::move(data)); },
            py::arg("data"), "Send a payload to the page (delivered to window.orca.onMessage handlers).")
        .def(
            "close", [](const UiWindowHandle& h) { handle_close(h.id); }, "Close the window (fires on_close).")
        .def(
            "is_open", [](const UiWindowHandle& h) { return UiRegistry::instance().is_open(h.id); },
            "Return True while the window is open.");

    ui.def("create_window", &ui_create_window, py::arg("html"), py::arg("title") = "OrcaSlicer", py::arg("width") = 820,
           py::arg("height") = 600, py::arg("on_message") = py::none(), py::arg("on_close") = py::none(),
           "Open a non-modal, persistent HTML window and return a UiWindow. on_message(data) is called on "
           "the UI thread when the page posts; offload heavy work to a thread and push results back with "
           "window.post().");

    py::class_<UiProgressHandle>(ui, "ProgressDialog", "Handle to a native progress dialog.")
        .def(py::init(&new_progress_dialog), py::arg("title"), py::arg("message"), py::arg("maximum") = 100,
             py::arg("style") = wxPD_APP_MODAL | wxPD_AUTO_HIDE)
        .def_property_readonly("id", [](const UiProgressHandle& h) { return h.id; })
        .def(
            "pulse", [](const UiProgressHandle& h, const std::string& message) { return progress_pulse(h.id, message); },
            py::arg("message") = "", "Pulse the dialog gauge; returns False if the dialog is closed or cancelled.")
        .def(
            "update",
            [](const UiProgressHandle& h, int value, const std::string& message) {
                return progress_update(h.id, value, message);
            },
            py::arg("value"), py::arg("message") = "",
            "Set the dialog progress value; returns False if the dialog is closed or cancelled.")
        .def(
            "start_pulse",
            [](const UiProgressHandle& h, int interval_ms, const std::string& message) {
                progress_start_pulse(h.id, interval_ms, message);
            },
            py::arg("interval_ms") = 100, py::arg("message") = "", "Start periodic pulsing.")
        .def(
            "stop_pulse", [](const UiProgressHandle& h) { progress_stop_pulse(h.id); }, "Stop periodic pulsing.")
        .def(
            "close", [](const UiProgressHandle& h) { progress_close(h.id); }, "Close the progress dialog.")
        .def(
            "is_open", [](const UiProgressHandle& h) { return progress_is_open(h.id); },
            "Return True while this progress dialog is registered.")
        .def("__enter__", [](UiProgressHandle& h) -> UiProgressHandle& { return h; }, py::return_value_policy::reference_internal)
        .def("__exit__", [](const UiProgressHandle& h, py::object, py::object, py::object) {
            progress_close(h.id);
            return false;
        });

    ui.def("create_progress_dialog", &ui_create_progress_dialog, py::arg("title"), py::arg("message"),
           py::arg("maximum") = 100, py::arg("style") = wxPD_APP_MODAL | wxPD_AUTO_HIDE,
           "Create a native progress dialog and return a ProgressDialog handle.");
}

void PluginHostUi::close_windows_for_plugin(const std::string& plugin_key)
{
    if (wxTheApp == nullptr)
        return;

    auto teardown = [plugin_key]() {
        // Destroy() bypasses wxEVT_CLOSE, so the plugin's on_close is not fired on
        // forced teardown (intended); the resource destructor still cleans the registry.
        for (auto* window : UiRegistry::instance().take_for_plugin(plugin_key)) {
            if (window != nullptr)
                window->Destroy();
        }
    };

    if (wxIsMainThread())
        teardown();
    else
        GUI::wxGetApp().CallAfter(teardown);
}

} // namespace Slic3r
