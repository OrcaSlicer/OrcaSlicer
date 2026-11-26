#pragma once

#include <memory>

namespace Slic3r { namespace GUI { namespace Mcp {

class AutomationRuntime;

// The only way a runtime is destroyed: stops it, then frees it once no lease is held and no call or job thread is
// inside its core. A runtime whose calls have not returned 10 s after its stop is leaked instead, as they hold
// references into it. Kept apart from AutomationRuntime.hpp so GUI_App.hpp can hold the pointer.
struct AutomationRuntimeDeleter
{
    void operator()(AutomationRuntime* runtime) const;
};

using AutomationRuntimePtr = std::unique_ptr<AutomationRuntime, AutomationRuntimeDeleter>;

}}} // namespace Slic3r::GUI::Mcp
