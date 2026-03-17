#ifndef orcaslicer_NullRenderBackend_hpp_
#define orcaslicer_NullRenderBackend_hpp_

#include "IRenderBackend.hpp"

namespace Slic3r::Portability::Render {

class NullRenderBackend final : public IRenderBackend
{
public:
    BackendType backend_type() const override { return BackendType::OpenGL; }
    bool initialize() override { return true; }
    void resize(int, int) override {}
    void render_frame() override {}
    void shutdown() override {}
};

} // namespace Slic3r::Portability::Render

#endif // orcaslicer_NullRenderBackend_hpp_
