#include "FinSupport.hpp"
#include "SupportCommon.hpp"

#include "../Fill/Fill.hpp"
#include "../AABBTreeLines.hpp"
#include "../Flow.hpp"
#include "../Geometry/ConvexHull.hpp"
#include "../Layer.hpp"
#include "../Model.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <boost/log/trivial.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace Slic3r {
FinSupportDimensions fin_support_dimensions(const PrintObject &object, const PrintRegion &region)
{
    const double reference = region.flow(object, frExternalPerimeter, object.config().layer_height.value).width();
    return {object.config().fin_support_contact_width.get_abs_value(reference),
            object.config().fin_support_thickness.get_abs_value(reference)};
}

namespace {

struct PaintedSegment {
    Vec2d a;
    Vec2d b;
    Vec2d normal;
    Vec3d surface_normal;
    Vec2d face_center = Vec2d::Zero();
    double face_span = 0;
};

struct PaintedLayer {
    std::array<std::vector<PaintedSegment>, 3> sections; // Middle, bottom and top of the layer.
};

struct FinCandidate {
    Vec2d point;
    Vec2d normal; // A: load-bearing sheet direction.
    Vec2d contact_normal; // B: direction from the painted wall to the sheet.
    size_t top_layer;
    FinSupportDimensions dimensions;
    double expansion_slope;
};

enum class FinReject {
    top_depth, top_wall, wall_facing, projection_intrusion, body_depth, bed_collision, model_collision,
    missing_wall, missing_paint, blocked_contact, contact_contour, contact_span, contact_normal,
    contact_turn, empty_neck, top_band, topology, thin_body, no_outline, seed_region, overlap,
    direction, paint_empty, paint_facing, paint_parallel, paint_distance,
    paint_endpoint, count
};

struct FinDiagnostics {
    struct Failure {
        size_t count = 0;
        double first_z = 0;
        double first_value = 0;
    };
    std::array<Failure, size_t(FinReject::count)> failures{};

    bool reject(FinReject reason, double z, double value = 0)
    {
        Failure &failure = failures[size_t(reason)];
        if (failure.count++ == 0) {
            failure.first_z = z;
            failure.first_value = value;
        }
        return false;
    }

    void merge(const FinDiagnostics &other)
    {
        for (size_t i = 0; i < failures.size(); ++i) {
            if (failures[i].count == 0)
                failures[i] = other.failures[i];
            else
                failures[i].count += other.failures[i].count;
        }
    }

