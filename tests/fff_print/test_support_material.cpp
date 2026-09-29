#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "test_helpers.hpp" // get access to init_print, etc

// Not self-contained: its inline constructor uses PrintObject, PrintRegion, SlicingParameters and
// Geometry, so it must follow the headers (pulled in via test_helpers.hpp) that define them.
#include "libslic3r/Support/SupportParameters.hpp"

using namespace Slic3r::Test;
using namespace Slic3r;

struct RibRun {
    size_t first_layer_paths = 0;
    size_t first_layer_closed_paths = 0;
    size_t first_layer_islands = 0;
    size_t contact_layers = 0;
    size_t gap_layers = 0;
    double foot_short_span = 0;
    double foot_long_span = 0;
    double outer_edge_spread = 0;
    double contact_edge_overrun = 0;
    size_t closed_paths = 0;
    size_t open_paths = 0;
    size_t contact_starts = 0;
    double body_thickness = 0;
    double contact_width = 0;
    size_t contact_tip_vertices = 0;
    size_t unsupported_contact_vertices = 0;
    size_t min_body_paths = std::numeric_limits<size_t>::max();
    size_t supported_contact_fronts = 0;
    double unsupported_contact_length = 0;
    double support_top_z = 0;
    double contact_top_z = 0;
    size_t projection_layers = 0;
    double projection_front_span = 0;
    double min_projection_gap = std::numeric_limits<double>::infinity();
    double top_outer_depth = 0;
    double first_body_z = 0;
    size_t first_body_paths = 0;
    size_t top_body_paths = 0;
    std::vector<Vec2d> top_body_directions;
    bool positive_x_sheet = false;
    bool negative_x_sheet = false;
    bool lower_y_contact = false;
    bool upper_y_contact = false;
    std::vector<double> top_contact_y;
    double max_rib_center_overlap = 0;
    double max_projection_side_overrun = 0;
    size_t bounded_projection_vertices = 0;
};

static RibRun slice_ribs(double max_spacing, bool paint = true, double lean = 0,
                         double thickness = 1.2, double contact_width = 1.2,
                         int wall_count = 0, double projection_angle = 30,
                         bool contact_percent = false, bool thickness_percent = false, double outer_width = 0.4,
                         double contact_spacing = 8.0, double expansion_factor = 0,
                         double model_height = 60, double model_width = 6,
                         const indexed_triangle_set *custom_mesh = nullptr, double lean_y = 0,
                         bool paint_all_sides = false, bool measure_projection_bounds = false, bool half_painted_face = false,
                         bool zaa_enabled = false, double zaa_min_z = 0,
                         double painted_max_z = std::numeric_limits<double>::infinity(),
                          const char *rib_style = "simulated",
                          double painted_min_z = -std::numeric_limits<double>::infinity(),
                          double object_gap = 0.6, double first_layer_expansion = 2.)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", 1 }, { "support_type", "rib" }, { "rib_support_style", rib_style },
        { "rib_support_max_spacing", max_spacing },
        { "rib_support_depth", 8.0 },
        { "rib_support_expansion_factor", expansion_factor },
        { "outer_wall_line_width", outer_width },
        { "support_object_xy_distance", object_gap }, { "support_line_width", 0.4 },
        { "raft_first_layer_expansion", first_layer_expansion },
        { "rib_support_contact_height", 0.6 }, { "rib_support_contact_start_height", 0.0 },
        { "rib_support_contact_spacing", contact_spacing },
        { "tree_support_wall_count", wall_count }, { "rib_support_contact_projection_angle", projection_angle },
        { "layer_height", 0.2 },
        { "initial_layer_print_height", 0.2 },
        { "zaa_enabled", zaa_enabled ? 1 : 0 }, { "zaa_min_z", zaa_min_z },
    });
    config.set_key_value("rib_support_contact_width", new ConfigOptionFloatOrPercent(contact_width, contact_percent));
    config.set_key_value("rib_support_thickness", new ConfigOptionFloatOrPercent(thickness, thickness_percent));
    Model model;
    Print print;
    indexed_triangle_set mesh = custom_mesh == nullptr ? its_make_cube(model_width, 60, model_height) : *custom_mesh;
    for (Vec3f &vertex : mesh.vertices) {
        vertex.x() += float(lean * vertex.z());
        vertex.y() += float(lean_y * vertex.z());
    }
    init_print({ TriangleMesh(std::move(mesh)) }, print, model, config);
    if (paint) {
        ModelVolume *volume = model.objects.front()->volumes.front();
        TriangleSelector selector(volume->mesh());
        const indexed_triangle_set &mesh = volume->mesh().its;
        bool painted_one_face_facet = false;
        for (size_t i = 0; i < mesh.indices.size(); ++i) {
            const auto &f = mesh.indices[i];
            const Vec3f n = (mesh.vertices[f(1)] - mesh.vertices[f(0)]).cross(
                            mesh.vertices[f(2)] - mesh.vertices[f(0)]).normalized();
            if (n.x() > 0.5f || (paint_all_sides && n.head<2>().norm() > 0.5f)) {
                const float facet_mid_z = (mesh.vertices[f(0)].z() + mesh.vertices[f(1)].z() +
                                           mesh.vertices[f(2)].z()) / 3.f;
                if (facet_mid_z < painted_min_z || facet_mid_z > painted_max_z)
                    continue;
                // A cube's side has two triangles; one is half the same plane.
                if (half_painted_face && painted_one_face_facet)
                    continue;
                selector.set_facet(i, EnforcerBlockerType::ENFORCER);
                painted_one_face_facet = true;
            }
        }
        REQUIRE(volume->supported_facets.set(selector));
    }
    print.apply(model, config);
    print.process();
    const PrintObject *object = print.objects().front();
    const auto layers = object->support_layers();
    RibRun result;
    if (layers.empty())
        return result;
    result.first_layer_paths = layers.front()->support_fills.entities.size();
    result.first_layer_islands = layers.front()->support_islands.size();
    for (const ExtrusionEntity *entity : layers.front()->support_fills.entities) {
        const auto *path = dynamic_cast<const ExtrusionPath *>(entity);
        REQUIRE(path != nullptr);
        result.first_layer_closed_paths += path->first_point() == path->last_point();
    }
    for (const ExPolygon &island : layers.front()->support_islands) {
        const Vec2d size = unscale(get_extents(island).size());
        result.foot_short_span = std::max(result.foot_short_span, std::min(size.x(), size.y()));
        result.foot_long_span = std::max(result.foot_long_span, std::max(size.x(), size.y()));
    }
    double min_outer = std::numeric_limits<double>::infinity();
    double max_outer = -std::numeric_limits<double>::infinity();
    const auto model_layers = object->layers();
    const double rib_half_width = 0.5 * support_material_flow(object).width();
    const SupportLayer *previous_layer = nullptr;
    std::vector<Points> projection_rings;
    for (const SupportLayer *layer : layers) {
        result.support_top_z = std::max(result.support_top_z, layer->print_z);
        Points points;
        layer->support_fills.collect_points(points);
        if (points.empty())
            continue;
        const coord_t min_x = std::min_element(points.begin(), points.end(),
            [](const Point &a, const Point &b) { return a.x() < b.x(); })->x();
        const auto model_layer = std::find_if(model_layers.begin(), model_layers.end(),
            [layer](const Layer *candidate) { return std::abs(candidate->print_z - layer->print_z) < 1e-6; });
        REQUIRE(model_layer != model_layers.end());
        const BoundingBox model_bounds = get_extents((*model_layer)->lslices);
        const Vec2d model_size = unscale(model_bounds.size());
        if (layer->print_z > 15 && layer->print_z < 25 && model_size.y() < 20)
            result.lower_y_contact |= std::any_of(points.begin(), points.end(), [&](const Point &point) {
                const double gap = unscale<double>(point.y() - model_bounds.max.y());
                return std::abs(unscale<double>(point.x() - model_bounds.center().x())) < 1 && gap > 0 && gap < 0.45;
            });
        if (layer->print_z > 40 && layer->print_z < 55 && model_size.y() > 50)
            result.upper_y_contact |= std::any_of(points.begin(), points.end(), [&](const Point &point) {
                const double gap = unscale<double>(point.y() - model_bounds.max.y());
                return std::abs(unscale<double>(point.x() - model_bounds.center().x())) < 1 && gap > 0 && gap < 0.45;
            });
        const coord_t face_x = model_bounds.max.x();
        const double distance = unscale<double>(min_x - face_x) - rib_half_width;
        result.contact_layers += distance < 0.01;
        if (distance < 0.01)
            result.contact_top_z = std::max(result.contact_top_z, layer->print_z);
        result.gap_layers += distance > 0.4;
        if (layer != layers.front()) {
            ExPolygons rib_center_areas;
            if (result.first_body_z == 0) {
                result.first_body_z = layer->print_z;
                result.first_body_paths = layer->support_fills.entities.size();
            }
            if (distance > 0.4)
                result.min_body_paths = std::min(result.min_body_paths, layer->support_fills.entities.size());
            for (const ExtrusionEntity *entity : layer->support_fills.entities) {
                const auto *path = dynamic_cast<const ExtrusionPath *>(entity);
                REQUIRE(path != nullptr);
                result.closed_paths += (path->first_point() - path->last_point()).squaredNorm() == 0;
                result.open_paths += (path->first_point() - path->last_point()).squaredNorm() != 0;
                result.contact_starts += unscale<double>(path->first_point().x() - face_x) < rib_half_width + 0.01;
                const Polyline ring_path = path->polyline.to_polyline();
                const Points &ring = ring_path.points;
                if (measure_projection_bounds)
                    projection_rings.push_back(ring);
                if (wall_count <= 1 && ring.size() >= 4 && ring.front() == ring.back()) {
                    Points contour = ring;
                    contour.pop_back();
                    Polygon polygon(std::move(contour));
                    polygon.make_counter_clockwise();
                    const ExPolygon center_area(std::move(polygon));
                    for (const ExPolygon &other : rib_center_areas) {
                        const ExPolygons overlap = intersection_ex(ExPolygons{center_area}, ExPolygons{other});
                        for (const ExPolygon &area : overlap)
                            result.max_rib_center_overlap = std::max(result.max_rib_center_overlap,
                                area.area() * SCALING_FACTOR * SCALING_FACTOR);
                    }
                    rib_center_areas.push_back(center_area);
                }
                const BoundingBox bounds = get_extents(ring);
                Vec2d longest_edge = Vec2d::Zero();
                for (size_t i = 1; i < ring.size(); ++i) {
                    const Vec2d edge = unscale(ring[i] - ring[i - 1]);
                    if (edge.squaredNorm() > longest_edge.squaredNorm())
                        longest_edge = edge;
                }
                if (longest_edge.norm() > 0 && std::abs(longest_edge.normalized().x()) > 0.95) {
                    const double offset = unscale<double>(bounds.center().x() - model_bounds.center().x());
                    result.positive_x_sheet |= offset > 1.;
                    result.negative_x_sheet |= offset < -1.;
                }
                if (layer == layers.back()) {
                    ++result.top_body_paths;
                    if (longest_edge.squaredNorm() > 0)
                        result.top_body_directions.push_back(longest_edge.normalized());
                    double front_min_y = std::numeric_limits<double>::infinity();
                    double front_max_y = -std::numeric_limits<double>::infinity();
                    for (const Point &point : ring)
                        if (unscale<double>(point.x() - face_x) < rib_half_width + 0.01) {
                            front_min_y = std::min(front_min_y, unscale<double>(point.y()));
                            front_max_y = std::max(front_max_y, unscale<double>(point.y()));
                        }
                    if (front_max_y >= front_min_y)
                        result.top_contact_y.push_back(0.5 * (front_min_y + front_max_y) -
                            unscale<double>(model_bounds.center().y()));
                }
                const double front_gap = unscale<double>(bounds.min.x() - face_x) - rib_half_width;
                if (front_gap > 0.01 && front_gap < 0.4) {
                    result.min_projection_gap = std::min(result.min_projection_gap, front_gap);
                    double front_min_y = std::numeric_limits<double>::infinity();
                    double front_max_y = -std::numeric_limits<double>::infinity();
                    for (const Point &point : ring)
                        if (point.x() == bounds.min.x()) {
                            front_min_y = std::min(front_min_y, unscale<double>(point.y()));
                            front_max_y = std::max(front_max_y, unscale<double>(point.y()));
                        }
                    ++result.projection_layers;
                    result.projection_front_span = std::max(result.projection_front_span, front_max_y - front_min_y);
                }
                if (front_gap > 0.4 && longest_edge.squaredNorm() > 0) {
                    const Vec2d axis = longest_edge.normalized();
                    const Vec2d side(-axis.y(), axis.x());
                    double minimum = std::numeric_limits<double>::infinity();
                    double maximum = -std::numeric_limits<double>::infinity();
                    for (const Point &point : ring) {
                        const double position = unscale(point).dot(side);
                        minimum = std::min(minimum, position);
                        maximum = std::max(maximum, position);
                    }
                    result.body_thickness = std::max(result.body_thickness, maximum - minimum + 2 * rib_half_width);
                }
                double front_min = std::numeric_limits<double>::infinity();
                double front_max = -std::numeric_limits<double>::infinity();
                for (const Point &point : ring) {
                    if (unscale<double>(point.x() - face_x) - rib_half_width < 0.01) {
                        front_min = std::min(front_min, unscale<double>(point.y()));
                        front_max = std::max(front_max, unscale<double>(point.y()));
                    }
                }
                if (front_max >= front_min) {
                    result.contact_width = std::max(result.contact_width, front_max - front_min);
                }
                if (previous_layer != nullptr) {
                    const ExPolygons supporting_area = offset_ex(previous_layer->support_islands, float(SCALED_EPSILON));
                    for (const Point &point : ring) {
                        if (std::abs(unscale<double>(point.x() - face_x) - rib_half_width) >= 0.01)
                            continue;
                        ++result.contact_tip_vertices;
                        const bool supported = std::any_of(supporting_area.begin(), supporting_area.end(),
                            [&point](const ExPolygon &area) { return area.contains(point); });
                        result.unsupported_contact_vertices += !supported;
                    }
                    for (size_t i = 1; i < ring.size(); ++i) {
                        const double a_gap = unscale<double>(ring[i - 1].x() - face_x) - rib_half_width;
                        const double b_gap = unscale<double>(ring[i].x() - face_x) - rib_half_width;
                        if (std::abs(a_gap) >= 0.01 || std::abs(b_gap) >= 0.01)
                            continue;
                        Polyline front;
                        front.points = {ring[i - 1], ring[i]};
                        ++result.supported_contact_fronts;
                        for (const Polyline &unsupported : diff_pl(Polylines{front}, supporting_area))
                            result.unsupported_contact_length += unscale<double>(unsupported.length());
                    }
                }
            }
            for (const Point &point : points) {
                if (unscale<double>(point.x() - face_x) - rib_half_width < 0.2) {
                    const double clearance = std::min(unscale<double>(point.y() - model_bounds.min.y()),
                                                      unscale<double>(model_bounds.max.y() - point.y()));
                    result.contact_edge_overrun = std::max(result.contact_edge_overrun, rib_half_width - clearance);
                }
            }
            const coord_t outer_x = std::max_element(points.begin(), points.end(),
                [](const Point &a, const Point &b) { return a.x() < b.x(); })->x();
            min_outer = std::min(min_outer, unscale<double>(outer_x));
            max_outer = std::max(max_outer, unscale<double>(outer_x));
            if (layer == layers.back())
                result.top_outer_depth = unscale<double>(outer_x - face_x) + rib_half_width;
        }
        previous_layer = layer;
    }
    result.outer_edge_spread = max_outer - min_outer;
    if (measure_projection_bounds && result.top_body_paths == 1 && result.top_contact_y.size() == 1) {
        Vec2d direction = result.top_body_directions.front();
        if (direction.x() < 0)
            direction = -direction;
        const Vec2d tangent(-direction.y(), direction.x());
        const BoundingBox top_bounds = get_extents(model_layers.back()->lslices);
        const Vec2d seed(unscale<double>(top_bounds.max.x()),
                         unscale<double>(top_bounds.center().y()) + result.top_contact_y.front());
        const double actual_thickness = config.option<ConfigOptionFloatOrPercent>("rib_support_thickness")->get_abs_value(outer_width);
        const double overlap_end = (0.6 + 0.5 * actual_thickness * std::abs(tangent.x())) / direction.x() +
                                   3 * rib_half_width;
        for (const Points &ring : projection_rings)
            for (const Point &point : ring) {
                const Vec2d relative = unscale(point) - seed;
                if (relative.dot(direction) <= overlap_end + 0.01)
                    continue;
                ++result.bounded_projection_vertices;
                result.max_projection_side_overrun = std::max(result.max_projection_side_overrun,
                    std::abs(relative.dot(tangent)) - 0.5 * actual_thickness + rib_half_width);
            }
    }
    return result;
}

