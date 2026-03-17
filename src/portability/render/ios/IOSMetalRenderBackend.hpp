#pragma once

#include "portability/render/IRenderBackend.hpp"

#include <memory>

namespace Slic3r::portability::render::ios {

#ifdef __OBJC__
@class CAMetalLayer;
#else
using CAMetalLayer = void;
#endif

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

    int                                m_width{0};
    int                                m_height{0};
    bool                               m_initialized{false};
    std::unique_ptr<MetalBackendState> m_state;
};

} // namespace Slic3r::portability::render::ios