    void log(const char *stage, const PrintObject &object) const
    {
        static constexpr std::array<const char *, size_t(FinReject::count)> names{
            "top-depth", "top-wall", "wall-facing", "projection-intrusion", "body-depth", "bed-collision", "model-collision",
            "contact-no-wall", "contact-no-paint", "contact-blocked", "contact-contour", "contact-span", "contact-normal",
            "contact-turn", "empty-neck", "top-band", "topology", "thin-body", "no-outline", "seed-region", "fin-overlap",
            "no-direction", "paint-empty", "paint-facing", "paint-parallel",
            "paint-distance", "paint-endpoint"};
        for (size_t i = 0; i < failures.size(); ++i)
            if (failures[i].count > 0)
                BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
                    << " stage=" << stage << " reject=" << names[i]
                    << " count=" << failures[i].count << " first_z=" << failures[i].first_z
                    << " value=" << failures[i].first_value;
    }
};

struct TracedFin {
    FinCandidate seed;
    FinDiagnostics diagnostics;
    size_t planned_contact_bands = 0;
    size_t accepted_contact_bands = 0;
    size_t suppressed_contact_bands = 0;
    std::vector<Vec2d> boundary; // One actual outer-contour position per layer, from the bed upward.
    std::vector<bool> wall_present;
    std::vector<Vec2d> body_start;
    std::vector<Vec2d> tips;
    std::vector<bool> contacts;
    std::vector<Vec3d> contact_normals;
    std::vector<ExPolygon> areas;
    BoundingBoxes area_bounds;
    std::vector<ExPolygons> centers;
    std::vector<Polylines> outlines;
};

Point scaled_point(const Vec2d &p) { return Point::new_scale(p.x(), p.y()); }

Polygon fin_bed_shape(const PrintObject &object)
{
    Polygon bed = get_bed_shape_with_excluded_area(object.print()->config());
    if (!object.instances().empty())
        bed.translate(-object.instances().front().shift_without_plate_offset());
    return bed;
}

constexpr double min_fin_contact_alignment = 0.86602540378443864676; // cos(30 degrees).

bool fin_contact_aligned(const Vec2d &body_direction, const Vec2d &contact_direction)
{
    return body_direction.dot(contact_direction) >= min_fin_contact_alignment - 1e-9;
}

struct SectionMoments {
    double area = 0;
    Vec2d first = Vec2d::Zero();
    Eigen::Matrix2d second = Eigen::Matrix2d::Zero();
};

// Area and first moments in world XY; second moments about a nearby origin.
// Polygon edge integrals preserve area weighting and subtract holes exactly.
SectionMoments area_moments(const ExPolygons &slices, const Vec2d &origin = Vec2d::Zero())
{
    SectionMoments result;
    auto add = [&](const Polygon &polygon, double sign) {
        const double signed_area = polygon.area();
        const double weight = sign * std::abs(signed_area) * SCALING_FACTOR * SCALING_FACTOR;
        if (weight == 0)
            return;
        result.area += weight;
        Vec2d local_first = Vec2d::Zero();
        Eigen::Matrix2d second = Eigen::Matrix2d::Zero();
        for (size_t i = 0; i < polygon.points.size(); ++i) {
            const Vec2d a = unscale(polygon.points[i]) - origin;
            const Vec2d b = unscale(polygon.points[(i + 1) % polygon.points.size()]) - origin;
            const double cross = a.x() * b.y() - b.x() * a.y();
            local_first += cross * (a + b) / 6;
            second(0, 0) += cross * (a.x() * a.x() + a.x() * b.x() + b.x() * b.x()) / 12;
            second(1, 1) += cross * (a.y() * a.y() + a.y() * b.y() + b.y() * b.y()) / 12;
            second(0, 1) += cross * (2 * a.x() * a.y() + a.x() * b.y() +
                                      b.x() * a.y() + 2 * b.x() * b.y()) / 24;
        }
        second(1, 0) = second(0, 1);
        const double orientation = signed_area > 0 ? sign : -sign;
        result.first += weight * origin + orientation * local_first;
        result.second += orientation * second;
    };
    for (const ExPolygon &slice : slices) {
        add(slice.contour, 1);
        for (const Polygon &hole : slice.holes)
            add(hole, -1);
    }
    return result;
}

bool inside_model(const Layer &layer, const Vec2d &p)
{
    const Point point = scaled_point(p);
    return std::any_of(layer.lslices.begin(), layer.lslices.end(),
                       [&point](const ExPolygon &poly) { return poly.contains(point, false); });
}

// The paint is stored on 3D mesh facets. Slice the actual side facets here: the usual
// support-enforcer projection loses the side position and normal needed by a fin.
std::vector<PaintedLayer> painted_sides(const PrintObject &object)
{
    std::vector<PaintedLayer> result(object.layer_count());
    const auto layers = object.layers();
    const double print_z_offset = object.slicing_parameters().object_print_z_min;
    for (const ModelVolume *volume : object.model_object()->volumes) {
        if (!volume->is_model_part())
            continue;
        const indexed_triangle_set facets = volume->supported_facets.get_facets_strict(*volume, EnforcerBlockerType::ENFORCER);
        const Transform3f transform = (object.trafo_centered() * volume->get_matrix()).cast<float>();
        for (const auto &face : facets.indices) {
            std::array<Vec3d, 3> v;
            for (int i = 0; i < 3; ++i)
                v[i] = (transform * facets.vertices[face(i)]).cast<double>();
            const Vec3d normal3 = (v[1] - v[0]).cross(v[2] - v[0]).normalized();
            Vec2d normal = normal3.head<2>();
            if (normal.norm() < 0.5) // Fins make side contacts, not underside contacts.
                continue;
            normal.normalize();
            const double zmin = std::min({v[0].z(), v[1].z(), v[2].z()});
            const double zmax = std::max({v[0].z(), v[1].z(), v[2].z()});
            auto layer_it = std::lower_bound(layers.begin(), layers.end(), zmin,
                [print_z_offset](const Layer *layer, double z) { return layer->print_z - print_z_offset < z; });
            for (; layer_it != layers.end() && (*layer_it)->print_z - print_z_offset - (*layer_it)->height <= zmax; ++layer_it) {
                const Layer &layer = **layer_it;
                // Sample just inside the bead's vertical extent to avoid choosing
                // the neighboring face solely because it shares a horizontal edge.
                const double top_z = layer.print_z - print_z_offset;
                const double bottom_z = top_z - layer.height;
                const double inset = std::min(1e-6, 0.5 * layer.height);
                // Z anti-aliasing can move slice_z away from the layer center.
                // Wall lookup uses that slice, but bead limits use physical Z.
                const std::array<double, 3> zs{layer.slice_z, bottom_z + inset, top_z - inset};
                for (size_t section = 0; section < zs.size(); ++section) {
                    const double z = zs[section];
                    if (z < zmin || z > zmax)
                        continue;
                    std::vector<Vec2d> intersections;
                    for (int i = 0; i < 3; ++i) {
                        const Vec3d &a = v[i], &b = v[(i + 1) % 3];
                        if (std::abs(b.z() - a.z()) < 1e-6 || z < std::min(a.z(), b.z()) || z > std::max(a.z(), b.z()))
                            continue;
                        const Vec2d p = a.head<2>() + (z - a.z()) / (b.z() - a.z()) * (b - a).head<2>();
                        if (std::none_of(intersections.begin(), intersections.end(),
                                         [&p](const Vec2d &q) { return (p - q).squaredNorm() < 1e-8; }))
                            intersections.push_back(p);
                    }
                    if (intersections.size() < 2)
                        continue;
                    const Vec2d mid = 0.5 * (intersections[0] + intersections[1]);
                    Vec2d section_normal = normal;
                    if (inside_model(layer, mid + 0.6 * section_normal))
                        section_normal = -section_normal;
                    if (inside_model(layer, mid + 0.6 * section_normal))
                        continue;
                    Vec3d outward_normal = normal3;
                    if (normal3.head<2>().dot(section_normal) < 0)
                        outward_normal = -outward_normal;
                    result[size_t(layer_it - layers.begin())].sections[section].push_back(
                        {intersections[0], intersections[1], section_normal, outward_normal});
                }
            }
        }
    }
    return result;
}

bool clear_fin_line(const Layer &layer, const Polygon &bed, const Vec2d &from, const Vec2d &to,
                    double width, FinDiagnostics &diagnostics)
{
    const Vec2d direction = to - from;
    if (direction.squaredNorm() < 1e-8)
        return diagnostics.reject(FinReject::body_depth, layer.print_z, direction.norm());
    const Vec2d side(-direction.y(), direction.x());
    const Vec2d half_width = 0.5 * width * side.normalized();
    const int samples = std::max(2, int(std::ceil((to - from).norm() / std::max(width, 0.4))));
    for (int i = 0; i <= samples; ++i) {
        const Vec2d p = from + double(i) / samples * (to - from);
        if (!bed.contains(scaled_point(p + half_width)) || !bed.contains(scaled_point(p - half_width)))
            return diagnostics.reject(FinReject::bed_collision, layer.print_z);
        if (inside_model(layer, p) || inside_model(layer, p + half_width) || inside_model(layer, p - half_width))
            return diagnostics.reject(FinReject::model_collision, layer.print_z);
    }
    return true;
}

bool projected_outer_contour(const Layer &layer, const Vec2d &origin, const Vec2d &normal,
                             double outer_distance, Vec2d &nearest, Vec2d *surface_normal = nullptr)
{
    double best_distance = -std::numeric_limits<double>::infinity();
    bool found = false;
    for (const ExPolygon &slice : layer.lslices) {
        const Points &contour = slice.contour.points;
        for (size_t i = 0; i < contour.size(); ++i) {
            const Vec2d a = unscale(contour[i]);
            const Vec2d b = unscale(contour[(i + 1) % contour.size()]);
            const Vec2d edge = b - a;
            const double denominator = normal.x() * edge.y() - normal.y() * edge.x();
            if (std::abs(denominator) < 1e-12)
                continue;
            const Vec2d delta = a - origin;
            const double distance = (delta.x() * edge.y() - delta.y() * edge.x()) / denominator;
            const double edge_fraction = (delta.x() * normal.y() - delta.y() * normal.x()) / denominator;
            if (edge_fraction < 0 || edge_fraction > 1 || distance >= outer_distance)
                continue;
            const Vec2d point = origin + distance * normal;
            if (distance > best_distance && !inside_model(layer, point + 0.6 * normal)) {
                nearest = point;
                best_distance = distance;
                found = true;
                if (surface_normal != nullptr) {
                    *surface_normal = Vec2d(edge.y(), -edge.x()).normalized();
                    if (surface_normal->dot(normal) < 0)
                        *surface_normal = -*surface_normal;
                }
            }
        }
    }
    return found;
}

Polygon foot_area(const Vec2d &start, const Vec2d &tip, const Vec2d &normal, double thickness, double width,
                  double expansion)
{
    const double envelope = expansion;
    const double half_width = 0.5 * thickness + envelope;
    const Vec2d tangent(-normal.y(), normal.x());
    const Vec2d inner = start - 0.5 * width * normal;
    const Vec2d outer = tip + envelope * normal;
    return Polygon{scaled_point(inner - half_width * tangent),
                   scaled_point(outer - half_width * tangent),
                   scaled_point(outer + half_width * tangent),
                   scaled_point(inner + half_width * tangent)};
}

bool trace_fin(const PrintObject &object, const Polygon &bed, const FinCandidate &candidate,
               double top_depth, double gap, double thickness, double line_width, TracedFin &fin)
{
    if (top_depth - gap < 2 * line_width)
        return fin.diagnostics.reject(FinReject::top_depth, object.get_layer(int(candidate.top_layer))->print_z, top_depth - gap);
    fin.boundary.resize(candidate.top_layer + 1);
    fin.wall_present.assign(candidate.top_layer + 1, false);
    fin.body_start.resize(candidate.top_layer + 1);
    fin.tips.resize(candidate.top_layer + 1);
    const double top_z = object.get_layer(int(candidate.top_layer))->print_z;
    Vec2d target = candidate.point;
    const Vec2d side(-candidate.normal.y(), candidate.normal.x());
    // A fixes the sheet plane through the seed. Find the wall in that plane;
    // projecting a B-ray wall displacement onto A loses its transverse part.
    double projected_inner = std::numeric_limits<double>::infinity();
    for (size_t remaining = candidate.top_layer + 1; remaining > 0; --remaining) {
        const size_t i = remaining - 1;
        const Layer &layer = *object.get_layer(int(i));
        const double outer_distance = top_depth + (top_z - layer.print_z) * candidate.expansion_slope;
        fin.tips[i] = candidate.point + outer_distance * candidate.normal;
        Vec2d point = target;
        Vec2d surface_normal = candidate.contact_normal;
        const bool have_surface = projected_outer_contour(layer, candidate.point, candidate.normal,
                                                         outer_distance, point, &surface_normal);
        if (i == candidate.top_layer && (!have_surface || (point - candidate.point).norm() > line_width))
            return fin.diagnostics.reject(FinReject::top_wall, layer.print_z, (point - candidate.point).norm());
        const double alignment = surface_normal.dot(candidate.normal);
        if (have_surface && alignment <= EPSILON)
            return fin.diagnostics.reject(FinReject::wall_facing, layer.print_z, alignment);
        const double surface_inner = have_surface ? (point - candidate.point).dot(candidate.normal) +
            (gap + 0.5 * thickness * std::abs(side.dot(surface_normal))) / alignment : projected_inner;
        // Project the existing sheet downward, extend its inner edge to the wall, and clip model clearance.
        // A lower wall may not remove the load-bearing vertical projection of the upper sheet.
        if (i < candidate.top_layer && surface_inner > projected_inner + 0.5 * line_width)
            return fin.diagnostics.reject(FinReject::projection_intrusion, layer.print_z, surface_inner - projected_inner);
        projected_inner = std::min(projected_inner, surface_inner);
        const Vec2d from = candidate.point + std::max(projected_inner, surface_inner) * candidate.normal;
        const double depth = (fin.tips[i] - from).dot(candidate.normal);
        if (depth < 2 * line_width)
            return fin.diagnostics.reject(FinReject::body_depth, layer.print_z, depth);
        if (!clear_fin_line(layer, bed, from, fin.tips[i], thickness, fin.diagnostics))
            return false;
        fin.boundary[i] = point;
        fin.wall_present[i] = have_surface;
        fin.body_start[i] = from;
        target = point;
    }

    return true;
}

Polygon fin_rectangle(const Vec2d &inner, const Vec2d &outer, const Vec2d &normal, double thickness)
{
    const Vec2d side = 0.5 * thickness * Vec2d(-normal.y(), normal.x());
    return Polygon{scaled_point(inner - side), scaled_point(outer - side),
                   scaled_point(outer + side), scaled_point(inner + side)};
}

Polygon fin_contact_polygon(const Vec2d &front, const Vec2d &base, const Vec2d &normal, const Vec2d &contact_normal,
                             double contact_width, double thickness, double line_width)
{
    const Vec2d tangent(-contact_normal.y(), contact_normal.x());
    const Vec2d side = 0.5 * (thickness - line_width) * Vec2d(-normal.y(), normal.x());
    const Vec2d rear = base + line_width * normal;
    Points points{scaled_point(front - 0.5 * contact_width * tangent),
                  scaled_point(base - side), scaled_point(rear - side),
                  scaled_point(rear + side), scaled_point(base + side)};
    if (contact_width > 0)
        points.push_back(scaled_point(front + 0.5 * contact_width * tangent));
    // The front and rear may have different orientations. Their convex bridge
    // avoids crossed polygon edges without changing the front segment length.
    return Geometry::convex_hull(std::move(points));
}

ExPolygons build_fin_contact_section(const Layer &layer, const Polygon &bed, const TracedFin &fin,
                                     size_t layer_id, const Vec2d &contact_normal,
                                     double contact_width, double thickness, double line_width)
{
    const Vec2d base = fin.body_start[layer_id] + 0.5 * line_width * fin.seed.normal;
    const Vec2d front = fin.boundary[layer_id] + 0.5 * line_width * contact_normal;
    const Polygon neck = fin_contact_polygon(front, base, fin.seed.normal, contact_normal,
                                             contact_width, thickness, line_width);
    return intersection_ex(diff_ex(neck, offset_ex(layer.lslices, float(scale_(0.5 * line_width)))),
                           offset_ex(bed, -float(scale_(0.5 * line_width))));
}

struct ProjectionClip {
    ExPolygons model;
    ExPolygons bed;
    bool ready = false;
};

enum class ProjectionStop { bed, no_downward, absorbed, outside, clipped };

struct ProjectionResult {
    size_t layers = 0;
    ProjectionStop stop = ProjectionStop::bed;
};

struct ContactProjectionRay {
    Vec2d normal;
    double slope;
};

std::optional<ContactProjectionRay> contact_projection_ray(const Vec3d &surface_normal, double angle)
{
    const double outward = surface_normal.head<2>().norm();
    if (outward <= EPSILON)
        return std::nullopt;
    const double radians = angle * M_PI / 180.;
    const double radial = outward * std::cos(radians) + surface_normal.z() * std::sin(radians);
    const double downward = outward * std::sin(radians) - surface_normal.z() * std::cos(radians);
    if (downward <= 1e-12)
        return std::nullopt;
    return ContactProjectionRay{surface_normal.head<2>() / outward, radial / downward};
}

ExPolygons fin_body_area(const Layer &layer, const TracedFin &fin, size_t layer_id,
                         double thickness, double gap)
{
    return diff_ex(fin_rectangle(fin.body_start[layer_id], fin.tips[layer_id], fin.seed.normal, thickness),
                   offset_ex(layer.lslices, float(scale_(gap))));
}

void build_fin_body_section(const PrintObject &object, const TracedFin &fin, size_t layer_id,
                            const Polygon &bed, double thickness, double gap,
                            ExPolygons &body, ExPolygons &body_center)
{
    const Layer &layer = *object.get_layer(int(layer_id));
    const double width = support_material_flow(&object, float(layer.height)).width();
    body = intersection_ex(fin_body_area(layer, fin, layer_id, thickness, gap), ExPolygons{ExPolygon(bed)});
    if (layer_id == 0) {
        ExPolygons foot = diff_ex(intersection_ex(
            Polygons{foot_area(fin.body_start.front(), fin.tips.front(), fin.seed.normal, thickness, width,
                               object.config().raft_first_layer_expansion.value)},
            offset_ex(bed, -float(scale_(0.5 * width)))),
            offset_ex(layer.lslices, float(scale_(gap + 0.5 * width))));
        expolygons_append(body, std::move(foot));
        body = union_ex(body);
    }
    body_center = offset_ex(body, -float(scale_(0.5 * width)));
}

ProjectionResult backfill_contact_projection(const PrintObject &object, const TracedFin &fin, size_t bottom,
                                             const Polygon &bed, double thickness,
                                             double contact_width, double gap, const Vec3d &surface_normal,
                                             const std::vector<ExPolygons> &body_centers, std::vector<ExPolygons> &contacts,
                                             std::vector<ProjectionClip> &clips,
                                             std::vector<ExPolygons> *projection_copy = nullptr)
{
    ProjectionResult result;
    const Vec2d &body_normal = fin.seed.normal;
    const auto ray = contact_projection_ray(surface_normal,
        object.config().fin_support_contact_projection_angle.value);
    if (!ray) {
        result.stop = ProjectionStop::no_downward;
        return result;
    }
    const Vec2d &normal = ray->normal;
    const double slope = ray->slope;
    // Project just the front segment (a point for zero length). Its endpoints
    // connect directly to the current lower layer's inner body corners.
    const Layer &contact_layer = *object.get_layer(int(bottom));
    const double contact_line_width = support_material_flow(&object, float(contact_layer.height)).width();
    Vec2d front = fin.boundary[bottom] + 0.5 * contact_line_width * normal;
    const Vec2d initial_front = front;
    double advance = 0;
    const double neck_depth = (fin.body_start[bottom] - fin.boundary[bottom]).dot(normal);
    for (size_t remaining = bottom; remaining > 0; --remaining) {
        const size_t i = remaining - 1;
        const Layer &layer = *object.get_layer(int(i));
        const double width = support_material_flow(&object, float(layer.height)).width();
        const double dz = object.get_layer(int(i + 1))->print_z - layer.print_z;
        front += dz * slope * normal;
        advance += dz * slope;
        // A projection only carries load; actual contacts come from this
        // layer's own band, including when another band's projection overlaps it.
        const double clearance = std::min(gap, 0.25 * width);
        const Vec2d base = fin.body_start[i] + 0.5 * width * body_normal;
        const double fraction = std::clamp(advance / std::max(neck_depth, EPSILON), 0., 1.);
        const double rear_span = (thickness - width) * std::abs(body_normal.dot(normal));
        const double projected_length = contact_width - std::max(0., contact_width - rear_span) * fraction;
        const Vec2d side = 0.5 * projected_length * Vec2d(-normal.y(), normal.x());
        const Point a = scaled_point(front - side), b = scaled_point(front + side);
        if (std::any_of(body_centers[i].begin(), body_centers[i].end(), [&a, &b](const ExPolygon &body) {
            return a == b ? body.contains(a) : body.contains(Line(a, b));
        })) {
            result.stop = ProjectionStop::absorbed;
            break;
        }
        const Vec2d body_side = 0.5 * (thickness - width) * Vec2d(-body_normal.y(), body_normal.x());
        const double outer_limit = std::max(base.dot(normal),
                                           (fin.tips[i] - 0.5 * width * body_normal).dot(normal)) +
                                   std::abs(body_side.dot(normal));
        if (front.dot(normal) > outer_limit + EPSILON) {
            result.stop = ProjectionStop::outside;
            break;
        }
        ProjectionClip &clip = clips[i];
        if (!clip.ready) {
            clip.model = offset_ex(layer.lslices, float(scale_(clearance + 0.5 * width)));
            clip.bed = offset_ex(bed, -float(scale_(0.5 * width)));
            clip.ready = true;
        }
        // B can advance sideways relative to A. Bounding its B coordinate does
        // not keep the projected segment inside the sheet's transverse edges.
        // Allow the original neck up to its short body overlap; beyond that,
        // every projection center must fit the actual lower body center area.
        const Polygon neck = fin_contact_polygon(initial_front, base, body_normal, normal,
                                                   contact_width, thickness, width);
        const Vec2d tangent(-body_normal.y(), body_normal.x());
        double inner = std::numeric_limits<double>::infinity();
        double side_min = std::numeric_limits<double>::infinity();
        double side_max = -std::numeric_limits<double>::infinity();
        for (const Point &point : neck.points) {
            const Vec2d p = unscale(point);
            inner = std::min(inner, p.dot(body_normal));
            side_min = std::min(side_min, p.dot(tangent));
            side_max = std::max(side_max, p.dot(tangent));
        }
        const Vec2d side_center = 0.5 * (side_min + side_max) * tangent;
        const Polygon neck_limit = fin_rectangle((inner - width) * body_normal + side_center,
            (base.dot(body_normal) + width) * body_normal + side_center,
            body_normal, side_max - side_min + 2 * width);
        ExPolygons allowed = intersection_ex(Polygons{neck}, Polygons{neck_limit});
        expolygons_append(allowed, body_centers[i]);
        allowed = union_ex(allowed);
        ExPolygons projection = intersection_ex(diff_ex(fin_contact_polygon(front, base, body_normal, normal,
            projected_length, thickness, width), clip.model), clip.bed);
        projection = intersection_ex(projection, allowed);
        if (projection.empty()) {
            result.stop = ProjectionStop::clipped;
            break;
        }
        if (diff_ex(projection, body_centers[i]).empty()) {
            result.stop = ProjectionStop::absorbed;
            break;
        }
        if (projection_copy != nullptr)
            expolygons_append((*projection_copy)[i], projection);
        expolygons_append(contacts[i], std::move(projection));
        ++result.layers;
    }
    return result;
}

bool closed_outline(const ExPolygons &area, const ExPolygons &outer_centerlines,
                    const Vec2d &normal, double inset, double line_width, Polyline &outline)
{
    // The complete bead, not just its centerline, must fit inside the sheet.
    if (offset_ex(area, -float(scale_(inset + 0.5 * line_width - EPSILON))).empty())
        return false;
    // The outer contour is the union of body and projected contact center areas.
    // Eroding a short combined neck again can disconnect it at its merge layer.
    ExPolygons centerlines = outer_centerlines.empty() ? offset_ex(area, -float(scale_(inset))) : outer_centerlines;
    if (centerlines.size() != 1 || !centerlines.front().holes.empty())
        return false;
    Polygon &contour = centerlines.front().contour;
    if (!contour.is_valid())
        return false;
    auto start = std::max_element(contour.points.begin(), contour.points.end(), [&normal](const Point &a, const Point &b) {
        return unscale(a).dot(normal) < unscale(b).dot(normal);
    });
    std::rotate(contour.points.begin(), start, contour.points.end());
    outline = contour.split_at_first_point();
    return true;
}

const PaintedSegment *painted_contact(const std::vector<PaintedSegment> &segments, const Vec2d &point,
                                     const Vec2d &normal, double width)
{
    const PaintedSegment *nearest = nullptr;
    double nearest_distance = width + EPSILON;
    for (const PaintedSegment &segment : segments) {
        if (segment.normal.dot(normal) <= 0)
            continue;
        // Follow the contact plane rather than projecting perpendicularly onto a
        // facet: those directions differ on curved surfaces and at other Zs.
        const Vec2d edge = segment.b - segment.a;
        const double denominator = normal.x() * edge.y() - normal.y() * edge.x();
        if (std::abs(denominator) < 1e-12)
            continue;
        const Vec2d delta = segment.a - point;
        const double distance = (delta.x() * edge.y() - delta.y() * edge.x()) / denominator;
        const double fraction = (delta.x() * normal.y() - delta.y() * normal.x()) / denominator;
        if (fraction < -1e-7 || fraction > 1 + 1e-7 || std::abs(distance) > nearest_distance)
            continue;
        nearest = &segment;
        nearest_distance = std::abs(distance);
    }
    return nearest;
}

// Diagnose a failed lookup without changing its acceptance rules. This extra
// scan runs only on failure and reports the condition hiding the painted cut.
void diagnose_paint_lookup(const std::vector<PaintedSegment> &segments, const Vec2d &point,
                           const Vec2d &normal, double z, FinDiagnostics &diagnostics)
{
    if (segments.empty()) {
        diagnostics.reject(FinReject::paint_empty, z);
        return;
    }
    bool facing = false, nonparallel = false;
    double nearest = std::numeric_limits<double>::infinity();
    double overrun = std::numeric_limits<double>::infinity();
    for (const PaintedSegment &segment : segments) {
        if (segment.normal.dot(normal) <= 0)
            continue;
        facing = true;
        const Vec2d edge = segment.b - segment.a;
        const double denominator = normal.x() * edge.y() - normal.y() * edge.x();
        if (std::abs(denominator) < 1e-12)
            continue;
        nonparallel = true;
        const Vec2d delta = segment.a - point;
        const double fraction = (delta.x() * normal.y() - delta.y() * normal.x()) / denominator;
        if (fraction < -1e-7 || fraction > 1 + 1e-7) {
            overrun = std::min(overrun, std::abs(fraction - std::clamp(fraction, 0., 1.)) * edge.norm());
            continue;
        }
        const double distance = (delta.x() * edge.y() - delta.y() * edge.x()) / denominator;
        nearest = std::min(nearest, std::abs(distance));
    }
    if (!facing)
        diagnostics.reject(FinReject::paint_facing, z);
    else if (!nonparallel)
        diagnostics.reject(FinReject::paint_parallel, z);
    else if (std::isfinite(nearest))
        diagnostics.reject(FinReject::paint_distance, z, nearest);
    else
        diagnostics.reject(FinReject::paint_endpoint, z, overrun);
}

bool painted_contact_width(const Layer &layer, const PaintedLayer &painted,
                           const Vec2d &point, const Vec2d &normal, double contact_width, double line_width,
                           Vec3d *surface_normal = nullptr, FinDiagnostics *diagnostics = nullptr,
                           bool check_top_section = true)
{
    auto reject = [&](FinReject reason, double value = 0) {
        if (diagnostics != nullptr)
            diagnostics->reject(reason, layer.print_z, value);
        return false;
    };
    const Vec2d tangent(-normal.y(), normal.x());
    // Both bead edges must land on the allowed wall, not just its mathematical centerline.
    const double bead_span = contact_width + line_width;
    const int samples = std::max(2, int(std::ceil(bead_span / line_width)));
    Vec3d normal_sum = Vec3d::Zero();
    for (int i = 0; i <= samples; ++i) {
        const Vec2d edge = point + (double(i) / samples - 0.5) * bead_span * tangent;
        Vec2d wall = edge;
        if (!projected_outer_contour(layer, edge, normal, 2 * line_width, wall))
            return reject(FinReject::contact_contour, -1);
        if ((wall - edge).norm() > line_width)
            return reject(FinReject::contact_contour, (wall - edge).norm());
        const size_t section_count = check_top_section ? painted.sections.size() : painted.sections.size() - 1;
        for (size_t section = 0; section < section_count; ++section) {
            const PaintedSegment *surface = painted_contact(painted.sections[section], wall, normal, line_width);
            if (surface == nullptr) {
                if (diagnostics != nullptr)
                    diagnose_paint_lookup(painted.sections[section], wall, normal, layer.print_z, *diagnostics);
                return reject(FinReject::contact_span, double(section));
            }
            // Horizontal is the fixed boundary. Upward-facing facets must not
            // influence a contact's tilt, even when the patch crosses a crease.
            if (surface->surface_normal.z() <= 0)
                normal_sum += surface->surface_normal;
        }
    }
    if (normal_sum.squaredNorm() <= EPSILON * EPSILON)
        return reject(FinReject::contact_normal);
    if (surface_normal != nullptr)
        *surface_normal = normal_sum.normalized();
    return true;
}

bool validate_fin_contact(const Layer &layer, const PaintedLayer &painted, const Polygons *blockers,
                          bool wall_present, const Vec2d &wall, const Vec2d &fin_normal,
                          double contact_width, double line_width, Vec3d &surface_normal,
                          FinDiagnostics &diagnostics, bool check_top_section = true)
{
    if (!wall_present)
        return diagnostics.reject(FinReject::missing_wall, layer.print_z);
    const PaintedSegment *surface = painted_contact(painted.sections[0], wall, fin_normal, line_width);
    if (surface == nullptr) {
        diagnose_paint_lookup(painted.sections[0], wall, fin_normal, layer.print_z, diagnostics);
        return diagnostics.reject(FinReject::missing_paint, layer.print_z);
    }
    if (blockers != nullptr && contains(*blockers, scaled_point(wall)))
        return diagnostics.reject(FinReject::blocked_contact, layer.print_z);
    if (!painted_contact_width(layer, painted, wall, surface->normal, contact_width,
                               line_width, &surface_normal, &diagnostics, check_top_section))
        return false;
    const Vec2d contact_normal = surface_normal.head<2>().normalized();
    if (!fin_contact_aligned(fin_normal, contact_normal)) {
        const double alignment = fin_normal.dot(contact_normal);
        return diagnostics.reject(FinReject::contact_turn, layer.print_z,
            std::acos(std::clamp(alignment, -1., 1.)) * 180 / M_PI);
    }
    return true;
}

bool finalize_fin_geometry(const PrintObject &object, TracedFin &fin,
                           std::vector<ExPolygons> &bodies, const std::vector<ExPolygons> &body_centers,
                           std::vector<ExPolygons> &contacts)
{
    const size_t count = bodies.size();
    fin.areas.resize(count);
    fin.centers.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const double width = support_material_flow(&object, float(object.get_layer(int(i))->height)).width();
        contacts[i] = union_ex(contacts[i]);
        fin.centers[i] = body_centers[i];
        expolygons_append(fin.centers[i], contacts[i]);
        fin.centers[i] = union_ex(fin.centers[i]);
        expolygons_append(bodies[i], offset_ex(contacts[i], float(scale_(0.5 * width)),
                                              jtRound, double(scale_(0.005))));
        ExPolygons area = union_ex(bodies[i]);
        if (area.size() != 1 || !area.front().holes.empty() || fin.centers[i].size() != 1 ||
            !fin.centers[i].front().holes.empty() || !fin.centers[i].front().contour.is_valid())
            return fin.diagnostics.reject(FinReject::topology, object.get_layer(int(i))->print_z);
        if (offset_ex(area, -float(scale_(width - EPSILON))).empty())
            return fin.diagnostics.reject(FinReject::thin_body, object.get_layer(int(i))->print_z);
        fin.areas[i] = std::move(area.front());
    }
    return true;
}

