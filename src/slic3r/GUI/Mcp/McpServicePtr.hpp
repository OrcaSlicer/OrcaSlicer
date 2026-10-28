#pragma once

#include <memory>

namespace Slic3r { namespace GUI { namespace Mcp {

class McpService;

// The only way a service is destroyed: stops it, then frees it once no request, call or job thread
// is inside it. A service whose calls do not return within a few seconds is leaked instead, as they
// hold references into it. Kept apart from McpService.hpp so GUI_App.hpp can hold the pointer.
struct McpServiceDeleter
{
    void operator()(McpService* service) const;
};

using McpServicePtr = std::unique_ptr<McpService, McpServiceDeleter>;

}}} // namespace Slic3r::GUI::Mcp
