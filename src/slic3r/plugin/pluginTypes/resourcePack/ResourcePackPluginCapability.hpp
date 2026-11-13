#pragma once

#include "../../PythonPluginInterface.hpp"
#include "../../ResourcePackKinds.hpp"
#include "pybind11/pybind11.h"

#include <string>
#include <utility>

namespace Slic3r {
// A resource pack: one directory of files (bed models, bed textures, displacement textures), each file declaring the
// kinds it serves through get_resource_kinds(). Consumers pick the files of their kind (ResourcePackKinds.hpp).
class ResourcePackPluginCapability : public PluginCapabilityInterface
{
public:
    static void RegisterBindings(pybind11::module_& module);

    PluginCapabilityType get_type() const override { return PluginCapabilityType::ResourcePack; }

    // Python contract: the directory holding the pack's files. Resolved once at materialization under the GIL;
    // never called from the GUI. Overridden in Python through the trampoline.
    virtual std::string get_resource_dir() const = 0;

    // Python contract: file name -> kinds it is declared for. Resolved once at materialization, like the directory.
    virtual ResourceKindMap get_resource_kinds() const = 0;

    // C++-only reads of the validated values cached by the loader; empty if resolution failed.
    const std::string&     resource_dir() const { return m_resource_dir; }
    void                   set_resource_dir(std::string dir) { m_resource_dir = std::move(dir); }
    const ResourceKindMap& resource_kinds() const { return m_resource_kinds; }
    void                   set_resource_kinds(ResourceKindMap kinds) { m_resource_kinds = std::move(kinds); }

private:
    std::string     m_resource_dir;
    ResourceKindMap m_resource_kinds;
};
} // namespace Slic3r