struct FinContactBand {
    size_t begin;
    size_t end;
};

std::vector<FinContactBand> periodic_fin_contact_bands(const PrintObject &object, const TracedFin &fin)
{
    const PrintObjectConfig &config = object.config();
    const double bed_z = object.get_layer(0)->print_z - object.get_layer(0)->height;
    const double first_top = bed_z + config.fin_support_contact_start_height.value;
    const double seed_top = object.get_layer(int(fin.seed.top_layer))->print_z;
    const double height = config.fin_support_contact_height.value;
    const double spacing = config.fin_support_contact_spacing.value;
    const size_t count = fin.seed.top_layer + 1;
    std::vector<FinContactBand> result;
    size_t layer_id = 0;
    for (size_t band_id = 0;; ++band_id) {
        const double top = first_top + double(band_id) * spacing;
        if (top >= seed_top - 1e-9)
            break;
        const double bottom = top - height;
        while (layer_id < count && object.get_layer(int(layer_id))->print_z <= bottom + 1e-9)
            ++layer_id;
        const size_t begin = layer_id;
        while (layer_id < count && object.get_layer(int(layer_id))->print_z <= top + 1e-9)
            ++layer_id;
        if (begin < layer_id)
            result.push_back({begin, layer_id});
    }
    return result;
}

FinContactBand seed_fin_contact_band(const PrintObject &object, const TracedFin &fin)
{
    const size_t end = fin.seed.top_layer + 1;
    const double top = object.get_layer(int(fin.seed.top_layer))->print_z;
    const double bottom = top - object.config().fin_support_contact_height.value;
    size_t begin = 0;
    while (begin < end && object.get_layer(int(begin))->print_z <= bottom + 1e-9)
        ++begin;
    return {begin, end};
}

bool generate_fin_geometry(const PrintObject &object, TracedFin &fin, const Polygon &bed,
                           const std::vector<PaintedLayer> &painted,
                           const std::vector<Polygons> &blockers)
{
    const PrintObjectConfig &config = object.config();
    const double thickness = fin.seed.dimensions.thickness;
    const double contact_width = fin.seed.dimensions.contact_width;
    const double gap = config.support_object_xy_distance.value;
    const size_t count = fin.boundary.size();
    std::vector<ExPolygons> bodies(count), body_centers(count), contacts(count);
    std::vector<ProjectionClip> clips(count);
    fin.contacts.assign(count, false);
    fin.contact_normals.assign(count, Vec3d::Zero());
    for (size_t i = 0; i < count; ++i)
        build_fin_body_section(object, fin, i, bed, thickness, gap, bodies[i], body_centers[i]);

    // 0: invalid painted band, 1: complete band, -1: fatal contact geometry.
    auto prepare_band = [&](const FinContactBand &band, std::vector<ExPolygons> &sections,
                            Vec3d &band_normal) {
        ++fin.planned_contact_bands;
        for (size_t i = band.begin; i < band.end; ++i) {
            const Layer &layer = *object.get_layer(int(i));
            const double line_width = support_material_flow(&object, float(layer.height)).width();
            const Polygons *layer_blockers = i < blockers.size() ? &blockers[i] : nullptr;
            if (!validate_fin_contact(layer, painted[i], layer_blockers, fin.wall_present[i],
                                      fin.boundary[i], fin.seed.normal, contact_width, line_width,
                                      fin.contact_normals[i], fin.diagnostics,
                                      i != fin.seed.top_layer))
                return 0;
        }
        sections.resize(band.end - band.begin);
        for (size_t i = band.begin; i < band.end; ++i) {
            const Layer &layer = *object.get_layer(int(i));
            const double line_width = support_material_flow(&object, float(layer.height)).width();
            const Vec2d contact_normal = fin.contact_normals[i].head<2>().normalized();
            sections[i - band.begin] = build_fin_contact_section(
                layer, bed, fin, i, contact_normal, contact_width, thickness, line_width);
            if (sections[i - band.begin].empty()) {
                fin.diagnostics.reject(FinReject::empty_neck, layer.print_z);
                return -1;
            }
        }
        band_normal = fin.contact_normals[band.begin];
        return 1;
    };
    auto append_band = [&](const FinContactBand &band, std::vector<ExPolygons> &sections,
                           std::vector<ExPolygons> *copy) {
        ++fin.accepted_contact_bands;
        for (size_t i = band.begin; i < band.end; ++i) {
            fin.contacts[i] = true;
            if (copy != nullptr)
                (*copy)[i].insert((*copy)[i].end(), sections[i - band.begin].begin(),
                                  sections[i - band.begin].end());
            expolygons_append(contacts[i], std::move(sections[i - band.begin]));
        }
    };

    // The seed contact is independent of the periodic bed-anchored schedule.
    // Build it first so its actual downward projection can suppress redundant
    // periodic contacts below it without regenerating either band.
    const FinContactBand seed_band = seed_fin_contact_band(object, fin);
    std::vector<ExPolygons> seed_sections;
    Vec3d seed_normal = Vec3d::Zero();
    const int seed_status = prepare_band(seed_band, seed_sections, seed_normal);
    if (seed_status < 0)
        return false;
    if (seed_status == 0)
        return fin.diagnostics.reject(FinReject::top_band,
                                      object.get_layer(int(fin.seed.top_layer))->print_z);
    std::vector<ExPolygons> seed_reach(count);
    append_band(seed_band, seed_sections, &seed_reach);
    if (seed_band.begin > 0)
        backfill_contact_projection(object, fin, seed_band.begin, bed, thickness, contact_width, gap,
                                    seed_normal, body_centers, contacts, clips, &seed_reach);

    const std::vector<FinContactBand> periodic_bands = periodic_fin_contact_bands(object, fin);
    for (auto band_it = periodic_bands.rbegin(); band_it != periodic_bands.rend(); ++band_it) {
        std::vector<ExPolygons> sections;
        Vec3d band_normal = Vec3d::Zero();
        const int status = prepare_band(*band_it, sections, band_normal);
        if (status < 0)
            return false;
        if (status == 0)
            continue;
        const size_t top_layer = band_it->end - 1;
        const Layer &layer = *object.get_layer(int(top_layer));
        const double line_width = support_material_flow(&object, float(layer.height)).width();
        const bool reaches_seed = !seed_reach[top_layer].empty() && !intersection_ex(
            offset_ex(sections.back(), float(scale_(0.5 * line_width))),
            offset_ex(seed_reach[top_layer], float(scale_(0.5 * line_width)))).empty();
        if (reaches_seed) {
            --fin.planned_contact_bands;
            ++fin.suppressed_contact_bands;
            continue;
        }
        append_band(*band_it, sections, nullptr);
        if (band_it->begin > 0)
            backfill_contact_projection(object, fin, band_it->begin, bed, thickness, contact_width, gap,
                                        band_normal, body_centers, contacts, clips);
    }
    if (!fin.contacts.back())
        return fin.diagnostics.reject(FinReject::top_band, object.get_layer(int(fin.seed.top_layer))->print_z);

    return finalize_fin_geometry(object, fin, bodies, body_centers, contacts);
}

