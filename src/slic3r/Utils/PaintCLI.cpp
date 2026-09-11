// PaintCLI.cpp — CLI paint-inspection primitives. See PaintCLI.hpp.
#include "PaintCLI.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace PaintCLI {

namespace {

using json = nlohmann::json;

double its_surface_area(const indexed_triangle_set &its)
{
    double total = 0.0;
    for (const stl_triangle_vertex_indices &t : its.indices) {
        const Vec3f &a = its.vertices[t(0)];
        const Vec3f &b = its.vertices[t(1)];
        const Vec3f &c = its.vertices[t(2)];
        total += 0.5 * (b - a).cross(c - a).norm();
    }
    return total;
}

// Bbox over triangle-referenced vertices only. get_facets_strict() returns
// an itset with the full source vertex list — using bounding_box() on it
// would report the whole mesh's bbox even when only a few facets are painted.
BoundingBoxf3 its_referenced_bbox(const indexed_triangle_set &its)
{
    BoundingBoxf3 bb;
    bool first = true;
    for (const stl_triangle_vertex_indices &t : its.indices) {
        for (int k = 0; k < 3; ++k) {
            const Vec3d v = its.vertices[t(k)].cast<double>();
            if (first) { bb.min = bb.max = v; first = false; }
            else       bb.merge(v);
        }
    }
    return bb;
}

json vec3_to_json(const Vec3d &v)
{
    return json::array({ v.x(), v.y(), v.z() });
}

json bbox_to_json(const BoundingBoxf3 &bb)
{
    return {
        { "min",  vec3_to_json(bb.min) },
        { "max",  vec3_to_json(bb.max) },
        { "size", vec3_to_json(Vec3d(bb.max - bb.min)) },
    };
}

// One (layer, state) row — empty ones are omitted at the caller level.
json state_entry(const std::string &label, const indexed_triangle_set &its)
{
    return {
        { "state",    label },
        { "facets",   its.indices.size() },
        { "area_mm2", its_surface_area(its) },
        { "bbox",     bbox_to_json(its_referenced_bbox(its)) },
    };
}

// Iterate the states relevant to one FacetsAnnotation kind, collecting
// non-empty entries. Empty layer → {"empty": true}. `n_facets_out` is the
// running total of painted facets — bumped for the summary.
json inspect_layer(const ModelVolume &mv, const FacetsAnnotation &fa,
                   const std::vector<std::pair<EnforcerBlockerType, std::string>> &states,
                   size_t &n_facets_out)
{
    if (fa.empty())
        return { { "empty", true } };

    json entries = json::array();
    for (const auto &st : states) {
        if (!fa.has_facets(mv, st.first))
            continue;
        indexed_triangle_set its = fa.get_facets_strict(mv, st.first);
        if (its.indices.empty())
            continue;
        n_facets_out += its.indices.size();
        entries.push_back(state_entry(st.second, its));
    }
    return {
        { "empty",  entries.empty() },
        { "states", std::move(entries) },
    };
}

const std::vector<std::pair<EnforcerBlockerType, std::string>> &supports_states()
{
    static const std::vector<std::pair<EnforcerBlockerType, std::string>> s = {
        { EnforcerBlockerType::ENFORCER, "ENFORCER" },
        { EnforcerBlockerType::BLOCKER,  "BLOCKER"  },
    };
    return s;
}

const std::vector<std::pair<EnforcerBlockerType, std::string>> &fuzzy_states()
{
    // FUZZY_SKIN is an enum alias for ENFORCER; the layer is single-state.
    static const std::vector<std::pair<EnforcerBlockerType, std::string>> s = {
        { EnforcerBlockerType::FUZZY_SKIN, "FUZZY_SKIN" },
    };
    return s;
}

const std::vector<std::pair<EnforcerBlockerType, std::string>> &mmu_states()
{
    static std::vector<std::pair<EnforcerBlockerType, std::string>> s = []{
        std::vector<std::pair<EnforcerBlockerType, std::string>> v;
        for (int i = 1; i <= int(EnforcerBlockerType::ExtruderMax); ++i)
            v.emplace_back(EnforcerBlockerType(i), "extruder_" + std::to_string(i));
        return v;
    }();
    return s;
}

} // namespace

void inspect_to_json(const Model &model, const std::string &source_path,
                     std::ostream &out)
{
    json root;
    root["source"] = source_path;
    root["frame"]  = "mesh_local";
    root["note"]   = "Coordinates are mesh-local (each volume's own frame). "
                     "Paint gizmos operate in this frame.";

    json objects = json::array();
    size_t total_objects = 0, total_volumes = 0, total_painted = 0, total_facets = 0;

    for (size_t oi = 0; oi < model.objects.size(); ++oi) {
        const ModelObject *mo = model.objects[oi];
        if (!mo) continue;
        ++total_objects;

        json obj;
        obj["index"] = oi;
        obj["name"]  = mo->name;

        json volumes = json::array();
        for (size_t vi = 0; vi < mo->volumes.size(); ++vi) {
            const ModelVolume *mv = mo->volumes[vi];
            if (!mv) continue;
            ++total_volumes;

            const indexed_triangle_set &its = mv->mesh().its;
            json vol;
            vol["index"]    = vi;
            vol["name"]     = mv->name;
            vol["n_facets"] = its.indices.size();
            vol["is_model_part"] = mv->is_model_part();
            vol["bbox_mesh_local"] = bbox_to_json(bounding_box(its));

            size_t vol_painted = 0;
            json paints;
            paints["supports"]         = inspect_layer(*mv, mv->supported_facets,
                                                      supports_states(),  vol_painted);
            paints["seam"]             = inspect_layer(*mv, mv->seam_facets,
                                                      supports_states(),  vol_painted);
            paints["mmu_segmentation"] = inspect_layer(*mv, mv->mmu_segmentation_facets,
                                                      mmu_states(),       vol_painted);
            paints["fuzzy_skin"]       = inspect_layer(*mv, mv->fuzzy_skin_facets,
                                                      fuzzy_states(),     vol_painted);
            vol["paints"] = std::move(paints);
            vol["painted_facets_total"] = vol_painted;

            if (vol_painted > 0) ++total_painted;
            total_facets += vol_painted;

            volumes.push_back(std::move(vol));
        }
        obj["volumes"] = std::move(volumes);
        objects.push_back(std::move(obj));
    }
    root["objects"] = std::move(objects);
    root["summary"] = {
        { "objects",                    total_objects },
        { "volumes",                    total_volumes },
        { "volumes_with_paint",         total_painted },
        { "painted_facets_total",       total_facets  },
    };

    out << root.dump(2) << std::endl;
}

} // namespace PaintCLI
} // namespace Slic3r
