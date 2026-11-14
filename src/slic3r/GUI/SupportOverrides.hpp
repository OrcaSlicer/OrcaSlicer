#pragma once

#include "libslic3r/Model.hpp"
#include "libslic3r/Config.hpp"

namespace Slic3r::GUI {

// The global support switch is a default, not a master switch: an imported
// object may still enable supports independently.
inline bool has_enabled_support_overrides(const Model& model)
{
    for (const ModelObject* object : model.objects) {
        const auto& config  = object->config.get();
        const auto* enabled = config.option<ConfigOptionBool>("enable_support");
        if (enabled && enabled->value)
            return true;
    }
    return false;
}

// Call only for an explicit all-object disable action, with global support
// disabled. Preserve support painting, support style, enforced layers,
// rafts and unrelated overrides, including intentional local support-off values.
inline bool clear_enabled_support_overrides(Model& model)
{
    bool changed = false;
    for (ModelObject* object : model.objects) {
        const auto* enabled = object->config.get().option<ConfigOptionBool>("enable_support");
        if (enabled && enabled->value) {
            object->config.erase("enable_support");
            changed = true;
        }
    }
    return changed;
}

} // namespace Slic3r::GUI
