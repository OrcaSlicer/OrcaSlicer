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
            Inactive,
            Loading
        };

        inline std::string to_string(PluginStatus status)
        {
            switch (status)
            {
            case PluginStatus::Activated: return "Activated";
            case PluginStatus::Error: return "Error";
            case PluginStatus::Inactive: return "Inactive";
            case PluginStatus::Loading: return "Loading";
            }

            return "Inactive";
        }
    }
} // namespace Slic3r::GUI