TEST_CASE("Rib style defaults to the existing simulated planner", "[SupportMaterial]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(config.opt_enum<RibSupportStyle>("rib_support_style") == rssSimulated);
    config.set_deserialize("rib_support_style", "simple");
    REQUIRE(config.opt_enum<RibSupportStyle>("rib_support_style") == rssSimple);
}

TEST_CASE("Simple ribs require painting and emit closed sheets from selected sites", "[SupportMaterial]")
{
    auto slice_simple = [](bool paint) {
        return slice_ribs(20, paint, 0, 1.2, 0.4, 1, 30,
                          false, false, 0.4, 8, 0, 30, 6, nullptr, 0,
                          false, false, false, false, 0,
                          std::numeric_limits<double>::infinity(), "simple");
    };
    REQUIRE(slice_simple(false).first_layer_paths == 0);
    const RibRun painted = slice_simple(true);
    REQUIRE(painted.first_layer_paths > 0);
    REQUIRE(painted.top_body_paths > 0);
    REQUIRE(painted.contact_layers > 0);
    REQUIRE(painted.open_paths == 0);
}

TEST_CASE("Simple rib contacts fit on the painted island edge", "[SupportMaterial]")
{
    const indexed_triangle_set mesh = its_make_cube(6, 1, 30);
    auto slice_simple = [&](double contact_width) {
        return slice_ribs(20, true, 0, 2, contact_width, 1, 30,
                          false, false, 0.4, 8, 0, 30, 6, &mesh, 0,
                          false, false, false, false, 0,
                          std::numeric_limits<double>::infinity(), "simple");
    };
    REQUIRE(slice_simple(1.2).first_layer_paths == 0);
    REQUIRE(slice_simple(0.2).first_layer_paths > 0);
}

TEST_CASE("Simple ribs reach the highest painted layer on an upward-leaning side", "[SupportMaterial]")
{
    const double height = 20.;
    const RibRun result = slice_ribs(20, true, -0.2, 1.2, 0.2, 1, 30,
                                     false, false, 0.4, 8, 0, height, 6, nullptr, 0,
                                     false, false, false, false, 0,
                                     std::numeric_limits<double>::infinity(), "simple");
    REQUIRE_THAT(result.support_top_z, Catch::Matchers::WithinAbs(height, 0.001));
}

