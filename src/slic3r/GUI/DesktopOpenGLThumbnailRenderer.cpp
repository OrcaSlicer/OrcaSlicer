#include "DesktopOpenGLThumbnailRenderer.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "Camera.hpp"
#include "GLCanvas3D.hpp"
#include "OpenGLManager.hpp"

namespace Slic3r {
namespace GUI {

bool DesktopOpenGLThumbnailRenderer::render(ThumbnailData&                   thumbnail_data,
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
                                            bool                             no_light) const
{
    const Camera::ViewAngleType view_angle = top_view ? Camera::ViewAngleType::Top_Plate : Camera::ViewAngleType::Iso;

    switch (OpenGLManager::get_framebuffers_type()) {
    case OpenGLManager::EFramebufferType::Arb: {
        BOOST_LOG_TRIVIAL(info) << boost::format("framebuffer_type: ARB");
        GLCanvas3D::render_thumbnail_framebuffer(thumbnail_data,
                                                 width,
                                                 height,
                                                 thumbnail_params,
                                                 partplate_list,
                                                 model_objects,
                                                 glvolume_collection,
                                                 colors_out,
                                                 shader,
                                                 Camera::EType::Ortho,
                                                 view_angle,
                                                 for_picking,
                                                 no_light);
        return true;
    }
    case OpenGLManager::EFramebufferType::Ext: {
        BOOST_LOG_TRIVIAL(info) << boost::format("framebuffer_type: EXT");
        GLCanvas3D::render_thumbnail_framebuffer_ext(thumbnail_data,
                                                     width,
                                                     height,
                                                     thumbnail_params,
                                                     partplate_list,
                                                     model_objects,
                                                     glvolume_collection,
                                                     colors_out,
                                                     shader,
                                                     Camera::EType::Ortho,
                                                     view_angle,
                                                     for_picking,
                                                     no_light);
        return true;
    }
    default:
        BOOST_LOG_TRIVIAL(info) << boost::format("framebuffer_type: unknown");
        return false;
    }
}

} // namespace GUI
} // namespace Slic3r
