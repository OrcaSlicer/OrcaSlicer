#pragma once

#include <slic3r/plugin/PythonPluginInterface.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// One ResourceTexture capability contributing a texture directory to the texture picker.
struct ResourceTextureEntry
{
    PluginCapabilityId id;
    std::string        plugin_key;
    std::string        display_name;
    std::string        texture_dir;
    std::string        plugin_root;   // so a scan can re-check containment
};

// Tracks every loaded and enabled ResourceTexture capability by id. Holds strings only: a
// shared_ptr here would keep a Python object alive past Py_Finalize.
class PluginResourceTextures
{
public:
    static PluginResourceTextures &instance();

    PluginResourceTextures(const PluginResourceTextures &)            = delete;
    PluginResourceTextures &operator=(const PluginResourceTextures &) = delete;

    void on_plugin_register(const std::string &plugin_key);
    void on_plugin_deregister(const std::string &plugin_key);
    void on_cap_register(const PluginCapabilityId &id);
    void on_cap_deregister(const PluginCapabilityId &id);
    void initial_sweep();

    std::vector<ResourceTextureEntry> entries() const;

private:
    PluginResourceTextures() = default;

    std::optional<ResourceTextureEntry> build_entry(const PluginCapabilityId &id) const;

    mutable std::mutex                                 m_mutex;
    std::map<PluginCapabilityId, ResourceTextureEntry> m_entries;
};

} // namespace Slic3r
