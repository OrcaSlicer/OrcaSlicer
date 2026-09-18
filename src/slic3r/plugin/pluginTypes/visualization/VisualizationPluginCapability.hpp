#pragma once

#include "../../PythonPluginInterface.hpp"

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pybind11 {
class module_;
}

namespace Slic3r {

namespace VisualizationInputs {
constexpr char TOOLPATH[] = "toolpath";
constexpr char MODEL[] = "model";
constexpr char CURRENT_PLATE[] = "current_plate";
constexpr char PROJECT[] = "project";
constexpr char GLTF_BINARY[] = "model/gltf-binary";
constexpr char STL[] = "model/stl";
constexpr char OBJ[] = "model/obj";
constexpr char DRACO[] = "model/vnd.google.draco";
constexpr char FILE_TRANSPORT[] = "file";
}

struct VisualizationResourceRequest
{
    std::string kind;
    std::string scope{VisualizationInputs::CURRENT_PLATE};
};

// Extensible identifiers are used deliberately: adding a new input kind, media format, or
// transport must not require extending an Orca-owned enum.
struct VisualizationInputSpec
{
    VisualizationInputSpec() = default;
    VisualizationInputSpec(std::string kind, std::string format, std::string transport = "file",
                           uint16_t minimum_major = 0, uint16_t minimum_minor = 0,
                           uint16_t maximum_major = std::numeric_limits<uint16_t>::max(),
                           uint16_t maximum_minor = std::numeric_limits<uint16_t>::max())
        : kind(std::move(kind)), format(std::move(format)), transport(std::move(transport)),
          minimum_major(minimum_major), minimum_minor(minimum_minor),
          maximum_major(maximum_major), maximum_minor(maximum_minor)
    {}

    std::string kind;
    std::string format;
    std::string transport{"file"};
    uint16_t    minimum_major{0};
    uint16_t    minimum_minor{0};
    uint16_t    maximum_major{std::numeric_limits<uint16_t>::max()};
    uint16_t    maximum_minor{std::numeric_limits<uint16_t>::max()};
};

struct VisualizationInput
{
    std::string kind;
    std::string format;
    std::string transport;
    std::string location;
    uint16_t    major_version{0};
    uint16_t    minor_version{0};
    std::string scope{VisualizationInputs::CURRENT_PLATE};
    std::map<std::string, std::string> metadata;
};

struct VisualizationContext : PluginContext
{
    uint64_t                        revision{0};
    VisualizationInput              input; // Compatibility alias for resources.front().
    std::vector<VisualizationInput> resources;
    std::map<std::string, std::string> metadata;
};

class VisualizationPluginCapability : public PluginCapabilityInterface
{
public:
    static void RegisterBindings(pybind11::module_& module);

    PluginCapabilityType get_type() const override { return PluginCapabilityType::Visualization; }

    // Called once during plugin materialization and cached by the host. Each entry describes one
    // independently acceptable input. A capability may return multiple formats or transports.
    virtual std::vector<VisualizationInputSpec> get_supported_inputs() = 0;
    virtual std::vector<VisualizationResourceRequest> get_requested_resources()
    {
        return {{VisualizationInputs::TOOLPATH, VisualizationInputs::CURRENT_PLATE}};
    }

    virtual ExecutionResult open(const VisualizationContext& ctx)   = 0;
    virtual ExecutionResult update(const VisualizationContext& ctx) = 0;
    virtual void            close() {}

    void resolve_type_metadata() override;
    const std::vector<VisualizationInputSpec>& supported_inputs() const { return m_supported_inputs; }
    const std::vector<VisualizationResourceRequest>& requested_resources() const { return m_requested_resources; }
    bool supports(const VisualizationInput& input) const;
    bool request_update(const std::vector<VisualizationResourceRequest>& resources = {});

private:
    std::vector<VisualizationInputSpec>      m_supported_inputs;
    std::vector<VisualizationResourceRequest> m_requested_resources;
};

} // namespace Slic3r