TEST_CASE("Simple ribs start in the top layer when only that layer is painted", "[SupportMaterial]")
{
    indexed_triangle_set mesh;
    for (float z : {0.f, 19.8f, 19.98f, 20.f}) {
        mesh.vertices.emplace_back(6.f, 60.f, z);
        mesh.vertices.emplace_back(6.f, 0.f, z);
        mesh.vertices.emplace_back(0.f, 0.f, z);
        mesh.vertices.emplace_back(0.f, 60.f, z);
    }
    mesh.indices.emplace_back(0, 1, 2);
    mesh.indices.emplace_back(0, 2, 3);
    mesh.indices.emplace_back(12, 14, 13);
    mesh.indices.emplace_back(12, 15, 14);
    for (int band = 0; band < 3; ++band)
        for (int side = 0; side < 4; ++side) {
            const int a = 4 * band + side;
            const int b = 4 * band + (side + 1) % 4;
            mesh.indices.emplace_back(a, b + 4, b);
            mesh.indices.emplace_back(a, a + 4, b + 4);
        }

    const RibRun result = slice_ribs(20, true, 0, 1.2, 0.4, 1, 30,
                                      false, false, 0.4, 8, 0, 20, 6, &mesh, 0,
                                      false, false, false, false, 0,
                                      19.98, "simple", 19.8);
    REQUIRE(result.first_layer_paths > 0);
    REQUIRE_THAT(result.support_top_z, Catch::Matchers::WithinAbs(20., 0.001));
    REQUIRE(result.contact_layers == 1);
    REQUIRE(result.projection_layers > 0);
}

TEST_CASE("Simple ribs unite overlapping first-layer feet", "[SupportMaterial]")
{
    indexed_triangle_set mesh = its_make_cube(6, 1.2, 30);
    indexed_triangle_set other = its_make_cube(6, 1.2, 30);
    its_transform(other, identity3f().translate(Vec3f{0.f, 1.5f, 0.f}));
    its_merge(mesh, other);
    const RibRun result = slice_ribs(20, true, 0, 1.2, 0.4, 1, 30,
                                      false, false, 0.4, 8, 0, 30, 6, &mesh, 0,
                                      false, false, false, false, 0,
                                      std::numeric_limits<double>::infinity(), "simple");
    REQUIRE(result.top_body_paths >= 2);
    REQUIRE(result.first_layer_closed_paths == 1);
}

TEST_CASE("Simple rib feet use first-layer expansion and contacts follow their projection angle", "[SupportMaterial]")
{
    auto slice_simple = [](double expansion) {
        return slice_ribs(20, true, 0, 1.2, 0.4, 1, 60,
                          false, false, 0.4, 8, 0, 30, 6, nullptr, 0,
                          false, false, false, false, 0,
                          std::numeric_limits<double>::infinity(), "simple",
                          -std::numeric_limits<double>::infinity(), 0.6, expansion);
    };
    const RibRun compact = slice_simple(0.);
    const RibRun expanded = slice_simple(2.);
    REQUIRE(expanded.first_layer_paths > 0);
    REQUIRE(expanded.foot_long_span > compact.foot_long_span + 1.8);
    REQUIRE(expanded.projection_layers > 0);
    REQUIRE_THAT(expanded.min_projection_gap, Catch::Matchers::WithinAbs(0.2 / std::tan(M_PI / 3), 0.005));
}

TEST_CASE("Simple rib unavailable ranges stay on their island contour", "[SupportMaterial]")
{
    indexed_triangle_set mesh = its_make_cube(6, 6, 30);
    indexed_triangle_set other = its_make_cube(6, 6, 30);
    its_transform(other, identity3f().translate(Vec3f{0.f, 10.f, 0.f}));
    its_merge(mesh, other);
    const RibRun result = slice_ribs(20, true, 0, 1.2, 0.4, 1, 30,
                                     false, false, 0.4, 8, 0, 30, 6, &mesh, 0,
                                     false, false, false, false, 0,
                                     std::numeric_limits<double>::infinity(), "simple");
    REQUIRE(result.top_body_paths >= 2);
}

TEST_CASE("Rib support does not create contacts without painted sides", "[SupportMaterial]")
{
    REQUIRE(slice_ribs(20, false).first_layer_paths == 0);
}

TEST_CASE("Rib count follows directional demand rather than uniform face spacing", "[SupportMaterial]")
{
    const RibRun first = slice_ribs(30);
    const RibRun second = slice_ribs(10);
    REQUIRE(first.top_body_paths > 0);
    REQUIRE(first.top_body_paths <= 8);
    REQUIRE(second.top_body_paths == first.top_body_paths);
    const indexed_triangle_set wide = its_make_cube(6, 120, 60);
    const RibRun large_face = slice_ribs(30, true, 0, 1.2, 1.2, 1, 30,
                                      false, false, 0.4, 8, 0, 60, 6, &wide);
    REQUIRE(large_face.top_body_paths == first.top_body_paths);
}

TEST_CASE("The first rib attaches near the middle of a large painted plane", "[SupportMaterial]")
{
    const RibRun result = slice_ribs(20);
    REQUIRE(!result.top_contact_y.empty());
    REQUIRE_THAT(result.top_contact_y.front(), Catch::Matchers::WithinAbs(0, 0.05));
}

