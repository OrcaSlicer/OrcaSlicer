#include "RibSupport.hpp"
#include "SupportCommon.hpp"

#include "../Fill/Fill.hpp"
#include "../Flow.hpp"
#include "../Layer.hpp"
#include "../Model.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
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
};

struct RibCandidate {
    Vec2d point;
    Vec2d normal;
    size_t top_layer;
    double residual;
    RibSupportDimensions dimensions;
};

struct TracedRib {
    RibCandidate seed;
    std::vector<Vec2d> boundary; // One actual outer-contour position per layer, from the bed upward.
    std::vector<Vec2d> body_start;
    Vec2d tip;
    std::vector<bool> contacts;
    std::vector<ExPolygon> areas;
    std::vector<ExPolygons> centers;
    std::vector<Polylines> outlines;
};

Point scaled_point(const Vec2d &p) { return Point::new_scale(p.x(), p.y()); }

bool inside_model(const Layer &layer, const Vec2d &p)
{
    const Point point = scaled_point(p);
    return std::any_of(layer.lslices.begin(), layer.lslices.end(),
                       [&point](const ExPolygon &poly) { return poly.contains(point, false); });
}

Vec2d project_to_segment(const Vec2d &point, const PaintedSegment &segment)
{
    const Vec2d delta = segment.b - segment.a;
    const double length2 = delta.squaredNorm();
    const double t = length2 > 0 ? std::clamp((point - segment.a).dot(delta) / length2, 0., 1.) : 0.;
    return segment.a + t * delta;
}

// The paint is stored on 3D mesh facets. Slice the actual side facets here: the usual
// support-enforcer projection loses the side position and normal needed by a rib.
std::vector<std::vector<PaintedSegment>> painted_sides(const PrintObject &object)
{
    std::vector<std::vector<PaintedSegment>> result(object.layer_count());
    const auto layers = object.layers();
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
                [](const Layer *layer, double z) { return layer->slice_z < z; });
            for (; layer_it != layers.end() && (*layer_it)->slice_z <= zmax; ++layer_it) {
                const Layer &layer = **layer_it;
                const double z = layer.slice_z;
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
                if (inside_model(layer, mid + 0.6 * normal))
                    normal = -normal;
                if (inside_model(layer, mid + 0.6 * normal))
                    continue;
                Vec3d outward_normal = normal3;
                if (normal3.head<2>().dot(normal) < 0)
                    outward_normal = -outward_normal;
                result[size_t(layer_it - layers.begin())].push_back({intersections[0], intersections[1], normal, outward_normal});
            }
        }
    }
    return result;
}

bool clear_rib_line(const Layer &layer, const Polygon &bed, const Vec2d &from, const Vec2d &to, double width)
{
    const Vec2d direction = to - from;
    if (direction.squaredNorm() < 1e-8)
        return false;
    const Vec2d side(-direction.y(), direction.x());
    const Vec2d half_width = 0.5 * width * side.normalized();
    const int samples = std::max(2, int(std::ceil((to - from).norm() / std::max(width, 0.4))));
    for (int i = 0; i <= samples; ++i) {
        const Vec2d p = from + double(i) / samples * (to - from);
        if (!bed.contains(scaled_point(p + half_width)) || !bed.contains(scaled_point(p - half_width)) ||
            inside_model(layer, p) || inside_model(layer, p + half_width) || inside_model(layer, p - half_width))
            return false;
    }
    return true;
}

