#include "FacetSelectors.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <nlohmann/json.hpp>

#include "AABBTreeIndirect.hpp"
#include "BoundingBox.hpp"
#include "Model.hpp"

namespace Slic3r {
namespace FacetSelectors {

using json = nlohmann::json;

namespace {

constexpr double DEG = M_PI / 180.;

double clamp_unit(double v) { return std::max(-1., std::min(1., v)); }

// Angle in degrees between a unit vector and a unit (or zero) vector; 90 for a zero vector.
double angle_deg(const Vec3d &a, const Vec3d &b) { return std::acos(clamp_unit(a.dot(b))) / DEG; }

double round6(double v) { return std::round(v * 1e6) / 1e6; }
json   vec_json_r(const Vec3d &v) { return json::array({round6(v.x()), round6(v.y()), round6(v.z())}); }

template<class Inside> bool test_points(const VolumeGeometry &g, size_t f, PointTest test, Inside inside)
{
    switch (test) {
    case PointTest::Centroid: return inside(g.centroid(f));
    case PointTest::All: return inside(g.vertex(f, 0)) && inside(g.vertex(f, 1)) && inside(g.vertex(f, 2));
    case PointTest::Any: return inside(g.vertex(f, 0)) || inside(g.vertex(f, 1)) || inside(g.vertex(f, 2));
    }
    return false;
}

// ----------------------------------------------------------------------------- leaf selectors

class AllSelector : public Selector
{
public:
    void evaluate(const EvalContext &, size_t, std::vector<uint8_t> &mask) const override { std::fill(mask.begin(), mask.end(), uint8_t(1)); }
};

class OverhangSelector : public Selector
{
public:
    explicit OverhangSelector(double max_angle_deg) : m_cos(std::cos(max_angle_deg * DEG)) {}
    void evaluate(const EvalContext &ctx, size_t v, std::vector<uint8_t> &mask) const override
    {
        const VolumeGeometry &g = ctx.volumes[v];
        for (size_t f = 0; f < g.size(); ++f)
            // Same rule as GLGizmoFdmSupports::select_facets_by_angle: dot(normal, down) > cos(threshold).
            if (-g.normal(f).z() > m_cos)
                mask[f] = 1;
    }

private:
    double m_cos;
};

class NormalSelector : public Selector
{
public:
    NormalSelector(const Vec3d &dir, double within_deg) : m_dir(dir.normalized()), m_cos(std::cos(within_deg * DEG)) {}
    void evaluate(const EvalContext &ctx, size_t v, std::vector<uint8_t> &mask) const override
    {
        const VolumeGeometry &g = ctx.volumes[v];
        for (size_t f = 0; f < g.size(); ++f)
            if (g.normal(f).squaredNorm() > 0.5 && g.normal(f).dot(m_dir) >= m_cos - 1e-12)
                mask[f] = 1;
    }

private:
    Vec3d  m_dir;
    double m_cos;
};

template<class Inside> class SpatialSelector : public Selector
{
public:
    SpatialSelector(Inside inside, PointTest test) : m_inside(std::move(inside)), m_test(test) {}
    void evaluate(const EvalContext &ctx, size_t v, std::vector<uint8_t> &mask) const override
    {
        const VolumeGeometry &g = ctx.volumes[v];
        for (size_t f = 0; f < g.size(); ++f)
            if (test_points(g, f, m_test, m_inside))
                mask[f] = 1;
    }

private:
    Inside    m_inside;
    PointTest m_test;
};

template<class Inside> SelectorPtr make_spatial(Inside inside, PointTest test)
{
    return std::make_shared<SpatialSelector<Inside>>(std::move(inside), test);
}

class FacetsSelector : public Selector
{
public:
    FacetsSelector(size_t volume_id, int volume_index, std::vector<int> indices)
        : m_volume_id(volume_id), m_volume_index(volume_index), m_indices(std::move(indices))
    {}
    void evaluate(const EvalContext &ctx, size_t v, std::vector<uint8_t> &mask) const override
    {
        const VolumeGeometry &g = ctx.volumes[v];
        if (m_volume_id != 0) {
            if (g.volume_id() != m_volume_id)
                return;
        } else if (m_volume_index >= 0 && g.volume_index() != m_volume_index)
            return;
        for (int i : m_indices)
            if (i >= 0 && size_t(i) < g.size())
                mask[size_t(i)] = 1;
    }

private:
    size_t           m_volume_id;
    int              m_volume_index;
    std::vector<int> m_indices;
};

// Flood fill over edge neighbours keeping facets within the tolerance of the seed normal.
// `smooth`: compare each facet with the neighbour it is reached from (the painting gizmos' smart
// fill, seed_fill_select_triangles), so the flood follows a gently curving surface such as a
// cylinder wall; otherwise with the seed's normal, which keeps it to one flat face.
void flood_planar(const VolumeGeometry &g, size_t seed, double cos_tol, std::vector<uint8_t> &mask, bool smooth = false)
{
    const Vec3d                 n0 = g.normal(seed);
    const std::vector<Vec3i32> &nb = g.neighbors();
    std::vector<size_t>         stack{seed};
    mask[seed] = 1;
    while (!stack.empty()) {
        size_t f = stack.back();
        stack.pop_back();
        for (int k = 0; k < 3; ++k) {
            int n = nb[f](k);
            if (n < 0 || mask[size_t(n)])
                continue;
            if (g.normal(size_t(n)).dot(smooth ? g.normal(f) : n0) >= cos_tol - 1e-12) {
                mask[size_t(n)] = 1;
                stack.push_back(size_t(n));
            }
        }
    }
}

struct Seed
{
    size_t volume = 0;
    size_t facet  = 0;
    bool   valid  = false;
};

class PlanarRegionSelector : public Selector
{
public:
    PlanarRegionSelector(bool by_point, const Vec3d &v, double tolerance_deg, bool smooth = false)
        : m_by_point(by_point), m_v(by_point ? v : Vec3d(v.normalized())), m_cos(std::cos(tolerance_deg * DEG)), m_smooth(smooth)
    {}

