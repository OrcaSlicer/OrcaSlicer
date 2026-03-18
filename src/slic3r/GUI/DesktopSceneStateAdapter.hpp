#ifndef slic3r_DesktopSceneStateAdapter_hpp_
#define slic3r_DesktopSceneStateAdapter_hpp_

#include "portability/render/ISceneRenderer.hpp"

namespace Slic3r {
namespace GUI {

class Camera;
class GLVolumeCollection;

class DesktopSceneStateAdapter
{
public:
    static portability::render::RenderSceneState make_scene_state(const Camera& camera,
                                                            const GLVolumeCollection& volumes,
                                                            bool gizmos_running,
                                                            bool render_opaque,
                                                            bool render_transparent);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_DesktopSceneStateAdapter_hpp_
