#pragma once

#include "ResourcePackPluginCapability.hpp"
#include "../../PyPluginTrampoline.hpp"
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/stl.h>

#include <string>
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include "slic3r/plugin/ResourcePackKinds.hpp"

namespace Slic3r {

class PyResourcePackPluginCapabilityTrampoline : public PyPluginCommonTrampoline<ResourcePackPluginCapability>
{
public:
    using PyPluginCommonTrampoline<ResourcePackPluginCapability>::PyPluginCommonTrampoline;

    std::string get_resource_dir() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {},
            PYBIND11_OVERRIDE_PURE,
            std::string,
            ResourcePackPluginCapability,
            get_resource_dir);
    }

    ResourceKindMap get_resource_kinds() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {},
            PYBIND11_OVERRIDE_PURE,
            ResourceKindMap,
            ResourcePackPluginCapability,
            get_resource_kinds);
    }
};

} // namespace Slic3r