    void evaluate(const EvalContext &ctx, size_t v, std::vector<uint8_t> &mask) const override
    {
        Seed seed = m_by_point ? seed_by_point(ctx) : seed_by_direction(ctx);
        if (seed.valid && seed.volume == v)
            flood_planar(ctx.volumes[v], seed.facet, m_cos, mask, m_smooth);
    }

private:
    Seed seed_by_direction(const EvalContext &ctx) const
    {
        // Outermost facet facing the direction within tolerance; larger area wins ties.
        Seed   best, fallback;
        double best_pos = -std::numeric_limits<double>::max(), best_area = -1.;
        double fallback_dot = -2.;
        for (size_t v = 0; v < ctx.volumes.size(); ++v) {
            const VolumeGeometry &g = ctx.volumes[v];
            for (size_t f = 0; f < g.size(); ++f) {
                if (g.area(f) <= 0.)
                    continue;
                double d = g.normal(f).dot(m_v);
                if (d > fallback_dot) {
                    fallback_dot = d;
                    fallback     = {v, f, true};
                }
                if (d < m_cos - 1e-12)
                    continue;
                double pos = g.centroid(f).dot(m_v);
                if (pos > best_pos + 1e-6 || (std::abs(pos - best_pos) <= 1e-6 && g.area(f) > best_area)) {
                    best_pos  = pos;
                    best_area = g.area(f);
                    best      = {v, f, true};
                }
            }
        }
        return best.valid ? best : fallback;
    }

    Seed seed_by_point(const EvalContext &ctx) const
    {
        Seed   best;
        double best_d2 = std::numeric_limits<double>::max();
        for (size_t v = 0; v < ctx.volumes.size(); ++v) {
            const VolumeGeometry &g = ctx.volumes[v];
            if (g.size() == 0)
                continue;
            std::vector<Vec3d> verts;
            verts.reserve(g.size() * 3);
            std::vector<Vec3i32> faces;
            faces.reserve(g.size());
            for (size_t f = 0; f < g.size(); ++f) {
                for (int k = 0; k < 3; ++k)
                    verts.push_back(g.vertex(f, k));
                faces.emplace_back(int(3 * f), int(3 * f + 1), int(3 * f + 2));
            }
            auto   tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(verts, faces);
            size_t hit  = 0;
            Vec3d  hp;
            double d2 = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(verts, faces, tree, m_v, hit, hp);
            if (d2 >= 0. && d2 < best_d2) {
                best_d2 = d2;
                best    = {v, hit, true};
            }
        }
        return best;
    }

    bool   m_by_point;
    Vec3d  m_v;
    double m_cos;
    bool   m_smooth;
};

// ----------------------------------------------------------------------------- combinators

class AllOfSelector : public Selector
{
public:
    explicit AllOfSelector(std::vector<SelectorPtr> c) : m_children(std::move(c)) {}
    void evaluate(const EvalContext &ctx, size_t v, std::vector<uint8_t> &mask) const override
    {
        if (m_children.empty()) {
            std::fill(mask.begin(), mask.end(), uint8_t(1));
            return;
        }
        std::vector<uint8_t> tmp(mask.size());
        m_children.front()->evaluate(ctx, v, mask);
        for (size_t i = 1; i < m_children.size(); ++i) {
            std::fill(tmp.begin(), tmp.end(), uint8_t(0));
            m_children[i]->evaluate(ctx, v, tmp);
            for (size_t f = 0; f < mask.size(); ++f)
                mask[f] = mask[f] && tmp[f];
        }
    }

private:
    std::vector<SelectorPtr> m_children;
};

class AnyOfSelector : public Selector
{
public:
    explicit AnyOfSelector(std::vector<SelectorPtr> c) : m_children(std::move(c)) {}
    void evaluate(const EvalContext &ctx, size_t v, std::vector<uint8_t> &mask) const override
    {
        std::vector<uint8_t> tmp(mask.size());
        for (const SelectorPtr &c : m_children) {
            std::fill(tmp.begin(), tmp.end(), uint8_t(0));
            c->evaluate(ctx, v, tmp);
            for (size_t f = 0; f < mask.size(); ++f)
                mask[f] = mask[f] || tmp[f];
        }
    }

private:
    std::vector<SelectorPtr> m_children;
};

class NotSelector : public Selector
{
public:
    explicit NotSelector(SelectorPtr c) : m_child(std::move(c)) {}
    void evaluate(const EvalContext &ctx, size_t v, std::vector<uint8_t> &mask) const override
    {
        m_child->evaluate(ctx, v, mask);
        for (uint8_t &m : mask)
            m = m ? 0 : 1;
    }

private:
    SelectorPtr m_child;
};

// ----------------------------------------------------------------------------- painting internals

// TriangleSelector with access to its leaves, for state replacement, collapsing and statistics.
class PaintSelector : public TriangleSelector
{
public:
    explicit PaintSelector(const TriangleMesh &mesh) : TriangleSelector(mesh) {}

    void replace_state(EnforcerBlockerType from, EnforcerBlockerType to)
    {
        for (Triangle &tr : m_triangles)
            if (tr.valid() && !tr.is_split() && tr.get_state() == from)
                tr.set_state(to);
    }

    // Merge every split original facet whose leaves all share one state back into one triangle,
    // so that erasing painted brush strokes really empties the annotation.
    void collapse_uniform()
    {
        for (int i = 0; i < m_orig_size_indices; ++i) {
            if (!m_triangles[size_t(i)].is_split())
                continue;
            EnforcerBlockerType s     = EnforcerBlockerType::NONE;
            bool                first = true;
            if (uniform(i, s, first))
                set_facet(i, s);
        }
    }

    StateStatsMap stats(const Transform3d &world) const
    {
        StateStatsMap out;
        for (const Triangle &tr : m_triangles) {
            if (!tr.valid() || tr.is_split() || tr.get_state() == EnforcerBlockerType::NONE)
                continue;
            const Vec3d a  = world * m_vertices[size_t(tr.verts_idxs[0])].v.cast<double>();
            const Vec3d b  = world * m_vertices[size_t(tr.verts_idxs[1])].v.cast<double>();
            const Vec3d c  = world * m_vertices[size_t(tr.verts_idxs[2])].v.cast<double>();
            StateStats &st = out[int(tr.get_state())];
            st.facets += 1;
            st.area_mm2 += 0.5 * (b - a).cross(c - a).norm();
        }
        return out;
    }

private:
    bool uniform(int idx, EnforcerBlockerType &s, bool &first) const
    {
        const Triangle &tr = m_triangles[size_t(idx)];
        if (!tr.is_split()) {
            if (first) {
                s     = tr.get_state();
                first = false;
                return true;
            }
            return tr.get_state() == s;
        }
        for (int c = 0; c <= tr.number_of_split_sides(); ++c)
            if (!uniform(tr.children[size_t(c)], s, first))
                return false;
        return true;
    }
};

// ----------------------------------------------------------------------------- JSON helpers

struct Parser
{
    ParseError *err;
    int         depth = 0;

    SelectorPtr fail(const std::string &path, const std::string &msg)
    {
        if (err && err->message.empty()) {
            err->path    = path;
            err->message = msg;
        }
        return nullptr;
    }

