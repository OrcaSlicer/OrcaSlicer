#include "DesktopSceneStateAdapter.hpp"

#include "Camera.hpp"
#include "GLModel.hpp"

#include <utility>

namespace Slic3r {
namespace GUI {

portability::render::SceneState DesktopSceneStateAdapter::make_scene_state(const Camera&             camera,
                                                                            const GLVolumeCollection& volumes,
                                                                            bool                      gizmos_running,
                                                                            bool                      render_opaque,
                                                                            bool                      render_transparent)
{
    portability::render::SceneState scene_state;
    scene_state.camera.view_matrix = camera.get_view_matrix();
    scene_state.camera.projection_matrix = camera.get_projection_matrix();
    scene_state.camera.is_looking_downward = camera.is_looking_downward();
    scene_state.model_states.reserve(volumes.volumes.size());

    for (const GLVolume* volume : volumes.volumes) {
        portability::render::SceneModelState model_state;
        model_state.transform = volume->world_matrix();
        model_state.is_visible = volume->is_active && !volume->disabled;
        scene_state.model_states.emplace_back(std::move(model_state));
    }

    scene_state.render_opaque = render_opaque;
    scene_state.render_transparent = render_transparent;
    scene_state.gizmos_running = gizmos_running;
    return scene_state;
}

} // namespace GUI
} // namespace Slic3r
