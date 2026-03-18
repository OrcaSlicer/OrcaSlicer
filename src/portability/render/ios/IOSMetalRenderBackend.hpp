#pragma once

#include "portability/render/IRenderBackend.hpp"
#include "portability/render/ISceneRenderer.hpp"

#include <memory>

#ifdef __OBJC__
@class CAMetalLayer;
#else
using CAMetalLayer = void;
#endif

namespace Slic3r::portability::render::ios {

class IOSMetalRenderBackend final : public IRenderBackend
{
public:
    IOSMetalRenderBackend();
    ~IOSMetalRenderBackend() override;

    std::string_view backend_name() const override;
    BackendType      backend_type() const override;
    bool             initialize() override;
    void             resize(int width, int height) override;
    void             render_frame() override;
    // Stores the latest portable scene state used by desktop and mobile renderers.
    void             submit_scene_state(const RenderSceneState& scene_state) override;

    void             shutdown() override;

    // Binds an externally-created CAMetalLayer before initialize().
    //
    // Ownership contract:
    // - The caller remains the logical owner of `layer`.
    // - The backend retains a strong reference while it is bound/initialized.
    // - The retained reference is released when another layer is bound, on shutdown(),
    //   or in the destructor.
    // - Passing nullptr unbinds any previously bound layer.
    void bind_metal_layer(CAMetalLayer* layer);

private:
    struct MetalBackendState;

    void configure_layer_for_current_state(CAMetalLayer* layer);
    void ensure_depth_resources();
    void update_draw_payload_buffers(const RenderSceneState& scene_state);

    int                                m_width{0};
    int                                m_height{0};
    bool                               m_initialized{false};
    std::unique_ptr<MetalBackendState> m_state;
    RenderSceneState           m_scene_state;
};

} // namespace Slic3r::portability::render::ios