    bool number(const json &j, const std::string &path, double &out)
    {
        if (!j.is_number() || !std::isfinite(j.get<double>())) {
            fail(path, "expected a finite number");
            return false;
        }
        out = j.get<double>();
        return true;
    }

    bool vec3(const json &j, const std::string &path, Vec3d &out)
    {
        if (!j.is_array() || j.size() != 3) {
            fail(path, "expected an array of 3 numbers");
            return false;
        }
        for (size_t i = 0; i < 3; ++i)
            if (!number(j[i], path + "[" + std::to_string(i) + "]", out[int(i)]))
                return false;
        return true;
    }

    bool only_keys(const json &obj, const std::string &path, std::initializer_list<const char *> allowed)
    {
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            bool ok = false;
            for (const char *k : allowed)
                if (it.key() == k)
                    ok = true;
            if (!ok) {
                fail(path + "." + it.key(), "unknown property");
                return false;
            }
        }
        return true;
    }

    bool point_test(const json &obj, const std::string &path, PointTest &out)
    {
        out = PointTest::Centroid;
        if (!obj.contains("test"))
            return true;
        const json &t = obj["test"];
        if (t == "centroid")
            out = PointTest::Centroid;
        else if (t == "all")
            out = PointTest::All;
        else if (t == "any")
            out = PointTest::Any;
        else {
            fail(path + ".test", "expected \"centroid\", \"all\" or \"any\"");
            return false;
        }
        return true;
    }

    bool angle(const json &obj, const char *key, const std::string &path, double def, double lo, double hi, double &out)
    {
        out = def;
        if (!obj.contains(key))
            return true;
        if (!number(obj[key], path + "." + key, out))
            return false;
        if (out < lo || out > hi) {
            fail(path + "." + key, "must be between " + std::to_string(int(lo)) + " and " + std::to_string(int(hi)));
            return false;
        }
        return true;
    }

    bool direction(const json &obj, const char *side_key, const char *dir_key, const std::string &path, Vec3d &out)
    {
        bool has_side = obj.contains(side_key), has_dir = obj.contains(dir_key);
        if (has_side == has_dir) {
            fail(path, std::string("exactly one of \"") + side_key + "\" or \"" + dir_key + "\" is required");
            return false;
        }
        if (has_side) {
            if (!obj[side_key].is_string() || !side_direction(obj[side_key].get<std::string>(), out)) {
                fail(path + "." + side_key, "expected rear, front, left, right, top or bottom");
                return false;
            }
            return true;
        }
        if (!vec3(obj[dir_key], path + "." + dir_key, out))
            return false;
        if (out.norm() < 1e-9) {
            fail(path + "." + dir_key, "direction must not be zero");
            return false;
        }
        return true;
    }

    std::vector<SelectorPtr> children(const json &arr, const std::string &path, bool &ok)
    {
        std::vector<SelectorPtr> out;
        ok = true;
        if (arr.size() > 64) {
            fail(path, "at most 64 children");
            ok = false;
            return out;
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            SelectorPtr c = parse(arr[i], path + "[" + std::to_string(i) + "]");
            if (!c) {
                ok = false;
                return {};
            }
            out.push_back(std::move(c));
        }
        return out;
    }

    SelectorPtr parse(const json &j, const std::string &path)
    {
        if (++depth > 16) {
            --depth;
            return fail(path, "selector nested too deeply (max 16)");
        }
        SelectorPtr out = parse_inner(j, path);
        --depth;
        return out;
    }

