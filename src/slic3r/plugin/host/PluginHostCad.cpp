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

CadFaceSelectionState& selection_state()
{
    static CadFaceSelectionState state;
    return state;
}

struct FaceCallback
{
    py::function function;
    explicit FaceCallback(py::function fn) : function(std::move(fn)) {}
    ~FaceCallback()
    {
        PythonGILState gil;
        if (gil)
            function = py::function();
        else
            (void) function.release();
    }
};

std::shared_ptr<CadFaceSubscription> subscribe(py::function callback)
{
    const auto& audit = PluginAuditManager::instance();
    const std::string owner = audit.current_plugin();
    const std::string capability_name = audit.current_capability();
    if (owner.empty() || capability_name.empty())
        throw std::runtime_error("on_face_selection_changed() must be called from a plugin capability callback");
    auto holder = std::make_shared<FaceCallback>(std::move(callback));
    return selection_state().subscribe(owner, [holder, owner, capability_name](const CadFaceSnapshot& face,
                                                                             const CadFaceSubscription& subscription) {
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
            holder->function(face ? py::cast(*face) : py::none());
        } catch (py::error_already_set& error) {
            BOOST_LOG_TRIVIAL(error) << "orca.host.cad face selection callback raised: " << error.what();
        } catch (const std::exception& error) {
            BOOST_LOG_TRIVIAL(error) << "orca.host.cad face selection callback raised: " << error.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "orca.host.cad face selection callback raised an unknown exception";
        }
    });
}

} // namespace

void PluginHostCad::RegisterBindings(py::module_& host)
{
    auto cad = host.def_submodule("cad", "Read-only Design-tab face selection snapshots and events.");
    py::class_<GUI::CadFaceSelection>(cad, "SelectedFace")
        .def_readonly("body_index", &GUI::CadFaceSelection::body_index)
        .def_readonly("face_index", &GUI::CadFaceSelection::face_index)
        .def_readonly("selection_revision", &GUI::CadFaceSelection::selection_revision)
        .def_readonly("centroid", &GUI::CadFaceSelection::centroid)
        .def_readonly("normal", &GUI::CadFaceSelection::normal)
        .def_readonly("area", &GUI::CadFaceSelection::area)
        .def_readonly("is_planar", &GUI::CadFaceSelection::is_planar)
        .def_readonly("body_to_design", &GUI::CadFaceSelection::body_to_design);
    py::class_<CadFaceSubscription, std::shared_ptr<CadFaceSubscription>>(cad, "FaceSelectionSubscription")
        .def("close", &CadFaceSubscription::close, "Disconnect. Safe to call more than once or from inside the callback.")
        .def("is_active", &CadFaceSubscription::is_active);
    cad.def("selected_face", [] { return selection_state().selected_face(); },
            "Return an owned SelectedFace snapshot, or None when no CAD face is selected. Safe on a Script worker.");
    cad.def("on_face_selection_changed", &subscribe, py::arg("callback"),
            "Subscribe to native face selection changes. Keep the returned handle; close it to unsubscribe. "
            "callback(face_or_none) runs later on the UI thread. No initial event; use selected_face(). "
            "Subscriptions are closed automatically on plugin unload.");
}

void PluginHostCad::selection_changed(std::optional<GUI::CadFaceSelection> face)
{
    auto notify = selection_state().update(std::move(face));
    if (notify && wxTheApp != nullptr && !GUI::wxGetApp().is_closing())
        GUI::wxGetApp().CallAfter([notify = std::move(notify)] {
            if (!GUI::wxGetApp().is_closing())
                notify();
        });
}

void PluginHostCad::close_for_plugin(const std::string& plugin_key)
{
    selection_state().close_for_plugin(plugin_key);
}

} // namespace Slic3r