bool generate_fin_toolpaths(const PrintObject &object, TracedFin &fin)
{
    const int wall_count = std::max(1, object.config().tree_support_wall_count.value);
    fin.outlines.resize(fin.areas.size());
    // The bed layer is generated from the union of all fin feet.
    for (size_t i = 1; i < fin.areas.size(); ++i) {
        const Layer &layer = *object.get_layer(int(i));
        const Flow flow = support_material_flow(&object, float(layer.height));
        for (int wall = 0; wall < wall_count; ++wall) {
            Polyline outline;
            if (!closed_outline(ExPolygons{fin.areas[i]}, wall == 0 ? fin.centers[i] : ExPolygons{}, fin.seed.normal,
                                0.5 * flow.width() + wall * flow.spacing(), flow.width(), outline))
                break;
            fin.outlines[i].push_back(std::move(outline));
        }
        if (fin.outlines[i].empty())
            return fin.diagnostics.reject(FinReject::no_outline, layer.print_z);
    }
    return true;
}

void generate_fin_foot_toolpaths(const PrintObject &object, const std::vector<TracedFin> &fins,
                                Polylines &paths, Polygons &fill)
{
    ExPolygons areas, centers;
    for (const TracedFin &fin : fins) {
        areas.push_back(fin.areas.front());
        if (!fin.centers.empty())
            expolygons_append(centers, fin.centers.front());
    }
    areas = union_ex(areas);
    const Layer &layer = *object.get_layer(0);
    const Flow flow = support_material_flow(&object, float(layer.height));
    // Erode the merged footprint, not each foot separately: this removes
    // internal seams even where individual center contours do not overlap.
    // Keep the saved contact centers so short fronts and zero-length tips survive.
    expolygons_append(centers, offset_ex(areas, -float(scale_(0.5 * flow.width()))));
    centers = union_ex(centers);
    const Lines model_edges = to_lines(layer.lslices);
    auto clearance = [&model_edges](const Point &point) {
        double distance = std::numeric_limits<double>::infinity();
        for (const Line &edge : model_edges)
            distance = std::min(distance, edge.distance_to_squared(point));
        return distance;
    };
    const int wall_count = std::max(1, object.config().tree_support_wall_count.value);
    for (int wall = 0; wall < wall_count; ++wall) {
        const double inset = 0.5 * flow.width() + wall * flow.spacing();
        if (offset_ex(areas, -float(scale_(inset + 0.5 * flow.width() - EPSILON))).empty())
            break;
        const ExPolygons loops = wall == 0 ? centers : offset_ex(areas, -float(scale_(inset)));
        for (Polygon contour : to_polygons(loops)) {
            if (!contour.is_valid())
                continue;
            auto start = std::max_element(contour.points.begin(), contour.points.end(),
                [&clearance](const Point &a, const Point &b) { return clearance(a) < clearance(b); });
            std::rotate(contour.points.begin(), start, contour.points.end());
            paths.push_back(contour.split_at_first_point());
        }
    }
    // Reserve the emitted walls once for the whole bed layer before support infill.
    fill = to_polygons(diff_ex(offset_ex(areas, -float(scale_(0.5 * flow.width()))),
                              offset(paths, float(scale_(0.5 * flow.spacing())))));
}

bool has_contact_normal(const PaintedLayer &painted)
{
    return std::any_of(painted.sections.begin(), painted.sections.end(), [](const auto &section) {
        return std::any_of(section.begin(), section.end(), [](const PaintedSegment &segment) {
            return segment.surface_normal.z() <= 0;
        });
    });
}

// Join coplanar painted cuts before scanning their XY angular intervals.
// Mesh triangulation must not split one usable interval into facet fragments.
std::vector<PaintedSegment> simulated_candidate_runs(const PaintedLayer &painted)
{
    std::vector<PaintedSegment> segments;
    if (!has_contact_normal(painted))
        return segments;
    // FinCandidate XY is defined at the model slice height. Keep candidate
    // discovery on the same section as layer.lslices and final wall lookup.
    for (PaintedSegment segment : painted.sections[0]) {
        const Vec2d tangent(-segment.normal.y(), segment.normal.x());
        if (segment.a.dot(tangent) > segment.b.dot(tangent))
            std::swap(segment.a, segment.b);
        segments.push_back(std::move(segment));
    }
    std::sort(segments.begin(), segments.end(), [](const PaintedSegment &a, const PaintedSegment &b) {
        const double angle_a = std::atan2(a.normal.y(), a.normal.x());
        const double angle_b = std::atan2(b.normal.y(), b.normal.x());
        if (angle_a != angle_b)
            return angle_a < angle_b;
        const double offset_a = a.a.dot(a.normal), offset_b = b.a.dot(b.normal);
        if (offset_a != offset_b)
            return offset_a < offset_b;
        const Vec2d tangent(-a.normal.y(), a.normal.x());
        return a.a.dot(tangent) < b.a.dot(tangent);
    });
    std::vector<PaintedSegment> runs;
    for (const PaintedSegment &segment : segments) {
        if (!runs.empty()) {
            PaintedSegment &run = runs.back();
            const Vec2d tangent(-run.normal.y(), run.normal.x());
            if ((run.normal - segment.normal).squaredNorm() < 1e-10 &&
                std::abs((segment.a - run.a).dot(run.normal)) < 1e-4 &&
                segment.a.dot(tangent) <= run.b.dot(tangent) + 1e-4) {
                if (segment.b.dot(tangent) > run.b.dot(tangent))
                    run.b = segment.b;
                continue;
            }
        }
        runs.push_back(segment);
    }
    return runs;
}

constexpr double fin_direction_merge_alignment = 0.96592582628906828675; // cos(15 degrees).

// Rank locations on a painted run against the entire sliced face. Painting
// limits where a seed is allowed; it does not define the face's center.
void locate_fin_face(const Layer &layer, PaintedSegment &run, double tolerance)
{
    const Vec2d tangent(-run.normal.y(), run.normal.x());
    const double run_min = std::min(run.a.dot(tangent), run.b.dot(tangent));
    const double run_max = std::max(run.a.dot(tangent), run.b.dot(tangent));
    double best_span = 0;
    double best_midpoint = 0.5 * (run_min + run_max);
    for (const ExPolygon &slice : layer.lslices) {
        const Points &contour = slice.contour.points;
        double face_min = std::numeric_limits<double>::infinity();
        double face_max = -std::numeric_limits<double>::infinity();
        bool touches_run = false;
        for (size_t i = 0; i < contour.size(); ++i) {
            const Vec2d a = unscale(contour[i]);
            const Vec2d b = unscale(contour[(i + 1) % contour.size()]);
            const Vec2d edge = b - a;
            if (edge.norm() <= EPSILON ||
                std::abs(edge.normalized().dot(tangent)) < fin_direction_merge_alignment ||
                std::abs((a - run.a).dot(run.normal)) > tolerance ||
                std::abs((b - run.a).dot(run.normal)) > tolerance)
                continue;
            const double low = std::min(a.dot(tangent), b.dot(tangent));
            const double high = std::max(a.dot(tangent), b.dot(tangent));
            touches_run |= high >= run_min - tolerance && low <= run_max + tolerance;
            face_min = std::min(face_min, low);
            face_max = std::max(face_max, high);
        }
        if (touches_run && face_max - face_min > best_span) {
            best_span = face_max - face_min;
            best_midpoint = 0.5 * (face_min + face_max);
        }
    }
    run.face_span = std::max(best_span, run_max - run_min);
    run.face_center = run.a + (best_midpoint - run.a.dot(tangent)) * tangent;
}

struct FinDemand {
    Vec2d direction;
    double initial;
    double remaining;
    bool blocked = false;
};

// Intersect a forward XY ray with a painted cut. A determines orientation;
// the intersection supplies the seed's XY and the cut's layer supplies Z.
bool fin_ray_contact(const Vec2d &origin, const Vec2d &direction,
                     const Vec2d &a, const Vec2d &b, Vec2d &point)
{
    const Vec2d edge = b - a;
    const double denominator = direction.x() * edge.y() - direction.y() * edge.x();
    if (std::abs(denominator) <= EPSILON)
        return false;
    const Vec2d delta = a - origin;
    const double distance = (delta.x() * edge.y() - delta.y() * edge.x()) / denominator;
    const double fraction = (delta.x() * direction.y() - delta.y() * direction.x()) / denominator;
    if (distance <= EPSILON || fraction < -1e-9 || fraction > 1 + 1e-9)
        return false;
    point = origin + distance * direction;
    return true;
}

struct FinPlan {
    FinCandidate seed;
    FinCandidate initial_seed;
    bool active = true;
    double farthest_along_sheet = 0;
    std::vector<FinCandidate> tops;
};

// A's plane stays fixed while its top follows the painted wall. Moving along A
// changes depth, not the sheet plane or its previously established contacts.
bool fin_line_contact(const Vec2d &origin, const Vec2d &direction,
                      const PaintedSegment &run, Vec2d &point)
{
    const Vec2d edge = run.b - run.a;
    const double denominator = direction.x() * edge.y() - direction.y() * edge.x();
    if (std::abs(denominator) <= EPSILON)
        return false;
    const Vec2d delta = run.a - origin;
    const double fraction = (delta.x() * direction.y() - delta.y() * direction.x()) / denominator;
    if (fraction < -1e-9 || fraction > 1 + 1e-9)
        return false;
    const double distance = (delta.x() * edge.y() - delta.y() * edge.x()) / denominator;
    point = origin + distance * direction;
    return true;
}

const PrintRegion *fin_wall_region(const Layer &layer, const Vec2d &wall, const Vec2d &normal, double width)
{
    const Point inside = scaled_point(wall - 0.25 * width * normal);
    for (const LayerRegion *region : layer.regions())
        if (std::any_of(region->slices.surfaces.begin(), region->slices.surfaces.end(),
            [&inside](const Surface &surface) { return surface.expolygon.contains(inside); }))
            return &region->region();
    return nullptr;
}

bool fin_seed_contact(const PrintObject &object, const PaintedLayer &painted,
                      const std::vector<Polygons> &blockers, size_t layer_id,
                      const Vec2d &point, const Vec2d &direction, const PaintedSegment &run,
                      FinSupportDimensions &dimensions, Vec3d &normal, FinDiagnostics &diagnostics)
{
    const Layer &layer = *object.get_layer(int(layer_id));
    const double width = support_material_flow(&object).width();
    Vec2d wall = point;
    if (!projected_outer_contour(layer, point, direction, 2 * width, wall) || (wall - point).norm() > width)
        return diagnostics.reject(FinReject::top_wall, layer.print_z);
    const PrintRegion *region = fin_wall_region(layer, wall, run.normal, width);
    if (region == nullptr)
        return diagnostics.reject(FinReject::seed_region, layer.print_z);
    dimensions = fin_support_dimensions(object, *region);
    const Polygons *layer_blockers = layer_id < blockers.size() ? &blockers[layer_id] : nullptr;
    return validate_fin_contact(layer, painted, layer_blockers, true, wall, direction,
                                dimensions.contact_width, width, normal, diagnostics, false);
}

bool select_simulated_fin_candidate(const PrintObject &object, const std::vector<PaintedLayer> &painted,
                                    const std::vector<Polygons> &blockers,
                                    const std::vector<PaintedSegment> &runs,
                                    size_t layer_id, const Vec2d &origin, const Vec2d &desired, double risk,
                                    const std::vector<FinPlan> &plans,
                                    const std::vector<FinCandidate> &rejected,
                                    FinDiagnostics &diagnostics, FinCandidate &result)
{
    struct Site { Vec2d point; Vec2d direction; const PaintedSegment *run; double alignment; double position_error; };
    std::vector<Site> sites;
    const double width = support_material_flow(&object).width();
    for (const PaintedSegment &run : runs) {
        const Vec2d edge = run.b - run.a;
        if (edge.norm() <= EPSILON)
            continue;
        // Choose A once for the painted run. Using centroid-to-site for every
        // alternative makes sheets on one flat face fan out radially.
        const double turn = std::atan2(desired.x() * run.normal.y() - desired.y() * run.normal.x(),
                                       desired.dot(run.normal));
        if (std::abs(turn) > M_PI / 3. + 1e-9)
            continue;
        const double correction = std::copysign(std::max(0., std::abs(turn) - M_PI / 6.), turn);
        const Vec2d direction(std::cos(correction) * desired.x() - std::sin(correction) * desired.y(),
                              std::sin(correction) * desired.x() + std::cos(correction) * desired.y());
        const double alignment = direction.dot(desired);
        std::vector<Vec2d> points;
        Vec2d direct;
        if (fin_ray_contact(origin, direction, run.a, run.b, direct))
            points.push_back(direct);
        const double inset = std::min(0.5, 0.5 * width / edge.norm());
        const double middle = std::clamp((run.face_center - run.a).dot(edge) / edge.squaredNorm(),
                                          inset, 1 - inset);
        points.push_back(run.a + middle * edge);
        // Other sites let the same layer add another sheet after the nearest
        // one has reduced demand. These are alternatives, not direction bins.
        for (double fraction : {0.25, 0.5, 0.75})
            points.push_back(run.a + fraction * edge);
        for (const Vec2d &point : points) {
            if ((point - origin).dot(direction) > EPSILON &&
                alignment >= min_fin_contact_alignment - 1e-9) {
                const Vec2d side(-direction.y(), direction.x());
                const Vec2d face_side(-run.normal.y(), run.normal.x());
                const double face_weight = std::clamp(run.face_span /
                    (2 * object.config().fin_support_max_spacing.value), 0., 1.);
                const double position_error = (1 - face_weight) * std::abs((point - origin).dot(side)) +
                    face_weight * std::abs((point - run.face_center).dot(face_side));
                sites.push_back({point, direction, &run, alignment, position_error});
            }
        }
    }
    std::stable_sort(sites.begin(), sites.end(), [](const Site &a, const Site &b) {
        if (a.alignment != b.alignment)
            return a.alignment > b.alignment;
        return a.position_error < b.position_error;
    });
    for (const Site &site : sites) {
        if (object.print()->canceled())
            throw CanceledException();
        if (std::any_of(plans.begin(), plans.end(), [&](const FinPlan &plan) {
                const Vec2d side(-plan.seed.normal.y(), plan.seed.normal.x());
                const Vec2d delta = site.point - plan.seed.point;
                const double spacing = std::max(0.5 * (plan.seed.dimensions.thickness + width),
                                                std::min(object.config().fin_support_max_spacing.value,
                                                         0.5 * (site.run->b - site.run->a).norm()));
                return plan.seed.normal.dot(site.direction) >= fin_direction_merge_alignment &&
                       plan.seed.contact_normal.dot(site.run->normal) >= fin_direction_merge_alignment &&
                       std::abs(delta.dot(side)) < spacing &&
                       std::abs(delta.dot(plan.seed.normal)) < object.config().fin_support_depth.value + width;
            }) || std::any_of(rejected.begin(), rejected.end(), [&](const FinCandidate &failed) {
                return failed.top_layer == layer_id && (failed.point - site.point).norm() < width &&
                       failed.normal.dot(site.direction) > min_fin_contact_alignment;
            }))
            continue;
        FinSupportDimensions dimensions;
        Vec3d surface_normal;
        if (!fin_seed_contact(object, painted[layer_id], blockers, layer_id, site.point, site.direction,
                              *site.run, dimensions, surface_normal, diagnostics))
            continue;
        const Vec2d contact_normal = surface_normal.head<2>().normalized();
        const double lean = -surface_normal.z() / surface_normal.head<2>().norm() *
                            std::max(0., contact_normal.dot(site.direction));
        const double weight = 1 - std::clamp(std::atan(lean) / (M_PI / 6.), 0., 1.);
        result = {site.point, site.direction, contact_normal, layer_id, dimensions,
                  object.config().fin_support_expansion_factor.value * risk * weight * weight};
        return true;
    }
    return diagnostics.reject(FinReject::direction, object.get_layer(int(layer_id))->print_z);
}