TEST_CASE("Half painting a leaning plane does not rotate its ribs", "[SupportMaterial]")
{
    const bool half_painted = GENERATE(false, true);
    const RibRun result = slice_ribs(20, true, 0.2, 1.2, 0.2, 1, 30,
                                  false, false, 0.4, 8, 0, 60, 6, nullptr, 0,
                                  false, false, half_painted);
    REQUIRE(!result.top_body_directions.empty());
    for (const Vec2d &direction : result.top_body_directions)
        REQUIRE(std::abs(direction.y()) < 0.002);
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("A full painted side starts the rib at its highest printable layer", "[SupportMaterial]")
{
    const double height = 60.;
    const RibRun result = slice_ribs(20, true, 0.2, 1.2, 0.2, 1, 30,
                                  false, false, 0.4, 8, 0, height);
    REQUIRE(result.top_body_paths > 0);
    REQUIRE_THAT(result.contact_top_z, Catch::Matchers::WithinAbs(height, 0.001));
}

TEST_CASE("Rib directions follow a continuous lean instead of angular bins", "[SupportMaterial]")
{
    const double angle = GENERATE(7., 17., 26.);
    const double radians = angle * M_PI / 180.;
    const RibRun result = slice_ribs(20, true, 0.2, 1.2, 0.2, 1, 30,
                                  false, false, 0.4, 8, 0, 60, 6, nullptr, 0.2 * std::tan(radians));
    REQUIRE(!result.top_body_directions.empty());
    const Vec2d expected(std::cos(radians), std::sin(radians));
    REQUIRE(std::abs(result.top_body_directions.front().dot(expected)) > 0.9999);
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("An unavailable lean direction does not hide other feasible support", "[SupportMaterial]")
{
    const double angle = GENERATE(45., 61., 75., 90.);
    const double radians = angle * M_PI / 180.;
    const RibRun result = slice_ribs(20, true, 0.2 * std::cos(radians), 1.2, 0.2, 1, 30,
                                  false, false, 0.4, 8, 0, 60, 6, nullptr, 0.2 * std::sin(radians));
    REQUIRE(result.top_body_paths > 0);
    // The unavailable lean is not rotated to fit the face. The independently
    // feasible +X bed demand still supplies a sheet after the lean is blocked.
    for (const Vec2d &direction : result.top_body_directions)
        REQUIRE(std::abs(direction.y()) < 0.002);
}

TEST_CASE("A cube resting on an edge receives ribs on both sides of its narrow base", "[SupportMaterial]")
{
    const double side = 40.;
    indexed_triangle_set mesh = its_make_cube(side, side, side);
    const double c = std::sqrt(0.5);
    for (Vec3f &vertex : mesh.vertices) {
        const double x = vertex.x() - side / 2;
        const double z = vertex.z() - side / 2;
        vertex.x() = float(c * (x + z));
        vertex.z() = float(c * (z - x) + side * c);
    }
    const RibRun result = slice_ribs(20, true, 0, 1.2, 0.2, 1, 30,
                                  false, false, 0.4, 8, 0, side * std::sqrt(2.), side,
                                  &mesh, 0, true);
    REQUIRE(result.positive_x_sheet);
    REQUIRE(result.negative_x_sheet);
    REQUIRE(result.max_rib_center_overlap < 1e-6);
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("Centered upper expansion supplies a direction beyond the bed axes", "[SupportMaterial]")
{
    const double degrees = GENERATE(25., 55.);
    const double angle = degrees * M_PI / 180.;
    const double c = std::cos(angle), sn = std::sin(angle);
    indexed_triangle_set mesh;
    const std::array<Vec2d, 4> corners{
        Vec2d(-1, -1), Vec2d(1, -1), Vec2d(1, 1), Vec2d(-1, 1)};
    for (const Vec2d &corner : corners)
        mesh.vertices.emplace_back(float(7.5 * corner.x()), float(7.5 * corner.y()), 0.f);
    for (const Vec2d &corner : corners) {
        const double x = 30 * corner.x(), y = 4 * corner.y();
        mesh.vertices.emplace_back(float(c * x - sn * y), float(sn * x + c * y), 60.f);
    }
    for (int i = 0; i < 4; ++i) {
        const int next = (i + 1) % 4;
        mesh.indices.emplace_back(i, next, next + 4);
        mesh.indices.emplace_back(i, next + 4, i + 4);
    }
    mesh.indices.emplace_back(0, 2, 1);
    mesh.indices.emplace_back(0, 3, 2);
    mesh.indices.emplace_back(4, 5, 6);
    mesh.indices.emplace_back(4, 6, 7);
    // Opposite vertices preserve the same XY centroid at every height.
    // The square bed has no preference for the rotated upper long axis.
    const RibRun result = slice_ribs(20, true, 0, 1.2, 0.2, 1, 30,
                                  false, false, 0.4, 8, 0, 60, 15, &mesh, 0, true);
    REQUIRE(!result.top_body_directions.empty());
    const Vec2d expansion(c, sn);
    REQUIRE(std::abs(result.top_body_directions.front().dot(expansion)) > std::cos(M_PI / 18.));
    REQUIRE(result.max_rib_center_overlap < 1e-6);
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("Ribs stay within thirty degrees of the only available painted face normal", "[SupportMaterial]")
{
    const RibRun result = slice_ribs(20, true, 0.2, 1.2, 1.2, 1, 30,
                                  false, false, 0.4, 8, 0, 60, 6, nullptr, 0.2 * std::tan(M_PI / 9.));
    REQUIRE(!result.top_body_directions.empty());
    const double minimum_alignment = std::cos(M_PI / 6.);
    for (const Vec2d &direction : result.top_body_directions)
        REQUIRE(std::abs(direction.x()) >= minimum_alignment - 0.002);
    // Oblique attachment remains allowed; a tangent Y sheet is not a fallback.
    REQUIRE(std::abs(result.top_body_directions.front().y()) > 0.1);
    REQUIRE(result.contact_layers > 6);
    REQUIRE(result.contact_width > 1.1);
    REQUIRE(result.open_paths == 0);
    REQUIRE(result.contact_starts == 0);
    REQUIRE(result.max_rib_center_overlap < 1e-6);
}

TEST_CASE("Ribs at different seed heights do not emit overlapping sheet contours", "[SupportMaterial]")
{
    const double lean = GENERATE(0., 0.2);
    const RibRun result = slice_ribs(20, true, lean, 1.2, 0.4, 1, 30,
                                  false, false, 0.4, 8, 0.05);
    REQUIRE(result.closed_paths > 0);
    REQUIRE(result.max_rib_center_overlap < 1e-6);
    REQUIRE(result.contact_layers > 6);
}

TEST_CASE("Oblique contact projections stay inside the sheet after the neck overlap", "[SupportMaterial]")
{
    const double angle = GENERATE(30., 60., 80.);
    const double contact_length = GENERATE(0., 0.2);
    const RibRun result = slice_ribs(20, true, 0.2, 2.4, contact_length, 1, angle,
                                  false, false, 0.4, 8, 0, 60, 6, nullptr, 0.2 * std::tan(M_PI / 9.), false, true);
    REQUIRE(result.top_body_paths == 1);
    REQUIRE(std::abs(result.top_body_directions.front().y()) > 0.1);
    REQUIRE(result.projection_layers > 0);
    REQUIRE(result.bounded_projection_vertices > 0);
    REQUIRE(result.max_projection_side_overrun < 0.002);
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("A new demand direction selects a differently facing painted side", "[SupportMaterial]")
{
    const RibRun result = slice_ribs(20, true, 0, 1.2, 1.2, 1, 30,
                                  false, false, 0.4, 8, 0, 60, 6, nullptr, 0.4, true);
    REQUIRE(!result.top_body_directions.empty());
    // With a +Y-facing side available, the first sheet follows +Y instead of
    // attaching tangentially to the large +X-facing side.
    REQUIRE(std::abs(result.top_body_directions.front().y()) > 0.95);
    REQUIRE(std::any_of(result.top_body_directions.begin(), result.top_body_directions.end(),
        [](const Vec2d &direction) { return std::abs(direction.x()) > 0.95; }));
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("Thicker ribs reduce remaining directional demand without creating more sheets", "[SupportMaterial]")
{
    const RibRun thin = slice_ribs(20, true, 0, 1.2, 0.2);
    const RibRun thick = slice_ribs(20, true, 0, 2.4, 0.2);
    REQUIRE(thin.top_body_paths > 0);
    REQUIRE(thick.top_body_paths > 0);
    REQUIRE(thick.top_body_paths <= thin.top_body_paths);
}

TEST_CASE("A centered column with painted sides receives ribs in multiple broad directions", "[SupportMaterial]")
{
    const RibRun result = slice_ribs(20, true, 0, 1.2, 0.2, 1, 30,
                                  false, false, 0.4, 8, 0, 60, 6, nullptr, 0, true);
    REQUIRE(result.top_body_paths > 1);
    REQUIRE(result.top_body_paths <= 8);
    REQUIRE(std::any_of(result.top_body_directions.begin(), result.top_body_directions.end(),
        [](const Vec2d &direction) { return std::abs(direction.x()) > 0.95; }));
    REQUIRE(std::any_of(result.top_body_directions.begin(), result.top_body_directions.end(),
        [](const Vec2d &direction) { return std::abs(direction.y()) > 0.95; }));
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("A centered narrow column can start several ribs on one layer", "[SupportMaterial]")
{
    const indexed_triangle_set column = its_make_cube(6, 6, 60);
    const RibRun result = slice_ribs(20, true, 0, 1.2, 0.2, 1, 30,
                                  false, false, 0.4, 8, 0, 60, 6, &column, 0, true);
    REQUIRE(result.first_body_z > 0);
    REQUIRE(result.first_body_paths >= 2);
    REQUIRE(result.max_rib_center_overlap < 1e-6);
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("Rib support alternates painted side contacts with side gaps", "[SupportMaterial]")
{
    const RibRun result = slice_ribs(20);
    REQUIRE(result.contact_layers > 0);
    REQUIRE(result.gap_layers > 0);
}

TEST_CASE("Rib support has a filled first-layer foot", "[SupportMaterial]")
{
    const RibRun result = slice_ribs(100);
    REQUIRE(result.foot_short_span > 3.0);
    REQUIRE(result.foot_long_span > 7.0);
}

TEST_CASE("Rib feet share one perimeter per merged bed island", "[SupportMaterial]")
{
    const double spacing = GENERATE(2.0, 20.0);
    CAPTURE(spacing);
    const RibRun result = slice_ribs(spacing, true, 0, 1.2, 1.2, 1);
    REQUIRE(result.first_layer_islands > 0);
    REQUIRE(result.min_body_paths > 0);
    REQUIRE(result.first_layer_closed_paths == result.first_layer_islands);
    REQUIRE(result.first_layer_paths > result.first_layer_closed_paths);
}

TEST_CASE("Rib outer edge stays vertical while contacts fit a leaning wall", "[SupportMaterial]")
{
    const RibRun result = slice_ribs(20, true, 0.2);
    REQUIRE(result.contact_layers > 0);
    REQUIRE(result.gap_layers > 0);
    REQUIRE(result.outer_edge_spread < 0.001);
}

TEST_CASE("Rib expansion widens lower layers and their foot without changing top depth", "[SupportMaterial]")
{
    const double factor = GENERATE(0.025, 0.05);
    const RibRun vertical = slice_ribs(20);
    const RibRun expanded = slice_ribs(20, true, 0, 1.2, 1.2, 1, 30, false, false, 0.4, 8, factor);
    REQUIRE(expanded.contact_layers > 0);
    REQUIRE_THAT(expanded.top_outer_depth, Catch::Matchers::WithinAbs(8, 0.001));
    // Constant 6x60mm cross-section: no XY centroid offset, so tan(theta) = k*H/sqrt(A0).
    const double slope = factor * expanded.support_top_z / std::sqrt(6. * 60.);
    REQUIRE_THAT(expanded.outer_edge_spread,
                 Catch::Matchers::WithinAbs(slope * (expanded.support_top_z - expanded.first_body_z), 0.002));
    REQUIRE(expanded.foot_long_span > vertical.foot_long_span);
    REQUIRE(expanded.open_paths == 0);
    REQUIRE(expanded.contact_starts == 0);
}

TEST_CASE("Rib expansion increases with height and smaller base area", "[SupportMaterial]")
{
    auto slice = [](double height, double width) {
        return slice_ribs(20, true, 0, 1.2, 1.2, 1, 30, false, false, 0.4, 8, 0.05, height, width);
    };
    const RibRun reference = slice(60, 6);
    const RibRun taller = slice(80, 6);
    const RibRun narrower = slice(60, 3);
    REQUIRE(reference.contact_layers > 0);
    REQUIRE(taller.contact_layers > 0);
    REQUIRE(narrower.contact_layers > 0);
    REQUIRE(taller.outer_edge_spread > reference.outer_edge_spread);
    REQUIRE(narrower.outer_edge_spread > reference.outer_edge_spread);
}

TEST_CASE("The seed face outward tilt reduces expansion for the whole rib", "[SupportMaterial]")
{
    const RibRun centered = slice_ribs(20, true, 0, 1.2, 1.2, 1, 30, false, false, 0.4, 8, 0.05);
    const RibRun leaning = slice_ribs(20, true, 0.2, 1.2, 1.2, 1, 30, false, false, 0.4, 8, 0.05);
    REQUIRE(centered.contact_layers > 0);
    REQUIRE(leaning.contact_layers > 0);
    REQUIRE_THAT(leaning.support_top_z, Catch::Matchers::WithinAbs(centered.support_top_z, 0.001));
    REQUIRE(leaning.outer_edge_spread > 0);
    REQUIRE(leaning.outer_edge_spread < centered.outer_edge_spread);
    // Uniform sheared prism: XY volume centroid is halfway up the printed body.
    const double tilt_fraction = std::atan(0.2) / (M_PI / 6.);
    const double weight = (1 - tilt_fraction) * (1 - tilt_fraction);
    const double base_scale = std::sqrt(6. * 60.);
    const double offset = 0.2 * (leaning.support_top_z / 2 - 0.1);
    const double slope = 0.05 * leaning.support_top_z / base_scale * (1 + offset / base_scale) * weight;
    REQUIRE_THAT(leaning.outer_edge_spread,
                 Catch::Matchers::WithinAbs(slope * (leaning.support_top_z - leaning.first_body_z), 0.002));
}

TEST_CASE("Outward tilt of thirty degrees or more leaves the rib outer edge vertical", "[SupportMaterial]")
{
    const double tilt = GENERATE(30., 40.);
    const double lean = std::tan(tilt * M_PI / 180.);
    const RibRun result = slice_ribs(20, true, lean, 1.2, 1.2, 1, 30, false, false, 0.4, 8, 0.05);
    REQUIRE(result.contact_layers > 0);
    REQUIRE_THAT(result.outer_edge_spread, Catch::Matchers::WithinAbs(0, 0.001));
    REQUIRE(result.open_paths == 0);
    REQUIRE(result.contact_starts == 0);
}

TEST_CASE("Painted patches containing only upward facing normals do not create ribs", "[SupportMaterial]")
{
    const RibRun result = slice_ribs(20, true, -0.2, 1.2, 1.2, 1, 30, false, false, 0.4, 8, 0.05);
    REQUIRE(result.first_layer_paths == 0);
    REQUIRE(result.contact_layers == 0);
}

TEST_CASE("An L shaped profile retains lower contacts and adds upper contacts", "[SupportMaterial]")
{
    indexed_triangle_set mesh;
    const std::array<float, 4> heights{0.f, 29.9f, 30.1f, 60.f};
    for (float z : heights) {
        const float reach = z < 30.f ? 12.f : 60.f;
        mesh.vertices.emplace_back(0.f, 0.f, z);
        mesh.vertices.emplace_back(6.f, 0.f, z);
        mesh.vertices.emplace_back(6.f, reach, z);
        mesh.vertices.emplace_back(0.f, reach, z);
    }
    for (int ring = 0; ring < 3; ++ring)
        for (int i = 0; i < 4; ++i) {
            const int a = 4 * ring + i, b = 4 * ring + (i + 1) % 4;
            mesh.indices.emplace_back(a, b, a + 4);
            mesh.indices.emplace_back(b, b + 4, a + 4);
        }
    mesh.indices.emplace_back(0, 2, 1);
    mesh.indices.emplace_back(0, 3, 2);
    mesh.indices.emplace_back(12, 13, 14);
    mesh.indices.emplace_back(12, 14, 15);
    const RibRun result = slice_ribs(20, true, 0, 1.2, 0.2, 1, 30,
                                  false, false, 0.4, 8, 0, 60, 6, &mesh, 0, true);
    REQUIRE(result.lower_y_contact);
    REQUIRE(result.upper_y_contact);
    REQUIRE(result.max_rib_center_overlap < 1e-6);
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("An active rib stops at the last painted height", "[SupportMaterial]")
{
    indexed_triangle_set mesh;
    for (float z : {0.f, 30.f, 60.f}) {
        mesh.vertices.emplace_back(0.f, 0.f, z);
        mesh.vertices.emplace_back(6.f, 0.f, z);
        mesh.vertices.emplace_back(6.f, 60.f, z);
        mesh.vertices.emplace_back(0.f, 60.f, z);
    }
    for (int ring = 0; ring < 2; ++ring)
        for (int i = 0; i < 4; ++i) {
            const int a = 4 * ring + i, b = 4 * ring + (i + 1) % 4;
            mesh.indices.emplace_back(a, b, a + 4);
            mesh.indices.emplace_back(b, b + 4, a + 4);
        }
    mesh.indices.emplace_back(0, 2, 1);
    mesh.indices.emplace_back(0, 3, 2);
    mesh.indices.emplace_back(8, 9, 10);
    mesh.indices.emplace_back(8, 10, 11);
    const RibRun result = slice_ribs(20, true, 0, 1.2, 0.2, 1, 30,
                                  false, false, 0.4, 8, 0, 60, 6, &mesh, 0,
                                  false, false, false, false, 0, 30);
    REQUIRE(result.first_layer_paths > 0);
    REQUIRE(result.contact_top_z > 20);
    REQUIRE(result.contact_top_z <= 30.001);
    REQUIRE(result.support_top_z <= 30.001);
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("Rib contact layers stay below the painted face top", "[SupportMaterial]")
{
    const double height = GENERATE(60., 60.1, 60.15);
    const double lean = GENERATE(0., 0.6);
    const double slice_offset = GENERATE(-1., 0., 0.05);
    const RibRun result = slice_ribs(20, true, lean, 1.2, 1.2, 1, 30,
                                    false, false, 0.4, 8, 0.05, height, 6, nullptr, 0,
                                    false, false, false, slice_offset >= 0, std::max(0., slice_offset));
    REQUIRE(result.contact_layers > 0);
    REQUIRE(result.contact_top_z <= height + 0.001);
    REQUIRE(result.support_top_z <= height + 0.001);
    if (lean > 0)
        REQUIRE_THAT(result.outer_edge_spread, Catch::Matchers::WithinAbs(0, 0.001));
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("Curved seed patches allow different normals across width and layer height", "[SupportMaterial]")
{
    // A faceted round column bends outward in its last 0.1mm. The last
    // 0.2mm contact layer spans both vertical and leaning side facets.
    indexed_triangle_set mesh;
    constexpr int sides = 90;
    const std::array<double, 3> zs{0., 59.9, 60.};
    for (double z : zs)
        for (int i = 0; i < sides; ++i) {
            const double angle = 2 * M_PI * i / sides;
            mesh.vertices.emplace_back(float(6 * std::cos(angle) + std::max(0., z - 59.9) * 0.2),
                                       float(6 * std::sin(angle)), float(z));
        }
    for (int ring = 0; ring < 2; ++ring)
        for (int i = 0; i < sides; ++i) {
            const int a = ring * sides + i, b = ring * sides + (i + 1) % sides;
            mesh.indices.emplace_back(a, b, a + sides);
            mesh.indices.emplace_back(b, b + sides, a + sides);
        }
    const int bottom = int(mesh.vertices.size());
    mesh.vertices.emplace_back(0.f, 0.f, 0.f);
    const int top = int(mesh.vertices.size());
    mesh.vertices.emplace_back(0.02f, 0.f, 60.f);
    for (int i = 0; i < sides; ++i) {
        mesh.indices.emplace_back(bottom, (i + 1) % sides, i);
        mesh.indices.emplace_back(top, 2 * sides + i, 2 * sides + (i + 1) % sides);
    }
    const RibRun result = slice_ribs(20, true, 0, 1.2, 1.2, 1, 30,
                                    false, false, 0.4, 8, 0, 60, 6, &mesh);
    REQUIRE(result.first_layer_paths > 0);
    REQUIRE_THAT(result.support_top_z, Catch::Matchers::WithinAbs(60, 0.001));
    REQUIRE(result.closed_paths > 0);
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("An upward facing neighbor does not change a downward facing contact normal", "[SupportMaterial]")
{
    auto column = [](double top_slope) {
        indexed_triangle_set mesh;
        const std::array<double, 3> zs{0., 59.9, 60.};
        for (double z : zs) {
            const double x = z <= 59.9 ? 0.2 * z : 0.2 * 59.9 + top_slope * (z - 59.9);
            mesh.vertices.emplace_back(float(x), 0.f, float(z));
            mesh.vertices.emplace_back(float(x + 6), 0.f, float(z));
            mesh.vertices.emplace_back(float(x + 6), 60.f, float(z));
            mesh.vertices.emplace_back(float(x), 60.f, float(z));
        }
        for (int ring = 0; ring < 2; ++ring)
            for (int i = 0; i < 4; ++i) {
                const int a = ring * 4 + i, b = ring * 4 + (i + 1) % 4;
                mesh.indices.emplace_back(a, b, a + 4);
                mesh.indices.emplace_back(b, b + 4, a + 4);
            }
        mesh.indices.emplace_back(0, 2, 1);
        mesh.indices.emplace_back(0, 3, 2);
        mesh.indices.emplace_back(8, 9, 10);
        mesh.indices.emplace_back(8, 10, 11);
        return mesh;
    };
    const indexed_triangle_set straight = column(0.2);
    const indexed_triangle_set creased = column(-0.8);
    // The last layer's top section is upward-facing, while the middle and
    // bottom lie on the original downward-facing wall. Only its normal applies.
    auto slice = [](const indexed_triangle_set &mesh) {
        return slice_ribs(20, true, 0, 1.2, 1.2, 1, 30, false, false, 0.4, 8, 0.05, 60, 6, &mesh);
    };
    const RibRun reference = slice(straight);
    const RibRun result = slice(creased);
    REQUIRE(reference.contact_layers > 0);
    REQUIRE(result.contact_layers > 0);
    REQUIRE_THAT(result.support_top_z, Catch::Matchers::WithinAbs(60, 0.001));
    REQUIRE_THAT(result.outer_edge_spread, Catch::Matchers::WithinAbs(reference.outer_edge_spread, 0.002));
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("Rib contact bead stays inside the wall edges", "[SupportMaterial]")
{
    const RibRun result = slice_ribs(20);
    REQUIRE(result.contact_layers > 0);
    REQUIRE(result.contact_edge_overrun < 0.0001);
}

TEST_CASE("Rib layers print closed outlines starting away from contacts", "[SupportMaterial]")
{
    const RibRun result = slice_ribs(20);
    REQUIRE(result.closed_paths > 0);
    REQUIRE(result.open_paths == 0);
    REQUIRE(result.contact_starts == 0);
}

TEST_CASE("Rib geometry uses separate body thickness and contact width", "[SupportMaterial]")
{
    const RibRun result = slice_ribs(20, true, 0, 2.4, 1.2);
    REQUIRE_THAT(result.body_thickness, Catch::Matchers::WithinAbs(2.4, 0.02));
    REQUIRE_THAT(result.contact_width, Catch::Matchers::WithinAbs(1.2, 0.02));
}

TEST_CASE("Rib wall loops use the requested count with a minimum of one", "[SupportMaterial]")
{
    const size_t one = slice_ribs(20, true, 0, 3.2, 3.2, 1).min_body_paths;
    REQUIRE(one > 0);
    REQUIRE(one != std::numeric_limits<size_t>::max());
    REQUIRE(slice_ribs(20, true, 0, 3.2, 3.2, 0).min_body_paths == one);
    REQUIRE(slice_ribs(20, true, 0, 3.2, 3.2, 2).min_body_paths == 2 * one);
    REQUIRE(slice_ribs(20, true, 0, 3.2, 3.2, 3).min_body_paths == 3 * one);
}

TEST_CASE("Thin ribs retain the loops that fit and omit the rest", "[SupportMaterial]")
{
    const RibRun one = slice_ribs(20, true, 0, 1.2, 1.2, 1);
    const RibRun three = slice_ribs(20, true, 0, 1.2, 1.2, 3);
    REQUIRE(one.min_body_paths > 0);
    REQUIRE(one.min_body_paths != std::numeric_limits<size_t>::max());
    REQUIRE(three.min_body_paths == one.min_body_paths);
    REQUIRE(three.contact_layers == one.contact_layers);
}

TEST_CASE("Rib contact front paths overlap the extrusion on the layer below", "[SupportMaterial]")
{
    const double angle = GENERATE(60., 80., 90.);
    const double contact_width = GENERATE(0., 0.1, 0.2, 1.2);
    const RibRun result = slice_ribs(20, true, 0, 1.2, contact_width, 1, angle);
    REQUIRE(result.contact_tip_vertices > 0);
    REQUIRE(result.unsupported_contact_vertices == 0);
    REQUIRE(result.unsupported_contact_length < 0.001);
    if (contact_width > 0)
        REQUIRE(result.supported_contact_fronts > 0);
    REQUIRE(result.open_paths == 0);
    REQUIRE(result.contact_starts == 0);
}

TEST_CASE("Rib contact spans preserve positive widths below one extrusion line", "[SupportMaterial]")
{
    const double contact_width = GENERATE(0., 0.05, 0.1, 0.2, 0.4, 0.8);
    const RibRun result = slice_ribs(20, true, 0, 2.4, contact_width, 3);
    REQUIRE(result.contact_layers > 0);
    REQUIRE(result.contact_tip_vertices > 0);
    REQUIRE_THAT(result.contact_width, Catch::Matchers::WithinAbs(contact_width, 0.001));
    REQUIRE(result.contact_edge_overrun < 0.0001);
    REQUIRE(result.open_paths == 0);
    REQUIRE(result.contact_starts == 0);
    REQUIRE_THAT(result.body_thickness, Catch::Matchers::WithinAbs(2.4, 0.02));
}

TEST_CASE("Rib contact projection angle controls how quickly the side gap returns", "[SupportMaterial]")
{
    const RibRun normal = slice_ribs(20, true, 0, 1.2, 1.2, 1, 0);
    const RibRun downward = slice_ribs(20, true, 0, 1.2, 1.2, 1, 80);
    REQUIRE(normal.contact_layers > 0);
    REQUIRE(downward.contact_layers == normal.contact_layers);
    REQUIRE(normal.gap_layers > downward.gap_layers);
    REQUIRE(normal.gap_layers > 0);
    REQUIRE(normal.projection_layers == 0);
    REQUIRE(normal.unsupported_contact_vertices > 0);
    REQUIRE(normal.open_paths == 0);
}

TEST_CASE("Rib projections use the requested angle without a line width retreat cap", "[SupportMaterial]")
{
    // With 0.2mm layers and 0.4mm lines, a 30-degree projection advances
    // 0.2 * cot(30) = 0.3464mm, beyond the former half-line (0.2mm) cap.
    const RibRun result = slice_ribs(20, true, 0, 2.4, 0.1, 1, 30);
    REQUIRE(result.contact_layers > 0);
    REQUIRE_THAT(result.min_projection_gap,
                 Catch::Matchers::WithinAbs(0.2 / std::tan(M_PI / 6), 0.001));
    REQUIRE(result.unsupported_contact_vertices > 0);
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("Surface normal contact projections preserve the selected rib height", "[SupportMaterial]")
{
    const double lean = GENERATE(0., 0.2);
    const double angle = GENERATE(0., 0.001, 0.01, 0.1, 1., 79., 80., 90.);
    const RibRun reference = slice_ribs(20, true, lean, 2.4, 0.1, 1, 30);
    const RibRun result = slice_ribs(20, true, lean, 2.4, 0.1, 1, angle);
    REQUIRE(reference.contact_layers > 0);
    REQUIRE(result.contact_layers > 0);
    REQUIRE_THAT(result.support_top_z, Catch::Matchers::WithinAbs(reference.support_top_z, 0.001));
    REQUIRE_THAT(result.contact_top_z, Catch::Matchers::WithinAbs(reference.contact_top_z, 0.001));
    REQUIRE(result.outer_edge_spread < 0.001);
}

TEST_CASE("Contact projections merge into the body without lowering the rib", "[SupportMaterial]")
{
    // A 0.6mm side gap retracts over about 2mm at this angle. Perturb the
    // layer alignment to cover very short residual sections at the merge.
    const double angle = GENERATE(73.29, 73.30, 73.31);
    const RibRun reference = slice_ribs(20, true, 0, 2.4, 0.1, 1, 30);
    const RibRun result = slice_ribs(20, true, 0, 2.4, 0.1, 1, angle);
    REQUIRE(reference.contact_layers > 0);
    REQUIRE(result.contact_layers == reference.contact_layers);
    REQUIRE(result.gap_layers > 0);
    REQUIRE_THAT(result.support_top_z, Catch::Matchers::WithinAbs(reference.support_top_z, 0.001));
    REQUIRE(result.unsupported_contact_vertices == 0);
    REQUIRE(result.open_paths == 0);
}

TEST_CASE("Overlapping rib contact projections remain supported on all bands", "[SupportMaterial]")
{
    // At 80 degrees from a vertical wall's normal, retracting the 0.6mm
    // gap needs about 3.4mm, exceeding these contact band spacings.
    const double spacing = GENERATE(2., 3.);
    const RibRun result = slice_ribs(20, true, 0, 2.4, 0.1, 3, 80, false, false, 0.4, spacing);
    REQUIRE(result.contact_layers > 3);
    REQUIRE(result.supported_contact_fronts > 0);
    REQUIRE(result.unsupported_contact_vertices == 0);
    REQUIRE(result.unsupported_contact_length < 0.001);
    REQUIRE(result.open_paths == 0);
    REQUIRE(result.contact_starts == 0);
    REQUIRE(result.outer_edge_spread < 0.001);
}

TEST_CASE("Rib percentages resolve against the outer wall width", "[SupportMaterial]")
{
    // 50% of 0.6mm is 0.3mm; 200% is 1.2mm. Support lines remain 0.4mm.
    const RibRun result = slice_ribs(20, true, 0, 200, 50, 1, 30, true, true, 0.6);
    REQUIRE(result.contact_layers > 0);
    REQUIRE_THAT(result.contact_width, Catch::Matchers::WithinAbs(0.3, 0.001));
    REQUIRE_THAT(result.body_thickness, Catch::Matchers::WithinAbs(1.2, 0.02));
}

TEST_CASE("Rib defaults use full outer wall contact length and absolute thickness", "[SupportMaterial]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    const auto *contact = config.option<ConfigOptionFloatOrPercent>("rib_support_contact_width");
    const auto *thickness = config.option<ConfigOptionFloatOrPercent>("rib_support_thickness");
    REQUIRE(contact->percent);
    REQUIRE_FALSE(thickness->percent);
    REQUIRE_THAT(contact->value, Catch::Matchers::WithinAbs(100, 0.001));
    REQUIRE_THAT(thickness->value, Catch::Matchers::WithinAbs(1.2, 0.001));
    const RibRun result = slice_ribs(20, true, 0, 1.2, 100, 1, 30, true, false, 0.6);
    REQUIRE_THAT(result.contact_width, Catch::Matchers::WithinAbs(0.6, 0.001));
    REQUIRE_THAT(result.body_thickness, Catch::Matchers::WithinAbs(1.2, 0.02));
    // Old project values without a percent suffix remain absolute millimeters.
    config.set_deserialize("rib_support_contact_width", "0.2");
    config.set_deserialize("rib_support_thickness", "1.4");
    contact = config.option<ConfigOptionFloatOrPercent>("rib_support_contact_width");
    thickness = config.option<ConfigOptionFloatOrPercent>("rib_support_thickness");
    REQUIRE_FALSE(contact->percent);
    REQUIRE_FALSE(thickness->percent);
    REQUIRE_THAT(contact->get_abs_value(0.6), Catch::Matchers::WithinAbs(0.2, 0.001));
    REQUIRE_THAT(thickness->get_abs_value(0.6), Catch::Matchers::WithinAbs(1.4, 0.001));
}

TEST_CASE("Rib projections retain just the projected front segment or point", "[SupportMaterial]")
{
    const double length = GENERATE(0., 0.1, 0.2);
    const RibRun result = slice_ribs(20, true, 0, 2.4, length, 1, 60);
    REQUIRE(result.projection_layers > 0);
    REQUIRE_THAT(result.projection_front_span, Catch::Matchers::WithinAbs(length, 0.001));
    REQUIRE(result.unsupported_contact_vertices == 0);
    REQUIRE(result.unsupported_contact_length < 0.001);
}

// Distinct layer Z heights carrying support interface extrusion.
static size_t support_interface_layer_count(const std::string &gcode)
{
    return layers_with_role(gcode, "support material interface").size();
}

// Distinct layer Z heights carrying support base extrusion. The base G-code label "support material"
// is a substring of "support material interface", so a base line is a support line that is not an
// interface line.
static size_t support_base_layer_count(const std::string &gcode)
{
    std::set<double> layers;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&layers](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self)) return;
        const std::string_view comment = line.comment();
        if (comment.find("support material") != std::string_view::npos &&
            comment.find("interface") == std::string_view::npos)
            layers.insert(self.z());
    });
    return layers.size();
}

// Dominant support-interface fill direction per interface layer, in radians [0, pi). Uses the
// length-weighted axial mean (each segment angle doubled so a line and its reverse agree, then
// halved): the parallel infill lines reinforce while the surrounding perimeter cancels.
static std::map<double, double> interface_fill_angle_by_layer(const std::string &gcode)
{
    std::map<double, std::pair<double, double>> acc; // z -> summed length*(cos2a, sin2a)
    GCodeReader parser;
    parser.parse_buffer(gcode, [&acc](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self)) return;
        if (line.comment().find("support material interface") == std::string_view::npos) return;
        const double dx = line.dist_X(self), dy = line.dist_Y(self);
        const double len = std::hypot(dx, dy);
        if (len < 1e-6) return;
        const double a2 = 2.0 * std::atan2(dy, dx);
        auto &p = acc[self.z()];
        p.first  += len * std::cos(a2);
        p.second += len * std::sin(a2);
    });
    std::map<double, double> out;
    for (const auto &kv : acc) {
        double a = 0.5 * std::atan2(kv.second.second, kv.second.first);
        if (a < 0) a += M_PI;
        out[kv.first] = a;
    }
    return out;
}

// Acute angle (degrees) between two axial fill directions in [0, pi).
static double axial_angle_diff_deg(double a, double b)
{
    const double d = std::fmod(std::fabs(a - b), M_PI);
    return std::min(d, M_PI - d) * 180.0 / M_PI;
}

// Denser interface spacing yields more extruded length.
static double support_interface_extrusion_length(const std::string &gcode)
{
    double len = 0;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&len](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self)) return;
        if (line.comment().find("support material interface") == std::string_view::npos) return;
        len += std::hypot(line.dist_X(self), line.dist_Y(self));
    });
    return len;
}

// A cap slab overhanging a base, joined by a central stem: the cap can only be supported by resting on the
// base, forcing a genuine bottom contact. A horizontal tunnel does not work here -- tree/organic can arch a
// branch in from the opening and avoid the floor entirely.
static TriangleMesh support_capital()
{
    TriangleMesh model = make_cube(40, 40, 2);                              // base  [0,40]x[0,40]x[0,2]
    TriangleMesh stem  = make_cube(8, 8, 12);   stem.translate(16, 16, 1);  // stem  centered, z 1..13
    TriangleMesh cap   = make_cube(40, 40, 2);  cap.translate(0, 0, 12);    // cap   z 12..14
    model.merge(stem);
    model.merge(cap);
    return model;
}

TEST_CASE("Three raft layers are created", "[SupportMaterial]")
{
	Slic3r::Print print;
	Slic3r::Test::init_and_process_print({ cube(20) }, print, {
        { "enable_support", 1 },
        { "raft_layers",    3 }
		});
    REQUIRE(print.objects().front()->support_layers().size() == 3);
}

TEST_CASE("Enforced support layers are generated", "[SupportMaterial]")
{
    // enforce_support_layers forces support on the first N layers even with support off.
    Slic3r::Print baseline;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, baseline, {
        { "enable_support",         0 },
        { "enforce_support_layers", 0 }
    });
    REQUIRE(baseline.objects().front()->support_layers().empty());

    Slic3r::Print enforced;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, enforced, {
        { "enable_support",         0 },
        { "enforce_support_layers", 100 }
    });
    REQUIRE(enforced.objects().front()->support_layers().size() > 0);
}

// Support-needed statuses raised while slicing support_capital() with support off. The CLI lists these
// in result.json and fails on them under --strict. Collected under a lock: generate_support_material()
// runs on TBB workers.
static std::vector<PrintBase::SlicingStatus> support_needed_statuses(bool no_check)
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({ support_capital() }, print, model, {
        { "enable_support",         0 },
        { "enforce_support_layers", 0 }
    });
    print.set_no_check_flag(no_check);

    std::mutex mutex;
    std::vector<PrintBase::SlicingStatus> statuses;
    print.set_status_callback([&mutex, &statuses](const PrintBase::SlicingStatus &status) {
        if (status.message_type != PrintStateBase::SlicingNeedSupportOn)
            return;
        std::lock_guard<std::mutex> lock(mutex);
        statuses.push_back(status);
    });
    print.process();
    return statuses;
}

TEST_CASE("An overhang sliced with support off reports that support is needed", "[SupportMaterial]")
{
    // The 40mm cap reaches ~22mm past its 8mm stem, beyond the 6mm cantilever limit of
    // PrintObject::is_support_necessary().
    const std::vector<PrintBase::SlicingStatus> statuses = support_needed_statuses(false);
    REQUIRE(! statuses.empty());
    for (const PrintBase::SlicingStatus &status : statuses) {
        // The CLI only considers step warnings (warning_step != -1), and --strict only NON_CRITICAL ones.
        CHECK(status.warning_level == PrintStateBase::WarningLevel::NON_CRITICAL);
        CHECK(status.warning_step != -1);
    }
}

TEST_CASE("The no-check flag skips the support-needed check", "[SupportMaterial]")
{
    CHECK(support_needed_statuses(true).empty());
}

SCENARIO("Support layer Z honors contact distance", "[SupportMaterial]")
{
    // Box h = 20mm, hole bottom at 5mm, hole height 10mm (top edge at 15mm).
    TriangleMesh mesh = Slic3r::Test::mesh(Slic3r::Test::TestMesh::cube_with_hole);
    mesh.rotate_x(float(M_PI / 2));

	auto check = [](Slic3r::Print &print, bool &first_support_layer_height_ok, bool &layer_height_minimum_ok, bool &layer_height_maximum_ok)
	{
        ConstSupportLayerPtrsAdaptor support_layers = print.objects().front()->support_layers();

		first_support_layer_height_ok = support_layers.front()->print_z == print.config().initial_layer_print_height.value;

		layer_height_minimum_ok = true;
		layer_height_maximum_ok = true;
		double min_layer_height = print.config().min_layer_height.values.front();
		double max_layer_height = print.config().nozzle_diameter.values.front();
		if (print.config().max_layer_height.values.front() > EPSILON)
			max_layer_height = std::min(max_layer_height, print.config().max_layer_height.values.front());
		for (size_t i = 1; i < support_layers.size(); ++ i) {
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z < min_layer_height - EPSILON)
				layer_height_minimum_ok = false;
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z > max_layer_height + EPSILON)
				layer_height_maximum_ok = false;
		}
	};

    GIVEN("A print object having one modelObject") {
        WHEN("Layer height = 0.2 and first layer height = 0.4") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
                { "enable_support",             1 },
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.4 },
                { "dont_support_bridges",       false },
			});
			bool first_layer_ok, layer_min_ok, layer_max_ok;
            check(print, first_layer_ok, layer_min_ok, layer_max_ok);
            THEN("First layer height is honored")			{ REQUIRE(first_layer_ok == true); }
            THEN("No null or negative support layers")		{ REQUIRE(layer_min_ok == true); }
            THEN("No layers thicker than nozzle diameter")	{ REQUIRE(layer_max_ok == true); }
        }
        WHEN("Layer height = 0.2 and first layer height = 0.3") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
                { "enable_support",             1 },
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.3 },
                { "dont_support_bridges",       false },
            });
            bool first_layer_ok, layer_min_ok, layer_max_ok;
            check(print, first_layer_ok, layer_min_ok, layer_max_ok);
            THEN("First layer height is honored")			{ REQUIRE(first_layer_ok == true); }
            THEN("No null or negative support layers")		{ REQUIRE(layer_min_ok == true); }
            THEN("No layers thicker than nozzle diameter")	{ REQUIRE(layer_max_ok == true); }
        }
    }
}

