#ifndef slic3r_ThumbnailRendererFactory_hpp_
#define slic3r_ThumbnailRendererFactory_hpp_

#include <memory>

namespace Slic3r {
namespace GUI {

class IThumbnailRenderer;

std::unique_ptr<IThumbnailRenderer> make_thumbnail_renderer();

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_ThumbnailRendererFactory_hpp_