bool extend_fin_plan(const PrintObject &object, const std::vector<PaintedLayer> &painted,
                     const std::vector<Polygons> &blockers, const std::vector<PaintedSegment> &runs,
                     size_t layer_id, double risk, FinPlan &plan, FinDiagnostics &diagnostics)
{
    const Layer &layer = *object.get_layer(int(layer_id));
    const double width = support_material_flow(&object).width();
    for (const PaintedSegment &run : runs) {
        Vec2d point;
        if (!fin_line_contact(plan.seed.point, plan.seed.normal, run, point) ||
            (point - plan.seed.point).norm() > std::max(2 * width, 4 * layer.height) ||
            point.dot(plan.seed.normal) < plan.farthest_along_sheet - 0.5 * width)
            continue;
        FinSupportDimensions dimensions;
        Vec3d surface_normal;
        if (!fin_seed_contact(object, painted[layer_id], blockers, layer_id, point, plan.seed.normal,
                              run, dimensions, surface_normal, diagnostics))
            continue;
        const Vec2d normal = surface_normal.head<2>().normalized();
        const double lean = -surface_normal.z() / surface_normal.head<2>().norm() *
                            std::max(0., normal.dot(plan.seed.normal));
        const double weight = 1 - std::clamp(std::atan(lean) / (M_PI / 6.), 0., 1.);
        plan.farthest_along_sheet = std::max(plan.farthest_along_sheet, point.dot(plan.seed.normal));
        plan.seed.point = point;
        plan.seed.top_layer = layer_id;
        plan.seed.contact_normal = normal;
        plan.seed.expansion_slope = object.config().fin_support_expansion_factor.value * risk * weight * weight;
        plan.tops.push_back(plan.seed);
        return true;
    }
    return false;
}

// A planning score. Exact contact capacity is still measured from the completed
// polygons, but no polygon is rebuilt on every layer of the upward sweep.
double planned_fin_capacity(const PrintObject &object, const FinPlan &plan, size_t layer_id, double bed_z)
{
    const double top = object.get_layer(int(plan.seed.top_layer))->print_z - bed_z;
    const double current = object.get_layer(int(layer_id))->print_z - bed_z;
    const double neck = object.config().support_object_xy_distance.value + 0.5 * plan.seed.dimensions.thickness;
    return std::pow(top / std::max(current, EPSILON), 2) * plan.seed.dimensions.thickness / 1.2 /
           (1 + neck / object.config().fin_support_max_spacing.value);
}

// A taller fin may replace a nearby lower one only if it preserves the lower
// contact schedule and at least the lower fin's reach at the bed.
bool fin_supersedes(const PrintObject &object, const TracedFin &higher, const TracedFin &lower)
{
    if (higher.seed.top_layer <= lower.seed.top_layer ||
        higher.seed.normal.dot(lower.seed.normal) < fin_direction_merge_alignment ||
        higher.seed.contact_normal.dot(lower.seed.contact_normal) < min_fin_contact_alignment ||
        higher.seed.dimensions.thickness + EPSILON < lower.seed.dimensions.thickness)
        return false;
    const Vec2d side(-lower.seed.normal.y(), lower.seed.normal.x());
    const double separation = std::abs((higher.seed.point - lower.seed.point).dot(side));
    if (separation > std::min(object.config().fin_support_max_spacing.value,
                              4 * lower.seed.dimensions.thickness) ||
        higher.tips.front().dot(lower.seed.normal) + support_material_flow(&object).width() <
            lower.tips.front().dot(lower.seed.normal))
        return false;
    auto contact_bands = [&object](const TracedFin &fin) {
        std::vector<double> centers;
        for (size_t i = 0; i < fin.contacts.size();) {
            if (!fin.contacts[i]) {
                ++i;
                continue;
            }
            const size_t first = i;
            while (i < fin.contacts.size() && fin.contacts[i])
                ++i;
            centers.push_back(0.5 * (object.get_layer(int(first))->print_z +
                                     object.get_layer(int(i - 1))->print_z));
        }
        return centers;
    };
    const double contact_window = object.config().fin_support_contact_spacing.value;
    const std::vector<double> lower_bands = contact_bands(lower);
    const std::vector<double> higher_bands = contact_bands(higher);
    size_t j = 0;
    for (double z : lower_bands) {
        while (j < higher_bands.size() && higher_bands[j] < z - contact_window)
            ++j;
        if (j == higher_bands.size() || higher_bands[j] > z)
            return false;
        ++j;
    }
    return true;
}

bool overlaps_fin(const TracedFin &candidate, const std::vector<TracedFin> &existing,
                   size_t &overlap_layer, const std::vector<size_t> &superseded = {})
{
    for (size_t fin_id = 0; fin_id < existing.size(); ++fin_id) {
        if (std::find(superseded.begin(), superseded.end(), fin_id) != superseded.end())
            continue;
        const TracedFin &fin = existing[fin_id];
        const size_t count = std::min(candidate.areas.size(), fin.areas.size());
        // Feet are deliberately united on the bed. Above it, overlapping
        // geometry would emit duplicate or crossing extrusion paths.
        for (size_t i = 1; i < count; ++i) {
            if (!candidate.area_bounds[i].overlap(fin.area_bounds[i]))
                continue;
            const ExPolygons overlap = intersection_ex(ExPolygons{candidate.areas[i]}, ExPolygons{fin.areas[i]});
            if (std::any_of(overlap.begin(), overlap.end(), [](const ExPolygon &area) {
                    return area.area() * SCALING_FACTOR * SCALING_FACTOR > EPSILON * EPSILON;
                })) {
                overlap_layer = i;
                return true;
            }
        }
    }
    return false;
}

struct FinPlacement {
    std::vector<size_t> superseded;
    std::optional<size_t> overlap_layer;
};

FinPlacement evaluate_fin_placement(const PrintObject &object, const TracedFin &candidate,
                                    const std::vector<TracedFin> &existing)
{
    FinPlacement result;
    for (size_t i = 0; i < existing.size(); ++i)
        if (fin_supersedes(object, candidate, existing[i]))
            result.superseded.push_back(i);
    size_t overlap_layer = 0;
    if (overlaps_fin(candidate, existing, overlap_layer, result.superseded))
        result.overlap_layer = overlap_layer;
    return result;
}

void cache_fin_bounds(TracedFin &fin)
{
    fin.area_bounds.clear();
    fin.area_bounds.reserve(fin.areas.size());
    for (const ExPolygon &area : fin.areas)
        fin.area_bounds.push_back(get_extents(area));
}

bool realize_fin_candidate(const PrintObject &object, const FinCandidate &candidate,
                           const Polygon &bed, const std::vector<PaintedLayer> &painted,
                           const std::vector<Polygons> &blockers, double line_width,
                           TracedFin &fin)
{
    fin = TracedFin{};
    fin.seed = candidate;
    if (!trace_fin(object, bed, candidate, object.config().fin_support_depth.value,
                   object.config().support_object_xy_distance.value,
                   candidate.dimensions.thickness, line_width, fin) ||
        !generate_fin_geometry(object, fin, bed, painted, blockers))
        return false;
    cache_fin_bounds(fin);
    return true;
}

struct SimpleFinSpan {
    size_t run;
    double begin;
    double end;
};

struct SimpleFinAnchor {
    double position;
    double score;
};

struct SimpleFinContour {
    std::vector<Linef> edges;
    std::vector<Vec2d> normals;
    std::vector<double> offsets;
    double length = 0.;
};

struct SimpleFinRunLocation {
    size_t contour;
    double begin;
    double end;
    double contact_span;
};

struct SimpleCandidateLayer {
    std::vector<SimpleFinContour> contours;
    std::vector<PaintedSegment> runs;
    std::vector<SimpleFinRunLocation> run_locations;
    std::vector<SimpleFinSpan> seed_ranges;
    std::vector<std::vector<SimpleFinAnchor>> anchors;
    size_t painted_cuts = 0;
    size_t matched_intervals = 0;
    size_t joined_intervals = 0;
    size_t short_contact_intervals = 0;
    double longest_painted_cut = 0.;
    double painted_extent = 0.;
    double best_contact_line = 0.;
    double best_required_contact = 0.;
    double best_contact_slack = -std::numeric_limits<double>::infinity();
};

struct SimpleFinFeature {
    Vec2d point;
    Vec2d normal;
    double score;
};

