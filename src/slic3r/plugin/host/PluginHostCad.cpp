#include "PluginHostCad.hpp"
#include "CadFaceSelectionState.hpp"
#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/cast.h>
#include <pybind11/stl.h>
#include <boost/log/trivial.hpp>
#include <wx/app.h>
#include <wx/event.h>

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace Slic3r {
namespace {

template<class Value>
CadSelectionState<Value>& selection_state()
{
    static CadSelectionState<Value> state;
    return state;
}

struct CadCallback
{
    py::function function;
    explicit CadCallback(py::function fn) : function(std::move(fn)) {}
    ~CadCallback()
    {
        PythonGILState gil;
        if (gil)
            function = py::function();
        else
            (void) function.release();
    }
};

template<class Value>
std::shared_ptr<CadSelectionSubscription<Value>> subscribe(py::function callback)
{
    const auto& audit = PluginAuditManager::instance();
    const std::string owner = audit.current_plugin();
    const std::string capability_name = audit.current_capability();
    if (owner.empty() || capability_name.empty())
        throw std::runtime_error("CAD subscriptions must be created from a plugin capability callback");
    auto holder = std::make_shared<CadCallback>(std::move(callback));
    return selection_state<Value>().subscribe(owner, [holder, owner, capability_name](const std::optional<Value>& selection,
                                                                             const CadSelectionSubscription<Value>& subscription) {
        PythonGILState gil;
        if (!gil || !subscription.is_active())
            return;
        try {
            // Resolve under the GIL, and keep the capability alive/count the callback just as
            // a normal plugin invocation does. Disabled or unloaded capabilities get no events.
            auto capabilities = PluginManager::instance().get_plugin_capabilities(owner);
            std::vector<std::unique_ptr<PluginCapabilityInterface::RefCounter>> active;
            for (const auto& capability : capabilities)
                if (capability->name() == capability_name)
                    active.push_back(std::make_unique<PluginCapabilityInterface::RefCounter>(*capability));
            if (active.empty())
                return;
            ScopedPluginAuditContext context(owner, capability_name);
            holder->function(selection ? py::cast(*selection) : py::none());
        } catch (py::error_already_set& error) {
            BOOST_LOG_TRIVIAL(error) << "orca.host.cad selection callback raised: " << error.what();
        } catch (const std::exception& error) {
            BOOST_LOG_TRIVIAL(error) << "orca.host.cad selection callback raised: " << error.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "orca.host.cad selection callback raised an unknown exception";
        }
    });
}

} // namespace

void PluginHostCad::initialize()
{
    selection_state<GUI::CadFaceSelection>();
    selection_state<GUI::CadShapeSelection>();
}

void PluginHostCad::RegisterBindings(py::module_& host)
{
    auto cad = host.def_submodule("cad", "Read-only Design-tab OCCT geometry and selection events.");
    register_cad_geometry(cad);
    py::class_<CadFaceSubscription, std::shared_ptr<CadFaceSubscription>>(cad, "FaceSelectionSubscription")
        .def("close", &CadFaceSubscription::close)
        .def("is_active", &CadFaceSubscription::is_active);
    py::class_<CadShapeSubscription, std::shared_ptr<CadShapeSubscription>>(cad, "ShapeSelectionSubscription")
        .def("close", &CadShapeSubscription::close)
        .def("is_active", &CadShapeSubscription::is_active);
    cad.def("selected_face", [] { return selection_state<GUI::CadFaceSelection>().selected(); },
            "Return an owned SelectedFace snapshot, or None. Safe on a Script worker.");
    cad.def("selected_shape", [] { return selection_state<GUI::CadShapeSelection>().selected(); },
            "Return the primary native face, edge, vertex or whole-body selection, or None.");
    cad.def("selected_edge", [] {
        auto value = selection_state<GUI::CadShapeSelection>().selected();
        return value && value->kind == "edge" ? value : std::nullopt;
    });
    cad.def("selected_vertex", [] {
        auto value = selection_state<GUI::CadShapeSelection>().selected();
        return value && value->kind == "vertex" ? value : std::nullopt;
    });
    cad.def("on_face_selection_changed", &subscribe<GUI::CadFaceSelection>, py::arg("callback"),
            "Subscribe to face changes. Keep the handle. callback(face_or_none) runs later on the UI thread. "
            "No initial event; use selected_face(). Automatically disconnected on plugin unload.");
    cad.def("on_selection_changed", &subscribe<GUI::CadShapeSelection>, py::arg("callback"),
            "Subscribe to native face, edge, vertex and whole-body picks. Keep the handle. "
            "callback(selection_or_none) runs later on the UI thread. No initial event; use selected_shape().");
}

void PluginHostCad::selection_changed(std::optional<GUI::CadShapeSelection> selection)
{
    auto face_notify = selection_state<GUI::CadFaceSelection>().update(selection ? selection->face : std::nullopt);
    auto shape_notify = selection_state<GUI::CadShapeSelection>().update(std::move(selection));
    if ((face_notify || shape_notify) && wxTheApp != nullptr && !GUI::wxGetApp().is_closing())
        GUI::wxGetApp().CallAfter([face_notify = std::move(face_notify), shape_notify = std::move(shape_notify)] {
            if (GUI::wxGetApp().is_closing()) return;
            if (face_notify) face_notify();
            if (shape_notify) shape_notify();
        });
}

void PluginHostCad::close_for_plugin(const std::string& plugin_key)
{
    selection_state<GUI::CadFaceSelection>().close_for_plugin(plugin_key);
    selection_state<GUI::CadShapeSelection>().close_for_plugin(plugin_key);
}

} // namespace Slic3r
