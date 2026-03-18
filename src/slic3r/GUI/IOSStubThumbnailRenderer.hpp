#ifndef slic3r_IOSStubThumbnailRenderer_hpp_
#define slic3r_IOSStubThumbnailRenderer_hpp_

#include "IThumbnailRenderer.hpp"

namespace Slic3r {
namespace GUI {

class IOSStubThumbnailRenderer final : public IThumbnailRenderer
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

#endif // slic3r_IOSStubThumbnailRenderer_hpp_
