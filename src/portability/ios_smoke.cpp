#include "libslic3r/Format/ModelIO.hpp"
#include "portability/platform/ios/IOSPlatformServices.hpp"
#include "portability/render/ios/IOSMetalRenderBackend.hpp"

namespace Slic3r::portability::smoke {

bool ios_smoke_target_links()
{
    platform::ios::IOSPlatformServices platform_services;
    render::ios::IOSMetalRenderBackend render_backend;

    const std::string unused_modelio_probe = Slic3r::make_temp_stl_with_modelio(std::string{});
    (void)unused_modelio_probe;

    return !platform_services.platform_name().empty() && render_backend.backend_type() == render::BackendType::Metal;
}

} // namespace Slic3r::portability::smoke
