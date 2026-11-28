#pragma once

#include <string>

namespace Slic3r
{
    namespace GUI
    {
        enum class PluginStatus
        {
            // IMPORTANT: ordinal order is the Plugins dialog Status sort priority.
            Activated,
            Error,
            RuntimeError,
            Inactive,
            Loading
        };

        inline std::string to_string(PluginStatus status)
        {
            switch (status)
            {
            case PluginStatus::Activated: return "Activated";
            case PluginStatus::Error: return "Error";
            case PluginStatus::RuntimeError: return "RuntimeError";
            case PluginStatus::Inactive: return "Inactive";
            case PluginStatus::Loading: return "Loading";
            }

            return "Inactive";
        }

        // why: a plugin whose module is live but whose catalog carries an error is a
        //   RUNTIME fault (e.g. a capability rejected at register time) - it stays
        //   loaded/checked and is only flagged, distinct from a load-time Error where
        //   the module never came up. Loading wins over both so an in-flight reload
        //   never flashes an error.
        inline PluginStatus resolve_plugin_status(bool loading, bool has_error, bool is_loaded)
        {
            if (loading)
                return PluginStatus::Loading;
            if (has_error)
                return is_loaded ? PluginStatus::RuntimeError : PluginStatus::Error;
            if (is_loaded)
                return PluginStatus::Activated;
            return PluginStatus::Inactive;
        }
    }
} // namespace Slic3r::GUI
