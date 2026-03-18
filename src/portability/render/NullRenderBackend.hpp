#pragma once

#include "IRenderBackend.hpp"
#include "ISceneRenderer.hpp"

namespace Slic3r::portability::render {

class NullRenderBackend final : public IRenderBackend
{
public:
    std::string_view backend_name() const override { return "null"; }
    BackendType      backend_type() const override { return BackendType::Null; }
    bool             initialize() override { return true; }
    void             submit_scene_state(const RenderSceneState&) override {}
    void             resize(int, int) override {}
    void             render_frame() override {}
    void             shutdown() override {}
};

} // namespace Slic3r::portability::render
