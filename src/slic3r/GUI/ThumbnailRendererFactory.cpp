#include "ThumbnailRendererFactory.hpp"

#include "DesktopOpenGLThumbnailRenderer.hpp"
#include "IOSStubThumbnailRenderer.hpp"

namespace Slic3r {
namespace GUI {

std::unique_ptr<IThumbnailRenderer> make_thumbnail_renderer()
{
#if defined(ORCASLICER_TARGET_IOS)
    return std::make_unique<IOSStubThumbnailRenderer>();
#else
    return std::make_unique<DesktopOpenGLThumbnailRenderer>();
#endif
}

} // namespace GUI
} // namespace Slic3r