bool projected_outer_contour(const Layer &layer, const Vec2d &origin, const Vec2d &normal,
                             double outer_distance, Vec2d &nearest)
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
        return false;
    rib.boundary.resize(candidate.top_layer + 1);
    rib.body_start.resize(candidate.top_layer + 1);
    rib.tip = candidate.point + top_depth * candidate.normal;
    Vec2d target = candidate.point;
    double projected_inner = gap;
    for (size_t remaining = candidate.top_layer + 1; remaining > 0; --remaining) {
        const size_t i = remaining - 1;
        const Layer &layer = *object.get_layer(int(i));
        Vec2d point = target;
        const bool have_surface = projected_outer_contour(layer, candidate.point, candidate.normal, top_depth, point);
        if (i == candidate.top_layer && (!have_surface || (point - candidate.point).norm() > line_width))
            return false;
        const double surface_inner = (point - candidate.point).dot(candidate.normal) + gap;
        // Project the existing sheet downward, extend its inner edge to the wall, and clip model clearance.
        // A lower wall may not remove the load-bearing vertical projection of the upper sheet.
        if (i < candidate.top_layer && surface_inner > projected_inner + 0.5 * line_width)
            return false;
        projected_inner = std::min(projected_inner, surface_inner);
        const Vec2d from = candidate.point + std::max(projected_inner, surface_inner) * candidate.normal;
        if ((rib.tip - from).dot(candidate.normal) < 2 * line_width ||
            !clear_rib_line(layer, bed, from, rib.tip, thickness))
            return false;
        rib.boundary[i] = point;
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

Polygon rib_contact_polygon(const Vec2d &front, const Vec2d &base, const Vec2d &normal,
                            double contact_width, double thickness, double line_width)
{
    const Vec2d tangent(-normal.y(), normal.x());
    const Vec2d side = 0.5 * (thickness - line_width) * tangent;
    const Vec2d rear = base + line_width * normal;
    Points points{scaled_point(front - 0.5 * contact_width * tangent),
                  scaled_point(base - side), scaled_point(rear - side),
                  scaled_point(rear + side), scaled_point(base + side)};
    if (contact_width > 0)
        points.push_back(scaled_point(front + 0.5 * contact_width * tangent));
    return Polygon(std::move(points));
}

struct ProjectionClip {
    ExPolygons model;
    ExPolygons bed;
    bool ready = false;
};

ExPolygons rib_body_area(const Layer &layer, const TracedRib &rib, size_t layer_id,
                         double thickness, double gap)
{
    return diff_ex(rib_rectangle(rib.body_start[layer_id], rib.tip, rib.seed.normal, thickness),
                   offset_ex(layer.lslices, float(scale_(gap))));
}

void backfill_contact_projection(const PrintObject &object, const TracedRib &rib, size_t bottom,
                                 const Polygon &bed, double thickness,
                                 double contact_width, double gap, const Vec3d &surface_normal,
                                 const std::vector<ExPolygons> &body_centers, std::vector<ExPolygons> &contacts,
                                 std::vector<ProjectionClip> &clips)
{
    const Vec2d &normal = rib.seed.normal;
    // Rotate the contact surface's outward normal toward -Z in the rib plane.
    const double angle = object.config().rib_support_contact_projection_angle.value * M_PI / 180.;
    const double outward = surface_normal.head<2>().dot(normal);
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
    double front_distance = rib.boundary[bottom].dot(normal) + 0.5 * contact_line_width;
    const double initial_front_distance = front_distance;
    const double neck_depth = (rib.body_start[bottom] - rib.boundary[bottom]).dot(normal);
    for (size_t remaining = bottom; remaining > 0; --remaining) {
        const size_t i = remaining - 1;
        const Layer &layer = *object.get_layer(int(i));
        const double width = support_material_flow(&object, float(layer.height)).width();
        const double dz = object.get_layer(int(i + 1))->print_z - layer.print_z;
        front_distance += dz * slope;
        const Vec2d front = rib.seed.point + (front_distance - rib.seed.point.dot(normal)) * normal;
        // A projection only carries load; actual contacts come from this
        // layer's own band, including when another band's projection overlaps it.
        const double clearance = std::min(gap, 0.25 * width);
        const Vec2d base = rib.body_start[i] + 0.5 * width * normal;
        if ((base - front).dot(normal) <= EPSILON)
            break;
        const double advance = std::max(0., front_distance - initial_front_distance);
        const double fraction = std::clamp(advance / std::max(neck_depth, EPSILON), 0., 1.);
        const double projected_length = contact_width - std::max(0., contact_width - (thickness - width)) * fraction;
        const Vec2d side = 0.5 * projected_length * Vec2d(-normal.y(), normal.x());
        const Point a = scaled_point(front - side), b = scaled_point(front + side);
        if (std::any_of(body_centers[i].begin(), body_centers[i].end(), [&a, &b](const ExPolygon &body) {
            return a == b ? body.contains(a) : body.contains(Line(a, b));
        }))
            break;
        ProjectionClip &clip = clips[i];
        if (!clip.ready) {
            clip.model = offset_ex(layer.lslices, float(scale_(clearance + 0.5 * width)));
            clip.bed = offset_ex(bed, -float(scale_(0.5 * width)));
            clip.ready = true;
        }
        ExPolygons projection = intersection_ex(diff_ex(rib_contact_polygon(front, base, normal,
            projected_length, thickness, width), clip.model), clip.bed);
        if (projection.empty())
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
    const auto segment = std::find_if(segments.begin(), segments.end(), [&point, &normal, width](const PaintedSegment &segment) {
        const Vec2d delta = project_to_segment(point, segment) - point;
        const Vec2d tangent(-normal.y(), normal.x());
        return segment.normal.dot(normal) > 0.8 && delta.norm() <= width &&
               std::abs(delta.dot(tangent)) <= 1e-4;
    });
    return segment == segments.end() ? nullptr : &*segment;
}

bool painted_contact_width(const Layer &layer, const std::vector<PaintedSegment> &segments,
                           const Vec2d &point, const Vec2d &normal, double contact_width, double line_width)
{
    const Vec2d tangent(-normal.y(), normal.x());
    // Both bead edges must land on the allowed wall, not just its mathematical centerline.
    const double bead_span = contact_width + line_width;
    const int samples = std::max(2, int(std::ceil(bead_span / line_width)));
    for (int i = 0; i <= samples; ++i) {
        const Vec2d edge = point + (double(i) / samples - 0.5) * bead_span * tangent;
        Vec2d wall;
        if (!projected_outer_contour(layer, edge, normal, 2 * line_width, wall) ||
            (wall - edge).norm() > line_width || !painted_contact(segments, wall, normal, line_width))
            return false;
    }
    return true;
}

bool generate_rib_geometry(const PrintObject &object, TracedRib &rib, const Polygon &bed,
                           const std::vector<std::vector<PaintedSegment>> &painted,
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
    size_t band_begin = 0, band_end = 0;
    bool band_allowed = false;
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
                Polygons{foot_area(rib.body_start.front(), rib.tip, rib.seed.normal, thickness, width)},
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
                while (band_end < count && in_band(band_end))
                    ++band_end;
                for (size_t j = i; j < band_end; ++j) {
                    const Layer &contact_layer = *object.get_layer(int(j));
                    const double contact_line_width = support_material_flow(&object, float(contact_layer.height)).width();
                    if ((j < blockers.size() && contains(blockers[j], scaled_point(rib.boundary[j]))) ||
                        !painted_contact_width(contact_layer, painted[j], rib.boundary[j], rib.seed.normal,
                                               contact_width, contact_line_width)) {
                        band_allowed = false;
                        break;
                    }
                }
            }
        }
        if (!band_allowed)
            continue;
        rib.contacts[i] = true;
        const Vec2d base = rib.body_start[i] + 0.5 * width * rib.seed.normal;
        const Polygon neck = rib_contact_polygon(rib.boundary[i] + 0.5 * width * rib.seed.normal,
                                                 base, rib.seed.normal, contact_width, thickness, width);
        const ExPolygons section = intersection_ex(diff_ex(neck,
            offset_ex(layer.lslices, float(scale_(0.5 * width)))), offset_ex(bed, -float(scale_(0.5 * width))));
        if (section.empty())
            return false;
        expolygons_append(contacts[i], section);
        if (i == band_begin && i > 0) {
            const PaintedSegment *surface = painted_contact(painted[i], rib.boundary[i], rib.seed.normal, width);
            if (surface == nullptr)
                return false;
            backfill_contact_projection(object, rib, i, bed, thickness, contact_width, gap, surface->surface_normal,
                                        body_centers, contacts, clips);
        }
        // Resume at i + 1 after the nested i - 1 traversal finishes. No path
        // exists yet in any of the lower layers modified by that traversal.
    }
    if (!rib.contacts.back())
        return false;

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
            !rib.centers[i].front().holes.empty() || !rib.centers[i].front().contour.is_valid() ||
            offset_ex(area, -float(scale_(width - EPSILON))).empty())
            return false;
        rib.areas[i] = std::move(area.front());
    }
    return true;
}

