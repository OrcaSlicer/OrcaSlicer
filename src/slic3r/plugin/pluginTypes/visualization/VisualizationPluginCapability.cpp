#include "VisualizationPluginCapability.hpp"

#include <boost/log/trivial.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "VisualizationPluginCapabilityTrampoline.hpp"
#include "../../host/PluginVisualizations.hpp"

#include <set>
#include <stdexcept>

namespace py = pybind11;

namespace Slic3r {

namespace {
bool version_less(uint16_t lhs_major, uint16_t lhs_minor, uint16_t rhs_major, uint16_t rhs_minor)
{
    return lhs_major < rhs_major || (lhs_major == rhs_major && lhs_minor < rhs_minor);
}
}

void VisualizationPluginCapability::resolve_type_metadata()
{
    auto inputs = get_supported_inputs();
    if (inputs.empty())
        throw std::runtime_error("Visualization capability must declare at least one supported input");
    std::set<std::string> supported_kinds;
    for (const VisualizationInputSpec& input : inputs) {
        if (input.kind.empty() || input.format.empty() || input.transport.empty())
            throw std::runtime_error("Visualization input kind, format, and transport must be non-empty");
        if (version_less(input.maximum_major, input.maximum_minor, input.minimum_major, input.minimum_minor))
            throw std::runtime_error("Visualization input version range is inverted");
        supported_kinds.insert(input.kind);
    }

    auto resources = get_requested_resources();
    if (resources.empty())
        throw std::runtime_error("Visualization capability must request at least one resource");
    for (const VisualizationResourceRequest& resource : resources) {
        if (supported_kinds.count(resource.kind) == 0)
            throw std::runtime_error("Visualization resource kind has no supported input: " + resource.kind);
        if (resource.scope != VisualizationInputs::CURRENT_PLATE && resource.scope != VisualizationInputs::PROJECT)
            throw std::runtime_error("Unsupported visualization resource scope: " + resource.scope);
    }
    m_supported_inputs = std::move(inputs);
    m_requested_resources = std::move(resources);
}

bool VisualizationPluginCapability::request_update(const std::vector<VisualizationResourceRequest>& resources)
{
    return PluginVisualizations::instance().request_update(identity(), resources);
}

bool VisualizationPluginCapability::supports(const VisualizationInput& input) const
{
    for (const VisualizationInputSpec& spec : m_supported_inputs) {
        if (spec.kind == input.kind && spec.format == input.format && spec.transport == input.transport &&
            !version_less(input.major_version, input.minor_version, spec.minimum_major, spec.minimum_minor) &&
            !version_less(spec.maximum_major, spec.maximum_minor, input.major_version, input.minor_version))
            return true;
    }
    return false;
}

void VisualizationPluginCapability::RegisterBindings(pybind11::module_& module)
{
    BOOST_LOG_TRIVIAL(debug) << "Registering orca.visualization bindings";

    auto visualization = module.def_submodule(
        "visualization",
        "Negotiated visualization API. Capabilities declare acceptable inputs once at load time; "
        "the host dispatches GLB, STL, OBJ, and Draco through the same immutable resource contract.");
    visualization.attr("INPUT_TOOLPATH") = VisualizationInputs::TOOLPATH;
    visualization.attr("INPUT_MODEL") = VisualizationInputs::MODEL;
    visualization.attr("SCOPE_CURRENT_PLATE") = VisualizationInputs::CURRENT_PLATE;
    visualization.attr("SCOPE_PROJECT") = VisualizationInputs::PROJECT;
    visualization.attr("FORMAT_GLTF_BINARY") = VisualizationInputs::GLTF_BINARY;
    visualization.attr("FORMAT_STL") = VisualizationInputs::STL;
    visualization.attr("FORMAT_OBJ") = VisualizationInputs::OBJ;
    visualization.attr("FORMAT_DRACO") = VisualizationInputs::DRACO;
    visualization.attr("TRANSPORT_FILE") = VisualizationInputs::FILE_TRANSPORT;

    py::class_<VisualizationResourceRequest>(visualization, "VisualizationResourceRequest")
        .def(py::init<>())
        .def(py::init<std::string, std::string>(), py::arg("kind"),
             py::arg("scope") = VisualizationInputs::CURRENT_PLATE)
        .def_readwrite("kind", &VisualizationResourceRequest::kind)
        .def_readwrite("scope", &VisualizationResourceRequest::scope);

    py::class_<VisualizationInputSpec>(
        visualization, "VisualizationInputSpec",
        "One independently acceptable input. Identifiers are extensible and case-sensitive. "
        "Version bounds are inclusive (major, minor) pairs; return multiple specs for alternatives.")
        .def(py::init<>())
        .def(py::init<std::string, std::string, std::string, uint16_t, uint16_t, uint16_t, uint16_t>(),
             py::arg("kind"), py::arg("format"), py::arg("transport") = "file",
             py::arg("minimum_major") = 0, py::arg("minimum_minor") = 0,
             py::arg("maximum_major") = std::numeric_limits<uint16_t>::max(),
             py::arg("maximum_minor") = std::numeric_limits<uint16_t>::max())
        .def_readwrite("kind", &VisualizationInputSpec::kind,
                       "Semantic kind, such as INPUT_TOOLPATH or a plugin-defined identifier.")
        .def_readwrite("format", &VisualizationInputSpec::format,
                       "Media type such as FORMAT_GLTF_BINARY, FORMAT_STL, FORMAT_OBJ, or FORMAT_DRACO.")
        .def_readwrite("transport", &VisualizationInputSpec::transport,
                       "Delivery mechanism, such as TRANSPORT_FILE.")
        .def_readwrite("minimum_major", &VisualizationInputSpec::minimum_major, "Inclusive minimum major version.")
        .def_readwrite("minimum_minor", &VisualizationInputSpec::minimum_minor, "Inclusive minimum minor version.")
        .def_readwrite("maximum_major", &VisualizationInputSpec::maximum_major, "Inclusive maximum major version.")
        .def_readwrite("maximum_minor", &VisualizationInputSpec::maximum_minor, "Inclusive maximum minor version.");

    py::class_<VisualizationInput>(
        visualization, "VisualizationInput",
        "Immutable negotiated resource. Its location is valid for the session; plugins must not "
        "move, replace, or delete the resource.")
        .def_property_readonly("kind", [](const VisualizationInput& input) { return input.kind; }, "Negotiated semantic kind.")
        .def_property_readonly("format", [](const VisualizationInput& input) { return input.format; }, "Negotiated media/serialization identifier.")
        .def_property_readonly("transport", [](const VisualizationInput& input) { return input.transport; }, "Negotiated delivery mechanism.")
        .def_property_readonly("location", [](const VisualizationInput& input) { return input.location; },
                               "Transport-specific location; for file transport this is a native path.")
        .def_property_readonly("major_version", [](const VisualizationInput& input) { return input.major_version; }, "Resource format major version.")
        .def_property_readonly("minor_version", [](const VisualizationInput& input) { return input.minor_version; }, "Resource format minor version.")
        .def_property_readonly("scope", [](const VisualizationInput& input) { return input.scope; }, "Requested resource scope.")
        .def_property_readonly("metadata", [](const VisualizationInput& input) { return input.metadata; }, "Resource-specific metadata.");

    py::class_<VisualizationContext, PluginContext>(
        visualization, "VisualizationContext",
        "Immutable invocation context. A larger revision identifies newer source content. metadata "
        "contains optional producer-specific strings; unknown keys must be ignored.")
        .def_property_readonly("orca_version", [](const VisualizationContext& ctx) { return ctx.orca_version; }, "Running OrcaSlicer version.")
        .def_property_readonly("revision", [](const VisualizationContext& ctx) { return ctx.revision; }, "Source-content revision for this session.")
        .def_property_readonly("input", [](const VisualizationContext& ctx) { return ctx.input; }, "First negotiated resource, retained for compatibility.")
        .def_property_readonly("resources", [](const VisualizationContext& ctx) { return ctx.resources; }, "All negotiated immutable resources.")
        .def_property_readonly("metadata", [](const VisualizationContext& ctx) { return ctx.metadata; },
                               "Producer-specific string metadata. Unknown keys must be ignored.");

    py::class_<VisualizationPluginCapability, PluginCapabilityInterface, PyVisualizationPluginCapabilityTrampoline,
               std::shared_ptr<VisualizationPluginCapability>>(visualization, "VisualizationPluginCapabilityBase")
        .def(py::init<>())
        .def("get_type", &VisualizationPluginCapability::get_type)
        .def("get_supported_inputs", &VisualizationPluginCapability::get_supported_inputs,
             "Return one or more VisualizationInputSpec alternatives. Called once during "
             "materialization; an empty or invalid declaration rejects the capability.")
        .def("get_requested_resources", &VisualizationPluginCapability::get_requested_resources,
             "Return the resource kinds and scopes required when the visualization opens.")
        .def("request_update", &VisualizationPluginCapability::request_update,
             py::arg("resources") = std::vector<VisualizationResourceRequest>{},
             "Ask the host to rebuild this session's resources. An empty list repeats the open request.")
        .def("open", &VisualizationPluginCapability::open,
             "Open a session. Success keeps the input alive until a successful update or close.")
        .def("update", &VisualizationPluginCapability::update,
             "Replace the session input. RecoverableError keeps the previous input active; "
             "FatalError closes the session.")
        .def("close", &VisualizationPluginCapability::close,
             "Close the session and release plugin-side resources. Implementations must be idempotent.");
}

} // namespace Slic3r
