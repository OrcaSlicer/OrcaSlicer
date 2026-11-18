#pragma once

#include "../../PythonPluginInterface.hpp"
#include "pybind11/pybind11.h"

#include <string>
#include <utility>

namespace Slic3r {
class ResourceTexturePluginCapability : public PluginCapabilityInterface
{
public:
    static void RegisterBindings(pybind11::module_& module);

    PluginCapabilityType get_type() const override { return PluginCapabilityType::ResourceTexture; }

    // Python contract: the directory the host scans for textures. Resolved once at materialization
    // under the GIL; never called from the GUI. Overridden in Python through the trampoline.
    virtual std::string get_texture_dir() const = 0;

    // C++-only read of the validated directory cached by the loader; empty if resolution failed.
    const std::string& texture_dir() const { return m_texture_dir; }
    void               set_texture_dir(std::string dir) { m_texture_dir = std::move(dir); }

private:
    std::string m_texture_dir;
};
} // namespace Slic3r