    SelectorPtr parse_inner(const json &j, const std::string &path)
    {
        if (!j.is_object() || j.size() != 1)
            return fail(path, "a selector is an object with exactly one key");
        const std::string key  = j.begin().key();
        const json       &a    = j.begin().value();
        const std::string here = path + "." + key;

        if (key == "all" || key == "any") {
            if (a.is_array()) {
                bool ok   = false;
                auto kids = children(a, here, ok);
                if (!ok)
                    return nullptr;
                return key == "all" ? select_all_of(std::move(kids)) : select_any_of(std::move(kids));
            }
            if (key == "all" && ((a.is_object() && a.empty()) || a == true))
                return select_all();
            return fail(here, key == "all" ? "expected an array of selectors, or {} for every facet" : "expected an array of selectors");
        }
        if (key == "not") {
            SelectorPtr c = parse(a, here);
            return c ? select_not(std::move(c)) : nullptr;
        }
        if (key == "zRange") {
            double    lo = 0., hi = 0.;
            PointTest t = PointTest::Centroid;
            if (a.is_array()) {
                if (a.size() != 2)
                    return fail(here, "expected [min, max]");
                if (!number(a[0], here + "[0]", lo) || !number(a[1], here + "[1]", hi))
                    return nullptr;
            } else if (a.is_object()) {
                if (!only_keys(a, here, {"min", "max", "test"}) || !point_test(a, here, t))
                    return nullptr;
                lo = -std::numeric_limits<double>::max();
                hi = std::numeric_limits<double>::max();
                if (a.contains("min") && !number(a["min"], here + ".min", lo))
                    return nullptr;
                if (a.contains("max") && !number(a["max"], here + ".max", hi))
                    return nullptr;
            } else
                return fail(here, "expected [min, max] or {min, max, test}");
            if (lo > hi)
                return fail(here, "min must not exceed max");
            return select_z_range(lo, hi, t);
        }
        if (!a.is_object())
            return fail(here, "expected an object");

        if (key == "overhang") {
            double ang = 0.;
            if (!only_keys(a, here, {"maxAngleDeg"}))
                return nullptr;
            if (!a.contains("maxAngleDeg"))
                return fail(here + ".maxAngleDeg", "required");
            if (!angle(a, "maxAngleDeg", here, 0., 0., 90., ang))
                return nullptr;
            return select_overhang(ang);
        }
        if (key == "normal") {
            Vec3d  dir;
            double within = 45.;
            if (!only_keys(a, here, {"side", "direction", "withinDeg"}) || !direction(a, "side", "direction", here, dir) ||
                !angle(a, "withinDeg", here, 45., 0., 180., within))
                return nullptr;
            return select_normal(dir, within);
        }
        if (key == "box") {
            Vec3d     mn, mx;
            PointTest t;
            if (!only_keys(a, here, {"min", "max", "test"}) || !vec3(a.value("min", json()), here + ".min", mn) ||
                !vec3(a.value("max", json()), here + ".max", mx) || !point_test(a, here, t))
                return nullptr;
            if ((mn.array() > mx.array()).any())
                return fail(here, "min must not exceed max on any axis");
            return select_box(mn, mx, t);
        }
        if (key == "sphere") {
            Vec3d     c;
            double    r = 0.;
            PointTest t;
            if (!only_keys(a, here, {"center", "radius", "test"}) || !vec3(a.value("center", json()), here + ".center", c) ||
                !number(a.value("radius", json()), here + ".radius", r) || !point_test(a, here, t))
                return nullptr;
            if (r <= 0.)
                return fail(here + ".radius", "must be positive");
            return select_sphere(c, r, t);
        }
        if (key == "halfspace") {
            Vec3d     p, n;
            PointTest t;
            if (!only_keys(a, here, {"point", "normal", "test"}) || !vec3(a.value("point", json()), here + ".point", p) ||
                !vec3(a.value("normal", json()), here + ".normal", n) || !point_test(a, here, t))
                return nullptr;
            if (n.norm() < 1e-9)
                return fail(here + ".normal", "normal must not be zero");
            return select_halfspace(p, n, t);
        }
        if (key == "facets") {
            if (!only_keys(a, here, {"volume", "volumeIndex", "indices"}))
                return nullptr;
            size_t vid = 0;
            int    vidx = -1;
            if (a.contains("volume")) {
                if (!a["volume"].is_number_unsigned() && !(a["volume"].is_number_integer() && a["volume"].get<long long>() > 0))
                    return fail(here + ".volume", "expected a volume id");
                vid = a["volume"].get<size_t>();
            }
            if (a.contains("volumeIndex")) {
                if (!a["volumeIndex"].is_number_integer() || a["volumeIndex"].get<long long>() < 0)
                    return fail(here + ".volumeIndex", "expected a non-negative integer");
                vidx = a["volumeIndex"].get<int>();
            }
            const json &ind = a.value("indices", json());
            if (!ind.is_array())
                return fail(here + ".indices", "expected an array of facet indices");
            if (ind.size() > 4000000)
                return fail(here + ".indices", "too many indices");
            std::vector<int> idx;
            idx.reserve(ind.size());
            for (size_t i = 0; i < ind.size(); ++i) {
                if (!ind[i].is_number_integer() || ind[i].get<long long>() < 0 || ind[i].get<long long>() > std::numeric_limits<int>::max())
                    return fail(here + ".indices[" + std::to_string(i) + "]", "expected a non-negative integer");
                idx.push_back(ind[i].get<int>());
            }
            return select_facets(vid, vidx, std::move(idx));
        }
        if (key == "planarRegion") {
            double tol = 5.;
            if (!only_keys(a, here, {"seedSide", "seedPoint", "toleranceDeg"}) || !angle(a, "toleranceDeg", here, 5., 0., 90., tol))
                return nullptr;
            bool has_side = a.contains("seedSide"), has_point = a.contains("seedPoint");
            if (has_side == has_point)
                return fail(here, "exactly one of \"seedSide\" or \"seedPoint\" is required");
            Vec3d v;
            if (has_side) {
                if (!a["seedSide"].is_string() || !side_direction(a["seedSide"].get<std::string>(), v))
                    return fail(here + ".seedSide", "expected rear, front, left, right, top or bottom");
                return select_planar_region_by_direction(v, tol);
            }
            if (!vec3(a["seedPoint"], here + ".seedPoint", v))
                return nullptr;
            return select_planar_region_by_point(v, tol);
        }
        if (key == "smoothRegion") {
            double tol = 30.;
            if (!only_keys(a, here, {"seedSide", "seedPoint", "maxNeighbourAngleDeg"}) ||
                !angle(a, "maxNeighbourAngleDeg", here, 30., 0., 90., tol))
                return nullptr;
            bool has_side = a.contains("seedSide"), has_point = a.contains("seedPoint");
            if (has_side == has_point)
                return fail(here, "exactly one of \"seedSide\" or \"seedPoint\" is required");
            Vec3d v;
            if (has_side) {
                if (!a["seedSide"].is_string() || !side_direction(a["seedSide"].get<std::string>(), v))
                    return fail(here + ".seedSide", "expected rear, front, left, right, top or bottom");
                return select_smooth_region_by_direction(v, tol);
            }
            if (!vec3(a["seedPoint"], here + ".seedPoint", v))
                return nullptr;
            return select_smooth_region_by_point(v, tol);
        }
        return fail(here, "unknown selector \"" + key + "\"");
    }
};

const char *const SIDE_NAMES[] = {"rear", "front", "right", "left", "top", "bottom"};

} // namespace

// ============================================================================= VolumeGeometry

VolumeGeometry::VolumeGeometry(const indexed_triangle_set &its, const Transform3d &world, size_t volume_id, int volume_index)
    : m_its(&its), m_world(world), m_volume_id(volume_id), m_volume_index(volume_index)
{
    m_vertices.reserve(its.vertices.size());
    for (const stl_vertex &v : its.vertices)
        m_vertices.emplace_back(world * v.cast<double>());

    const Matrix3d normal_matrix = world.linear().inverse().transpose();
    m_normals.reserve(its.indices.size());
    m_centroids.reserve(its.indices.size());
    m_areas.reserve(its.indices.size());
    for (size_t f = 0; f < its.indices.size(); ++f) {
        const Vec3d &a = vertex(f, 0), &b = vertex(f, 1), &c = vertex(f, 2);
        m_centroids.emplace_back((a + b + c) / 3.);
        m_areas.emplace_back(0.5 * (b - a).cross(c - a).norm());
        Vec3d n = normal_matrix * its_face_normal(its, int(f)).cast<double>();
        double len = n.norm();
        m_normals.emplace_back(len > 1e-12 && std::isfinite(len) ? Vec3d(n / len) : Vec3d::Zero());
    }
}

const std::vector<Vec3i32> &VolumeGeometry::neighbors() const
{
    if (!m_has_neighbors) {
        m_neighbors     = its_face_neighbors(*m_its);
        m_has_neighbors = true;
    }
    return m_neighbors;
}

BoundingBoxf3 VolumeGeometry::bounding_box() const
{
    BoundingBoxf3 bb;
    for (const Vec3d &v : m_vertices)
        bb.merge(v);
    return bb;
}

EvalContext object_geometry(const ModelObject &object, size_t instance_idx, bool model_parts_only, const std::vector<size_t> &volume_ids)
{
    EvalContext ctx;
    if (instance_idx >= object.instances.size())
        return ctx;
    const Transform3d &inst = object.instances[instance_idx]->get_matrix();
    for (size_t i = 0; i < object.volumes.size(); ++i) {
        const ModelVolume *mv = object.volumes[i];
        if (model_parts_only && !mv->is_model_part())
            continue;
        if (!volume_ids.empty() && std::find(volume_ids.begin(), volume_ids.end(), mv->id().id) == volume_ids.end())
            continue;
        ctx.volumes.emplace_back(mv->mesh().its, inst * mv->get_matrix(), mv->id().id, int(i));
    }
    return ctx;
}

// ============================================================================= selector factories

bool side_direction(const std::string &side, Vec3d &dir)
{
    if (side == "rear" || side == "back") dir = Vec3d::UnitY();
    else if (side == "front") dir = -Vec3d::UnitY();
    else if (side == "right") dir = Vec3d::UnitX();
    else if (side == "left") dir = -Vec3d::UnitX();
    else if (side == "top") dir = Vec3d::UnitZ();
    else if (side == "bottom") dir = -Vec3d::UnitZ();
    else return false;
    return true;
}

SelectorPtr select_all() { return std::make_shared<AllSelector>(); }
SelectorPtr select_overhang(double max_angle_deg) { return std::make_shared<OverhangSelector>(max_angle_deg); }
SelectorPtr select_normal(const Vec3d &direction, double within_deg) { return std::make_shared<NormalSelector>(direction, within_deg); }

SelectorPtr select_z_range(double z_min, double z_max, PointTest test)
{
    return make_spatial([z_min, z_max](const Vec3d &p) { return p.z() >= z_min && p.z() <= z_max; }, test);
}

SelectorPtr select_box(const Vec3d &mn, const Vec3d &mx, PointTest test)
{
    return make_spatial([mn, mx](const Vec3d &p) { return (p.array() >= mn.array()).all() && (p.array() <= mx.array()).all(); }, test);
}

SelectorPtr select_sphere(const Vec3d &center, double radius, PointTest test)
{
    const double r2 = radius * radius;
    return make_spatial([center, r2](const Vec3d &p) { return (p - center).squaredNorm() <= r2; }, test);
}

SelectorPtr select_halfspace(const Vec3d &point, const Vec3d &normal, PointTest test)
{
    const Vec3d n = normal.normalized();
    return make_spatial([point, n](const Vec3d &p) { return (p - point).dot(n) >= 0.; }, test);
}

SelectorPtr select_facets(size_t volume_id, int volume_index, std::vector<int> indices)
{
    return std::make_shared<FacetsSelector>(volume_id, volume_index, std::move(indices));
}

SelectorPtr select_planar_region_by_direction(const Vec3d &direction, double tolerance_deg)
{
    return std::make_shared<PlanarRegionSelector>(false, direction, tolerance_deg);
}

SelectorPtr select_planar_region_by_point(const Vec3d &point, double tolerance_deg)
{
    return std::make_shared<PlanarRegionSelector>(true, point, tolerance_deg);
}

SelectorPtr select_smooth_region_by_direction(const Vec3d &direction, double max_neighbour_deg)
{
    // Seeded like a planar region, with the seed tolerance standing in for "faces this side".
    return std::make_shared<PlanarRegionSelector>(false, direction, max_neighbour_deg, true);
}

SelectorPtr select_smooth_region_by_point(const Vec3d &point, double max_neighbour_deg)
{
    return std::make_shared<PlanarRegionSelector>(true, point, max_neighbour_deg, true);
}

SelectorPtr select_all_of(std::vector<SelectorPtr> children) { return std::make_shared<AllOfSelector>(std::move(children)); }
SelectorPtr select_any_of(std::vector<SelectorPtr> children) { return std::make_shared<AnyOfSelector>(std::move(children)); }
SelectorPtr select_not(SelectorPtr child) { return std::make_shared<NotSelector>(std::move(child)); }

std::vector<std::vector<uint8_t>> evaluate(const Selector &selector, const EvalContext &ctx)
{
    std::vector<std::vector<uint8_t>> out(ctx.volumes.size());
    for (size_t v = 0; v < ctx.volumes.size(); ++v) {
        out[v].assign(ctx.volumes[v].size(), uint8_t(0));
        selector.evaluate(ctx, v, out[v]);
    }
    return out;
}

SelectorPtr parse_selector(const json &j, ParseError *error, const std::string &path)
{
    Parser p{error};
    try {
        return p.parse(j, path);
    } catch (const std::exception &ex) {
        return p.fail(path, ex.what());
    }
}

// ============================================================================= painting

bool parse_paint_layer(const std::string &s, PaintLayer &out)
{
    if (s == "support") out = PaintLayer::Support;
    else if (s == "seam") out = PaintLayer::Seam;
    else if (s == "fuzzy") out = PaintLayer::Fuzzy;
    else if (s == "color" || s == "colour") out = PaintLayer::Color;
    else return false;
    return true;
}

const char *paint_layer_name(PaintLayer layer)
{
    switch (layer) {
    case PaintLayer::Support: return "support";
    case PaintLayer::Seam: return "seam";
    case PaintLayer::Fuzzy: return "fuzzy";
    case PaintLayer::Color: return "color";
    }
    return "";
}

bool parse_paint_mode(const std::string &s, PaintMode &out)
{
    if (s == "add") out = PaintMode::Add;
    else if (s == "replace") out = PaintMode::Replace;
    else if (s == "clear") out = PaintMode::Clear;
    else return false;
    return true;
}

bool parse_paint_state(const json &j, PaintLayer layer, EnforcerBlockerType &out, std::string &error)
{
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s == "erase") {
            out = EnforcerBlockerType::NONE;
            return true;
        }
        if (s == "enforce" && layer != PaintLayer::Color) {
            out = EnforcerBlockerType::ENFORCER;
            return true;
        }
        if (s == "block" && (layer == PaintLayer::Support || layer == PaintLayer::Seam)) {
            out = EnforcerBlockerType::BLOCKER;
            return true;
        }
    } else if (j.is_object() && j.size() == 1 && j.contains("filament") && layer == PaintLayer::Color) {
        const json &f = j["filament"];
        if (f.is_number_integer() && f.get<long long>() >= 1 && f.get<long long>() <= (long long) EnforcerBlockerType::ExtruderMax) {
            out = EnforcerBlockerType(f.get<int>());
            return true;
        }
        error = "filament must be an integer from 1 to " + std::to_string(int(EnforcerBlockerType::ExtruderMax));
        return false;
    }
    switch (layer) {
    case PaintLayer::Support:
    case PaintLayer::Seam: error = "expected \"enforce\", \"block\" or \"erase\""; break;
    case PaintLayer::Fuzzy: error = "expected \"enforce\" or \"erase\""; break;
    case PaintLayer::Color: error = "expected {\"filament\": n} or \"erase\""; break;
    }
    return false;
}

