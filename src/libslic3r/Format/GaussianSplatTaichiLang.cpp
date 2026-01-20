#include "GaussianSplatTaichiLang.hpp"

#include <algorithm>
#include <locale>
#include <sstream>

namespace Slic3r {

static std::string escape_quotes(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"')
            out += "\\\"";
        else
            out += c;
    }
    return out;
}

std::string gaussian_splat_to_taichi_lang(const GaussianSplatImportSpec& spec)
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss.setf(std::ios::fixed);
    ss.precision(6);

    const std::string name = spec.object_name.empty() ? std::string("Gaussian Splat") : spec.object_name;

    std::vector<std::string> images = spec.image_paths;
    images.erase(std::remove_if(images.begin(), images.end(), [](const std::string& p) { return p.empty(); }), images.end());
    std::sort(images.begin(), images.end());
    images.erase(std::unique(images.begin(), images.end()), images.end());

    const double cube = (spec.preview_cube_size_mm > 0.0) ? spec.preview_cube_size_mm : 20.0;

    ss << "# orcaslicer_taichi v1\n";
    ss << "# generator gaussian_splat v0\n";
    ss << "# This script is forward-compatible: Gaussian Splat directives are embedded,\n";
    ss << "# and a preview cube is emitted so it produces geometry today.\n\n";

    ss << "model_objects 1\n\n";
    ss << "object 0 \"" << escape_quotes(name) << "\" {\n";

    ss << "  gaussian_splat_begin\n";
    ss << "  gaussian_splat_images " << images.size() << "\n";
    for (const std::string& p : images)
        ss << "  gaussian_splat_image \"" << escape_quotes(p) << "\"\n";
    ss << "  gaussian_splat_end\n\n";

    ss << "  geometry cube [" << cube << "," << cube << "," << cube << "]\n";
    ss << "}\n";

    return ss.str();
}

} // namespace Slic3r
