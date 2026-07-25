#pragma once

#include <string>

namespace Slic3r
{
    namespace GUI
    {
        enum class PluginSource
        {
            // IMPORTANT: ordinal order is the Plugins dialog Source sort priority.
            Mine,
            Subscribed,
            Local
        };

        inline std::string to_string(PluginSource source)
        {
            switch (source)
            {
            case PluginSource::Mine: return "mine";
            case PluginSource::Subscribed: return "subscribed";
            case PluginSource::Local: return "local";
            }

            return "local";
        }
    }
} // namespace Slic3r::GUI