std::string paint_state_name(PaintLayer layer, EnforcerBlockerType state)
{
    if (state == EnforcerBlockerType::NONE)
        return "none";
    if (layer == PaintLayer::Color)
        return "filament " + std::to_string(int(state));
    if (layer == PaintLayer::Fuzzy)
        return "fuzzy";
    return state == EnforcerBlockerType::ENFORCER ? "enforce" : state == EnforcerBlockerType::BLOCKER ? "block" : "state " + std::to_string(int(state));
}

FacetsAnnotation &annotation(ModelVolume &volume, PaintLayer layer)
{
    switch (layer) {
    case PaintLayer::Support: return volume.supported_facets;
    case PaintLayer::Seam: return volume.seam_facets;
    case PaintLayer::Fuzzy: return volume.fuzzy_skin_facets;
    case PaintLayer::Color: break;
    }
    return volume.mmu_segmentation_facets;
}

const FacetsAnnotation &annotation(const ModelVolume &volume, PaintLayer layer)
{
    return annotation(const_cast<ModelVolume &>(volume), layer);
}

namespace {

void apply_to_selector(PaintSelector &sel, EnforcerBlockerType state, PaintMode mode, const std::vector<uint8_t> &mask)
{
    if (mode == PaintMode::Replace && state != EnforcerBlockerType::NONE)
        sel.replace_state(state, EnforcerBlockerType::NONE);
    const EnforcerBlockerType target = mode == PaintMode::Clear ? EnforcerBlockerType::NONE : state;
    for (size_t f = 0; f < mask.size(); ++f)
        if (mask[f])
            sel.set_facet(int(f), target);
    sel.collapse_uniform();
}

void count_hits(const std::vector<uint8_t> &mask, const VolumeGeometry *g, const TriangleMesh &mesh, const Transform3d &world, PaintResult &res)
{
    for (size_t f = 0; f < mask.size(); ++f) {
        if (!mask[f])
            continue;
        ++res.hits;
        if (g)
            res.hit_area_mm2 += g->area(f);
        else {
            const auto &idx = mesh.its.indices[f];
            const Vec3d a = world * mesh.its.vertices[idx[0]].cast<double>();
            const Vec3d b = world * mesh.its.vertices[idx[1]].cast<double>();
            const Vec3d c = world * mesh.its.vertices[idx[2]].cast<double>();
            res.hit_area_mm2 += 0.5 * (b - a).cross(c - a).norm();
        }
    }
}

} // namespace

