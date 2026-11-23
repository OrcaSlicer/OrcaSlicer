#pragma once

#include <map>
#include <memory>
#include <tuple>

namespace Slic3r {
namespace GUI {

class GLTexture;

namespace CanvasUi {

enum class Icon : unsigned char {
    Home,
    RotateLeft,
    RotateRight,
    Cube,
    Eye,
    EyeOff,
    Fit,
    Check,
    ChevronsUp,
    ChevronsDown,
    Sliders,
    Reset,
    Clock,
    ChevronDown,
    TriangleAlert,
    Info,
    ArrowRight,
    Count
};

// The file of the icon, resources/images/<name>.svg among the other images: one colour, on a 24 unit grid.
const char* icon_name(Icon icon);

// The icons rasterized from their SVG files, once per size and line width in pixels, for one OpenGL context.
class IconCache
{
public:
    IconCache();
    ~IconCache();
    IconCache(const IconCache&)            = delete;
    IconCache& operator=(const IconCache&) = delete;

    // The texture of the icon at size × size pixels with lines stroke pixels wide, whatever the width in its file;
    // 0 when its file cannot be read.
    unsigned int texture(Icon icon, int size, float stroke);

private:
    std::map<std::tuple<Icon, int, int>, std::unique_ptr<GLTexture>> m_textures;
};

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
