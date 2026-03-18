#include "IOSStubThumbnailRenderer.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace GUI {

bool IOSStubThumbnailRenderer::render(ThumbnailData&,
                                      unsigned int,
                                      unsigned int,
                                      const ThumbnailsParams&,
                                      PartPlateList&,
                                      ModelObjectPtrs&,
                                      GLVolumeCollection&,
                                      std::vector<ColorRGBA>&,
                                      GLShaderProgram*,
                                      bool,
                                      bool,
                                      bool) const
{
    BOOST_LOG_TRIVIAL(warning) << "thumbnail rendering is not available on iOS portability stub";
    return false;
}

} // namespace GUI
} // namespace Slic3r
