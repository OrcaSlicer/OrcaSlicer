#ifndef slic3r_ISceneRenderer_hpp_
#define slic3r_ISceneRenderer_hpp_

#include <array>
#include <cstdint>
#include <vector>

namespace Slic3r::portability::render {

using RenderMatrix4x4 = std::array<double, 16>;

constexpr RenderMatrix4x4 identity_matrix4x4()
{
    return {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
}

struct RenderCameraState
{
    RenderMatrix4x4 view_matrix{identity_matrix4x4()};
    RenderMatrix4x4 projection_matrix{identity_matrix4x4()};
    bool        is_looking_downward{false};
};

struct RenderModelState
{
    RenderMatrix4x4 transform{identity_matrix4x4()};
    bool        is_visible{false};
};

struct RenderVertex
{
    float position[3]{0.0f, 0.0f, 0.0f};
    float color[4]{1.0f, 1.0f, 1.0f, 1.0f};
};

struct RenderDrawPayload
{
    std::vector<RenderVertex> vertices;
    std::vector<uint32_t>     indices;
};

struct RenderSceneState
{
    RenderCameraState             camera;
    std::vector<RenderModelState> model_states;
    RenderDrawPayload             model_draw_payload;
    RenderDrawPayload             toolpath_draw_payload;
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