bool generate_rib_toolpaths(const PrintObject &object, TracedRib &rib)
{
    const int wall_count = std::max(1, object.config().tree_support_wall_count.value);
    rib.outlines.resize(rib.areas.size());
    for (size_t i = 0; i < rib.areas.size(); ++i) {
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
            return false;
    }
    return true;
}

} // namespace

void generate_rib_support(PrintObject &object)
{
    if (object.layer_count() < 2 || object.has_raft())
        return;

    const PrintObjectConfig &config = object.config();
    const auto painted = painted_sides(object);
    const auto blockers = object.slice_support_blockers();
    const Layer &first = *object.get_layer(0);
    if (first.lslices.empty())
        return;

    // A narrow first-layer footprint and high painted sides increase the demand.
    // This is a geometric proxy for wobble; it does not claim to predict bed adhesion.
    const BoundingBox footprint = get_extents(first.lslices);
    const Vec2d center = unscale(footprint.center());
    const Vec2d size = unscale(footprint.size());
    const double radius = std::max(1., 0.5 * std::min(size.x(), size.y()));
    Vec2d volume_center = Vec2d::Zero();
    double volume_weight = 0;
    for (const Layer *layer : object.layers())
        for (const ExPolygon &slice : layer->lslices) {
            const double weight = slice.area() * layer->height;
            volume_center += weight * unscale(slice.contour.centroid());
            volume_weight += weight;
        }
    if (volume_weight > 0)
        volume_center /= volume_weight;
    else
        volume_center = center;
    const double max_spacing = config.rib_support_max_spacing.value;
    const double sample_step = std::max(2., max_spacing * 0.5);
    const double depth = config.rib_support_depth.value;
    const double gap = config.support_object_xy_distance.value;
    const double width = support_material_flow(&object).width();
    const Vec3d plate_origin = object.print()->get_plate_origin();
    Polygon bed = get_bed_shape_with_excluded_area(object.print()->config());
    bed.translate(Point(scale_(plate_origin.x()), scale_(plate_origin.y())) - object.instances().front().shift);

    std::vector<RibCandidate> candidates;
    for (size_t layer_id = 1; layer_id < painted.size(); ++layer_id) {
        const Layer &layer = *object.get_layer(int(layer_id));
        for (const PaintedSegment &segment : painted[layer_id]) {
            const double length = (segment.b - segment.a).norm();
            const int count = std::max(1, int(std::ceil(length / sample_step)));
            for (int i = 0; i < count; ++i) {
                const Vec2d point = segment.a + (i + 0.5) / count * (segment.b - segment.a);
                const Point inside = scaled_point(point - 0.25 * width * segment.normal);
                const PrintRegion *wall_region = nullptr;
                for (const LayerRegion *region : layer.regions())
                    if (std::any_of(region->slices.surfaces.begin(), region->slices.surfaces.end(),
                        [&inside](const Surface &surface) { return surface.expolygon.contains(inside); })) {
                        wall_region = &region->region();
                        break;
                    }
                if (wall_region == nullptr)
                    continue;
                const RibSupportDimensions dimensions = rib_support_dimensions(object, *wall_region);
                if (!painted_contact_width(layer, painted[layer_id], point, segment.normal, dimensions.contact_width, width))
                    continue;
                if (layer_id < blockers.size() && contains(blockers[layer_id], scaled_point(point)))
                    continue;
                const double outward = std::max(0., (point - center).dot(segment.normal));
                const double imbalance = std::max(0., (volume_center - center).dot(segment.normal));
                const double risk = layer.print_z / (2 * radius) *
                                    (1 + outward / (2 * radius) + imbalance / radius);
                if (risk > 0.8)
                    candidates.push_back({point, segment.normal, layer_id, risk, dimensions});
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const RibCandidate &a, const RibCandidate &b) {
        return a.residual > b.residual;
    });
    std::vector<TracedRib> ribs;
    for (RibCandidate &candidate_seed : candidates) {
        if (ribs.size() >= 256)
            break;
        if (candidate_seed.residual <= 0.8)
            continue;
        const RibCandidate selected = candidate_seed;
        candidate_seed.residual = 0;
        TracedRib rib;
        rib.seed = selected;
        if (!trace_rib(object, bed, selected, depth, gap, selected.dimensions.thickness, width, rib))
            continue;
        if (!generate_rib_geometry(object, rib, bed, painted, blockers))
            continue;
        ribs.push_back(std::move(rib));
        for (RibCandidate &candidate : candidates) {
            if (candidate.normal.dot(selected.normal) < 0.8)
                continue;
            if (candidate.top_layer >= ribs.back().boundary.size())
                continue;
            const double distance = (candidate.point - ribs.back().boundary[candidate.top_layer]).norm();
            if (distance < max_spacing * 0.5)
                candidate.residual = 0;
        }
    }

    // Match the existing support pipeline: all rib geometry, including lower
    // layer backfills, is complete before the first outline is generated.
    ribs.erase(std::remove_if(ribs.begin(), ribs.end(), [&object](TracedRib &rib) {
        return !generate_rib_toolpaths(object, rib);
    }), ribs.end());

    size_t support_id = 0;
    for (size_t layer_id = 0; layer_id < object.layer_count(); ++layer_id) {
        const Layer &layer = *object.get_layer(int(layer_id));
        Polylines paths;
        Polygons feet;
        for (const TracedRib &rib : ribs) {
            if (layer_id > rib.seed.top_layer)
                continue;
            paths.insert(paths.end(), rib.outlines[layer_id].begin(), rib.outlines[layer_id].end());
            if (layer_id == 0) {
                const Flow flow = support_material_flow(&object, float(layer.height));
                const double fill_inset = 0.5 * flow.width() + (rib.outlines.front().size() - 0.5) * flow.spacing();
                polygons_append(feet, to_polygons(offset_ex(rib.areas.front(), -float(scale_(fill_inset)))));
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