// Paint grants permission, but candidate coordinates must live on the actual
// sliced outer contour rather than on a facet cut spanning a convex envelope.
SimpleCandidateLayer build_simple_candidate_space(const PrintObject &object, const Layer &layer,
                                                  const std::vector<PaintedSegment> &painted_cuts,
                                                  double tolerance)
{
    SimpleCandidateLayer result;
    result.painted_cuts = painted_cuts.size();
    if (painted_cuts.empty())
        return result;
    std::vector<Linef> edges;
    std::vector<Vec2d> normals;
    std::vector<SimpleFinRunLocation> edge_locations;
    for (const ExPolygon &slice : layer.lslices) {
        const size_t contour_id = result.contours.size();
        SimpleFinContour &outline = result.contours.emplace_back();
        const Points &contour = slice.contour.points;
        const double orientation = slice.contour.area() >= 0 ? 1. : -1.;
        for (size_t i = 0; i < contour.size(); ++i) {
            const Vec2d a = unscale(contour[i]);
            const Vec2d b = unscale(contour[(i + 1) % contour.size()]);
            const Vec2d edge = b - a;
            if (edge.squaredNorm() <= EPSILON * EPSILON)
                continue;
            edges.emplace_back(a, b);
            const Vec2d normal = orientation * Vec2d(edge.y(), -edge.x()).normalized();
            normals.push_back(normal);
            edge_locations.push_back({contour_id, outline.length, outline.length + edge.norm()});
            outline.edges.emplace_back(a, b);
            outline.normals.push_back(normal);
            outline.offsets.push_back(outline.length);
            outline.length += edge.norm();
        }
    }
    struct EdgeInterval {
        double begin;
        double end;
        Vec3d surface_normal;
    };
    std::vector<std::vector<EdgeInterval>> allowed(edges.size());
    const auto edge_tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(edges);
    Vec2d painted_min = Vec2d::Constant(std::numeric_limits<double>::infinity());
    Vec2d painted_max = Vec2d::Constant(-std::numeric_limits<double>::infinity());
    auto clip = [](double value, double change, double low, double high, double &begin, double &end) {
        if (std::abs(change) <= EPSILON)
            return value >= low && value <= high;
        double first = (low - value) / change;
        double last = (high - value) / change;
        if (first > last)
            std::swap(first, last);
        begin = std::max(begin, first);
        end = std::min(end, last);
        return end > begin + 1e-9;
    };
    for (const PaintedSegment &run : painted_cuts) {
        painted_min = painted_min.cwiseMin(run.a).cwiseMin(run.b);
        painted_max = painted_max.cwiseMax(run.a).cwiseMax(run.b);
        const Vec2d direction = run.b - run.a;
        const double length_squared = direction.squaredNorm();
        result.longest_painted_cut = std::max(result.longest_painted_cut, std::sqrt(length_squared));
        if (length_squared <= EPSILON * EPSILON)
            continue;
        AABBTreeIndirect::Tree<2, double>::BoundingBox query;
        query.extend(run.a);
        query.extend(run.b);
        query.min() -= Vec2d::Constant(tolerance);
        query.max() += Vec2d::Constant(tolerance);
        AABBTreeIndirect::traverse(edge_tree, AABBTreeIndirect::intersecting(query), [&](const auto &node) {
                if (normals[node.idx].dot(run.normal) < min_fin_contact_alignment)
                    return true;
                const Vec2d &a = edges[node.idx].a;
                const Vec2d &b = edges[node.idx].b;
                const Vec2d edge = b - a;
                double edge_begin = 0., edge_end = 1.;
                if (clip((a - run.a).dot(run.normal), edge.dot(run.normal),
                         -tolerance, tolerance, edge_begin, edge_end) &&
                    clip((a - run.a).dot(direction) / length_squared,
                         edge.dot(direction) / length_squared, 0., 1., edge_begin, edge_end))
                    allowed[node.idx].push_back({edge_begin, edge_end, run.surface_normal});
                return true;
            });
    }
    result.painted_extent = (painted_max - painted_min).norm();
    for (size_t i = 0; i < edges.size(); ++i) {
        auto &intervals = allowed[i];
        std::sort(intervals.begin(), intervals.end(), [](const EdgeInterval &a, const EdgeInterval &b) {
            return a.begin < b.begin;
        });
        for (size_t j = 0; j < intervals.size();) {
            const double begin = intervals[j].begin;
            double end = intervals[j].end;
            const Vec3d surface_normal = intervals[j].surface_normal;
            while (++j < intervals.size() && intervals[j].begin <= end + 1e-5)
                end = std::max(end, intervals[j].end);
            if ((end - begin) * edges[i].length() <= EPSILON)
                continue;
            ++result.matched_intervals;
            const Vec2d edge = edges[i].b - edges[i].a;
            const Vec2d midpoint = edges[i].a + 0.5 * (begin + end) * edge;
            const PrintRegion *region = fin_wall_region(layer, midpoint, normals[i], tolerance);
            const FinSupportDimensions dimensions = fin_support_dimensions(object,
                region != nullptr ? *region : object.printing_region(0));
            result.runs.push_back({edges[i].a + begin * edge, edges[i].a + end * edge,
                                   normals[i], surface_normal});
            result.run_locations.push_back({edge_locations[i].contour,
                edge_locations[i].begin + begin * edge.norm(),
                edge_locations[i].begin + end * edge.norm(), dimensions.contact_width + tolerance});
        }
    }
    // The contact may slide across consecutive painted edges of one island.
    // Split at a turn that moves the straight contact beyond half a line width.
    constexpr double joined_gap = 1e-3;
    for (size_t contour_id = 0; contour_id < result.contours.size(); ++contour_id) {
        std::vector<size_t> runs;
        for (size_t i = 0; i < result.run_locations.size(); ++i)
            if (result.run_locations[i].contour == contour_id)
                runs.push_back(i);
        if (runs.empty())
            continue;
        const double perimeter = result.contours[contour_id].length;
        for (size_t pos = 0; pos < runs.size(); ++pos) {
            const size_t run_id = runs[pos];
            const SimpleFinRunLocation &location = result.run_locations[run_id];
            const double max_turn = std::min(M_PI / 12.,
                std::asin(std::min(1., tolerance / location.contact_span)));
            const double joined_normal_dot = std::cos(max_turn);
            double line_begin = location.begin, line_end = location.end;
            size_t current = pos;
            double shift = 0.;
            for (size_t step = 1; step < runs.size(); ++step) {
                const size_t previous = (current + runs.size() - 1) % runs.size();
                if (previous > current)
                    shift -= perimeter;
                const SimpleFinRunLocation &other = result.run_locations[runs[previous]];
                if (std::abs(other.end + shift - line_begin) > joined_gap ||
                    result.runs[runs[previous]].normal.dot(result.runs[runs[current]].normal) < joined_normal_dot ||
                    result.runs[runs[previous]].normal.dot(result.runs[run_id].normal) < joined_normal_dot)
                    break;
                line_begin = other.begin + shift;
                current = previous;
            }
            current = pos;
            shift = 0.;
            for (size_t step = 1; step < runs.size(); ++step) {
                const size_t next = (current + 1) % runs.size();
                if (next < current)
                    shift += perimeter;
                const SimpleFinRunLocation &other = result.run_locations[runs[next]];
                if (std::abs(other.begin + shift - line_end) > joined_gap ||
                    result.runs[runs[next]].normal.dot(result.runs[runs[current]].normal) < joined_normal_dot ||
                    result.runs[runs[next]].normal.dot(result.runs[run_id].normal) < joined_normal_dot)
                    break;
                line_end = other.end + shift;
                current = next;
            }
            result.joined_intervals += line_begin < location.begin - 1e-9 ||
                                       line_end > location.end + 1e-9;
            const double line_length = line_end - line_begin;
            const double slack = line_length - location.contact_span;
            if (slack > result.best_contact_slack) {
                result.best_contact_slack = slack;
                result.best_contact_line = line_length;
                result.best_required_contact = location.contact_span;
            }
            const double half_contact = 0.5 * location.contact_span;
            const double begin = std::max(location.begin, line_begin + half_contact);
            const double end = std::min(location.end, line_end - half_contact);
            if (end <= begin + 1e-9) {
                ++result.short_contact_intervals;
                continue;
            }
            const double length = location.end - location.begin;
            result.seed_ranges.push_back({run_id, (begin - location.begin) / length,
                                          (end - location.begin) / length});
        }
    }
    return result;
}

// Paint only grants permission to attach. Rank sites using the complete model
// contour, so a paint boundary or a triangulated facet cannot become a corner.
std::vector<SimpleFinFeature> simple_fin_features(const Layer &layer, double width, double spacing)
{
    std::vector<SimpleFinFeature> features;
    for (const ExPolygon &slice : layer.lslices) {
        const Polygon &contour = slice.contour;
        const double perimeter = unscale<double>(contour.length());
        const double context = std::max(2 * width, std::min(0.5 * spacing, 0.05 * perimeter));
        Polygon oriented = contour;
        if (oriented.area() < 0)
            std::reverse(oriented.points.begin(), oriented.points.end());
        for (const Polygon &smoothed : oriented.simplify(scale_(std::max(width, 0.25 * context)))) {
            const Points &points = smoothed.points;
            if (points.size() < 3)
                continue;
            const double orientation = smoothed.area() >= 0 ? 1. : -1.;
            for (size_t i = 0; i < points.size(); ++i) {
                const Vec2d before = unscale(points[(i + points.size() - 1) % points.size()]);
                const Vec2d at = unscale(points[i]);
                const Vec2d after = unscale(points[(i + 1) % points.size()]);
                const Vec2d incoming = at - before, outgoing = after - at;
                const double first = incoming.norm(), second = outgoing.norm();
                if (first <= EPSILON || second <= EPSILON)
                    continue;
                const double cross = orientation * (incoming.x() * outgoing.y() - incoming.y() * outgoing.x());
                const double turn = std::atan2(cross, incoming.dot(outgoing));
                if (turn >= M_PI / 6.) {
                    const double size = std::min(1., std::min(first, second) / context);
                    const double score = 1.3 + size * turn / (M_PI / 2.);
                    features.push_back({at, orientation * Vec2d(incoming.y(), -incoming.x()) / first, score});
                    features.push_back({at, orientation * Vec2d(outgoing.y(), -outgoing.x()) / second, score});
                }
                if (second >= context)
                    features.push_back({0.5 * (at + after),
                                        orientation * Vec2d(outgoing.y(), -outgoing.x()) / second,
                                        1. + std::min(1., second / (4 * context))});
            }
        }
    }
    return features;
}

void prepare_simple_fin_anchors(SimpleCandidateLayer &layer, const Layer &model_layer,
                                double width, double spacing)
{
    if (!layer.anchors.empty())
        return;
    layer.anchors.resize(layer.runs.size());
    const double tolerance = std::max(width, 0.05 * spacing);
    std::vector<Linef> runs;
    runs.reserve(layer.runs.size());
    for (const PaintedSegment &run : layer.runs)
        runs.emplace_back(run.a, run.b);
    const auto run_tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(runs);
    for (const SimpleFinFeature &feature : simple_fin_features(model_layer, width, spacing)) {
        AABBTreeIndirect::Tree<2, double>::BoundingBox query;
        query.extend(feature.point - Vec2d::Constant(tolerance));
        query.extend(feature.point + Vec2d::Constant(tolerance));
        AABBTreeIndirect::traverse(run_tree, AABBTreeIndirect::intersecting(query), [&](const auto &node) {
            const size_t i = node.idx;
            const PaintedSegment &run = layer.runs[i];
            if (feature.normal.dot(run.normal) < min_fin_contact_alignment)
                return true;
            const Vec2d edge = run.b - run.a;
            if (edge.squaredNorm() <= EPSILON * EPSILON)
                return true;
            const double position = (feature.point - run.a).dot(edge) / edge.squaredNorm();
            if (position < 0. || position > 1.)
                return true;
            const double distance = (feature.point - run.a - position * edge).norm();
            if (distance <= tolerance)
                layer.anchors[i].push_back({position, feature.score - 0.2 * distance / tolerance});
            return true;
        });
    }
}

struct SimpleFinSite {
    Vec2d point;
    size_t layer;
    size_t run;
};

SimpleFinSite select_simple_fin_site(const SimpleCandidateLayer &layer, size_t layer_id, double width, double spacing)
{
    SimpleFinSite best{Vec2d::Zero(), layer_id, 0};
    double best_score = -1;
    for (const SimpleFinSpan &span : layer.seed_ranges) {
        const PaintedSegment &run = layer.runs[span.run];
        const Vec2d edge = run.b - run.a;
        const double length = edge.norm();
        const double remaining = (span.end - span.begin) * length;
        // The fallback midpoint belongs to the remaining paint interval, but
        // an unpainted model corner never gains priority from that boundary.
        const double fallback = 0.25 + 0.25 * std::min(1., remaining / spacing);
        if (fallback > best_score) {
            best = {run.a + 0.5 * (span.begin + span.end) * edge, layer_id, span.run};
            best_score = fallback;
        }
        for (const SimpleFinAnchor &anchor : layer.anchors[span.run]) {
            if (anchor.position < span.begin || anchor.position > span.end || anchor.score <= best_score)
                continue;
            const double inset = std::min(width / length, 0.5 * (span.end - span.begin));
            const double position = std::clamp(anchor.position, span.begin + inset, span.end - inset);
            best = {run.a + position * edge, layer_id, span.run};
            best_score = anchor.score;
        }
    }
    return best;
}

// Subtract the unavailable range connected to the wall intersection. Nearby
// edges on another island or across a fold do not belong to this range.
std::optional<Vec2d> subtract_simple_fin_contour_range(SimpleCandidateLayer &layer, const Vec2d &center,
                                                        const Vec2d &facing, double radius, double max_anchor_distance,
                                                        std::optional<size_t> required_contour = std::nullopt)
{
    size_t contour_id = 0, edge_id = 0;
    double fraction = 0., nearest_squared = std::min(radius * radius,
                                                    max_anchor_distance * max_anchor_distance);
    bool found = false;
    for (size_t c = 0; c < layer.contours.size(); ++c) {
        if (required_contour && c != *required_contour)
            continue;
        const SimpleFinContour &contour = layer.contours[c];
        for (size_t e = 0; e < contour.edges.size(); ++e) {
            if (contour.normals[e].dot(facing) <= 0.)
                continue;
            const Vec2d edge = contour.edges[e].b - contour.edges[e].a;
            const double t = std::clamp((center - contour.edges[e].a).dot(edge) / edge.squaredNorm(), 0., 1.);
            const double distance_squared = (center - contour.edges[e].a - t * edge).squaredNorm();
            if (distance_squared < nearest_squared) {
                contour_id = c;
                edge_id = e;
                fraction = t;
                nearest_squared = distance_squared;
                found = true;
            }
        }
    }
    if (!found)
        return std::nullopt;

    const SimpleFinContour &contour = layer.contours[contour_id];
    const Vec2d edge = contour.edges[edge_id].b - contour.edges[edge_id].a;
    const Vec2d intersection = contour.edges[edge_id].a + fraction * edge;
    const double reach = std::sqrt(radius * radius - nearest_squared);
    const double position = contour.offsets[edge_id] + fraction * contour.edges[edge_id].length();
    auto boundary = [&](int direction) {
        size_t edge = edge_id;
        double offset = position;
        double remaining = reach;
        for (size_t visited = 0; visited < contour.edges.size(); ++visited) {
            if (contour.normals[edge].dot(facing) <= 0.)
                break;
            const double edge_begin = contour.offsets[edge];
            const double edge_end = edge_begin + contour.edges[edge].length();
            const double available = direction > 0 ? edge_end - offset : offset - edge_begin;
            const double step = std::min(remaining, available);
            offset += direction * step;
            remaining -= step;
            if (remaining <= 1e-9)
                break;
            edge = direction > 0 ? (edge + 1) % contour.edges.size() :
                                   (edge + contour.edges.size() - 1) % contour.edges.size();
            offset = direction > 0 ? contour.offsets[edge] :
                                     contour.offsets[edge] + contour.edges[edge].length();
        }
        return position + direction * (reach - remaining);
    };
    const double unavailable_begin = boundary(-1);
    const double unavailable_end = boundary(1);
    std::vector<SimpleFinSpan> remaining;
    remaining.reserve(layer.seed_ranges.size());
    for (const SimpleFinSpan &span : layer.seed_ranges) {
        const SimpleFinRunLocation &location = layer.run_locations[span.run];
        if (location.contour != contour_id) {
            remaining.push_back(span);
            continue;
        }
        const double length = location.end - location.begin;
        std::vector<std::pair<double, double>> pieces{{location.begin + span.begin * length,
                                                        location.begin + span.end * length}};
        for (int wrap = -1; wrap <= 1; ++wrap) {
            const double begin = unavailable_begin + wrap * contour.length;
            const double end = unavailable_end + wrap * contour.length;
            std::vector<std::pair<double, double>> next;
            for (const auto &piece : pieces) {
                if (begin > piece.first + 1e-9)
                    next.emplace_back(piece.first, std::min(begin, piece.second));
                if (end < piece.second - 1e-9)
                    next.emplace_back(std::max(end, piece.first), piece.second);
            }
            pieces = std::move(next);
            if (pieces.empty())
                break;
        }
        for (const auto &piece : pieces)
            remaining.push_back({span.run, (piece.first - location.begin) / length,
                                 (piece.second - location.begin) / length});
    }
    layer.seed_ranges = std::move(remaining);
    return intersection;
}

