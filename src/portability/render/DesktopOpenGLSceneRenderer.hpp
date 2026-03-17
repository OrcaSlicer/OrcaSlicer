#ifndef slic3r_DesktopOpenGLSceneRenderer_hpp_
#define slic3r_DesktopOpenGLSceneRenderer_hpp_

#include <functional>

#include "ISceneRenderer.hpp"

namespace Slic3r::Portability::Render {

class DesktopOpenGLSceneRenderer final : public ISceneRenderer
{
public:
    using SetViewportFn      = std::function<void(unsigned int, unsigned int, unsigned int, unsigned int)>;
    using SubmitSceneStateFn = std::function<void(const SceneState&)>;
    using RenderFrameFn      = std::function<void(const SceneState&)>;

    DesktopOpenGLSceneRenderer(SetViewportFn set_viewport_fn, SubmitSceneStateFn submit_scene_state_fn, RenderFrameFn render_frame_fn);

    void set_viewport(unsigned int x, unsigned int y, unsigned int width, unsigned int height) override;
    void submit_scene_state(const SceneState& scene_state) override;
    void render_frame() override;

private:
    SetViewportFn      m_set_viewport_fn;
    SubmitSceneStateFn m_submit_scene_state_fn;
    RenderFrameFn      m_render_frame_fn;
    SceneState         m_scene_state;
};

} // namespace Slic3r::Portability::Render

#endif // slic3r_DesktopOpenGLSceneRenderer_hpp_
