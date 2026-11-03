#include "PagesPluginCapability.hpp"
#include "PagesPluginCapabilityTrampoline.hpp"

#include "../../PluginFsUtils.hpp"
#include "../../PluginJsonPy.hpp"
#include <pybind11/pytypes.h>
#include <pybind11/cast.h>

#include <boost/log/trivial.hpp>
#include <memory>
#include <functional>
#include <mutex>
#include <pybind11/pybind11.h>

#include "slic3r/plugin/PythonPluginInterface.hpp"
#include <string>
#include <utility>

namespace py = pybind11;

namespace Slic3r {

void PagesPluginCapability::RegisterBindings(pybind11::module_& module)
{
    BOOST_LOG_TRIVIAL(debug) << "Registering orca.pages bindings";

    auto pages = module.def_submodule("pages", "Plugin page API");

    py::class_<PagesPluginCapability, PluginCapabilityInterface, PyPagesPluginCapabilityTrampoline,
               std::shared_ptr<PagesPluginCapability>>(
        pages, "PagesPluginCapabilityBase",
        "Base for a page capability: a tab in OrcaSlicer's main window, labelled with get_name(), that shows\n"
        "the HTML get_ui() returns. The page sends data to the plugin with window.orca.postMessage(data), which\n"
        "calls on_message(), and receives what post_message() sends through window.orca.onMessage(callback).")
        .def(py::init<>())
        .def("get_type", &PagesPluginCapability::get_type, "Return PluginType.Pages.")
        .def("get_ui", &PagesPluginCapability::get_ui,
             "Required. Return the page as an HTML document string. Called when the tab is first shown and again\n"
             "whenever the page reloads.")
        .def("get_icon", &PagesPluginCapability::get_icon,
             "Return the tab icon: an absolute path to an .svg or .png file, or the name of an image bundled with\n"
             "OrcaSlicer. Called once when the tab is created. The default returns \"\", for no icon.")
        .def("on_message", &PagesPluginCapability::on_message, py::arg("data"),
             "Called on the UI thread when the page calls window.orca.postMessage(data). Receives data decoded\n"
             "from JSON: a dict, list, str, number, bool or None. The return value is ignored; answer with\n"
             "post_message(). The default does nothing.")
        .def(
            "post_message",
            [](PagesPluginCapability& capability, py::object data) {
                capability.post_message(py_to_json(data).dump());
            },
            py::arg("data"), "Send a JSON-compatible value to the page's window.orca.onMessage handlers.");
}

void PagesPluginCapability::post_message(std::string message)
{
    std::function<void(const std::string&)> sender;
    {
        std::lock_guard<std::mutex> lock(m_message_mutex);
        sender = m_message_sender;
    }

    if (sender)
        sender(message);
}

void PagesPluginCapability::set_message_sender(std::function<void(const std::string&)> sender)
{
    std::lock_guard<std::mutex> lock(m_message_mutex);
    m_message_sender = std::move(sender);
}

void PagesPluginCapability::clear_message_sender()
{
    std::lock_guard<std::mutex> lock(m_message_mutex);
    m_message_sender = nullptr;
}

} // namespace Slic3r
