#pragma once

#include "../../PythonPluginInterface.hpp"
#include "pybind11/pybind11.h"

#include <string>
#include <utility>

namespace Slic3r {
// A printer asset pack: bed models (.stl) and bed textures (.png/.svg) that printer presets pick in the Bed
// Shape dialog and refer to as "orcacloud-asset/<plugin id>/<file>" (libslic3r/PrinterAssetRef.hpp).
class ResourcePrinterAssetPluginCapability : public PluginCapabilityInterface
{
public:
    static void RegisterBindings(pybind11::module_& module);

    PluginCapabilityType get_type() const override { return PluginCapabilityType::ResourcePrinterAsset; }

    // Python contract: the directory holding the pack's bed files. Resolved once at materialization
    // under the GIL; never called from the GUI. Overridden in Python through the trampoline.
    virtual std::string get_asset_dir() const = 0;

    // C++-only read of the validated directory cached by the loader; empty if resolution failed.
    const std::string& asset_dir() const { return m_asset_dir; }
    void               set_asset_dir(std::string dir) { m_asset_dir = std::move(dir); }

private:
    std::string m_asset_dir;
};
} // namespace Slic3r