PaintResult paint_data(const TriangleMesh &mesh, const Transform3d &world, TriangleSelector::TriangleSplittingData &data,
                       EnforcerBlockerType state, PaintMode mode, const std::vector<uint8_t> &mask, bool dry_run)
{
    PaintResult res;
    if (mask.size() != mesh.its.indices.size())
        return res;
    count_hits(mask, nullptr, mesh, world, res);
    PaintSelector sel(mesh);
    sel.deserialize(data);
    apply_to_selector(sel, state, mode, mask);
    res.states = sel.stats(world);
    TriangleSelector::TriangleSplittingData out = sel.serialize();
    res.would_change = out != data;
    if (!dry_run && res.would_change) {
        data        = std::move(out);
        res.changed = true;
    }
    return res;
}

PaintResult paint_volume(ModelVolume &volume, const EvalContext &ctx, size_t ctx_volume, PaintLayer layer,
                         EnforcerBlockerType state, PaintMode mode, const Selector &selector, bool dry_run)
{
    PaintResult res;
    if (ctx_volume >= ctx.volumes.size())
        return res;
    const VolumeGeometry &g = ctx.volumes[ctx_volume];
    const TriangleMesh   &mesh = volume.mesh();
    if (&g.its() != &mesh.its || g.size() != mesh.its.indices.size())
        return res;
    std::vector<uint8_t> mask(g.size(), uint8_t(0));
    selector.evaluate(ctx, ctx_volume, mask);
    count_hits(mask, &g, mesh, g.world(), res);

    FacetsAnnotation &ann = annotation(volume, layer);
    PaintSelector     sel(mesh);
    sel.deserialize(ann.get_data());
    apply_to_selector(sel, state, mode, mask);
    res.states = sel.stats(g.world());
    if (dry_run)
        res.would_change = sel.serialize() != ann.get_data();
    else
        res.would_change = res.changed = ann.set(sel);
    return res;
}

StateStatsMap annotation_stats(const TriangleMesh &mesh, const Transform3d &world, const TriangleSelector::TriangleSplittingData &data)
{
    if (data.triangles_to_split.empty())
        return {};
    PaintSelector sel(mesh);
    sel.deserialize(data);
    return sel.stats(world);
}

json stats_to_json(PaintLayer layer, const StateStatsMap &stats)
{
    json out = json::object();
    for (const auto &[state, st] : stats)
        out[paint_state_name(layer, EnforcerBlockerType(state))] = {{"facets", st.facets}, {"areaMm2", round6(st.area_mm2)}};
    return out;
}

// ============================================================================= geometry queries

