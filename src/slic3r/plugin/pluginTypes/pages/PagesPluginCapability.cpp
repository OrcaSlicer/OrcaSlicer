#include "PagesPluginCapability.hpp"
#include "PagesPluginCapabilityTrampoline.hpp"

#include "../../PluginFsUtils.hpp"

#include <boost/log/trivial.hpp>
#include <pybind11/pybind11.h>

#include <utility>

namespace py = pybind11;

namespace Slic3r {

void PagesPluginCapability::RegisterBindings(pybind11::module_& module)
{
    BOOST_LOG_TRIVIAL(debug) << "Registering orca.pages bindings";

    auto pages = module.def_submodule("pages", "Plugin page API");

    py::class_<PagesPluginCapability, PluginCapabilityInterface, PyPagesPluginCapabilityTrampoline,
               std::shared_ptr<PagesPluginCapability>>(pages, "PagesPluginCapabilityBase")
        .def(py::init<>())
        .def("get_type", &PagesPluginCapability::get_type)
        .def("get_ui", &PagesPluginCapability::get_ui)
        .def("get_icon", &PagesPluginCapability::get_icon)
        .def("on_message", &PagesPluginCapability::on_message)
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
