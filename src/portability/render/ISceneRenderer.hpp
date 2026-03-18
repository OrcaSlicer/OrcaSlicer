#ifndef slic3r_ISceneRenderer_hpp_
#define slic3r_ISceneRenderer_hpp_

#include <vector>

#include "libslic3r/Point.hpp"

namespace Slic3r::portability::render {

struct RenderCameraState
{
    Transform3d view_matrix{Transform3d::Identity()};
    Transform3d projection_matrix{Transform3d::Identity()};
    bool        is_looking_downward{false};
};

struct RenderModelState
{
    Transform3d transform{Transform3d::Identity()};
    bool        is_visible{false};
};

struct RenderSceneState
{
    RenderCameraState             camera;
    std::vector<RenderModelState> model_states;
    bool                          render_opaque{true};
    bool                          render_transparent{false};
    bool                          gizmos_running{false};
};

class ISceneRenderer
{
public:
    virtual ~ISceneRenderer() = default;

    virtual void set_viewport(unsigned int x, unsigned int y, unsigned int width, unsigned int height) = 0;
    virtual void submit_scene_state(const RenderSceneState& scene_state)                               = 0;
    virtual void render_frame()                                                                        = 0;
};

} // namespace Slic3r::portability::render

#endif // slic3r_ISceneRenderer_hpp_
