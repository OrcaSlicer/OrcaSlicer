#pragma once

#include "ResourcePrinterAssetPluginCapability.hpp"
#include "../../PyPluginTrampoline.hpp"
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <string>
#include "slic3r/plugin/PythonPluginInterface.hpp"

namespace Slic3r {

class PyResourcePrinterAssetPluginCapabilityTrampoline : public PyPluginCommonTrampoline<ResourcePrinterAssetPluginCapability>
{
public:
    using PyPluginCommonTrampoline<ResourcePrinterAssetPluginCapability>::PyPluginCommonTrampoline;

    std::string get_asset_dir() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {},
            PYBIND11_OVERRIDE_PURE,
            std::string,
            ResourcePrinterAssetPluginCapability,
            get_asset_dir);
    }
};

} // namespace Slic3r
