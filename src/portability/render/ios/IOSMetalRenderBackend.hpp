#pragma once

#include "portability/render/IRenderBackend.hpp"

namespace Slic3r::portability::render::ios {

class IOSMetalRenderBackend final : public IRenderBackend
{
public:
    std::string_view backend_name() const override;
    BackendType      backend_type() const override;
    bool             initialize() override;
    void             resize(int width, int height) override;
    void             render_frame() override;
    void             shutdown() override;

private:
    int  m_width{0};
    int  m_height{0};
    bool m_initialized{false};
};

} // namespace Slic3r::portability::render::ios
