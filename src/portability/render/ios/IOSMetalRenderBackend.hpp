#pragma once

#include "portability/render/IRenderBackend.hpp"

#include <memory>

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
    void             shutdown() override;

    void bind_metal_layer(void *layer_handle, bool take_ownership = false);

private:
    struct MetalBackendState;

    int                              m_width{0};
    int                              m_height{0};
    bool                             m_initialized{false};
    std::unique_ptr<MetalBackendState> m_state;
};

} // namespace Slic3r::portability::render::ios
