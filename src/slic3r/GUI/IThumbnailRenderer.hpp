#ifndef slic3r_IThumbnailRenderer_hpp_
#define slic3r_IThumbnailRenderer_hpp_

#include <string>
#include <vector>

#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r {
class ColorRGBA;

namespace GUI {
class GLShaderProgram;
class GLVolumeCollection;
class PartPlateList;

class IThumbnailRenderer
{
public:
    virtual ~IThumbnailRenderer() = default;

    virtual bool render(ThumbnailData&                     thumbnail_data,
                        unsigned int                       width,
                        unsigned int                       height,
                        const ThumbnailsParams&            thumbnail_params,
                        PartPlateList&                     partplate_list,
                        ModelObjectPtrs&                   model_objects,
                        GLVolumeCollection&                glvolume_collection,
                        std::vector<ColorRGBA>&            colors_out,
                        GLShaderProgram*                   shader,
                        bool                               top_view,
                        bool                               for_picking,
                        bool                               no_light) const = 0;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_IThumbnailRenderer_hpp_
