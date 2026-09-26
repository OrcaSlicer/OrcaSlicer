///|/ Fuzzy skin for top surfaces.
///|/
///|/ The wall generator displaces points along the surface normal, which a horizontal surface
///|/ does not have. Top surfaces are textured by moving the nozzle in Z, by modulating the
///|/ deposited volume, or both, sampling the same noise field as the walls.
///|/
#include "FuzzySkin.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/VariableWidth.hpp"

#include "libnoise/noise.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace Slic3r {

namespace {

// A path carrying per-point Z has its extrusion scaled by (height + z_diff) / height, with no
// lower bound. Downward displacement is limited so the flow cannot fall to zero and leave gaps.
constexpr double MIN_FLOW_RATIO = 0.25;

constexpr double MIN_FLOW_FACTOR = 0.25;
constexpr double MAX_FLOW_FACTOR = 2.0;
// Flow::spacing() and Flow::mm3_per_mm() throw rather than clamp on a degenerate value.
constexpr double MIN_SPACING = 0.02;
constexpr double WIDTH_MERGE_TOLERANCE = 0.05;

struct TopFuzzSettings
{
    FuzzySkinTopMode  mode{FuzzySkinTopMode::Disabled};
    double            thickness{0.};
    double            point_distance{0.};
    double            layer_height{0.};
    double            slice_z{0.};
    FuzzySkinConfig   noise;
    const ExPolygons *painted_areas{nullptr};
};

std::optional<TopFuzzSettings> resolve_settings(const LayerRegion &region)
{
    const PrintRegionConfig &config = region.region().config();
    if (config.fuzzy_skin_top == FuzzySkinTopMode::Disabled)
        return std::nullopt;

    const Layer *layer = region.layer();
    TopFuzzSettings settings;

    if (config.fuzzy_skin_top_area == FuzzySkinTopArea::PaintedOnly) {
        if (layer->fuzzy_skin_painted_areas.empty())
            return std::nullopt;
        settings.painted_areas = &layer->fuzzy_skin_painted_areas;
    }

    const bool custom = config.fuzzy_skin_top_params == FuzzySkinTopParams::Custom;
    settings.thickness      = custom ? config.fuzzy_skin_top_thickness.value      : config.fuzzy_skin_thickness.value;
    settings.point_distance = custom ? config.fuzzy_skin_top_point_distance.value : config.fuzzy_skin_point_distance.value;
    if (settings.thickness <= 0. || settings.point_distance <= 0.)
        return std::nullopt;

    settings.mode         = config.fuzzy_skin_top;
    settings.layer_height = layer->height;
    settings.slice_z      = layer->slice_z;

    // Ripple follows a wall loop's arc length, which a horizontal surface does not have.
    settings.noise.noise_type = config.fuzzy_skin_noise_type == NoiseType::Ripple ? NoiseType::Classic
                                                                                 : config.fuzzy_skin_noise_type;
    settings.noise.noise_scale       = config.fuzzy_skin_scale;
    settings.noise.noise_octaves     = config.fuzzy_skin_octaves;
    settings.noise.noise_persistence = config.fuzzy_skin_persistence;
    settings.noise.thickness         = scaled<coord_t>(settings.thickness);
    settings.noise.point_distance    = scaled<coord_t>(settings.point_distance);
    return settings;
}

// Keeps the original vertices so corners stay sharp. Z is carried through unchanged.
Points3 resample(const Points3 &in, double point_distance)
{
    if (in.size() < 2)
        return in;

    const double step = std::max(scaled<double>(point_distance), 1.0);
    Points3      out;
    out.reserve(in.size() * 2);
    out.emplace_back(in.front());

    for (size_t i = 1; i < in.size(); ++i) {
        const Vec3crd &a   = in[i - 1];
        const Vec3crd &b   = in[i];
        const double   dx  = double(b.x()) - double(a.x());
        const double   dy  = double(b.y()) - double(a.y());
        const double   len = std::sqrt(dx * dx + dy * dy);
        for (int k = 1; k <= int(len / step); ++k) {
            const double t = double(k) * step / len;
            if (t >= 1.0)
                break;
            out.emplace_back(Vec3crd(coord_t(a.x() + dx * t),
                                     coord_t(a.y() + dy * t),
                                     coord_t(a.z() + (double(b.z()) - double(a.z())) * t)));
        }
        out.emplace_back(b);
    }
    return out;
}

// Classic noise returns a fresh random value on every call, so samples cannot be recreated.
std::vector<double> sample_noise(const Points3 &points, noise::module::Module &noise, double slice_z)
{
    std::vector<double> samples;
    samples.reserve(points.size());
    for (const Vec3crd &p : points)
        samples.emplace_back(noise.GetValue(unscale_(p.x()), unscale_(p.y()), slice_z));
    return samples;
}

// Z on a contoured path is a delta from the layer plane, so this composes with Z anti-aliasing.
coord_t displaced_z(coord_t base_z, double noise, double amplitude, double path_height)
{
    const double limit = (1.0 - MIN_FLOW_RATIO) * path_height;
    const double dz    = noise >= 0. ? noise * amplitude : noise * std::min(amplitude, limit);
    return coord_t(std::max(double(base_z) + scaled<double>(dz), -scaled<double>(limit)));
}

double spacing_to_width(double spacing, double height) { return spacing + height * (1. - 0.25 * M_PI); }
double width_to_spacing(double width, double height) { return width - height * (1. - 0.25 * M_PI); }

// Resolves Z for the points the width split inserts. They lie on the original path and arrive in
// order, so how far along the path each one is equals how far the walk has travelled. Locating
// them by projection onto the nearest segment instead stalls wherever the path doubles back,
// which on a dense Hilbert curve is immediately.
class ZAlongPath
{
public:
    explicit ZAlongPath(const Points3 &points) : m_points(points)
    {
        m_distance_to.reserve(points.size());
        m_distance_to.emplace_back(0.);
        for (size_t i = 1; i < points.size(); ++i)
            m_distance_to.emplace_back(m_distance_to.back() + distance_2d(points[i - 1], points[i]));
    }