void subtract_simple_fin_projection(const PrintObject &object, std::vector<SimpleCandidateLayer> &layers,
                                    const FinCandidate &seed, size_t seed_run,
                                    double top_depth, double expansion_slope, double radius, double width)
{
    // The seed already lies on the sliced contour. Remove its local interval
    // before projecting to lower layers, even if no lower wall is found.
    subtract_simple_fin_contour_range(layers[seed.top_layer], seed.point, seed.normal, radius, 1e-4,
                                      layers[seed.top_layer].run_locations[seed_run].contour);

    // Keep the last wall position when the ray has no intersection, as the fin
    // geometry does. Snap each fallback to a nearby contour before continuing.
    Vec2d wall = seed.point;
    const double top_z = object.get_layer(int(seed.top_layer))->print_z;
    for (size_t remaining = seed.top_layer; remaining > 1; --remaining) {
        const size_t i = remaining - 1;
        const Layer &model_layer = *object.get_layer(int(i));
        const double distance = top_depth + (top_z - model_layer.print_z) * expansion_slope;
        const bool found_wall = projected_outer_contour(model_layer, seed.point, seed.normal, distance, wall);
        if (found_wall) {
            if (!layers[i].seed_ranges.empty())
                subtract_simple_fin_contour_range(layers[i], wall, seed.normal, radius, 1e-4);
        } else if (const auto fallback = subtract_simple_fin_contour_range(layers[i], wall, seed.normal, radius, width)) {
            wall = *fallback;
        }
    }
}

void emit_fin_support(PrintObject &object, const std::vector<TracedFin> &fins)
{
    size_t support_id = 0;
    for (size_t layer_id = 0; layer_id < object.layer_count(); ++layer_id) {
        const Layer &layer = *object.get_layer(int(layer_id));
        Polylines paths;
        Polygons feet;
        if (layer_id == 0) {
            generate_fin_foot_toolpaths(object, fins, paths, feet);
        } else {
            for (const TracedFin &fin : fins) {
                if (layer_id > fin.seed.top_layer)
                    continue;
                paths.insert(paths.end(), fin.outlines[layer_id].begin(), fin.outlines[layer_id].end());
            }
        }
        if (paths.empty() && feet.empty())
            continue;
        SupportLayer *support_layer = object.add_support_layer(int(support_id++), 0, layer.height, layer.print_z);
        support_layer->support_fills.no_sort = true;
        const Flow flow = support_material_flow(&object, float(layer.height));
        // Keep a closed oriented path: generic ExtrusionLoop emission relocates its seam to the last nozzle position.
        extrusion_entities_append_paths(support_layer->support_fills.entities, std::move(paths), erSupportMaterial,
                                        flow.mm3_per_mm(), flow.width(), flow.height(), false);
        if (!feet.empty()) {
            std::unique_ptr<Fill> filler(Fill::new_from_type(ipRectilinear));
            filler->set_bounding_box(get_extents(feet));
            filler->spacing = flow.spacing();
            filler->layer_id = 0;
            const SupportParameters support_params(object);
            fill_expolygons_with_sheath_generate_paths(support_layer->support_fills.entities, feet,
                filler.get(), 1.f, erSupportMaterial, flow, support_params, false, false);
        }
        support_layer->support_islands = union_ex(
            support_layer->support_fills.polygons_covered_by_width(float(SCALED_EPSILON)));
    }
}

std::vector<FinCandidate> select_simple_fin_candidates(const PrintObject &object,
                                                       const std::vector<PaintedLayer> &painted)
{
    const PrintObjectConfig &config = object.config();
    const double width = support_material_flow(&object).width();
    const double placement_distance = config.fin_support_max_spacing.value;

    std::vector<SimpleCandidateLayer> layers(painted.size());
    double painted_top_z = 0.;
    size_t painted_top_layer = 0;
    for (size_t i = 1; i < painted.size(); ++i) {
        if (object.print()->canceled())
            throw CanceledException();
        if (!painted[i].sections[0].empty()) {
            painted_top_z = object.get_layer(int(i))->print_z;
            painted_top_layer = i;
        }
        // lslices are sampled at slice_z; compare paint at the same height.
        // A cut infinitesimally below print_z may collapse against a top edge.
        // Match every painted facet cut separately. The contour intervals below
        // are sorted and united, regardless of triangle order or tiny normal differences.
        layers[i] = build_simple_candidate_space(
            object, *object.get_layer(int(i)), painted[i].sections[0], width);
    }

    std::vector<FinCandidate> candidates;
    size_t highest = layers.size() - 1;
    while (highest > 0) {
        if (object.print()->canceled())
            throw CanceledException();
        while (highest > 0 && layers[highest].seed_ranges.empty())
            --highest;
        if (highest == 0)
            break;
        SimpleCandidateLayer &layer = layers[highest];
        prepare_simple_fin_anchors(layer, *object.get_layer(int(highest)), width, placement_distance);
        const SimpleFinSite site = select_simple_fin_site(layer, highest, width, placement_distance);
        const PaintedSegment &run = layer.runs[site.run];
        const Vec3d &surface_normal = run.surface_normal;
        const double horizontal = surface_normal.head<2>().norm();
        if (horizontal <= EPSILON) {
            layer.seed_ranges.erase(std::remove_if(layer.seed_ranges.begin(), layer.seed_ranges.end(),
                [&site](const SimpleFinSpan &span) { return span.run == site.run; }), layer.seed_ranges.end());
            continue;
        }
        const Vec2d contact_normal = surface_normal.head<2>() / horizontal;
        const Layer &model_layer = *object.get_layer(int(highest));
        const PrintRegion *region = fin_wall_region(model_layer, site.point, run.normal, width);
        const FinSupportDimensions dimensions = fin_support_dimensions(object,
            region != nullptr ? *region : object.printing_region(0));
        const double lean = -surface_normal.z() / horizontal *
                            std::max(0., contact_normal.dot(run.normal));
        const double weight = 1 - std::clamp(std::atan(lean) / (M_PI / 6.), 0., 1.);
        const FinCandidate candidate{site.point, run.normal, contact_normal, highest, dimensions,
            config.fin_support_expansion_factor.value * weight * weight};
        candidates.push_back(candidate);
        subtract_simple_fin_projection(object, layers, candidate, site.run,
                                       config.fin_support_depth.value, candidate.expansion_slope,
                                       placement_distance, width);
    }

    const double candidate_top_z = candidates.empty() ? 0. :
        object.get_layer(int(candidates.front().top_layer))->print_z;
    BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
        << " stage=simple-candidates candidates=" << candidates.size()
        << " painted_top_z=" << painted_top_z << " candidate_top_z=" << candidate_top_z
        << " top_paint_sections=" << painted[painted_top_layer].sections[0].size()
        << " top_painted_cuts=" << layers[painted_top_layer].painted_cuts
        << " top_matched_intervals=" << layers[painted_top_layer].matched_intervals
        << " top_joined_intervals=" << layers[painted_top_layer].joined_intervals
        << " top_short_contact_intervals=" << layers[painted_top_layer].short_contact_intervals
        << " top_initial_seed_intervals=" << (layers[painted_top_layer].matched_intervals -
                                               layers[painted_top_layer].short_contact_intervals)
        << " top_slice_z=" << object.get_layer(int(painted_top_layer))->slice_z
        << " top_longest_painted_cut_mm=" << layers[painted_top_layer].longest_painted_cut
        << " top_painted_extent_mm=" << layers[painted_top_layer].painted_extent
        << " top_best_contact_line_mm=" << layers[painted_top_layer].best_contact_line
        << " top_required_contact_mm=" << layers[painted_top_layer].best_required_contact
        << " top_best_contact_slack_mm=" << (layers[painted_top_layer].matched_intervals > 0 ?
                                             layers[painted_top_layer].best_contact_slack : 0.);
    return candidates;
}

void generate_simple_fin_support(PrintObject &object, const std::vector<PaintedLayer> &painted,
                                 const std::vector<Polygons> &blockers, const Polygon &bed,
                                 double line_width)
{
    const std::vector<FinCandidate> candidates = select_simple_fin_candidates(object, painted);
    std::vector<TracedFin> fins;
    fins.reserve(candidates.size());
    FinDiagnostics diagnostics;
    size_t rejected_geometry = 0;
    size_t superseded_count = 0;
    for (const FinCandidate &candidate : candidates) {
        if (object.print()->canceled())
            throw CanceledException();
        TracedFin fin;
        const bool generated = realize_fin_candidate(
            object, candidate, bed, painted, blockers, line_width, fin);
        diagnostics.merge(fin.diagnostics);
        if (!generated) {
            ++rejected_geometry;
            continue;
        }
        const FinPlacement placement = evaluate_fin_placement(object, fin, fins);
        if (placement.overlap_layer) {
            diagnostics.reject(FinReject::overlap,
                object.get_layer(int(*placement.overlap_layer))->print_z);
            ++rejected_geometry;
            continue;
        }
        for (auto it = placement.superseded.rbegin(); it != placement.superseded.rend(); ++it) {
            fins.erase(fins.begin() + *it);
            ++superseded_count;
        }
        fins.push_back(std::move(fin));
    }
    for (size_t i = 0; i < fins.size();) {
        if (!generate_fin_toolpaths(object, fins[i])) {
            diagnostics.reject(FinReject::no_outline,
                fins[i].diagnostics.failures[size_t(FinReject::no_outline)].first_z);
            fins.erase(fins.begin() + i);
            ++rejected_geometry;
        } else {
            ++i;
        }
    }
    size_t lower_contacts_accepted = 0;
    size_t suppressed_contact_bands = 0;
    for (const TracedFin &fin : fins) {
        lower_contacts_accepted += std::count(fin.contacts.begin(), fin.contacts.end(), true) - 1;
        suppressed_contact_bands += fin.suppressed_contact_bands;
    }
    const double candidate_top_z = candidates.empty() ? 0. :
        object.get_layer(int(candidates.front().top_layer))->print_z;
    double emitted_top_z = 0.;
    for (const TracedFin &fin : fins)
        emitted_top_z = std::max(emitted_top_z, object.get_layer(int(fin.seed.top_layer))->print_z);
    BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
        << " stage=simple candidates=" << candidates.size() << " fins=" << fins.size()
        << " candidate_top_z=" << candidate_top_z
        << " emitted_top_z=" << emitted_top_z
        << " lower_contacts_accepted=" << lower_contacts_accepted
        << " suppressed_seed_projection=" << suppressed_contact_bands
        << " superseded=" << superseded_count
        << " rejected_geometry=" << rejected_geometry;
    diagnostics.log("simple-geometry", object);
    if (!fins.empty())
        emit_fin_support(object, fins);
}

} // namespace

