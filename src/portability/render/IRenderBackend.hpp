#ifndef orcaslicer_IRenderBackend_hpp_
#define orcaslicer_IRenderBackend_hpp_

#include <cstdint>

namespace Slic3r::Portability::Render {

enum class BackendType : uint8_t
{
    OpenGL,
    Metal,
    Vulkan,
    OpenGLES
};

class IRenderBackend
{
public:
    virtual ~IRenderBackend() = default;

    virtual BackendType backend_type() const = 0;
    virtual bool initialize() = 0;
    virtual void resize(int width, int height) = 0;
    virtual void render_frame() = 0;
    virtual void shutdown() = 0;
};

} // namespace Slic3r::Portability::Render

#endif // orcaslicer_IRenderBackend_hpp_
