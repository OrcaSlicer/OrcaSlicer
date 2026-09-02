#include "OozeShield.hpp"

#include "ClipperUtils.hpp"
#include "Flow.hpp"
#include "Geometry.hpp"
#include "GCode/ToolOrdering.hpp"
#include "Layer.hpp"
#include "Print.hpp"

#include <cmath>

namespace Slic3r {

static ExPolygons collect_model_layer_slices(const Print &print, size_t layer_id)
{
    ExPolygons slices;
    for (const PrintObject *object : print.objects()) {
        if (layer_id >= object->layer_count())
            continue;
        const Layer *layer = object->get_layer(layer_id);
        for (const PrintInstance &instance : object->instances()) {
            for (const ExPolygon &expoly : layer->lslices) {
                ExPolygon translated = expoly;
                translated.translate(instance.shift);
                slices.emplace_back(std::move(translated));
            }
        }
    }
    return union_ex(slices);
}

static Polygons offset_outside(const ExPolygons &slices, double distance_mm)
{
    if (slices.empty())
        return {};
    const float offset = float(scale_(distance_mm));
    ExPolygons offseted = offset_ex(slices, offset, ClipperLib::jtRound, float(scale_(0.1)));
    Polygons result;
    result.reserve(offseted.size());
    for (const ExPolygon &expoly : offseted)
        result.emplace_back(expoly.contour);
    return result;
}

static void taper_shield_layers(
    std::vector<Polygons> &layers,
    double                 angle_deg,
    double                 layer_height_mm)
{
    if (layers.size() < 2 || angle_deg >= 89.9)
        return;

    const double angle_rad   = Geometry::deg2rad(angle_deg);
    const float  taper_delta = float(scale_(std::tan(angle_rad) * layer_height_mm));

    for (size_t layer_nr = 1; layer_nr < layers.size(); ++layer_nr) {
        Polygons combined = layers[layer_nr];
        append(combined, offset(layers[layer_nr - 1], -taper_delta));
        layers[layer_nr] = to_polygons(union_ex(combined));
    }

    for (size_t layer_nr = layers.size(); layer_nr-- > 1;) {
        Polygons combined = layers[layer_nr - 1];
        append(combined, offset(layers[layer_nr], -taper_delta));
        layers[layer_nr - 1] = to_polygons(union_ex(combined));
    }
}

static double nominal_layer_height_mm(const Print &print)
{
    for (const PrintObject *object : print.objects())
        if (!object->layers().empty())
            return object->layers().front()->height;
    return 0.2;
}

static size_t object_layer_id_at_print_z(const Print &print, coordf_t print_z)
{
    size_t layer_id = 0;
    bool   found    = false;
    for (const PrintObject *object : print.objects()) {
        for (size_t i = 0; i < object->layer_count(); ++i) {
            if (std::abs(object->get_layer(i)->print_z - print_z) < EPSILON) {
                layer_id = std::max(layer_id, i);
                found    = true;
            }
        }
    }
    return found ? layer_id : 0;
}

static Polygons wipe_tower_exclusion(const Print &print, double line_width_mm)
{
    if (!print.has_wipe_tower())
        return {};

    Points corners = print.first_layer_wipe_tower_corners(false);
    if (corners.size() < 3)
        return {};

    Polygon tower;
    tower.points = std::move(corners);
    const float margin = float(scale_(0.5 * line_width_mm));
    return offset(tower, margin, ClipperLib::jtRound, float(scale_(0.1)));
}

std::vector<Polygons> OozeShield::generate_layer_polygons(const Print &print)
{
    std::vector<Polygons> result;
    if (!print.has_ooze_shield())
        return result;

    size_t layer_count = 0;
    for (const PrintObject *object : print.objects())
        layer_count = std::max(layer_count, object->layer_count());

    const double distance_mm = print.config().ooze_shield_distance.value;
    const double angle_deg   = print.config().ooze_shield_angle.value;
    const double layer_height_mm = nominal_layer_height_mm(print);
    const double min_area    = scale_(scale_(1.0)); // 1 mm² in scaled units

    result.resize(layer_count);
    for (size_t layer_id = 0; layer_id < layer_count; ++layer_id)
        result[layer_id] = offset_outside(collect_model_layer_slices(print, layer_id), distance_mm);

    taper_shield_layers(result, angle_deg, layer_height_mm);

    const Polygons tower_exclusion = wipe_tower_exclusion(print, print.skirt_flow().width());
    if (!tower_exclusion.empty()) {
        for (Polygons &layer : result)
            layer = diff(layer, tower_exclusion);
    }

    for (Polygons &layer : result)
        remove_small(layer, min_area);

    const size_t max_layer = max_shield_layer(print);
    if (max_layer + 1 < result.size())
        result.resize(max_layer + 1);

    return result;
}

void OozeShield::polygons_to_extrusion_entities(
    const Polygons              &polygons,
    ExtrusionEntityCollection   &dst,
    const Flow                  &flow)
{
    const float mm3_per_mm = flow.mm3_per_mm();
    for (const Polygon &polygon : polygons) {
        if (polygon.points.size() < 3)
            continue;
        ExtrusionLoop eloop(elrSkirt);
        eloop.paths.emplace_back(ExtrusionPath(
            erSkirt,
            mm3_per_mm,
            flow.width(),
            flow.height()));
        eloop.paths.back().polyline = Polyline3(polygon.split_at_first_point());
        dst.append(eloop);
    }
}

size_t OozeShield::max_shield_layer(const Print &print)
{
    size_t max_layer = 0;
    bool   found_tool_change = false;

    ToolOrdering ordering(print, -1, false);
    if (ordering.empty())
        return 0;

    for (size_t i = 0; i < ordering.layer_tools().size(); ++i) {
        const LayerTools &lt = ordering.layer_tools()[i];
        if (!lt.has_object)
            continue;
        if (lt.extruders.size() > 1) {
            found_tool_change = true;
            max_layer = std::max(max_layer, object_layer_id_at_print_z(print, lt.print_z));
        } else if (i > 0) {
            const LayerTools &prev = ordering.layer_tools()[i - 1];
            if (prev.has_object && !prev.extruders.empty() && !lt.extruders.empty()
                && prev.extruders.back() != lt.extruders.front()) {
                found_tool_change = true;
                max_layer = std::max(max_layer, object_layer_id_at_print_z(print, lt.print_z));
            }
        }
    }

  return found_tool_change ? max_layer : 0;
}

} // namespace Slic3r