// extrude_support once held a `static` lambda capturing `this`, so a second export in the
// same process dereferenced a returned stack frame (ASan: stack-use-after-return).
TEST_CASE("Support G-code emission survives a second slice in the same process", "[SupportMaterial][Regression]")
{
    const std::string first = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(! layers_with_role(first, "support").empty());

    const std::string second = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(! layers_with_role(second, "support").empty());
}

// The contact layer counts toward the configured interface layer count, so N configured top
// interface layers produce exactly N interface layers, not N+1.
TEST_CASE("Support top interface layer count matches the configured value", "[SupportMaterial]")
{
    const int top = GENERATE(1, 2, 3, 4, 6);
    const std::string g = slice({ TestMesh::overhang }, {
        { "enable_support",                  1 },
        { "layer_height",                    0.2 },
        { "support_on_build_plate_only",     1 },
        { "support_interface_top_layers",    top },
        { "support_interface_bottom_layers", 0 },
    });
    CAPTURE(top);
    REQUIRE(support_base_layer_count(g)      > 0);          // support actually formed
    REQUIRE(support_interface_layer_count(g) == size_t(top));
}

// A rotated cube-with-hole is a horizontal tunnel whose ceiling and floor both receive support, so top
// and bottom interfaces can be exercised independently (the floor is the bottom contact).
static TriangleMesh support_tunnel()
{
    TriangleMesh tunnel = Slic3r::Test::mesh(TestMesh::cube_with_hole);
    tunnel.rotate_x(float(M_PI / 2));
    return tunnel;
}