void generate_fin_support(PrintObject &object)
{
    if (object.layer_count() < 2 || object.has_raft()) {
        BOOST_LOG_TRIVIAL(info) << "Fin support skipped: insufficient layers or raft";
        return;
    }
    const PrintObjectConfig &config = object.config();
    const Layer &first = *object.get_layer(0);
    if (first.lslices.empty()) {
        BOOST_LOG_TRIVIAL(info) << "Fin support skipped: empty first-layer footprint";
        return;
    }
    const auto painted = painted_sides(object);
    const auto blockers = object.slice_support_blockers();
    const double line_width = support_material_flow(&object).width();
    const Polygon bed = fin_bed_shape(object);
    if (config.fin_support_style.value == fssSimple) {
        generate_simple_fin_support(object, painted, blockers, bed, line_width);
        return;
    }
    FinDiagnostics diagnostics;

    // The original footprint stays fixed while each printed prefix gets its
    // own centroid and directional demand. Fins change capacity, not mass.
    const SectionMoments base = area_moments(first.lslices);
    const double base_area = base.area;
    if (base_area <= EPSILON * EPSILON) {
        BOOST_LOG_TRIVIAL(info) << "Fin support skipped: first-layer area=" << base_area;
        return;
    }
    const Vec2d base_center = base.first / base_area;
    const Eigen::Matrix2d base_distribution = area_moments(first.lslices, base_center).second / base_area;
    const double base_scale = std::sqrt(base_area);
    const double bed_z = first.print_z - first.height;
    struct BaseEdge { Vec2d normal; double limit; double initial_margin; };
    struct LayerState { Vec2d center; double risk = 0; std::vector<FinDemand> demand; };
    Points base_points;
    for (const ExPolygon &slice : first.lslices)
        base_points.insert(base_points.end(), slice.contour.points.begin(), slice.contour.points.end());
    const Polygon base_hull = Geometry::convex_hull(base_points);
    std::vector<BaseEdge> base_edges;
    for (size_t j = 0; j < base_hull.points.size(); ++j) {
        const Vec2d a = unscale(base_hull.points[j]);
        const Vec2d edge = unscale(base_hull.points[(j + 1) % base_hull.points.size()]) - a;
        if (edge.norm() <= EPSILON)
            continue;
        const Vec2d normal = Vec2d(edge.y(), -edge.x()).normalized();
        double limit = -std::numeric_limits<double>::infinity();
        for (const Point &point : base_hull.points)
            limit = std::max(limit, unscale(point).dot(normal));
        base_edges.push_back({normal, limit, limit - base_center.dot(normal)});
    }
    if (base_edges.empty())
        return;
    std::vector<std::vector<PaintedSegment>> runs(painted.size());
    for (size_t i = 1; i < painted.size(); ++i) {
        if (object.print()->canceled())
            throw CanceledException();
        runs[i] = simulated_candidate_runs(painted[i]);
        for (PaintedSegment &run : runs[i]) {
            run.face_center = 0.5 * (run.a + run.b);
            run.face_span = (run.b - run.a).norm();
            // Many short cuts indicate a faceted curved outline; its D-ray
            // ranking needs no full-contour scan for each facet.
            if (runs[i].size() <= 16)
                locate_fin_face(*object.get_layer(int(i)), run, line_width);
        }
    }

    auto add_demand = [](std::vector<FinDemand> &demands, const Vec2d &direction, double value) {
        if (value <= EPSILON)
            return;
        for (FinDemand &item : demands)
            if (item.direction.dot(direction) >= fin_direction_merge_alignment) {
                item.initial = item.remaining = std::max(item.initial, value);
                return;
            }
        demands.push_back({direction, value, value});
    };
    std::vector<LayerState> states(object.layer_count());
    double peak_risk = 0;
    Vec2d volume_moment = Vec2d::Zero();
    double volume = 0;
    for (size_t i = 0; i < object.layer_count(); ++i) {
        if (object.print()->canceled())
            throw CanceledException();
        const Layer &layer = *object.get_layer(int(i));
        const SectionMoments section = area_moments(layer.lslices, base_center);
        const double area = section.area;
        volume += area * layer.height;
        volume_moment += section.first * layer.height;
        LayerState &state = states[i];
        state.center = volume > EPSILON ? Vec2d(volume_moment / volume) : base_center;
        const double height = layer.print_z - bed_z;
        const double eccentricity = (state.center - base_center).norm();
        state.risk = height / base_scale * (1 + eccentricity / base_scale);
        peak_risk = std::max(peak_risk, state.risk);
        if (state.risk <= 0.8)
            continue;
        // Height raises urgency but a fixed footprint direction does not
        // demand arbitrarily many parallel sheets. New shape/lean can still
        // add stronger, distinct demands at later heights.
        const double excess = state.risk / 0.8 - 1;
        const double height_need = excess / (1 + excess);
        // Each footprint edge is a directional tipping boundary. The centroid
        // changes its margin; the fin capacity will be subtracted separately.
        for (const BaseEdge &edge : base_edges) {
            const double margin = edge.limit - state.center.dot(edge.normal);
            const double need = height_need * edge.initial_margin / std::max(margin, 0.5 * line_width);
            add_demand(state.demand, edge.normal, need);
        }
        const Vec2d offset = state.center - base_center;
        if (offset.norm() > EPSILON)
            add_demand(state.demand, offset.normalized(), height_need * (1 + offset.norm() / base_scale));
        if (area <= EPSILON * EPSILON)
            continue;
        // The current section's spread contributes without cancelling a
        // different lean or spread observed at an earlier print height.
        const Vec2d section_center = section.first / area;
        const Vec2d section_offset = section_center - base_center;
        if (section_offset.norm() > EPSILON)
            add_demand(state.demand, section_offset.normalized(),
                       height_need * (1 + section_offset.norm() / base_scale));
        const Eigen::Matrix2d distribution = section.second / area - section_offset * section_offset.transpose();
        const double difference = distribution(0, 0) - distribution(1, 1);
        const double angle = 0.5 * std::atan2(2 * distribution(0, 1), difference);
        const Vec2d axis(std::cos(angle), std::sin(angle));
        const Vec2d cross(-axis.y(), axis.x());
        for (const Vec2d &direction : {axis, cross}) {
            // A near-line first layer has almost zero variance across its
            // narrow axis. Use a bead-sized floor and compress large growth
            // ratios so one early layer cannot exhaust the plan budget.
            const double floor = line_width * line_width / 12.;
            const double baseline = std::max(0., direction.dot(base_distribution * direction));
            const double spread = std::max(0., direction.dot(distribution * direction));
            const double growth = std::max(0., 0.5 * std::log((spread + floor) / (baseline + floor)));
            if (growth > 1e-6) {
                add_demand(state.demand, direction, height_need * (1 + growth));
                add_demand(state.demand, -direction, height_need * (1 + growth));
            }
        }
    }
    if (peak_risk <= 0.8) {
        BOOST_LOG_TRIVIAL(info) << "Fin support skipped: peak risk <=0.8";
        return;
    }

    struct CapacityOverride { FinCandidate initial_seed; double fraction; };
    struct TopLimit { FinCandidate initial_seed; size_t layer; };
    auto same_seed = [](const FinCandidate &a, const FinCandidate &b) {
        return a.top_layer == b.top_layer && (a.point - b.point).squaredNorm() < 1e-8 &&
               (a.normal - b.normal).squaredNorm() < 1e-10;
    };
    std::vector<CapacityOverride> capacity_overrides;
    std::vector<TopLimit> top_limits;
    std::vector<TracedFin> fins;
    std::vector<FinCandidate> rejected;
    size_t planning_passes = 0;
    size_t geometry_attempts = 0;
    size_t superseded_count = 0;
    bool retry;
    do {
        ++planning_passes;
        std::vector<FinPlan> plans;
        // One upward traversal builds all candidate histories. Existing plans
        // contribute before another seed is considered on the same layer.
        for (size_t layer_id = 1; layer_id < states.size(); ++layer_id) {
            if (object.print()->canceled())
                throw CanceledException();
            std::vector<FinCandidate> previous_tops;
            previous_tops.reserve(plans.size());
            for (FinPlan &plan : plans) {
                previous_tops.push_back(plan.seed);
                if (plan.active && plan.seed.top_layer + 1 == layer_id) {
                    const auto limit = std::find_if(top_limits.begin(), top_limits.end(), [&](const TopLimit &item) {
                        return same_seed(item.initial_seed, plan.initial_seed);
                    });
                    if ((limit != top_limits.end() && layer_id > limit->layer) ||
                        !extend_fin_plan(object, painted, blockers, runs[layer_id], layer_id,
                                         states[layer_id].risk, plan, diagnostics))
                        plan.active = false;
                }
            }
            // Converging plans share future demand. Stop the later one at its
            // previous valid top while preserving both lower fin histories.
            for (size_t j = 0; j < plans.size(); ++j) {
                if (!plans[j].active || plans[j].seed.top_layer != layer_id ||
                    previous_tops[j].top_layer == layer_id)
                    continue;
                for (size_t k = 0; k < j; ++k)
                    if (plans[k].active && plans[k].seed.top_layer == layer_id &&
                        plans[k].seed.normal.dot(plans[j].seed.normal) >= fin_direction_merge_alignment &&
                        (plans[k].seed.point - plans[j].seed.point).norm() <
                            0.5 * (plans[k].seed.dimensions.thickness + plans[j].seed.dimensions.thickness)) {
                        plans[j].seed = previous_tops[j];
                        plans[j].tops.pop_back();
                        plans[j].active = false;
                        break;
                    }
            }
            std::vector<FinDemand> residual = states[layer_id].demand;
            std::vector<bool> unavailable(residual.size(), false);
            auto deduct_plans = [&] {
                residual = states[layer_id].demand;
                for (size_t j = 0; j < residual.size(); ++j)
                    residual[j].blocked = unavailable[j];
                for (const FinPlan &plan : plans) {
                    double capacity = planned_fin_capacity(object, plan, layer_id, bed_z);
                    for (const CapacityOverride &override : capacity_overrides)
                        if (override.initial_seed.top_layer == plan.initial_seed.top_layer &&
                            (override.initial_seed.point - plan.initial_seed.point).squaredNorm() < 1e-8 &&
                            (override.initial_seed.normal - plan.initial_seed.normal).squaredNorm() < 1e-10) {
                            capacity *= override.fraction;
                            break;
                        }
                    for (FinDemand &need : residual) {
                        const double alignment = std::max(0., plan.seed.normal.dot(need.direction));
                        need.remaining = std::max(0., need.remaining - capacity * alignment * alignment);
                    }
                }
            };
            deduct_plans();
            // Closed lower plans still support their printed heights, but do
            // not consume every slot needed by later shape changes.
            size_t added_on_layer = 0;
            size_t active_count = std::count_if(plans.begin(), plans.end(),
                [](const FinPlan &plan) { return plan.active; });
            while (plans.size() < 32 && active_count < 8 && added_on_layer < 4 && !residual.empty()) {
                size_t best = residual.size();
                double greatest = 0;
                for (size_t j = 0; j < residual.size(); ++j)
                    if (!residual[j].blocked && residual[j].remaining > 0.15 * residual[j].initial &&
                        residual[j].remaining > greatest) {
                        greatest = residual[j].remaining;
                        best = j;
                    }
                if (best == residual.size())
                    break;
                FinCandidate seed;
                if (!select_simulated_fin_candidate(object, painted, blockers, runs[layer_id], layer_id,
                        states[layer_id].center, residual[best].direction, states[layer_id].risk,
                        plans, rejected, diagnostics, seed)) {
                    unavailable[best] = true;
                    residual[best].blocked = true;
                    continue;
                }
                plans.push_back({seed, seed, true, seed.point.dot(seed.normal), {seed}});
                ++active_count;
                ++added_on_layer;
                deduct_plans();
            }
        }
        fins.clear();
        superseded_count = 0;
        std::vector<size_t> accepted_plan_indices;
        retry = false;
        for (size_t plan_id = 0; plan_id < plans.size(); ++plan_id) {
            const FinPlan &plan = plans[plan_id];
            ++geometry_attempts;
            TracedFin fin;
            const bool geometry_ok = realize_fin_candidate(
                object, plan.seed, bed, painted, blockers, line_width, fin);
            const FinPlacement placement = geometry_ok ?
                evaluate_fin_placement(object, fin, fins) : FinPlacement{};
            const bool overlapping = placement.overlap_layer.has_value();
            if (!geometry_ok || overlapping) {
                diagnostics.merge(fin.diagnostics);
                if (overlapping)
                    diagnostics.reject(FinReject::overlap,
                        object.get_layer(int(*placement.overlap_layer))->print_z);
                // The final top may fail while the original contact remains
                // printable. Keep the highest printable earlier top and let
                // the next upward sweep fill the demand it no longer covers.
                bool earlier_top_valid = false;
                if (plan.tops.size() > 1) {
                    auto valid_top = [&](const FinCandidate &candidate) {
                        ++geometry_attempts;
                        TracedFin shorter;
                        if (!realize_fin_candidate(
                                object, candidate, bed, painted, blockers, line_width, shorter))
                            return false;
                        return !evaluate_fin_placement(object, shorter, fins).overlap_layer;
                    };
                    if (valid_top(plan.tops.front())) {
                        // Most extension failures form an upper invalid range.
                        // Bracket a valid top exponentially, then refine; the
                        // exceptional retry must not rebuild every lower layer.
                        size_t lower = 0, upper = plan.tops.size() - 1, step = 1;
                        while (upper > 1) {
                            const size_t probe = upper > step ? upper - step : 0;
                            if (valid_top(plan.tops[probe])) {
                                lower = probe;
                                break;
                            }
                            upper = probe;
                            step = std::min(step * 2, plan.tops.size());
                        }
                        while (upper > lower + 1) {
                            const size_t middle = lower + (upper - lower) / 2;
                            if (valid_top(plan.tops[middle]))
                                lower = middle;
                            else
                                upper = middle;
                        }
                        auto limit = std::find_if(top_limits.begin(), top_limits.end(), [&](const TopLimit &item) {
                            return same_seed(item.initial_seed, plan.initial_seed);
                        });
                        if (limit == top_limits.end())
                            top_limits.push_back({plan.initial_seed, plan.tops[lower].top_layer});
                        else
                            limit->layer = std::min(limit->layer, plan.tops[lower].top_layer);
                        earlier_top_valid = true;
                    }
                }
                if (!earlier_top_valid)
                    rejected.push_back(plan.initial_seed);
                retry = true;
                // Collect all failed plans in this sweep before replanning.
                // Earlier fins remain available for overlap checks below.
                continue;
            }
            diagnostics.merge(fin.diagnostics);
            for (auto it = placement.superseded.rbegin(); it != placement.superseded.rend(); ++it) {
                fins.erase(fins.begin() + *it);
                accepted_plan_indices.erase(accepted_plan_indices.begin() + *it);
                ++superseded_count;
            }
            fins.push_back(std::move(fin));
            accepted_plan_indices.push_back(plan_id);
        }
        if (retry)
            continue;
        // Band validity is known only after geometry. Correct the optimistic
        // planning score and rerun the inexpensive upward sweep when needed.
        for (size_t i = 0; i < fins.size(); ++i) {
            const double fraction = double(fins[i].accepted_contact_bands) /
                                    std::max<size_t>(fins[i].planned_contact_bands, 1);
            auto found = std::find_if(capacity_overrides.begin(), capacity_overrides.end(), [&](const CapacityOverride &item) {
                return item.initial_seed.top_layer == plans[accepted_plan_indices[i]].initial_seed.top_layer &&
                       (item.initial_seed.point - plans[accepted_plan_indices[i]].initial_seed.point).squaredNorm() < 1e-8 &&
                       (item.initial_seed.normal - plans[accepted_plan_indices[i]].initial_seed.normal).squaredNorm() < 1e-10;
            });
            if (found == capacity_overrides.end()) {
                if (fraction < 0.98) {
                    capacity_overrides.push_back({plans[accepted_plan_indices[i]].initial_seed, fraction});
                    retry = true;
                }
            } else if (fraction + 0.02 < found->fraction) {
                found->fraction = fraction;
                retry = true;
            }
        }
        if (retry)
            continue;
        for (size_t i = 0; i < fins.size(); ++i)
            if (!generate_fin_toolpaths(object, fins[i])) {
                diagnostics.reject(FinReject::no_outline,
                    fins[i].diagnostics.failures[size_t(FinReject::no_outline)].first_z);
                rejected.push_back(plans[accepted_plan_indices[i]].initial_seed);
                retry = true;
                break;
            }
    } while (retry);

    BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
                            << " accepted=" << fins.size() << " planning_passes=" << planning_passes
                            << " geometry_attempts=" << geometry_attempts
                            << " superseded=" << superseded_count;
    diagnostics.log("all-attempts", object);
    for (size_t i = 0; i < fins.size(); ++i) {
        const TracedFin &fin = fins[i];
        BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name << " selected=" << i << " seed=(" << fin.seed.point.x() << ','
            << fin.seed.point.y() << ',' << object.get_layer(int(fin.seed.top_layer))->print_z << ") A="
            << std::atan2(fin.seed.normal.y(), fin.seed.normal.x()) * 180 / M_PI << " bands="
            << fin.accepted_contact_bands << '/' << fin.planned_contact_bands
            << " suppressed_seed_projection=" << fin.suppressed_contact_bands;
        fin.diagnostics.log("selected-fin", object);
    }

    emit_fin_support(object, fins);
}

} // namespace Slic3r
