#include "IOSMetalRenderBackend.hpp"

namespace Slic3r::portability::render::ios {

std::string_view IOSMetalRenderBackend::backend_name() const
{
    return "ios-metal";
}

BackendType IOSMetalRenderBackend::backend_type() const
{
    return BackendType::Metal;
}

bool IOSMetalRenderBackend::initialize()
{
    m_initialized = true;
    return true;
}

void IOSMetalRenderBackend::resize(int width, int height)
{
    m_width = width;
    m_height = height;
}

void IOSMetalRenderBackend::render_frame()
{
    if (!m_initialized)
        return;

    (void)m_width;
    (void)m_height;
}

void IOSMetalRenderBackend::shutdown()
{
    m_initialized = false;
}

} // namespace Slic3r::portability::render::ios