static size_t tunnel_interface_layers(const TriangleMesh &tunnel, int top, int bottom)
{
    const std::string g = slice({ tunnel }, {
        { "enable_support",                  1 },
        { "layer_height",                    0.2 },
        { "support_on_build_plate_only",     0 },
        { "support_interface_top_layers",    top },
        { "support_interface_bottom_layers", bottom },
    });
    REQUIRE(support_base_layer_count(g) > 0); // support actually formed
    return support_interface_layer_count(g);
}

TEST_CASE("No support interface is generated when neither top nor bottom is configured", "[SupportMaterial]")
{
    REQUIRE(tunnel_interface_layers(support_tunnel(), 0, 0) == 0);
}

TEST_CASE("Bottom interface layer count matches its setting with top interface off", "[SupportMaterial]")
{
    const int bottom = GENERATE(1, 3, 6);
    CAPTURE(bottom);
    REQUIRE(tunnel_interface_layers(support_tunnel(), 0, bottom) == size_t(bottom));
}

// support_interface_bottom_layers = -1 means "same as top".
TEST_CASE("Support interface bottom layers default to the top layer count", "[SupportMaterial]")
{
    const TriangleMesh tunnel = support_tunnel();
    REQUIRE(tunnel_interface_layers(tunnel, 0, -1) == tunnel_interface_layers(tunnel, 0, 0));
    REQUIRE(tunnel_interface_layers(tunnel, 3, -1) == tunnel_interface_layers(tunnel, 3, 3));
}

