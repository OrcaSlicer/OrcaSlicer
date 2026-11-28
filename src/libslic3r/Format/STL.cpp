#include "../Model.hpp"
#include "../TexturePainting.hpp"
#include "../TriangleMesh.hpp"

#include "STL.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <string_view>
#include <utility>

#ifdef _WIN32
#define DIR_SEPARATOR '\\'
#else
#define DIR_SEPARATOR '/'
#endif

namespace Slic3r {

// Binary STL has no standard colour, but two conventions keep one in the 2-byte attribute of each facet:
// VisCAM and SolidView store 15-bit BGR with bit 15 set when the colour is valid; Materialise Magics stores
// 15-bit RGB with bit 15 clear when the facet has its own colour, and otherwise takes the "COLOR=" RGBA the
// file header gives the whole part. A header with "COLOR=" selects Magics. The result is one colour per facet,
// packed as 0xFFRRGGBB with 0 where the facet has none, or empty when the file stores no colours.
static std::vector<std::uint32_t> stl_facet_colors(const stl_file& stl)
{
    const size_t count = std::min<size_t>(stl.stats.number_of_facets, stl.facet_start.size());
    if (stl.stats.type != binary || count == 0)
        return {};

    const auto attribute = [&stl](size_t facet) {
        std::uint16_t value;
        std::memcpy(&value, stl.facet_start[facet].extra, sizeof(value));
        return value;
    };
    // A plain STL zeroes the attribute everywhere.
    bool any = false;
    for (size_t i = 0; i < count && !any; ++i)
        any = attribute(i) != 0;
    if (!any)
        return {};

    const auto pack = [](std::uint32_t r, std::uint32_t g, std::uint32_t b) {
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    };
    const auto from_5_bits = [](std::uint32_t v) { return (v * 255 + 15) / 31; };

    const std::string_view header(stl.stats.header.data(), stl.stats.header.size());
    const size_t           color_at = header.find("COLOR=");
    const bool             magics   = color_at != std::string_view::npos && color_at + 10 <= header.size();
    std::uint32_t          part_color = 0;
    if (magics)
        part_color = pack(std::uint8_t(header[color_at + 6]), std::uint8_t(header[color_at + 7]), std::uint8_t(header[color_at + 8]));

    std::vector<std::uint32_t> colors(count, 0);
    for (size_t i = 0; i < count; ++i) {
        const std::uint32_t a    = attribute(i);
        const std::uint32_t low  = from_5_bits(a & 0x1F);
        const std::uint32_t mid  = from_5_bits((a >> 5) & 0x1F);
        const std::uint32_t high = from_5_bits((a >> 10) & 0x1F);
        const bool          bit  = (a & 0x8000) != 0;
        if (magics)
            colors[i] = bit ? part_color : pack(low, mid, high);
        else if (bit)
            colors[i] = pack(high, mid, low);
    }
    return colors;
}

bool load_stl(const char *path, Model *model, const char *object_name_in, ImportstlProgressFn stlFn, int custom_header_length, bool import_colors)
{
    TriangleMesh mesh;
    std::string design_id;
    std::vector<std::uint32_t> colors;

    {
        stl_file stl;
        if (!stl_open(&stl, path, stlFn, custom_header_length)) {
            //    die "Failed to open $file\n" if !-e $path;
            return false;
        }
        mesh.from_stl(stl, true);
        // The repair keeps each facet's attribute with it, so the colours still line up with the faces of the mesh.
        if (import_colors)
            colors = stl_facet_colors(stl);
    }
    if (mesh.empty()) {
        // die "This STL file couldn't be read because it's empty.\n"
        return false;
    }

    std::string object_name;
    if (object_name_in == nullptr) {
        const char *last_slash = strrchr(path, DIR_SEPARATOR);
        object_name.assign((last_slash == nullptr) ? path : last_slash + 1);
    } else
       object_name.assign(object_name_in);

    ModelObject *object = model->add_object(object_name.c_str(), path, std::move(mesh));
    if (!colors.empty())
        model->texture_mesh = make_face_color_mesh({ { object, 0, std::move(colors) } });
    return true;
}

bool store_stl(const char *path, TriangleMesh *mesh, bool binary)
{
    if (binary)
        mesh->write_binary(path);
    else
        mesh->write_ascii(path);
    //FIXME returning false even if write failed.
    return true;
}

bool store_stl(const char *path, ModelObject *model_object, bool binary)
{
    TriangleMesh mesh = model_object->mesh();
    return store_stl(path, &mesh, binary);
}

bool store_stl(const char *path, Model *model, bool binary)
{
    TriangleMesh mesh = model->mesh();
    return store_stl(path, &mesh, binary);
}

}; // namespace Slic3r