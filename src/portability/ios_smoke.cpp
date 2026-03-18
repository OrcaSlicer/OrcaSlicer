#include "portability/platform/ios/IOSPlatformServices.hpp"
#include "portability/render/ios/IOSMetalRenderBackend.hpp"

namespace Slic3r::portability::smoke {

bool ios_smoke_target_links()
{
    platform::ios::IOSPlatformServices platform_services;
    render::ios::IOSMetalRenderBackend render_backend;

    return !platform_services.platform_name().empty() && render_backend.backend_type() == render::BackendType::Metal;
}

} // namespace Slic3r::portability::smoke
