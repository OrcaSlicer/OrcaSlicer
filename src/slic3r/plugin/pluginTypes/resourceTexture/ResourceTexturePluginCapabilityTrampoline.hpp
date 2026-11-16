#pragma once

#include "ResourceTexturePluginCapability.hpp"
#include "../../PyPluginTrampoline.hpp"
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <string>
#include "slic3r/plugin/PythonPluginInterface.hpp"

namespace Slic3r {

class PyResourceTexturePluginCapabilityTrampoline :
    public PyPluginCommonTrampoline<ResourceTexturePluginCapability>
{
public:
    using PyPluginCommonTrampoline<ResourceTexturePluginCapability>::PyPluginCommonTrampoline;

    std::string get_texture_dir() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {},
            PYBIND11_OVERRIDE_PURE,
            std::string,
            ResourceTexturePluginCapability,
            get_texture_dir);
    }
};

} // namespace Slic3r
