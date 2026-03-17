#include "DesktopOpenGLSceneRenderer.hpp"

#include <utility>

namespace Slic3r::Portability::Render {

DesktopOpenGLSceneRenderer::DesktopOpenGLSceneRenderer(SetViewportFn      set_viewport_fn,
                                                       SubmitSceneStateFn submit_scene_state_fn,
                                                       RenderFrameFn      render_frame_fn)
    : m_set_viewport_fn(std::move(set_viewport_fn))
    , m_submit_scene_state_fn(std::move(submit_scene_state_fn))
    , m_render_frame_fn(std::move(render_frame_fn))
{}

void DesktopOpenGLSceneRenderer::set_viewport(unsigned int x, unsigned int y, unsigned int width, unsigned int height)
{
    if (m_set_viewport_fn)
        m_set_viewport_fn(x, y, width, height);
}

void DesktopOpenGLSceneRenderer::submit_scene_state(const SceneState& scene_state)
{
    m_scene_state = scene_state;
    if (m_submit_scene_state_fn)
        m_submit_scene_state_fn(scene_state);
}

void DesktopOpenGLSceneRenderer::render_frame()
{
    if (m_render_frame_fn)
        m_render_frame_fn(m_scene_state);
}

} // namespace Slic3r::Portability::Render