TEST_CASE("Default support still emits base and interface material", "[SupportMaterial][Regression]")
{
    const std::string g = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(support_base_layer_count(g)      > 0);
    REQUIRE(support_interface_layer_count(g) > 0);
}

// Organic runs TreeSupport3D + TreeModelVolumes, the others the classic TreeSupport.cpp path.
TEST_CASE("Every tree support style produces base and interface material", "[SupportMaterial]")
{
    const char *style = GENERATE("organic", "tree_slim", "tree_strong", "tree_hybrid");
    INFO("style=" << style);
    const std::string g = slice({ TestMesh::overhang }, {
        { "enable_support",               1 },
        { "layer_height",                 0.2 },
        { "support_type",                 "tree(auto)" },
        { "support_style",                style },
        { "support_interface_top_layers", 3 },
    });
    CHECK(support_base_layer_count(g)      > 0);
    CHECK(support_interface_layer_count(g) > 0);
}

TEST_CASE("Raft interface angle alternates by 45 degrees per interface id", "[SupportMaterial]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, print, { { "enable_support", 1 } });
    SupportParameters sp(*print.objects().front());
    sp.raft_angle_interface = 0.5f;
    REQUIRE_THAT(sp.raft_interface_angle(0), Catch::Matchers::WithinAbs(0.5 + M_PI / 4., 1e-6));
    REQUIRE_THAT(sp.raft_interface_angle(1), Catch::Matchers::WithinAbs(0.5 - M_PI / 4., 1e-6));
}

