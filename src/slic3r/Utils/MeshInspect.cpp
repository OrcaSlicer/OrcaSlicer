#include "MeshInspect.hpp"

#include "libslic3r/LayOnFace.hpp"
#include "libslic3r/Model.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <ostream>

namespace Slic3r {
namespace MeshInspect {

using json = nlohmann::json;

static json to_json(const Vec3d &v) { return json::array({ v.x(), v.y(), v.z() }); }

static json to_json(const BoundingBoxf3 &bb)
{
    return { { "min", to_json(bb.min) }, { "max", to_json(bb.max) }, { "size", to_json(bb.size()) } };
}

void inspect_to_json(const Model &model, const std::vector<std::string> &source_paths, std::ostream &out, size_t max_planes)
{
    json objects = json::array();
    for (const ModelObject *mo : model.objects) {
        json obj = { { "name", mo->name },
                     { "triangle_count", mo->facets_count() },
                     { "instance_count", mo->instances.size() },
                     { "bbox_object", to_json(mo->raw_mesh_bounding_box()) } };
        if (!mo->instances.empty()) {
            const std::vector<LayOnFacePlane> planes = lay_on_face_planes(*mo, mo->instances.front()->get_matrix_no_offset());
            json planes_json = json::array();
            for (size_t i = 0; i < std::min(planes.size(), max_planes); ++i)
                planes_json.push_back({ { "normal", to_json(planes[i].normal) },
                                        { "area_mm2", std::round(double(planes[i].area) * 1000.) / 1000. },
                                        { "center", to_json(planes[i].center) } });
            obj["bbox_world"]      = to_json(mo->instance_bounding_box(0));
            obj["instance_offset"] = to_json(mo->instances.front()->get_offset());
            obj["plane_count"]     = planes.size();
            obj["planes"]          = std::move(planes_json);
        }
        objects.push_back(std::move(obj));
    }

    const json root = {
        { "sources", source_paths },
        { "note", "Lengths in mm. bbox_object and the plane normals and centers are in object coordinates: the parts as "
                  "currently transformed, without the instance transformation. --ground-face-normal and "
                  "--ground-face-point take values in these coordinates. area_mm2 uses instance 0's scale, "
                  "bbox_world is instance 0 on the plate." },
        { "objects", std::move(objects) },
    };
    // Object names and file paths are arbitrary bytes, and dump() throws on invalid UTF-8 by default.
    // Replace such sequences with U+FFFD so the output is always valid JSON.
    out << root.dump(2, ' ', false, json::error_handler_t::replace) << std::endl;
}

} // namespace MeshInspect
} // namespace Slic3r