    coord_t next(const Point &point)
    {
        if (m_started)
            m_travelled += (point - m_previous).cast<double>().norm();
        m_previous = point;
        m_started  = true;

        while (m_segment + 2 < m_points.size() && m_distance_to[m_segment + 1] < m_travelled)
            ++m_segment;

        const Vec3crd &a      = m_points[m_segment];
        const Vec3crd &b      = m_points[m_segment + 1];
        const double   length = m_distance_to[m_segment + 1] - m_distance_to[m_segment];
        const double   t      = length > 0. ? std::clamp((m_travelled - m_distance_to[m_segment]) / length, 0., 1.) : 0.;
        return coord_t(double(a.z()) + (double(b.z()) - double(a.z())) * t);
    }

private:
    static double distance_2d(const Vec3crd &a, const Vec3crd &b)
    {
        const double dx = double(b.x()) - double(a.x());
        const double dy = double(b.y()) - double(a.y());
        return std::sqrt(dx * dx + dy * dy);
    }

    const Points3      &m_points;
    std::vector<double> m_distance_to;
    double              m_travelled{0.};
    Point               m_previous;
    bool                m_started{false};
    size_t              m_segment{0};
};

bool displace_in_place(ExtrusionPath &path, const TopFuzzSettings &settings)
{
    auto noise = Feature::FuzzySkin::get_noise_module(settings.noise);
    if (!noise)
        return false;

    Points3 points = resample(path.polyline.points, settings.point_distance);
    if (points.size() < 2)
        return false;

    const std::vector<double> samples = sample_noise(points, *noise, settings.slice_z);
    for (size_t i = 0; i < points.size(); ++i)
        points[i].z() = displaced_z(points[i].z(), samples[i], settings.thickness, path.height);

    path.polyline.points = std::move(points);
    path.polyline.fitting_result.clear(); // arc fitting cannot represent per-point Z
    path.z_contoured     = true;
    return true;
}

// ThickPolyline::width holds spacing, in scaled units, indexed as two entries per segment.
void modulate_flow(const ExtrusionPath &path, const TopFuzzSettings &settings, bool displace, ExtrusionEntitiesPtr &out)
{
    auto noise = Feature::FuzzySkin::get_noise_module(settings.noise);
    if (!noise)
        return;

    Points3 points = resample(path.polyline.points, settings.point_distance);
    if (points.size() < 2)
        return;

    const double nominal_spacing = width_to_spacing(path.width, path.height);
    if (nominal_spacing <= 0.)
        return;

    const std::vector<double> samples = sample_noise(points, *noise, settings.slice_z);

    std::vector<coordf_t> spacings;
    spacings.reserve(points.size());
    for (double sample : samples) {
        // Volume is proportional to spacing, so thickness converts directly into a flow factor.
        const double factor = settings.layer_height > 0. ? 1.0 + (sample * settings.thickness) / settings.layer_height
                                                         : 1.0;
        spacings.emplace_back(scaled<double>(std::max(nominal_spacing * std::clamp(factor, MIN_FLOW_FACTOR, MAX_FLOW_FACTOR),
                                                      MIN_SPACING)));
    }

    // Resolve Z before splitting: the inserted points cannot be re-sampled, only interpolated.
    if (displace)
        for (size_t i = 0; i < points.size(); ++i)
            points[i].z() = displaced_z(points[i].z(), samples[i], settings.thickness, path.height);

    ThickPolyline thick;
    thick.points.reserve(points.size());
    for (const Vec3crd &p : points)
        thick.points.emplace_back(Point(p.x(), p.y()));
    thick.width.reserve(2 * (thick.points.size() - 1));
    for (size_t i = 0; i + 1 < spacings.size(); ++i) {
        thick.width.emplace_back(spacings[i]);
        thick.width.emplace_back(spacings[i + 1]);
    }
    thick.endpoints = std::make_pair(false, false);

    const Flow         flow(path.width, path.height, 0.f);
    ExtrusionMultiPath multi = thick_polyline_to_multi_path(thick, path.role(), flow,
                                                            scaled<float>(WIDTH_MERGE_TOLERANCE),
                                                            float(SCALED_EPSILON));

    ZAlongPath z_along_path(points);
    for (ExtrusionPath &piece : multi.paths) {
        piece.set_extrusion_role(path.role());
        piece.height = path.height;
        if (displace) {
            for (Vec3crd &p : piece.polyline.points)
                p.z() = z_along_path.next(Point(p.x(), p.y()));
            piece.z_contoured = true;
        }
        out.emplace_back(piece.clone());
    }
}

// Textures a path, either in place or by producing replacements in `out`.
void texture_path(ExtrusionPath &path, const TopFuzzSettings &settings, ExtrusionEntitiesPtr &out)
{
    if (path.polyline.size() < 2 || path.height <= 0.f)
        return;

    if (settings.mode == FuzzySkinTopMode::Displacement)
        displace_in_place(path, settings);
    else
        modulate_flow(path, settings, settings.mode == FuzzySkinTopMode::Combined, out);
}

std::unique_ptr<ExtrusionPath> clone_with(const ExtrusionPath &path, Polyline &&polyline)
{
    auto piece = std::unique_ptr<ExtrusionPath>(static_cast<ExtrusionPath *>(path.clone()));
    piece->polyline = Polyline3(std::move(polyline));
    piece->polyline.fitting_result.clear();
    piece->z_contoured = false;
    return piece;
}

void texture_painted_parts(const ExtrusionPath &path, const TopFuzzSettings &settings, ExtrusionEntitiesPtr &out)
{
    const Polylines whole{path.polyline.to_polyline()};
    Polylines       painted   = intersection_pl(whole, *settings.painted_areas);
    Polylines       unpainted = diff_pl(whole, *settings.painted_areas);

    for (Polyline &polyline : painted) {
        if (polyline.size() < 2)
            continue;
        std::unique_ptr<ExtrusionPath> piece = clone_with(path, std::move(polyline));
        ExtrusionEntitiesPtr           produced;
        texture_path(*piece, settings, produced);
        if (produced.empty())
            out.emplace_back(piece.release());
        else
            append(out, std::move(produced));
    }
    for (Polyline &polyline : unpainted)
        if (polyline.size() >= 2)
            out.emplace_back(clone_with(path, std::move(polyline)).release());
}

void texture_collection(ExtrusionEntityCollection &collection, const TopFuzzSettings &settings);

void texture_entity(ExtrusionEntity *entity, const TopFuzzSettings &settings, ExtrusionEntitiesPtr &out)
{
    if (auto *collection = dynamic_cast<ExtrusionEntityCollection *>(entity)) {
        // Unconditionally: a mixed collection reports erMixed, not the roles it contains.
        texture_collection(*collection, settings);
        out.emplace_back(entity);
        return;
    }

    if (auto *multi = dynamic_cast<ExtrusionMultiPath *>(entity)) {
        // Only Z: flow modulation would have to restructure the multipath.
        for (ExtrusionPath &piece : multi->paths)
            if (piece.role() == erTopSolidInfill)
                displace_in_place(piece, settings);
        out.emplace_back(entity);
        return;
    }

    auto *path = dynamic_cast<ExtrusionPath *>(entity);
    if (path == nullptr || path->role() != erTopSolidInfill) {
        out.emplace_back(entity);
        return;
    }

    if (settings.painted_areas != nullptr) {
        const size_t before = out.size();
        texture_painted_parts(*path, settings, out);
        if (out.size() == before) {
            out.emplace_back(entity);
            return;
        }
        delete entity;
        return;
    }

    ExtrusionEntitiesPtr produced;
    texture_path(*path, settings, produced);
    if (produced.empty()) {
        out.emplace_back(entity);
    } else {
        append(out, std::move(produced));
        delete entity;
    }
}

void texture_collection(ExtrusionEntityCollection &collection, const TopFuzzSettings &settings)
{
    ExtrusionEntitiesPtr replacement;
    replacement.reserve(collection.entities.size());
    for (ExtrusionEntity *entity : collection.entities)
        texture_entity(entity, settings, replacement);
    collection.entities = std::move(replacement);
}

} // namespace

void Layer::make_fuzzy_skin_top()
{
    for (LayerRegion *region : this->regions())
        if (const std::optional<TopFuzzSettings> settings = resolve_settings(*region))
            texture_collection(region->fills, *settings);
}

} // namespace Slic3r
