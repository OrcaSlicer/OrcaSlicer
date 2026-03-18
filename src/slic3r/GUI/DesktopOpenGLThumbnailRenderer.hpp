#ifndef slic3r_DesktopOpenGLThumbnailRenderer_hpp_
#define slic3r_DesktopOpenGLThumbnailRenderer_hpp_

#include "IThumbnailRenderer.hpp"

namespace Slic3r {
namespace GUI {

class DesktopOpenGLThumbnailRenderer final : public IThumbnailRenderer
{
public:
    bool render(ThumbnailData&                   thumbnail_data,
                unsigned int                     width,
                unsigned int                     height,
                const ThumbnailsParams&          thumbnail_params,
                PartPlateList&                   partplate_list,
                ModelObjectPtrs&                 model_objects,
                GLVolumeCollection&              glvolume_collection,
                std::vector<ColorRGBA>&          colors_out,
                GLShaderProgram*                 shader,
                bool                             top_view,
                bool                             for_picking,
                bool                             no_light) const override;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_DesktopOpenGLThumbnailRenderer_hpp_
