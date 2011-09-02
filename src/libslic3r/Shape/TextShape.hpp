#ifndef slic3r_Text_Shape_hpp_
#define slic3r_Text_Shape_hpp_

#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r {
class TriangleMesh;

struct TextResult
{
    TriangleMesh text_mesh;
    double       text_width;
};

extern std::vector<std::string> init_occt_fonts();
extern void load_text_shape(const char *text, const char *font, const float text_height, const float thickness, bool is_bold, bool is_italic, TextResult &text_result);

std::map<std::string, std::string> get_occt_fonts_maps();

}; // namespace Slic3r

#endif // slic3r_Text_Shape_hpp_
