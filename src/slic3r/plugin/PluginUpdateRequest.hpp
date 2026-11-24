#pragma once

#include "PluginDescriptor.hpp"

#include <functional>
#include <set>
#include <string>

namespace Slic3r {

// Main-thread gate. The app owns it, so queued work outlives the calling plugin.
class PluginUpdateRequests
{
public:
    enum class Result { Accepted, NoCaller, WrongPlugin, LocalPlugin, NotInstalled, NotSignedIn, Unavailable, Busy };
    using Task = std::function<void()>;

    Result request(const std::string& caller, const PluginDescriptor& plugin, bool signed_in,
                   const std::function<void(Task)>& defer, const Task& execute)
    {
        if (caller.empty()) return Result::NoCaller;
        if (caller != plugin.plugin_key) return Result::WrongPlugin;
        if (!plugin.cloud) return Result::LocalPlugin;
        if (!plugin.cloud->installed) return Result::NotInstalled;
        if (!signed_in) return Result::NotSignedIn;
        if (plugin.cloud->unauthorized || plugin.cloud->orphaned) return Result::Unavailable;
        if (!m_pending.empty()) return Result::Busy;
        m_pending.insert(caller);
        try {
            defer([this, caller, execute] {
                try {
                    execute();
                } catch (...) {
                    m_pending.erase(caller);
                    throw;
                }
                m_pending.erase(caller);
            });
        } catch (...) {
            m_pending.erase(caller);
            throw;
        }
        return Result::Accepted;
    }

private:
    std::set<std::string> m_pending;
};

} // namespace Slic3r
