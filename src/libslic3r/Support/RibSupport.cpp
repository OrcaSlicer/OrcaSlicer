#include "RibSupport.hpp"
#include "SupportCommon.hpp"

#include "../Fill/Fill.hpp"
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
#include <utility>
#include <vector>

namespace Slic3r {
RibSupportDimensions rib_support_dimensions(const PrintObject &object, const PrintRegion &region)
{
    const double reference = region.flow(object, frExternalPerimeter, object.config().layer_height.value).width();
    return {object.config().rib_support_contact_width.get_abs_value(reference),
            object.config().rib_support_thickness.get_abs_value(reference)};
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

struct RibCandidate {
    Vec2d point;
    Vec2d normal; // A: load-bearing sheet direction.
    Vec2d contact_normal; // B: direction from the painted wall to the sheet.
    size_t top_layer;
    RibSupportDimensions dimensions;
    double expansion_slope;
};

enum class RibReject {
    top_depth, top_wall, wall_facing, projection_intrusion, body_depth, bed_collision, model_collision,
    missing_wall, missing_paint, blocked_contact, contact_contour, contact_span, contact_normal,
    contact_turn, empty_neck, top_band, topology, thin_body, no_outline, seed_region, overlap,
    direction, paint_empty, paint_facing, paint_parallel, paint_distance,
    paint_endpoint, count
};

struct RibDiagnostics {
    struct Failure {
        size_t count = 0;
        double first_z = 0;
        double first_value = 0;
    };
    std::array<Failure, size_t(RibReject::count)> failures{};

    bool reject(RibReject reason, double z, double value = 0)
    {
        Failure &failure = failures[size_t(reason)];
        if (failure.count++ == 0) {
            failure.first_z = z;
            failure.first_value = value;
        }
        return false;
    }

    void merge(const RibDiagnostics &other)
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
        static constexpr std::array<const char *, size_t(RibReject::count)> names{
            "top-depth", "top-wall", "wall-facing", "projection-intrusion", "body-depth", "bed-collision", "model-collision",
            "contact-no-wall", "contact-no-paint", "contact-blocked", "contact-contour", "contact-span", "contact-normal",
            "contact-turn", "empty-neck", "top-band", "topology", "thin-body", "no-outline", "seed-region", "rib-overlap",
            "no-direction", "paint-empty", "paint-facing", "paint-parallel",
            "paint-distance", "paint-endpoint"};
        for (size_t i = 0; i < failures.size(); ++i)
            if (failures[i].count > 0)
                BOOST_LOG_TRIVIAL(info) << "Rib support object=" << object.model_object()->name
                    << " stage=" << stage << " reject=" << names[i]
                    << " count=" << failures[i].count << " first_z=" << failures[i].first_z
                    << " value=" << failures[i].first_value;
    }
};

struct TracedRib {
    RibCandidate seed;
    RibDiagnostics diagnostics;
    size_t planned_contact_bands = 0;
    size_t accepted_contact_bands = 0;
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

constexpr double min_rib_contact_alignment = 0.86602540378443864676; // cos(30 degrees).

bool rib_contact_aligned(const Vec2d &body_direction, const Vec2d &contact_direction)
{
    return body_direction.dot(contact_direction) >= min_rib_contact_alignment - 1e-9;
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
// support-enforcer projection loses the side position and normal needed by a rib.
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
            if (normal.norm() < 0.5) // Ribs make side contacts, not underside contacts.
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

bool clear_rib_line(const Layer &layer, const Polygon &bed, const Vec2d &from, const Vec2d &to,
                    double width, RibDiagnostics &diagnostics)
{
    const Vec2d direction = to - from;
    if (direction.squaredNorm() < 1e-8)
        return diagnostics.reject(RibReject::body_depth, layer.print_z, direction.norm());
    const Vec2d side(-direction.y(), direction.x());
    const Vec2d half_width = 0.5 * width * side.normalized();
    const int samples = std::max(2, int(std::ceil((to - from).norm() / std::max(width, 0.4))));
    for (int i = 0; i <= samples; ++i) {
        const Vec2d p = from + double(i) / samples * (to - from);
        if (!bed.contains(scaled_point(p + half_width)) || !bed.contains(scaled_point(p - half_width)))
            return diagnostics.reject(RibReject::bed_collision, layer.print_z);
        if (inside_model(layer, p) || inside_model(layer, p + half_width) || inside_model(layer, p - half_width))
            return diagnostics.reject(RibReject::model_collision, layer.print_z);
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

Polygon foot_area(const Vec2d &start, const Vec2d &tip, const Vec2d &normal, double thickness, double width)
{
    const double half_width = 0.5 * thickness + 1.6;
    const Vec2d tangent(-normal.y(), normal.x());
    const Vec2d inner = start - 0.5 * width * normal;
    const Vec2d outer = tip + 0.5 * width * normal;
    return Polygon{scaled_point(inner - half_width * tangent),
                   scaled_point(outer - half_width * tangent),
                   scaled_point(outer + half_width * tangent),
                   scaled_point(inner + half_width * tangent)};
}

bool trace_rib(const PrintObject &object, const Polygon &bed, const RibCandidate &candidate,
               double top_depth, double gap, double thickness, double line_width, TracedRib &rib)
{
    if (top_depth - gap < 2 * line_width)
        return rib.diagnostics.reject(RibReject::top_depth, object.get_layer(int(candidate.top_layer))->print_z, top_depth - gap);
    rib.boundary.resize(candidate.top_layer + 1);
    rib.wall_present.assign(candidate.top_layer + 1, false);
    rib.body_start.resize(candidate.top_layer + 1);
    rib.tips.resize(candidate.top_layer + 1);
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
        rib.tips[i] = candidate.point + outer_distance * candidate.normal;
        Vec2d point = target;
        Vec2d surface_normal = candidate.contact_normal;
        const bool have_surface = projected_outer_contour(layer, candidate.point, candidate.normal,
                                                         outer_distance, point, &surface_normal);
        if (i == candidate.top_layer && (!have_surface || (point - candidate.point).norm() > line_width))
            return rib.diagnostics.reject(RibReject::top_wall, layer.print_z, (point - candidate.point).norm());
        const double alignment = surface_normal.dot(candidate.normal);
        if (have_surface && alignment <= EPSILON)
            return rib.diagnostics.reject(RibReject::wall_facing, layer.print_z, alignment);
        const double surface_inner = have_surface ? (point - candidate.point).dot(candidate.normal) +
            (gap + 0.5 * thickness * std::abs(side.dot(surface_normal))) / alignment : projected_inner;
        // Project the existing sheet downward, extend its inner edge to the wall, and clip model clearance.
        // A lower wall may not remove the load-bearing vertical projection of the upper sheet.
        if (i < candidate.top_layer && surface_inner > projected_inner + 0.5 * line_width)
            return rib.diagnostics.reject(RibReject::projection_intrusion, layer.print_z, surface_inner - projected_inner);
        projected_inner = std::min(projected_inner, surface_inner);
        const Vec2d from = candidate.point + std::max(projected_inner, surface_inner) * candidate.normal;
        const double depth = (rib.tips[i] - from).dot(candidate.normal);
        if (depth < 2 * line_width)
            return rib.diagnostics.reject(RibReject::body_depth, layer.print_z, depth);
        if (!clear_rib_line(layer, bed, from, rib.tips[i], thickness, rib.diagnostics))
            return false;
        rib.boundary[i] = point;
        rib.wall_present[i] = have_surface;
        rib.body_start[i] = from;
        target = point;
    }

    return true;
}

Polygon rib_rectangle(const Vec2d &inner, const Vec2d &outer, const Vec2d &normal, double thickness)
{
    const Vec2d side = 0.5 * thickness * Vec2d(-normal.y(), normal.x());
    return Polygon{scaled_point(inner - side), scaled_point(outer - side),
                   scaled_point(outer + side), scaled_point(inner + side)};
}

Polygon rib_contact_polygon(const Vec2d &front, const Vec2d &base, const Vec2d &normal, const Vec2d &contact_normal,
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

struct ProjectionClip {
    ExPolygons model;
    ExPolygons bed;
    bool ready = false;
};

ExPolygons rib_body_area(const Layer &layer, const TracedRib &rib, size_t layer_id,
                         double thickness, double gap)
{
    return diff_ex(rib_rectangle(rib.body_start[layer_id], rib.tips[layer_id], rib.seed.normal, thickness),
                   offset_ex(layer.lslices, float(scale_(gap))));
}

void backfill_contact_projection(const PrintObject &object, const TracedRib &rib, size_t bottom,
                                 const Polygon &bed, double thickness,
                                 double contact_width, double gap, const Vec3d &surface_normal,
                                 const std::vector<ExPolygons> &body_centers, std::vector<ExPolygons> &contacts,
                                 std::vector<ProjectionClip> &clips)
{
    const Vec2d normal = surface_normal.head<2>().normalized();
    const Vec2d &body_normal = rib.seed.normal;
    // Rotate B toward -Z in the contact plane, independently of body direction A.
    const double angle = object.config().rib_support_contact_projection_angle.value * M_PI / 180.;
    const double outward = surface_normal.head<2>().norm();
    const double radial = outward * std::cos(angle) + surface_normal.z() * std::sin(angle);
    const double downward = outward * std::sin(angle) - surface_normal.z() * std::cos(angle);
    // A horizontal or upward ray does not intersect any lower layer. Keep the
    // contact geometry on its own layer instead of inventing a downward ramp.
    if (downward <= 1e-12)
        return;
    const double slope = radial / downward;
    // Project just the front segment (a point for zero length). Its endpoints
    // connect directly to the current lower layer's inner body corners.
    const Layer &contact_layer = *object.get_layer(int(bottom));
    const double contact_line_width = support_material_flow(&object, float(contact_layer.height)).width();
    Vec2d front = rib.boundary[bottom] + 0.5 * contact_line_width * normal;
    const Vec2d initial_front = front;
    double advance = 0;
    const double neck_depth = (rib.body_start[bottom] - rib.boundary[bottom]).dot(normal);
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
        const Vec2d base = rib.body_start[i] + 0.5 * width * body_normal;
        const double fraction = std::clamp(advance / std::max(neck_depth, EPSILON), 0., 1.);
        const double rear_span = (thickness - width) * std::abs(body_normal.dot(normal));
        const double projected_length = contact_width - std::max(0., contact_width - rear_span) * fraction;
        const Vec2d side = 0.5 * projected_length * Vec2d(-normal.y(), normal.x());
        const Point a = scaled_point(front - side), b = scaled_point(front + side);
        if (std::any_of(body_centers[i].begin(), body_centers[i].end(), [&a, &b](const ExPolygon &body) {
            return a == b ? body.contains(a) : body.contains(Line(a, b));
        }))
            break;
        const Vec2d body_side = 0.5 * (thickness - width) * Vec2d(-body_normal.y(), body_normal.x());
        const double outer_limit = std::max(base.dot(normal),
                                           (rib.tips[i] - 0.5 * width * body_normal).dot(normal)) +
                                   std::abs(body_side.dot(normal));
        if (front.dot(normal) > outer_limit + EPSILON)
            break;
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
        const Polygon neck = rib_contact_polygon(initial_front, base, body_normal, normal,
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
        const Polygon neck_limit = rib_rectangle((inner - width) * body_normal + side_center,
            (base.dot(body_normal) + width) * body_normal + side_center,
            body_normal, side_max - side_min + 2 * width);
        ExPolygons allowed = intersection_ex(Polygons{neck}, Polygons{neck_limit});
        expolygons_append(allowed, body_centers[i]);
        allowed = union_ex(allowed);
        ExPolygons projection = intersection_ex(diff_ex(rib_contact_polygon(front, base, body_normal, normal,
            projected_length, thickness, width), clip.model), clip.bed);
        projection = intersection_ex(projection, allowed);
        if (projection.empty())
            break;
        if (diff_ex(projection, body_centers[i]).empty())
            break;
        expolygons_append(contacts[i], std::move(projection));
    }
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
                           const Vec2d &normal, double z, RibDiagnostics &diagnostics)
{
    if (segments.empty()) {
        diagnostics.reject(RibReject::paint_empty, z);
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
        diagnostics.reject(RibReject::paint_facing, z);
    else if (!nonparallel)
        diagnostics.reject(RibReject::paint_parallel, z);
    else if (std::isfinite(nearest))
        diagnostics.reject(RibReject::paint_distance, z, nearest);
    else
        diagnostics.reject(RibReject::paint_endpoint, z, overrun);
}

bool painted_contact_width(const Layer &layer, const PaintedLayer &painted,
                           const Vec2d &point, const Vec2d &normal, double contact_width, double line_width,
                           Vec3d *surface_normal = nullptr, RibDiagnostics *diagnostics = nullptr)
{
    auto reject = [&](RibReject reason, double value = 0) {
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
            return reject(RibReject::contact_contour, -1);
        if ((wall - edge).norm() > line_width)
            return reject(RibReject::contact_contour, (wall - edge).norm());
        for (size_t section = 0; section < painted.sections.size(); ++section) {
            const PaintedSegment *surface = painted_contact(painted.sections[section], wall, normal, line_width);
            if (surface == nullptr) {
                if (diagnostics != nullptr)
                    diagnose_paint_lookup(painted.sections[section], wall, normal, layer.print_z, *diagnostics);
                return reject(RibReject::contact_span, double(section));
            }
            // Horizontal is the fixed boundary. Upward-facing facets must not
            // influence a contact's tilt, even when the patch crosses a crease.
            if (surface->surface_normal.z() <= 0)
                normal_sum += surface->surface_normal;
        }
    }
    if (normal_sum.squaredNorm() <= EPSILON * EPSILON)
        return reject(RibReject::contact_normal);
    if (surface_normal != nullptr)
        *surface_normal = normal_sum.normalized();
    return true;
}

bool generate_rib_geometry(const PrintObject &object, TracedRib &rib, const Polygon &bed,
                           const std::vector<PaintedLayer> &painted,
                           const std::vector<Polygons> &blockers)
{
    const PrintObjectConfig &config = object.config();
    const double thickness = rib.seed.dimensions.thickness;
    const double contact_width = rib.seed.dimensions.contact_width;
    const double gap = config.support_object_xy_distance.value;
    const size_t count = rib.boundary.size();
    std::vector<ExPolygons> bodies(count), body_centers(count), contacts(count);
    std::vector<ProjectionClip> clips(count);
    rib.contacts.assign(count, false);
    rib.contact_normals.assign(count, Vec3d::Zero());
    size_t band_begin = 0, band_end = 0;
    bool band_allowed = false;
    Vec3d band_normal = Vec3d::Zero();
    auto in_band = [&object, &config, &rib](size_t i) {
        const double dz = object.get_layer(int(rib.seed.top_layer))->print_z - object.get_layer(int(i))->print_z;
        const double period = config.rib_support_contact_spacing.value;
        const double phase = std::fmod(dz, period);
        return phase < config.rib_support_contact_height.value || period - phase < 1e-4;
    };

    for (size_t i = 0; i < count; ++i) {
        const Layer &layer = *object.get_layer(int(i));
        const double width = support_material_flow(&object, float(layer.height)).width();
        bodies[i] = intersection_ex(rib_body_area(layer, rib, i, thickness, gap), ExPolygons{ExPolygon(bed)});
        if (i == 0) {
            ExPolygons foot = diff_ex(intersection_ex(
                Polygons{foot_area(rib.body_start.front(), rib.tips.front(), rib.seed.normal, thickness, width)},
                offset_ex(bed, -float(scale_(0.5 * width)))),
                offset_ex(layer.lslices, float(scale_(gap + 0.5 * width))));
            expolygons_append(bodies[i], std::move(foot));
            bodies[i] = union_ex(bodies[i]);
        }
        body_centers[i] = offset_ex(bodies[i], -float(scale_(0.5 * width)));

        // Validate only the band encountered by this geometry traversal. The
        // short look-ahead preserves the all-or-none painted-contact rule.
        if (i == band_end) {
            band_begin = i;
            band_end = i + 1;
            band_allowed = in_band(i);
            if (band_allowed) {
                ++rib.planned_contact_bands;
                while (band_end < count && in_band(band_end))
                    ++band_end;
                for (size_t j = i; j < band_end; ++j) {
                    const Layer &contact_layer = *object.get_layer(int(j));
                    const double contact_line_width = support_material_flow(&object, float(contact_layer.height)).width();
                    const PaintedSegment *surface = nullptr;
                    if (!rib.wall_present[j]) {
                        rib.diagnostics.reject(RibReject::missing_wall, contact_layer.print_z);
                        band_allowed = false;
                    } else {
                        surface = painted_contact(painted[j].sections[0], rib.boundary[j], rib.seed.normal, contact_line_width);
                        if (surface == nullptr) {
                            diagnose_paint_lookup(painted[j].sections[0], rib.boundary[j], rib.seed.normal,
                                                  contact_layer.print_z, rib.diagnostics);
                            rib.diagnostics.reject(RibReject::missing_paint, contact_layer.print_z);
                            band_allowed = false;
                        } else if (j < blockers.size() && contains(blockers[j], scaled_point(rib.boundary[j]))) {
                            rib.diagnostics.reject(RibReject::blocked_contact, contact_layer.print_z);
                            band_allowed = false;
                        } else if (!painted_contact_width(contact_layer, painted[j], rib.boundary[j], surface->normal,
                                                          contact_width, contact_line_width, &rib.contact_normals[j],
                                                          &rib.diagnostics)) {
                            band_allowed = false;
                        } else if (!rib_contact_aligned(rib.seed.normal, rib.contact_normals[j].head<2>().normalized())) {
                            const double alignment = rib.seed.normal.dot(rib.contact_normals[j].head<2>().normalized());
                            rib.diagnostics.reject(RibReject::contact_turn, contact_layer.print_z,
                                std::acos(std::clamp(alignment, -1., 1.)) * 180 / M_PI);
                            band_allowed = false;
                        }
                    }
                    if (!band_allowed)
                        break;
                }
                if (band_allowed) {
                    ++rib.accepted_contact_bands;
                    band_normal = rib.contact_normals[i];
                }
            }
        }
        if (!band_allowed)
            continue;
        rib.contacts[i] = true;
        const Vec2d contact_normal = rib.contact_normals[i].head<2>().normalized();
        const Vec2d base = rib.body_start[i] + 0.5 * width * rib.seed.normal;
        const Polygon neck = rib_contact_polygon(rib.boundary[i] + 0.5 * width * contact_normal,
                                                 base, rib.seed.normal, contact_normal, contact_width, thickness, width);
        const ExPolygons section = intersection_ex(diff_ex(neck,
            offset_ex(layer.lslices, float(scale_(0.5 * width)))), offset_ex(bed, -float(scale_(0.5 * width))));
        if (section.empty())
            return rib.diagnostics.reject(RibReject::empty_neck, layer.print_z);
        expolygons_append(contacts[i], section);
        if (i == band_begin && i > 0) {
            backfill_contact_projection(object, rib, i, bed, thickness, contact_width, gap, band_normal,
                                        body_centers, contacts, clips);
        }
        // Resume at i + 1 after the nested i - 1 traversal finishes. No path
        // exists yet in any of the lower layers modified by that traversal.
    }
    if (!rib.contacts.back())
        return rib.diagnostics.reject(RibReject::top_band, object.get_layer(int(rib.seed.top_layer))->print_z);

    rib.areas.resize(count);
    rib.centers.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const double width = support_material_flow(&object, float(object.get_layer(int(i))->height)).width();
        contacts[i] = union_ex(contacts[i]);
        rib.centers[i] = body_centers[i];
        expolygons_append(rib.centers[i], contacts[i]);
        rib.centers[i] = union_ex(rib.centers[i]);
        // For round joins the last argument is a SCALED arc tolerance, not a
        // miter ratio. Bound envelope error to 5 microns instead of 3 scaled units.
        expolygons_append(bodies[i], offset_ex(contacts[i], float(scale_(0.5 * width)), jtRound, double(scale_(0.005))));
        ExPolygons area = union_ex(bodies[i]);
        if (area.size() != 1 || !area.front().holes.empty() || rib.centers[i].size() != 1 ||
            !rib.centers[i].front().holes.empty() || !rib.centers[i].front().contour.is_valid())
            return rib.diagnostics.reject(RibReject::topology, object.get_layer(int(i))->print_z);
        if (offset_ex(area, -float(scale_(width - EPSILON))).empty())
            return rib.diagnostics.reject(RibReject::thin_body, object.get_layer(int(i))->print_z);
        rib.areas[i] = std::move(area.front());
    }
    return true;
}

bool generate_rib_toolpaths(const PrintObject &object, TracedRib &rib)
{
    const int wall_count = std::max(1, object.config().tree_support_wall_count.value);
    rib.outlines.resize(rib.areas.size());
    // The bed layer is generated from the union of all rib feet.
    for (size_t i = 1; i < rib.areas.size(); ++i) {
        const Layer &layer = *object.get_layer(int(i));
        const Flow flow = support_material_flow(&object, float(layer.height));
        for (int wall = 0; wall < wall_count; ++wall) {
            Polyline outline;
            if (!closed_outline(ExPolygons{rib.areas[i]}, wall == 0 ? rib.centers[i] : ExPolygons{}, rib.seed.normal,
                                0.5 * flow.width() + wall * flow.spacing(), flow.width(), outline))
                break;
            rib.outlines[i].push_back(std::move(outline));
        }
        if (rib.outlines[i].empty())
            return rib.diagnostics.reject(RibReject::no_outline, layer.print_z);
    }
    return true;
}

void generate_rib_foot_toolpaths(const PrintObject &object, const std::vector<TracedRib> &ribs,
                                Polylines &paths, Polygons &fill)
{
    ExPolygons areas, centers;
    for (const TracedRib &rib : ribs) {
        areas.push_back(rib.areas.front());
        expolygons_append(centers, rib.centers.front());
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
std::vector<PaintedSegment> painted_runs(const PaintedLayer &painted)
{
    std::vector<PaintedSegment> segments;
    if (!has_contact_normal(painted))
        return segments;
    for (PaintedSegment segment : painted.sections[2]) {
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

constexpr double rib_direction_merge_alignment = 0.96592582628906828675; // cos(15 degrees).

// Rank locations on a painted run against the entire sliced face. Painting
// limits where a seed is allowed; it does not define the face's center.
void locate_rib_face(const Layer &layer, PaintedSegment &run, double tolerance)
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
                std::abs(edge.normalized().dot(tangent)) < rib_direction_merge_alignment ||
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

struct RibDemand {
    Vec2d direction;
    double initial;
    double remaining;
    bool blocked = false;
};

// Intersect a forward XY ray with a painted cut. A determines orientation;
// the intersection supplies the seed's XY and the cut's layer supplies Z.
bool rib_ray_contact(const Vec2d &origin, const Vec2d &direction,
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

struct RibPlan {
    RibCandidate seed;
    RibCandidate initial_seed;
    bool active = true;
    double farthest_along_sheet = 0;
    std::vector<RibCandidate> tops;
};

// A's plane stays fixed while its top follows the painted wall. Moving along A
// changes depth, not the sheet plane or its previously established contacts.
bool rib_line_contact(const Vec2d &origin, const Vec2d &direction,
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

const PrintRegion *rib_wall_region(const Layer &layer, const Vec2d &wall, const Vec2d &normal, double width)
{
    const Point inside = scaled_point(wall - 0.25 * width * normal);
    for (const LayerRegion *region : layer.regions())
        if (std::any_of(region->slices.surfaces.begin(), region->slices.surfaces.end(),
            [&inside](const Surface &surface) { return surface.expolygon.contains(inside); }))
            return &region->region();
    return nullptr;
}

bool rib_seed_contact(const PrintObject &object, const PaintedLayer &painted,
                      const std::vector<Polygons> &blockers, size_t layer_id,
                      const Vec2d &point, const Vec2d &direction, const PaintedSegment &run,
                      RibSupportDimensions &dimensions, Vec3d &normal, RibDiagnostics &diagnostics)
{
    const Layer &layer = *object.get_layer(int(layer_id));
    const double width = support_material_flow(&object).width();
    Vec2d wall = point;
    if (!projected_outer_contour(layer, point, direction, 2 * width, wall) || (wall - point).norm() > width)
        return diagnostics.reject(RibReject::top_wall, layer.print_z);
    if (layer_id < blockers.size() && contains(blockers[layer_id], scaled_point(wall)))
        return diagnostics.reject(RibReject::blocked_contact, layer.print_z);
    const PrintRegion *region = rib_wall_region(layer, wall, run.normal, width);
    if (region == nullptr)
        return diagnostics.reject(RibReject::seed_region, layer.print_z);
    dimensions = rib_support_dimensions(object, *region);
    if (!painted_contact_width(layer, painted, wall, run.normal, dimensions.contact_width,
                               width, &normal, &diagnostics))
        return false;
    if (!rib_contact_aligned(direction, normal.head<2>().normalized()))
        return diagnostics.reject(RibReject::contact_turn, layer.print_z);
    return true;
}

bool find_rib_seed(const PrintObject &object, const std::vector<PaintedLayer> &painted,
                   const std::vector<Polygons> &blockers, const std::vector<PaintedSegment> &runs,
                   size_t layer_id, const Vec2d &origin, const Vec2d &desired, double risk,
                   const std::vector<RibPlan> &plans, const std::vector<RibCandidate> &rejected,
                   RibDiagnostics &diagnostics, RibCandidate &result)
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
        if (rib_ray_contact(origin, direction, run.a, run.b, direct))
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
                alignment >= min_rib_contact_alignment - 1e-9) {
                const Vec2d side(-direction.y(), direction.x());
                const Vec2d face_side(-run.normal.y(), run.normal.x());
                const double face_weight = std::clamp(run.face_span /
                    (2 * object.config().rib_support_max_spacing.value), 0., 1.);
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
        if (std::any_of(plans.begin(), plans.end(), [&](const RibPlan &plan) {
                const Vec2d side(-plan.seed.normal.y(), plan.seed.normal.x());
                const Vec2d delta = site.point - plan.seed.point;
                const double spacing = std::max(0.5 * (plan.seed.dimensions.thickness + width),
                                                std::min(object.config().rib_support_max_spacing.value,
                                                         0.5 * (site.run->b - site.run->a).norm()));
                return plan.seed.normal.dot(site.direction) >= rib_direction_merge_alignment &&
                       plan.seed.contact_normal.dot(site.run->normal) >= rib_direction_merge_alignment &&
                       std::abs(delta.dot(side)) < spacing &&
                       std::abs(delta.dot(plan.seed.normal)) < object.config().rib_support_depth.value + width;
            }) || std::any_of(rejected.begin(), rejected.end(), [&](const RibCandidate &failed) {
                return failed.top_layer == layer_id && (failed.point - site.point).norm() < width &&
                       failed.normal.dot(site.direction) > min_rib_contact_alignment;
            }))
            continue;
        RibSupportDimensions dimensions;
        Vec3d surface_normal;
        if (!rib_seed_contact(object, painted[layer_id], blockers, layer_id, site.point, site.direction,
                              *site.run, dimensions, surface_normal, diagnostics))
            continue;
        const Vec2d contact_normal = surface_normal.head<2>().normalized();
        const double lean = -surface_normal.z() / surface_normal.head<2>().norm() *
                            std::max(0., contact_normal.dot(site.direction));
        const double weight = 1 - std::clamp(std::atan(lean) / (M_PI / 6.), 0., 1.);
        result = {site.point, site.direction, contact_normal, layer_id, dimensions,
                  object.config().rib_support_expansion_factor.value * risk * weight * weight};
        return true;
    }
    return diagnostics.reject(RibReject::direction, object.get_layer(int(layer_id))->print_z);
}

bool extend_rib_plan(const PrintObject &object, const std::vector<PaintedLayer> &painted,
                     const std::vector<Polygons> &blockers, const std::vector<PaintedSegment> &runs,
                     size_t layer_id, double risk, RibPlan &plan, RibDiagnostics &diagnostics)
{
    const Layer &layer = *object.get_layer(int(layer_id));
    const double width = support_material_flow(&object).width();
    for (const PaintedSegment &run : runs) {
        Vec2d point;
        if (!rib_line_contact(plan.seed.point, plan.seed.normal, run, point) ||
            (point - plan.seed.point).norm() > std::max(2 * width, 4 * layer.height) ||
            point.dot(plan.seed.normal) < plan.farthest_along_sheet - 0.5 * width)
            continue;
        RibSupportDimensions dimensions;
        Vec3d surface_normal;
        if (!rib_seed_contact(object, painted[layer_id], blockers, layer_id, point, plan.seed.normal,
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
        plan.seed.expansion_slope = object.config().rib_support_expansion_factor.value * risk * weight * weight;
        plan.tops.push_back(plan.seed);
        return true;
    }
    return false;
}

// A planning score. Exact contact capacity is still measured from the completed
// polygons, but no polygon is rebuilt on every layer of the upward sweep.
double planned_rib_capacity(const PrintObject &object, const RibPlan &plan, size_t layer_id, double bed_z)
{
    const double top = object.get_layer(int(plan.seed.top_layer))->print_z - bed_z;
    const double current = object.get_layer(int(layer_id))->print_z - bed_z;
    const double neck = object.config().support_object_xy_distance.value + 0.5 * plan.seed.dimensions.thickness;
    return std::pow(top / std::max(current, EPSILON), 2) * plan.seed.dimensions.thickness / 1.2 /
           (1 + neck / object.config().rib_support_max_spacing.value);
}

// A taller rib may replace a nearby lower one only if it preserves the lower
// contact schedule and at least the lower rib's reach at the bed.
bool rib_supersedes(const PrintObject &object, const TracedRib &higher, const TracedRib &lower)
{
    if (higher.seed.top_layer <= lower.seed.top_layer ||
        higher.seed.normal.dot(lower.seed.normal) < rib_direction_merge_alignment ||
        higher.seed.contact_normal.dot(lower.seed.contact_normal) < min_rib_contact_alignment ||
        higher.seed.dimensions.thickness + EPSILON < lower.seed.dimensions.thickness)
        return false;
    const Vec2d side(-lower.seed.normal.y(), lower.seed.normal.x());
    const double separation = std::abs((higher.seed.point - lower.seed.point).dot(side));
    if (separation > std::min(object.config().rib_support_max_spacing.value,
                              4 * lower.seed.dimensions.thickness) ||
        higher.tips.front().dot(lower.seed.normal) + support_material_flow(&object).width() <
            lower.tips.front().dot(lower.seed.normal))
        return false;
    auto contact_bands = [&object](const TracedRib &rib) {
        std::vector<double> centers;
        for (size_t i = 0; i < rib.contacts.size();) {
            if (!rib.contacts[i]) {
                ++i;
                continue;
            }
            const size_t first = i;
            while (i < rib.contacts.size() && rib.contacts[i])
                ++i;
            centers.push_back(0.5 * (object.get_layer(int(first))->print_z +
                                     object.get_layer(int(i - 1))->print_z));
        }
        return centers;
    };
    const double contact_window = object.config().rib_support_contact_spacing.value;
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

bool overlaps_rib(const TracedRib &candidate, const std::vector<TracedRib> &existing,
                  size_t &overlap_layer, const std::vector<size_t> &superseded = {})
{
    for (size_t rib_id = 0; rib_id < existing.size(); ++rib_id) {
        if (std::find(superseded.begin(), superseded.end(), rib_id) != superseded.end())
            continue;
        const TracedRib &rib = existing[rib_id];
        const size_t count = std::min(candidate.areas.size(), rib.areas.size());
        // Feet are deliberately united on the bed. Above it, overlapping
        // geometry would emit duplicate or crossing extrusion paths.
        for (size_t i = 1; i < count; ++i) {
            if (!candidate.area_bounds[i].overlap(rib.area_bounds[i]))
                continue;
            const ExPolygons overlap = intersection_ex(ExPolygons{candidate.areas[i]}, ExPolygons{rib.areas[i]});
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

void cache_rib_bounds(TracedRib &rib)
{
    rib.area_bounds.clear();
    rib.area_bounds.reserve(rib.areas.size());
    for (const ExPolygon &area : rib.areas)
        rib.area_bounds.push_back(get_extents(area));
}

} // namespace

void generate_rib_support(PrintObject &object)
{
    if (object.layer_count() < 2 || object.has_raft()) {
        BOOST_LOG_TRIVIAL(info) << "Rib support skipped: insufficient layers or raft";
        return;
    }
    RibDiagnostics diagnostics;

    const PrintObjectConfig &config = object.config();
    const auto painted = painted_sides(object);
    const auto blockers = object.slice_support_blockers();
    const Layer &first = *object.get_layer(0);
    if (first.lslices.empty()) {
        BOOST_LOG_TRIVIAL(info) << "Rib support skipped: empty first-layer footprint";
        return;
    }

    // The original footprint stays fixed while each printed prefix gets its
    // own centroid and directional demand. Ribs change capacity, not mass.
    const SectionMoments base = area_moments(first.lslices);
    const double base_area = base.area;
    if (base_area <= EPSILON * EPSILON) {
        BOOST_LOG_TRIVIAL(info) << "Rib support skipped: first-layer area=" << base_area;
        return;
    }
    const Vec2d base_center = base.first / base_area;
    const Eigen::Matrix2d base_distribution = area_moments(first.lslices, base_center).second / base_area;
    const double base_scale = std::sqrt(base_area);
    const double bed_z = first.print_z - first.height;
    struct BaseEdge { Vec2d normal; double limit; double initial_margin; };
    struct LayerState { Vec2d center; double risk = 0; std::vector<RibDemand> demand; };
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
    const double line_width = support_material_flow(&object).width();
    const Vec3d plate_origin = object.print()->get_plate_origin();
    Polygon bed = get_bed_shape_with_excluded_area(object.print()->config());
    bed.translate(Point(scale_(plate_origin.x()), scale_(plate_origin.y())) - object.instances().front().shift);
    std::vector<std::vector<PaintedSegment>> runs(painted.size());
    for (size_t i = 1; i < painted.size(); ++i) {
        if (object.print()->canceled())
            throw CanceledException();
        runs[i] = painted_runs(painted[i]);
        for (PaintedSegment &run : runs[i]) {
            run.face_center = 0.5 * (run.a + run.b);
            run.face_span = (run.b - run.a).norm();
            // Many short cuts indicate a faceted curved outline; its D-ray
            // ranking needs no full-contour scan for each facet.
            if (runs[i].size() <= 16)
                locate_rib_face(*object.get_layer(int(i)), run, line_width);
        }
    }

    auto add_demand = [](std::vector<RibDemand> &demands, const Vec2d &direction, double value) {
        if (value <= EPSILON)
            return;
        for (RibDemand &item : demands)
            if (item.direction.dot(direction) >= rib_direction_merge_alignment) {
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
        // changes its margin; the rib capacity will be subtracted separately.
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
        BOOST_LOG_TRIVIAL(info) << "Rib support skipped: peak risk <=0.8";
        return;
    }

    struct CapacityOverride { RibCandidate initial_seed; double fraction; };
    struct TopLimit { RibCandidate initial_seed; size_t layer; };
    auto same_seed = [](const RibCandidate &a, const RibCandidate &b) {
        return a.top_layer == b.top_layer && (a.point - b.point).squaredNorm() < 1e-8 &&
               (a.normal - b.normal).squaredNorm() < 1e-10;
    };
    std::vector<CapacityOverride> capacity_overrides;
    std::vector<TopLimit> top_limits;
    std::vector<TracedRib> ribs;
    std::vector<RibCandidate> rejected;
    size_t planning_passes = 0;
    size_t geometry_attempts = 0;
    size_t superseded_count = 0;
    bool retry;
    do {
        ++planning_passes;
        std::vector<RibPlan> plans;
        // One upward traversal builds all candidate histories. Existing plans
        // contribute before another seed is considered on the same layer.
        for (size_t layer_id = 1; layer_id < states.size(); ++layer_id) {
            if (object.print()->canceled())
                throw CanceledException();
            std::vector<RibCandidate> previous_tops;
            previous_tops.reserve(plans.size());
            for (RibPlan &plan : plans) {
                previous_tops.push_back(plan.seed);
                if (plan.active && plan.seed.top_layer + 1 == layer_id) {
                    const auto limit = std::find_if(top_limits.begin(), top_limits.end(), [&](const TopLimit &item) {
                        return same_seed(item.initial_seed, plan.initial_seed);
                    });
                    if ((limit != top_limits.end() && layer_id > limit->layer) ||
                        !extend_rib_plan(object, painted, blockers, runs[layer_id], layer_id,
                                         states[layer_id].risk, plan, diagnostics))
                        plan.active = false;
                }
            }
            // Converging plans share future demand. Stop the later one at its
            // previous valid top while preserving both lower rib histories.
            for (size_t j = 0; j < plans.size(); ++j) {
                if (!plans[j].active || plans[j].seed.top_layer != layer_id ||
                    previous_tops[j].top_layer == layer_id)
                    continue;
                for (size_t k = 0; k < j; ++k)
                    if (plans[k].active && plans[k].seed.top_layer == layer_id &&
                        plans[k].seed.normal.dot(plans[j].seed.normal) >= rib_direction_merge_alignment &&
                        (plans[k].seed.point - plans[j].seed.point).norm() <
                            0.5 * (plans[k].seed.dimensions.thickness + plans[j].seed.dimensions.thickness)) {
                        plans[j].seed = previous_tops[j];
                        plans[j].tops.pop_back();
                        plans[j].active = false;
                        break;
                    }
            }
            std::vector<RibDemand> residual = states[layer_id].demand;
            std::vector<bool> unavailable(residual.size(), false);
            auto deduct_plans = [&] {
                residual = states[layer_id].demand;
                for (size_t j = 0; j < residual.size(); ++j)
                    residual[j].blocked = unavailable[j];
                for (const RibPlan &plan : plans) {
                    double capacity = planned_rib_capacity(object, plan, layer_id, bed_z);
                    for (const CapacityOverride &override : capacity_overrides)
                        if (override.initial_seed.top_layer == plan.initial_seed.top_layer &&
                            (override.initial_seed.point - plan.initial_seed.point).squaredNorm() < 1e-8 &&
                            (override.initial_seed.normal - plan.initial_seed.normal).squaredNorm() < 1e-10) {
                            capacity *= override.fraction;
                            break;
                        }
                    for (RibDemand &need : residual) {
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
                [](const RibPlan &plan) { return plan.active; });
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
                RibCandidate seed;
                if (!find_rib_seed(object, painted, blockers, runs[layer_id], layer_id,
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
        ribs.clear();
        superseded_count = 0;
        std::vector<size_t> accepted_plan_indices;
        retry = false;
        for (size_t plan_id = 0; plan_id < plans.size(); ++plan_id) {
            const RibPlan &plan = plans[plan_id];
            ++geometry_attempts;
            TracedRib rib;
            rib.seed = plan.seed;
            const bool geometry_ok = trace_rib(object, bed, rib.seed, config.rib_support_depth.value,
                                                config.support_object_xy_distance.value,
                                                rib.seed.dimensions.thickness, line_width, rib) &&
                                     generate_rib_geometry(object, rib, bed, painted, blockers);
            if (geometry_ok)
                cache_rib_bounds(rib);
            std::vector<size_t> superseded;
            if (geometry_ok)
                for (size_t i = 0; i < ribs.size(); ++i)
                    if (rib_supersedes(object, rib, ribs[i]))
                        superseded.push_back(i);
            size_t overlap_layer = 0;
            const bool overlapping = geometry_ok && overlaps_rib(rib, ribs, overlap_layer, superseded);
            if (!geometry_ok || overlapping) {
                diagnostics.merge(rib.diagnostics);
                if (overlapping)
                    diagnostics.reject(RibReject::overlap, object.get_layer(int(overlap_layer))->print_z);
                // The final top may fail while the original contact remains
                // printable. Keep the highest printable earlier top and let
                // the next upward sweep fill the demand it no longer covers.
                bool earlier_top_valid = false;
                if (plan.tops.size() > 1) {
                    auto valid_top = [&](const RibCandidate &candidate) {
                        ++geometry_attempts;
                        TracedRib shorter;
                        shorter.seed = candidate;
                        size_t overlap_layer = 0;
                        if (!trace_rib(object, bed, candidate, config.rib_support_depth.value,
                                       config.support_object_xy_distance.value, candidate.dimensions.thickness,
                                       line_width, shorter) ||
                            !generate_rib_geometry(object, shorter, bed, painted, blockers))
                            return false;
                        cache_rib_bounds(shorter);
                        return !overlaps_rib(shorter, ribs, overlap_layer);
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
                // Earlier ribs remain available for overlap checks below.
                continue;
            }
            diagnostics.merge(rib.diagnostics);
            for (auto it = superseded.rbegin(); it != superseded.rend(); ++it) {
                ribs.erase(ribs.begin() + *it);
                accepted_plan_indices.erase(accepted_plan_indices.begin() + *it);
                ++superseded_count;
            }
            ribs.push_back(std::move(rib));
            accepted_plan_indices.push_back(plan_id);
        }
        if (retry)
            continue;
        // Band validity is known only after geometry. Correct the optimistic
        // planning score and rerun the inexpensive upward sweep when needed.
        for (size_t i = 0; i < ribs.size(); ++i) {
            const double fraction = double(ribs[i].accepted_contact_bands) /
                                    std::max<size_t>(ribs[i].planned_contact_bands, 1);
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
        for (size_t i = 0; i < ribs.size(); ++i)
            if (!generate_rib_toolpaths(object, ribs[i])) {
                diagnostics.reject(RibReject::no_outline,
                    ribs[i].diagnostics.failures[size_t(RibReject::no_outline)].first_z);
                rejected.push_back(plans[accepted_plan_indices[i]].initial_seed);
                retry = true;
                break;
            }
    } while (retry);

    BOOST_LOG_TRIVIAL(info) << "Rib support object=" << object.model_object()->name
                            << " accepted=" << ribs.size() << " planning_passes=" << planning_passes
                            << " geometry_attempts=" << geometry_attempts
                            << " superseded=" << superseded_count;
    diagnostics.log("all-attempts", object);
    for (size_t i = 0; i < ribs.size(); ++i) {
        const TracedRib &rib = ribs[i];
        BOOST_LOG_TRIVIAL(info) << "Rib support object=" << object.model_object()->name << " selected=" << i << " seed=(" << rib.seed.point.x() << ','
            << rib.seed.point.y() << ',' << object.get_layer(int(rib.seed.top_layer))->print_z << ") A="
            << std::atan2(rib.seed.normal.y(), rib.seed.normal.x()) * 180 / M_PI << " bands="
            << rib.accepted_contact_bands << '/' << rib.planned_contact_bands;
        rib.diagnostics.log("selected-rib", object);
    }

    size_t support_id = 0;
    for (size_t layer_id = 0; layer_id < object.layer_count(); ++layer_id) {
        const Layer &layer = *object.get_layer(int(layer_id));
        Polylines paths;
        Polygons feet;
        if (layer_id == 0) {
            generate_rib_foot_toolpaths(object, ribs, paths, feet);
        } else {
            for (const TracedRib &rib : ribs) {
                if (layer_id > rib.seed.top_layer)
                    continue;
                paths.insert(paths.end(), rib.outlines[layer_id].begin(), rib.outlines[layer_id].end());
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

} // namespace Slic3r
