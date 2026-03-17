#ifndef slic3r_ISceneRenderer_hpp_
#define slic3r_ISceneRenderer_hpp_

#include <vector>

#include "libslic3r/Point.hpp"

namespace Slic3r {
namespace GUI {
class Camera;
}

namespace portability::render {

struct SceneModelState
{
    Transform3d transform{Transform3d::Identity()};
    bool        is_visible{false};
};

struct SceneState
{
    const GUI::Camera*           camera{nullptr};
    std::vector<SceneModelState> model_states;
    bool                         render_opaque{true};
    bool                         render_transparent{false};
    bool                         gizmos_running{false};
};

class ISceneRenderer
{
public:
    virtual ~ISceneRenderer() = default;

    virtual void set_viewport(unsigned int x, unsigned int y, unsigned int width, unsigned int height) = 0;
    virtual void submit_scene_state(const SceneState& scene_state)                                     = 0;
    virtual void render_frame()                                                                        = 0;
};

} // namespace portability::render
} // namespace Slic3r

#endif // slic3r_ISceneRenderer_hpp_
