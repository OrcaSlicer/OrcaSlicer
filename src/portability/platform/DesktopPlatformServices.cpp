#include "portability/platform/DesktopPlatformServices.hpp"

namespace OrcaSlicer::Portability {

std::string_view DesktopPlatformServices::platform_name() const
{
    return "desktop";
}

} // namespace OrcaSlicer::Portability
