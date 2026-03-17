#pragma once

#include <cstdint>
#include <string_view>

namespace Slic3r::portability::render {

enum class BackendType : uint8_t
{
    OpenGL,
    Metal,
    Vulkan,
    OpenGLES,
    Null
};

class IRenderBackend
{
public:
    virtual ~IRenderBackend() = default;

    virtual std::string_view backend_name() const = 0;
    virtual BackendType      backend_type() const = 0;
    virtual bool             initialize() = 0;
    virtual void             resize(int width, int height) = 0;
    virtual void             render_frame() = 0;
    virtual void             shutdown() = 0;
};

} // namespace Slic3r::portability::render