namespace {

json bad(const std::string &path, const std::string &msg) { return {{"ok", false}, {"path", path}, {"error", msg}}; }

bool opt_number(const json &p, const char *key, double def, double lo, double hi, double &out, json &err)
{
    out = def;
    if (!p.contains(key))
        return true;
    const json &v = p[key];
    if (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < lo || v.get<double>() > hi) {
        err = bad(std::string("params.") + key, "expected a number between " + std::to_string(lo) + " and " + std::to_string(hi));
        return false;
    }
    out = v.get<double>();
    return true;
}

struct Region
{
    size_t        volume = 0;
    std::vector<size_t> facets;
    double        area = 0.;
    Vec3d         weighted_centroid = Vec3d::Zero();
    Vec3d         weighted_normal   = Vec3d::Zero();
    BoundingBoxf3 bb;
};

void grow_region(const VolumeGeometry &g, size_t v, size_t f, Region &r)
{
    r.volume = v;
    r.facets.push_back(f);
    r.area += g.area(f);
    r.weighted_centroid += g.centroid(f) * g.area(f);
    r.weighted_normal += g.normal(f) * g.area(f);
    for (int k = 0; k < 3; ++k)
        r.bb.merge(g.vertex(f, k));
}

// Connected components of facets passing `keep`, joined across shared edges while `join` holds.
template<class Keep, class Join> std::vector<Region> regions(const EvalContext &ctx, Keep keep, Join join)
{
    std::vector<Region> out;
    for (size_t v = 0; v < ctx.volumes.size(); ++v) {
        const VolumeGeometry       &g  = ctx.volumes[v];
        const std::vector<Vec3i32> &nb = g.neighbors();
        std::vector<uint8_t>        seen(g.size(), uint8_t(0));
        for (size_t s = 0; s < g.size(); ++s) {
            if (seen[s] || !keep(g, s))
                continue;
            Region r;
            std::vector<size_t> stack{s};
            seen[s] = 1;
            while (!stack.empty()) {
                size_t f = stack.back();
                stack.pop_back();
                grow_region(g, v, f, r);
                for (int k = 0; k < 3; ++k) {
                    int n = nb[f](k);
                    if (n < 0 || seen[size_t(n)] || !keep(g, size_t(n)) || !join(g, s, f, size_t(n)))
                        continue;
                    seen[size_t(n)] = 1;
                    stack.push_back(size_t(n));
                }
            }
            out.push_back(std::move(r));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Region &a, const Region &b) { return a.area > b.area; });
    return out;
}

const char *nearest_side(const Vec3d &n, double within_deg)
{
    const char *best = nullptr;
    double      best_ang = within_deg;
    for (const char *s : SIDE_NAMES) {
        Vec3d d;
        side_direction(s, d);
        double a = angle_deg(n, d);
        if (a <= best_ang) {
            best_ang = a;
            best     = s;
        }
    }
    return best;
}

json region_json(const EvalContext &ctx, const Region &r, bool with_normal)
{
    const VolumeGeometry &g = ctx.volumes[r.volume];
    json j = {{"volume", g.volume_id()}, {"volumeIndex", g.volume_index()}, {"facets", r.facets.size()},
              {"areaMm2", round6(r.area)}, {"centroid", vec_json_r(r.area > 0. ? Vec3d(r.weighted_centroid / r.area) : r.bb.center())},
              {"min", vec_json_r(r.bb.min)}, {"max", vec_json_r(r.bb.max)}};
    if (with_normal) {
        Vec3d n = r.weighted_normal.norm() > 1e-12 ? Vec3d(r.weighted_normal.normalized()) : Vec3d::Zero();
        j["normal"] = vec_json_r(n);
        const char *side = nearest_side(n, 10.);
        j["side"] = side ? json(side) : json(nullptr);
    }
    // Small regions carry their facet indices so they can be fed straight back as a "facets" selector.
    if (r.facets.size() <= 64) {
        json idx = json::array();
        for (size_t f : r.facets)
            idx.push_back(f);
        j["indices"] = std::move(idx);
    }
    return j;
}

json query_bbox(const EvalContext &ctx)
{
    BoundingBoxf3 all;
    size_t        facets = 0;
    json          vols   = json::array();
    for (const VolumeGeometry &g : ctx.volumes) {
        BoundingBoxf3 bb = g.bounding_box();
        if (bb.defined)
            all.merge(bb);
        facets += g.size();
        vols.push_back({{"volume", g.volume_id()}, {"volumeIndex", g.volume_index()}, {"facets", g.size()},
                        {"min", bb.defined ? vec_json_r(bb.min) : json(nullptr)}, {"max", bb.defined ? vec_json_r(bb.max) : json(nullptr)}});
    }
    json out = {{"ok", true}, {"query", "bbox"}, {"facets", facets}, {"volumes", std::move(vols)}};
    if (all.defined) {
        out["min"]  = vec_json_r(all.min);
        out["max"]  = vec_json_r(all.max);
        out["size"] = vec_json_r(all.size());
    }
    return out;
}

json query_overhangs(const EvalContext &ctx, const json &p)
{
    json   err;
    double max_angle = 45., limit = 10.;
    if (!opt_number(p, "maxAngleDeg", 45., 0., 90., max_angle, err) || !opt_number(p, "limit", 10., 0., 100., limit, err))
        return err;
    const double cos_t = std::cos(max_angle * DEG);
    auto keep = [cos_t](const VolumeGeometry &g, size_t f) { return -g.normal(f).z() > cos_t; };
    auto join = [](const VolumeGeometry &, size_t, size_t, size_t) { return true; };
    std::vector<Region> rs = regions(ctx, keep, join);
    double area = 0.;
    size_t facets = 0;
    double zmin = std::numeric_limits<double>::max(), zmax = -std::numeric_limits<double>::max();
    for (const Region &r : rs) {
        area += r.area;
        facets += r.facets.size();
        zmin = std::min(zmin, r.bb.min.z());
        zmax = std::max(zmax, r.bb.max.z());
    }
    json list = json::array();
    for (size_t i = 0; i < rs.size() && i < size_t(limit); ++i)
        list.push_back(region_json(ctx, rs[i], false));
    json out = {{"ok", true}, {"query", "overhangs"}, {"maxAngleDeg", max_angle}, {"facets", facets}, {"areaMm2", round6(area)},
                {"regionCount", rs.size()}, {"regions", std::move(list)}};
    out["zMin"] = facets ? json(round6(zmin)) : json(nullptr);
    out["zMax"] = facets ? json(round6(zmax)) : json(nullptr);
    return out;
}

json query_sides(const EvalContext &ctx, const json &p)
{
    json   err;
    double within = 45.;
    if (!opt_number(p, "withinDeg", 45., 0., 90., within, err))
        return err;
    const double cos_w = std::cos(within * DEG);
    json sides = json::object();
    for (const char *s : SIDE_NAMES) {
        Vec3d d;
        side_direction(s, d);
        double area = 0., extreme = -std::numeric_limits<double>::max();
        size_t facets = 0;
        for (const VolumeGeometry &g : ctx.volumes)
            for (size_t f = 0; f < g.size(); ++f)
                if (g.normal(f).dot(d) >= cos_w - 1e-12) {
                    area += g.area(f);
                    ++facets;
                    for (int k = 0; k < 3; ++k)
                        extreme = std::max(extreme, g.vertex(f, k).dot(d));
                }
        // "extreme": the outermost coordinate along the side's axis (e.g. max y for rear, min y for front).
        sides[s] = {{"facets", facets}, {"areaMm2", round6(area)}, {"extreme", facets ? json(round6(d.sum() * extreme)) : json(nullptr)}};
    }
    return {{"ok", true}, {"query", "sides"}, {"withinDeg", within}, {"sides", std::move(sides)}};
}

json query_planar_regions(const EvalContext &ctx, const json &p)
{
    json   err;
    double tol = 5., min_area = 1., limit = 20.;
    if (!opt_number(p, "toleranceDeg", 5., 0., 90., tol, err) || !opt_number(p, "minAreaMm2", 1., 0., 1e12, min_area, err) ||
        !opt_number(p, "limit", 20., 0., 200., limit, err))
        return err;
    const double cos_t = std::cos(tol * DEG);
    auto keep = [](const VolumeGeometry &g, size_t f) { return g.area(f) > 0.; };
    auto join = [cos_t](const VolumeGeometry &g, size_t seed, size_t, size_t n) { return g.normal(n).dot(g.normal(seed)) >= cos_t - 1e-12; };
    std::vector<Region> rs = regions(ctx, keep, join);
    json   list  = json::array();
    size_t count = 0;
    for (const Region &r : rs) {
        if (r.area < min_area)
            continue;
        ++count;
        if (list.size() < size_t(limit))
            list.push_back(region_json(ctx, r, true));
    }
    return {{"ok", true}, {"query", "planarRegions"}, {"toleranceDeg", tol}, {"regionCount", count}, {"regions", std::move(list)}};
}

json query_height_at(const EvalContext &ctx, const json &p)
{
    if (!p.contains("x") || !p["x"].is_number())
        return bad("params.x", "required number");
    if (!p.contains("y") || !p["y"].is_number())
        return bad("params.y", "required number");
    const double x = p["x"].get<double>(), y = p["y"].get<double>();
    struct Hit { double z; bool up; size_t volume; };
    std::vector<Hit> hits;
    for (const VolumeGeometry &g : ctx.volumes) {
        for (size_t f = 0; f < g.size(); ++f) {
            const Vec3d &a = g.vertex(f, 0), &b = g.vertex(f, 1), &c = g.vertex(f, 2);
            const double d = (b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y());
            if (std::abs(d) < 1e-12)
                continue; // vertical or degenerate in plan view
            const double l1 = ((b.x() - x) * (c.y() - y) - (c.x() - x) * (b.y() - y)) / d;
            const double l2 = ((c.x() - x) * (a.y() - y) - (a.x() - x) * (c.y() - y)) / d;
            const double l3 = 1. - l1 - l2;
            const double eps = -1e-9;
            if (l1 < eps || l2 < eps || l3 < eps)
                continue;
            hits.push_back({l1 * a.z() + l2 * b.z() + l3 * c.z(), g.normal(f).z() > 0., g.volume_id()});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit &l, const Hit &r) { return l.z < r.z; });
    // A line through a shared edge or vertex hits several facets at the same z.
    std::vector<Hit> uniq;
    for (const Hit &h : hits)
        if (uniq.empty() || std::abs(uniq.back().z - h.z) > 1e-6 || uniq.back().up != h.up)
            uniq.push_back(h);
    json list = json::array();
    for (const Hit &h : uniq)
        list.push_back({{"z", round6(h.z)}, {"facing", h.up ? "up" : "down"}, {"volume", h.volume}});
    json out = {{"ok", true}, {"query", "heightAt"}, {"x", x}, {"y", y}, {"hits", std::move(list)}};
    out["top"] = uniq.empty() ? json(nullptr) : json(round6(uniq.back().z));
    return out;
}

} // namespace

