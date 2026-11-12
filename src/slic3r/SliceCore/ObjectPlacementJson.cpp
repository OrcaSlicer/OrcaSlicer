// ObjectPlacementJson.cpp — JSON -> SliceCore::ObjectPlacement parsing.
//
// Implementation moved from Server/RequestMapping.cpp so that the parsing
// logic lives in liborca_slice_core and is available to both the OrcaSlicer
// app (--placement-json CLI) and orca-server without either needing to
// compile the other's translation units.

#include "ObjectPlacementJson.hpp"

namespace Slic3r {
namespace SliceCore {

using json = nlohmann::json;

void parse_objects(const json &arr, std::vector<ObjectPlacement> &out)
{
    // Helper: parse a 3-element {x,y,z} object OR a 3-element JSON array into
    // a std::array<double,3>.  Returns true on success.
    auto parse_xyz_double = [](const json &v, std::array<double, 3> &result) -> bool {
        if (v.is_object()
            && v.contains("x") && v.contains("y") && v.contains("z")) {
            result[0] = v["x"].get<double>();
            result[1] = v["y"].get<double>();
            result[2] = v["z"].get<double>();
            return true;
        }
        if (v.is_array() && v.size() == 3) {
            result[0] = v[0].get<double>();
            result[1] = v[1].get<double>();
            result[2] = v[2].get<double>();
            return true;
        }
        return false;
    };

    // Helper: parse a 3-element {x,y,z} object OR a 3-element JSON array of
    // booleans into a std::array<bool,3>.
    auto parse_xyz_bool = [](const json &v, std::array<bool, 3> &result) -> bool {
        if (v.is_object()
            && v.contains("x") && v.contains("y") && v.contains("z")) {
            result[0] = v["x"].get<bool>();
            result[1] = v["y"].get<bool>();
            result[2] = v["z"].get<bool>();
            return true;
        }
        if (v.is_array() && v.size() == 3) {
            result[0] = v[0].get<bool>();
            result[1] = v[1].get<bool>();
            result[2] = v[2].get<bool>();
            return true;
        }
        return false;
    };

    if (!arr.is_array())
        return;

    for (const auto &elem : arr) {
        if (!elem.is_object())
            continue;

        ObjectPlacement op;

        if (elem.contains("index") && elem["index"].is_number_integer())
            op.index = elem["index"].get<int>();

        if (elem.contains("name") && elem["name"].is_string())
            op.name = elem["name"].get<std::string>();

        if (elem.contains("position")) {
            std::array<double, 3> pos{};
            if (parse_xyz_double(elem["position"], pos))
                op.position = pos;
        }

        if (elem.contains("rotation")) {
            std::array<double, 3> rot{};
            if (parse_xyz_double(elem["rotation"], rot))
                op.rotation = rot;
        }

        if (elem.contains("scale")) {
            const auto &sv = elem["scale"];
            if (sv.is_number()) {
                // Scalar → uniform_scale.
                op.uniform_scale = sv.get<double>();
            } else {
                // Object {x,y,z} → per-axis scale.
                std::array<double, 3> sc{};
                if (parse_xyz_double(sv, sc))
                    op.scale = sc;
            }
        }

        if (elem.contains("mirror")) {
            std::array<bool, 3> mir{};
            if (parse_xyz_bool(elem["mirror"], mir))
                op.mirror = mir;
        }

        if (elem.contains("orient") && elem["orient"].is_number_integer())
            op.orient = elem["orient"].get<int>();

        if (elem.contains("ensure_on_bed") && elem["ensure_on_bed"].is_boolean())
            op.ensure_on_bed = elem["ensure_on_bed"].get<bool>();

        if (elem.contains("printable") && elem["printable"].is_boolean())
            op.printable = elem["printable"].get<bool>();

        // instances: array of {position, rotation_z, scale}
        if (elem.contains("instances") && elem["instances"].is_array()) {
            for (const auto &inst : elem["instances"]) {
                if (!inst.is_object())
                    continue;
                InstancePlacement ip;
                if (inst.contains("position")) {
                    std::array<double, 3> ipos{};
                    if (parse_xyz_double(inst["position"], ipos))
                        ip.position = ipos;
                }
                if (inst.contains("rotation_z") && inst["rotation_z"].is_number())
                    ip.rotation_z = inst["rotation_z"].get<double>();
                if (inst.contains("scale") && inst["scale"].is_number())
                    ip.scale = inst["scale"].get<double>();
                op.instances.push_back(std::move(ip));
            }
        }

        out.push_back(std::move(op));
    }
}

} // namespace SliceCore
} // namespace Slic3r