// The angle inputs are overwritten directly, so the pattern-to-angle mapping is checked
// independently of the sliced object's configuration.
TEST_CASE("Support interface fill angle follows the configured interface pattern", "[SupportMaterial]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, print, { { "enable_support", 1 } });
    SupportParameters sp(*print.objects().front());
    sp.interface_angle = 0.3f;
    sp.base_angle      = 1.1f;
    const double tol   = 1e-6;

    SECTION("Rectilinear shifts the interface angle by -45deg for snug support") {
        sp.support_interface_pattern = smipRectilinear;
        sp.support_style             = smsSnug;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle - M_PI_4, tol));
        REQUIRE_THAT(sp.support_interface_angle(3), Catch::Matchers::WithinAbs(sp.interface_angle - M_PI_4, tol));
    }
    SECTION("Rectilinear leaves the interface angle alone for the other styles") {
        sp.support_interface_pattern = smipRectilinear;
        sp.support_style             = smsGrid;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle, tol));
    }
    SECTION("Rectilinear interlaced alternates -/+45deg by interface id parity") {
        sp.support_interface_pattern = smipRectilinearInterlaced;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle - M_PI_4, tol));
        REQUIRE_THAT(sp.support_interface_angle(1), Catch::Matchers::WithinAbs(sp.interface_angle + M_PI_4, tol));
    }
    SECTION("Grid uses the base angle") {
        sp.support_interface_pattern = smipGrid;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.base_angle, tol));
    }
    SECTION("Auto and concentric use the interface angle unchanged") {
        sp.support_interface_pattern = smipAuto;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle, tol));
        sp.support_interface_pattern = smipConcentric;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle, tol));
    }
}

// End-to-end that the pattern reaches the emitted fill, not just support_interface_angle().
TEST_CASE("Interlaced support interface alternates fill angle while rectilinear does not", "[SupportMaterial]")
{
    auto interface_angles = [](const char *pattern) {
        std::vector<double> a;
        for (const auto &kv : interface_fill_angle_by_layer(slice({ TestMesh::overhang }, {
                 { "enable_support",               1 },
                 { "layer_height",                 0.2 },
                 { "support_on_build_plate_only",  1 },
                 { "support_interface_top_layers", 6 },
                 { "support_interface_pattern",    pattern } })))
            a.push_back(kv.second);
        return a;
    };

    const std::vector<double> rectilinear = interface_angles("rectilinear");
    const std::vector<double> interlaced  = interface_angles("rectilinear_interlaced");
    REQUIRE(rectilinear.size() >= 3);
    REQUIRE(interlaced.size()  >= 3);

    for (size_t i = 1; i < rectilinear.size(); ++i)
        REQUIRE(axial_angle_diff_deg(rectilinear[i], rectilinear[0]) < 15.0);

    for (size_t i = 1; i < interlaced.size(); ++i)
        REQUIRE(axial_angle_diff_deg(interlaced[i], interlaced[i - 1]) > 60.0);
}

// Normal and non-organic tree support share the same interface angle logic: with a rectilinear interface
// pattern both emit their interface fill at the same angle (both go through support_interface_angle()).
TEST_CASE("Normal and tree support use the same interface fill angle", "[SupportMaterial]")
{
    auto mean_interface_angle = [](const char *type, const char *style) {
        const auto angles = interface_fill_angle_by_layer(slice({ TestMesh::overhang }, {
            { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_on_build_plate_only", 1 },
            { "support_type", type }, { "support_style", style },
            { "support_interface_top_layers", 6 }, { "support_interface_pattern", "rectilinear" } }));
        REQUIRE(angles.size() >= 3);
        // Axial mean, as in interface_fill_angle_by_layer: a plain mean would split angles either
        // side of the [0, pi) wrap.
        double x = 0, y = 0;
        for (const auto &kv : angles) {
            x += std::cos(2.0 * kv.second);
            y += std::sin(2.0 * kv.second);
        }
        double mean = 0.5 * std::atan2(y, x);
        if (mean < 0) mean += M_PI;
        return mean;
    };
    REQUIRE(axial_angle_diff_deg(mean_interface_angle("normal(auto)", "default"),
                                 mean_interface_angle("tree(auto)", "tree_slim")) < 10.0);
}

// Every style, because the non-organic tree styles once emitted one more top interface layer than the rest.
TEST_CASE("Top interface layer count equals the configured value for every support style", "[SupportMaterial]")
{
    auto [type, style] = GENERATE(table<const char *, const char *>({
        { "normal(auto)", "grid" },        { "normal(auto)", "snug" },
        { "tree(auto)",   "organic" },     { "tree(auto)",   "tree_slim" },
        { "tree(auto)",   "tree_strong" }, { "tree(auto)",   "tree_hybrid" },
    }));
    CAPTURE(style);
    const std::string g = slice({ TestMesh::overhang }, {
        { "enable_support",               1 },
        { "layer_height",                 0.2 },
        { "support_type",                 type },
        { "support_style",                style },
        { "support_interface_top_layers", 4 },
    });
    REQUIRE(support_interface_layer_count(g) == 4u);
}

// The bottom interface was dropped in earlier versions when support started on the model rather
// than the plate.
TEST_CASE("Non-organic tree support generates a bottom interface on internal geometry", "[SupportMaterial]")
{
    const std::string g = slice({ support_tunnel() }, {
        { "enable_support",                  1 },
        { "layer_height",                    0.2 },
        { "support_on_build_plate_only",     0 },
        { "support_type",                    "tree(auto)" },
        { "support_style",                   "tree_slim" },
        { "support_interface_top_layers",    0 },
        { "support_interface_bottom_layers", 6 },
    });
    REQUIRE(support_base_layer_count(g)      > 0);
    REQUIRE(support_interface_layer_count(g) > 0);
}

// The capital forces the model contact; on a horizontal tunnel organic can arch a branch in and make none.
TEST_CASE("A bottom interface is produced for every support style on a forced model contact", "[SupportMaterial]")
{
    auto [type, style] = GENERATE(table<const char *, const char *>({
        { "normal(auto)", "default" },     { "tree(auto)", "tree_slim" },
        { "tree(auto)",   "tree_strong" }, { "tree(auto)", "tree_hybrid" },
        { "tree(auto)",   "organic" },
    }));
    CAPTURE(style);
    REQUIRE(support_interface_layer_count(slice({ support_capital() }, {
        { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_on_build_plate_only", 0 },
        { "support_type", type }, { "support_style", style },
        { "support_interface_top_layers", 0 }, { "support_interface_bottom_layers", 6 } })) > 0);
}

TEST_CASE("Bottom interface spacing controls bottom interface density for every support style", "[SupportMaterial]")
{
    auto [type, style] = GENERATE(table<const char *, const char *>({
        { "normal(auto)", "default" },     { "tree(auto)", "tree_slim" },
        { "tree(auto)",   "tree_strong" }, { "tree(auto)", "tree_hybrid" },
        { "tree(auto)",   "organic" },
    }));
    CAPTURE(style);
    const TriangleMesh model = support_capital();
    auto len = [&model](const char *support_type, const char *support_style, double spacing) {
        return support_interface_extrusion_length(slice({ model }, {
            { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_on_build_plate_only", 0 },
            { "support_type", support_type }, { "support_style", support_style }, { "support_interface_top_layers", 0 },
            { "support_interface_bottom_layers", 6 }, { "support_bottom_interface_spacing", spacing } }));
    };
    REQUIRE(len(type, style, 0.0) > len(type, style, 4.0) * 1.5);
}

// Interface and base flows are identical in width and rate unless a separate support-interface
// filament is used, so density is the observable here, not flow.
TEST_CASE("Bottom-only support interface keeps the dense interface density", "[SupportMaterial]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, print, {
        { "enable_support",                   1 },
        { "support_interface_top_layers",     0 },
        { "support_interface_bottom_layers",  6 },
        { "support_bottom_interface_spacing", 0.0 },  // solid: density resolves to 1.0
        { "support_base_pattern_spacing",     2.5 },  // sparse: density stays below 1.0
    });
    SupportParameters sp(*print.objects().front());
    REQUIRE(sp.bottom_interface_density > sp.support_density);
}