json geometry_query(const EvalContext &ctx, const std::string &query, const json &params)
{
    const json p = params.is_object() ? params : json::object();
    try {
        if (query == "bbox") return query_bbox(ctx);
        if (query == "overhangs") return query_overhangs(ctx, p);
        if (query == "sides") return query_sides(ctx, p);
        if (query == "planarRegions") return query_planar_regions(ctx, p);
        if (query == "heightAt") return query_height_at(ctx, p);
    } catch (const std::exception &ex) {
        return bad("params", ex.what());
    }
    return bad("query", "expected bbox, overhangs, sides, planarRegions or heightAt");
}

namespace {

struct WorldMesh
{
    std::vector<Vec3d>   vertices;
    std::vector<Vec3i32> faces;
    BoundingBoxf3        bb;
    double               area = 0.;
    Vec3d                weighted_centroid = Vec3d::Zero();
};

WorldMesh world_mesh(const EvalContext &ctx)
{
    WorldMesh m;
    for (const VolumeGeometry &g : ctx.volumes)
        for (size_t f = 0; f < g.size(); ++f) {
            int base = int(m.vertices.size());
            for (int k = 0; k < 3; ++k) {
                m.vertices.push_back(g.vertex(f, k));
                m.bb.merge(g.vertex(f, k));
            }
            m.faces.emplace_back(base, base + 1, base + 2);
            m.area += g.area(f);
            m.weighted_centroid += g.centroid(f) * g.area(f);
        }
    return m;
}

// Smallest distance from any vertex of `from` to the surface of `to`.
double min_vertex_distance(const WorldMesh &from, const WorldMesh &to, Vec3d &pa, Vec3d &pb)
{
    auto   tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(to.vertices, to.faces);
    double best = std::numeric_limits<double>::max();
    for (const Vec3d &v : from.vertices) {
        size_t idx = 0;
        Vec3d  hp;
        double d2 = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(to.vertices, to.faces, tree, v, idx, hp);
        if (d2 >= 0. && d2 < best) {
            best = d2;
            pa   = v;
            pb   = hp;
        }
    }
    return best == std::numeric_limits<double>::max() ? -1. : std::sqrt(best);
}

} // namespace

json geometry_measure(const EvalContext &a, const EvalContext &b)
{
    WorldMesh ma = world_mesh(a), mb = world_mesh(b);
    if (ma.faces.empty() || mb.faces.empty())
        return bad("target", "both targets need at least one facet");
    Vec3d  a1, b1, b2, a2;
    double d1 = min_vertex_distance(ma, mb, a1, b1);
    double d2 = min_vertex_distance(mb, ma, b2, a2);
    json   closest;
    double dist;
    if (d2 >= 0. && (d1 < 0. || d2 < d1)) {
        dist    = d2;
        closest = {{"a", vec_json_r(a2)}, {"b", vec_json_r(b2)}};
    } else {
        dist    = d1;
        closest = {{"a", vec_json_r(a1)}, {"b", vec_json_r(b1)}};
    }
    // Per-axis gap between the boxes: positive = separated, negative = overlap depth.
    Vec3d gap;
    for (int i = 0; i < 3; ++i)
        gap[i] = std::max(ma.bb.min[i] - mb.bb.max[i], mb.bb.min[i] - ma.bb.max[i]);
    Vec3d ca = ma.area > 0. ? Vec3d(ma.weighted_centroid / ma.area) : ma.bb.center();
    Vec3d cb = mb.area > 0. ? Vec3d(mb.weighted_centroid / mb.area) : mb.bb.center();
    return {{"ok", true},
            {"distanceMm", round6(dist)},
            {"closest", std::move(closest)},
            {"bboxGapMm", vec_json_r(gap)},
            {"centroidA", vec_json_r(ca)},
            {"centroidB", vec_json_r(cb)},
            {"centroidDistanceMm", round6((ca - cb).norm())},
            {"note", "vertex-to-surface distance checked both ways; does not detect one body inside the other"}};
}

} // namespace FacetSelectors
} // namespace Slic3r
