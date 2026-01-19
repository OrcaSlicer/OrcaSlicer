#include "libslic3r/libslic3r.h"
#include "Measure.hpp"
#include "MeasureUtils.hpp"

#include "libslic3r/Geometry/Circle.hpp"
#include "libslic3r/SurfaceMesh.hpp"


#include <numeric>
#include <tbb/parallel_for.h>

#define DEBUG_EXTRACT_ALL_FEATURES_AT_ONCE 0

namespace Slic3r {
namespace Measure {
bool get_point_projection_to_plane(const Vec3d &pt, const Vec3d &plane_origin, const Vec3d &plane_normal, Vec3d &intersection_pt)
{
    Vec3d normal     = plane_normal.normalized();
    Vec3d BA         = plane_origin - pt;
    auto length     = BA.dot(normal);
    intersection_pt = pt + length * normal;
    return true;
}

Vec3d get_one_point_in_plane(const Vec3d &plane_origin, const Vec3d &plane_normal)
{
    Vec3d dir(1, 0, 0);
    float eps = 1e-3;
    if (abs(plane_normal.dot(dir)) > 1 - eps) {
        dir = Vec3d(0, 1, 0);
    }
    Vec3d new_pt = plane_origin + dir;
    Vec3d retult;
    get_point_projection_to_plane(new_pt, plane_origin, plane_normal, retult);
    return retult;
}

constexpr double feature_hover_limit = 0.5; // how close to a feature the mouse must be to highlight it

static std::tuple<Vec3d, double, double> get_center_and_radius(const std::vector<Vec3d>& points, const Transform3d& trafo, const Transform3d& trafo_inv)
{
    Vec2ds out;
    double z = 0.;
    for (const Vec3d& pt : points) {
        Vec3d pt_transformed = trafo * pt;
        z = pt_transformed.z();
        out.emplace_back(pt_transformed.x(), pt_transformed.y());
    }

    const int iter = points.size() < 10  ? 2 :
                     points.size() < 100 ? 4 :
                     6;

    double error = std::numeric_limits<double>::max();
    auto circle = Geometry::circle_ransac(out, iter, &error);

    return std::make_tuple(trafo.inverse() * Vec3d(circle.center.x(), circle.center.y(), z), circle.radius, error);
}



static std::array<Vec3d, 3> orthonormal_basis(const Vec3d& v)
{
    std::array<Vec3d, 3> ret;
    ret[2] = v.normalized();
    int index;
    ret[2].cwiseAbs().maxCoeff(&index);
    switch (index)
    {
    case 0: { ret[0] = Vec3d(ret[2].y(), -ret[2].x(), 0.0).normalized(); break; }
    case 1: { ret[0] = Vec3d(0.0, ret[2].z(), -ret[2].y()).normalized(); break; }
    case 2: { ret[0] = Vec3d(-ret[2].z(), 0.0, ret[2].x()).normalized(); break; }
    }
    ret[1] = ret[2].cross(ret[0]).normalized();
    return ret;
}



class MeasuringImpl {
public:
    explicit MeasuringImpl(const indexed_triangle_set& its);
    struct PlaneData {
        std::vector<int> facets;
        std::vector<std::vector<Vec3d>> borders; // FIXME: should be in fact local in update_planes()
        std::vector<SurfaceFeature> surface_features;
        Vec3d normal;
        float area;
        bool features_extracted = false;
    };

    std::optional<SurfaceFeature>      get_feature(size_t face_idx, const Vec3d &point, const Transform3d &world_tran,bool only_select_plane);
    int get_num_of_planes() const;
    const std::vector<int>& get_plane_triangle_indices(int idx) const;
    std::vector<int>* get_plane_tri_indices(int idx);
    const std::vector<SurfaceFeature>& get_plane_features(unsigned int plane_id);
    std::vector<SurfaceFeature>* get_plane_features_pointer(unsigned int plane_id);
    const indexed_triangle_set& get_its() const;

private:
    void update_planes();
    void extract_features(int plane_idx);

    std::vector<PlaneData> m_planes;
    std::vector<size_t>    m_face_to_plane;
    indexed_triangle_set   m_its;
};






MeasuringImpl::MeasuringImpl(const indexed_triangle_set& its)
: m_its(its)
{
    update_planes();

    // Extracting features will be done as needed.
    // To extract all planes at once, run the following:
#if DEBUG_EXTRACT_ALL_FEATURES_AT_ONCE
    for (int i=0; i<int(m_planes.size()); ++i)
        extract_features(i);
#endif
}


void MeasuringImpl::update_planes()
{
    // Now we'll go through all the facets and append Points of facets sharing the same normal.
    // This part is still performed in mesh coordinate system.
    const size_t             num_of_facets = m_its.indices.size();
    m_face_to_plane.resize(num_of_facets, size_t(-1));
    const std::vector<Vec3f> face_normals = its_face_normals(m_its);
    const std::vector<Vec3i32> face_neighbors = its_face_neighbors(m_its);
    std::vector<int>         facet_queue(num_of_facets, 0);
    int                      facet_queue_cnt = 0;
    const stl_normal*        normal_ptr      = nullptr;
    size_t seed_facet_idx = 0;

    auto is_same_normal = [](const stl_normal& a, const stl_normal& b) -> bool {
        return (std::abs(a(0) - b(0)) < 0.001 && std::abs(a(1) - b(1)) < 0.001 && std::abs(a(2) - b(2)) < 0.001);
    };

    m_planes.clear();
    m_planes.reserve(num_of_facets / 5); // empty plane data object is quite lightweight, let's save the initial reallocations


    // First go through all the triangles and fill in m_planes vector. For each "plane"
    // detected on the model, it will contain list of facets that are part of it.
    // We will also fill in m_face_to_plane, which contains index into m_planes
    // for each of the source facets.
    while (1) {
        // Find next unvisited triangle:
        for (; seed_facet_idx < num_of_facets; ++ seed_facet_idx)
            if (m_face_to_plane[seed_facet_idx] == size_t(-1)) {
                facet_queue[facet_queue_cnt ++] = seed_facet_idx;
                normal_ptr = &face_normals[seed_facet_idx];
                m_face_to_plane[seed_facet_idx] = m_planes.size();
                m_planes.emplace_back();
                break;
            }
        if (seed_facet_idx == num_of_facets)
            break; // Everything was visited already

        while (facet_queue_cnt > 0) {
            int facet_idx = facet_queue[-- facet_queue_cnt];
            const stl_normal& this_normal = face_normals[facet_idx];
            if (is_same_normal(this_normal, *normal_ptr)) {
//                const Vec3i32& face = m_its.indices[facet_idx];

                m_face_to_plane[facet_idx] = m_planes.size() - 1;
                m_planes.back().facets.emplace_back(facet_idx);
                for (int j = 0; j < 3; ++ j)
                    if (int neighbor_idx = face_neighbors[facet_idx][j]; neighbor_idx >= 0 && m_face_to_plane[neighbor_idx] == size_t(-1))
                        facet_queue[facet_queue_cnt ++] = neighbor_idx;
            }
        }

        m_planes.back().normal = normal_ptr->cast<double>();
        std::sort(m_planes.back().facets.begin(), m_planes.back().facets.end());
    }

    // Check that each facet is part of one of the planes.
    assert(std::none_of(m_face_to_plane.begin(), m_face_to_plane.end(), [](size_t val) { return val == size_t(-1); }));

    // Now we will walk around each of the planes and save vertices which form the border.
    const SurfaceMesh sm(m_its);

    const auto& face_to_plane = m_face_to_plane;
    auto& planes = m_planes;

    tbb::parallel_for(tbb::blocked_range<size_t>(0, m_planes.size()),
        [&planes, &face_to_plane, &face_neighbors, &sm](const tbb::blocked_range<size_t>& range) {
            for (size_t plane_id = range.begin(); plane_id != range.end(); ++plane_id) {

        const auto& facets = planes[plane_id].facets;
        planes[plane_id].borders.clear();
        std::vector<std::array<bool, 3>> visited(facets.size(), {false, false, false});

        for (int face_id=0; face_id<int(facets.size()); ++face_id) {
            assert(face_to_plane[facets[face_id]] == plane_id);

            for (int edge_id=0; edge_id<3; ++edge_id) {
                // Every facet's edge which has a neighbor from a different plane is
                // part of an edge that we want to walk around. Skip the others.
                int neighbor_idx = face_neighbors[facets[face_id]][edge_id];
                if (neighbor_idx == -1)
                    goto PLANE_FAILURE;
                if (visited[face_id][edge_id] || face_to_plane[neighbor_idx] == plane_id) {
                    visited[face_id][edge_id] = true;
                    continue;
                }

                Halfedge_index he = sm.halfedge(Face_index(facets[face_id]));
                while (he.side() != edge_id)
                    he = sm.next(he);

                // he is the first halfedge on the border. Now walk around and append the points.
                //const Halfedge_index he_orig = he;
                planes[plane_id].borders.emplace_back();
                std::vector<Vec3d>& last_border = planes[plane_id].borders.back();
                last_border.reserve(4);
                last_border.emplace_back(sm.point(sm.source(he)).cast<double>());
                //Vertex_index target = sm.target(he);
                const Halfedge_index he_start = he;

                Face_index fi = he.face();
                auto face_it = std::lower_bound(facets.begin(), facets.end(), int(fi));
                assert(face_it != facets.end());
                assert(*face_it == int(fi));
                visited[face_it - facets.begin()][he.side()] = true;

                do {
                    const Halfedge_index he_orig = he;
                    he = sm.next_around_target(he);
                    if (he.is_invalid())
                        goto PLANE_FAILURE;

                    // For broken meshes, the iteration might never get back to he_orig.
                    // Remember all halfedges we saw to break out of such infinite loops.
                    boost::container::small_vector<Halfedge_index, 10> he_seen;

                    while ( face_to_plane[sm.face(he)] == plane_id && he != he_orig) {
                        he_seen.emplace_back(he);
                        he = sm.next_around_target(he);
                        if (he.is_invalid() || std::find(he_seen.begin(), he_seen.end(), he) != he_seen.end())
                            goto PLANE_FAILURE;
                    }
                    he = sm.opposite(he);
                    if (he.is_invalid())
                        goto PLANE_FAILURE;

                    Face_index fi = he.face();
                    auto face_it = std::lower_bound(facets.begin(), facets.end(), int(fi));
                    if (face_it == facets.end() || *face_it != int(fi)) // This indicates a broken mesh.
                        goto PLANE_FAILURE;

                    if (visited[face_it - facets.begin()][he.side()] && he != he_start) {
                        last_border.resize(1);
                        break;
                    }
                    visited[face_it - facets.begin()][he.side()] = true;

                    last_border.emplace_back(sm.point(sm.source(he)).cast<double>());

                    // In case of broken meshes, this loop might be infinite. Break
                    // out in case it is clearly going bad.
                    if (last_border.size() > 3*facets.size()+1)
                        goto PLANE_FAILURE;

                } while (he != he_start);

                if (last_border.size() == 1)
                    planes[plane_id].borders.pop_back();
                else {
                    assert(last_border.front() == last_border.back());
                    last_border.pop_back();
                }
            }
        }
        continue; // There was no failure.

        PLANE_FAILURE:
            planes[plane_id].borders.clear();
    }});
    m_planes.shrink_to_fit();
}

void MeasuringImpl::extract_features(int plane_idx)
{
    assert(! m_planes[plane_idx].features_extracted);

    PlaneData& plane = m_planes[plane_idx];
    plane.surface_features.clear();
    const Vec3d& normal = plane.normal;

    Eigen::Quaterniond q;
    q.setFromTwoVectors(plane.normal, Vec3d::UnitZ());
    Transform3d trafo = Transform3d::Identity();
    trafo.rotate(q);
    const Transform3d trafo_inv = trafo.inverse();

    std::vector<double> angles; // placed in outer scope to prevent reallocations
    std::vector<double> lengths;

    for (const std::vector<Vec3d>& border : plane.borders) {
        if (border.size() <= 1)
            continue;

        bool done = false;

        if (border.size() > 4) {
            const auto& [center, radius, err] = get_center_and_radius(border, trafo, trafo_inv);

            if (err < 0.05) {
                // The whole border is one circle. Just add it into the list of features
                // and we are done.

                bool is_polygon = border.size()>4 && border.size()<=8;
                bool lengths_match = std::all_of(border.begin()+2, border.end(), [is_polygon](const Vec3d& pt) {
                        return Slic3r::is_approx((pt - *((&pt)-1)).squaredNorm(), (*((&pt)-1) - *((&pt)-2)).squaredNorm(), is_polygon ? 0.01 : 0.01);
                    });

                if (lengths_match && (is_polygon || border.size() > 8)) {
                    if (is_polygon) {
                        // This is a polygon, add the separate edges with the center.
                        for (int j=0; j<int(border.size()); ++j)
                            plane.surface_features.emplace_back(SurfaceFeature(SurfaceFeatureType::Edge,
                                border[j==0 ? border.size()-1 : j-1], border[j],
                                std::make_optional(center)));
                    } else {
                        // The fit went well and it has more than 8 points - let's consider this a circle.
                        plane.surface_features.emplace_back(SurfaceFeature(SurfaceFeatureType::Circle, center, plane.normal, std::nullopt, radius));
                    }
                    done = true;
                }
            }
        }

        if (! done) {
            // In this case, the border is not a circle and may contain circular
            // segments. Try to find them and then add all remaining edges as edges.

            auto are_angles_same  = [](double a, double b) { return Slic3r::is_approx(a,b,0.01); };
            auto are_lengths_same = [](double a, double b) { return Slic3r::is_approx(a,b,0.01); };


            // Given an idx into border, return the index that is idx+offset position,
            // while taking into account the need for wrap-around and the fact that
            // the first and last point are the same.
            auto offset_to_index = [border_size = int(border.size())](int idx, int offset) -> int {
                assert(std::abs(offset) < border_size);
                int out = idx+offset;
                if (out >= border_size)
                    out = out - border_size;
                else if (out < 0)
                    out = border_size + out;

                return out;
            };

            // First calculate angles at all the vertices.
            angles.clear();
            lengths.clear();
            int first_different_angle_idx = 0;
            for (int i=0; i<int(border.size()); ++i) {
                const Vec3d& v2 = border[i] - (i == 0 ? border[border.size()-1] : border[i-1]);
                const Vec3d& v1 = (i == int(border.size()-1) ? border[0] : border[i+1]) - border[i];
                double angle = atan2(-normal.dot(v1.cross(v2)), -v1.dot(v2)) + M_PI;
                if (angle > M_PI)
                    angle = 2*M_PI - angle;

                angles.push_back(angle);
                lengths.push_back(v2.norm());
                if (first_different_angle_idx == 0 && angles.size() > 1) {
                    if (! are_angles_same(angles.back(), angles[angles.size()-2]))
                        first_different_angle_idx = angles.size()-1;
                }
            }
            assert(border.size() == angles.size());
            assert(border.size() == lengths.size());

            // First go around the border and pick what might be circular segments.
            // Save pair of indices to where such potential segments start and end.
            // Also remember the length of these segments.
            int start_idx = -1;
            bool circle = false;
            bool first_iter = true;
            std::vector<SurfaceFeature> circles;
            std::vector<SurfaceFeature> edges;
            std::vector<std::pair<int, int>> circles_idxs;
            //std::vector<double> circles_lengths;
            std::vector<Vec3d> single_circle; // could be in loop-scope, but reallocations
            double single_circle_length = 0.;
            int first_pt_idx = offset_to_index(first_different_angle_idx, 1);
            int i = first_pt_idx;
            while (i != first_pt_idx || first_iter) {
                if (are_angles_same(angles[i], angles[offset_to_index(i,-1)])
                && i != offset_to_index(first_pt_idx, -1) // not the last point
                && i != start_idx  ) {
                    // circle
                    if (! circle) {
                        circle = true;
                        single_circle.clear();
                        single_circle_length = 0.;
                        start_idx = offset_to_index(i, -2);
                        single_circle = { border[start_idx], border[offset_to_index(start_idx,1)] };
                        single_circle_length += lengths[offset_to_index(i, -1)];
                    }
                    single_circle.emplace_back(border[i]);
                    single_circle_length += lengths[i];
                } else {
                    if (circle && single_circle.size() >= 5) { // Less than 5 vertices? Not a circle.
                        single_circle.emplace_back(border[i]);
                        single_circle_length += lengths[i];

                        bool accept_circle = true;
                        {
                            // Check that lengths of internal (!!!) edges match.
                            int j = offset_to_index(start_idx, 3);
                            while (j != i) {
                                if (! are_lengths_same(lengths[offset_to_index(j,-1)], lengths[j])) {
                                    accept_circle = false;
                                    break;
                                }
                                j = offset_to_index(j, 1);
                            }
                        }

                        if (accept_circle) {
                            const auto& [center, radius, err] = get_center_and_radius(single_circle, trafo, trafo_inv);

                            // Check that the fit went well. The tolerance is high, only to
                            // reject complete failures.
                            accept_circle &= err < 0.05;

                            // If the segment subtends less than 90 degrees, throw it away.
                            accept_circle &= single_circle_length / radius > 0.9*M_PI/2.;

                            if (accept_circle) {
                                // Add the circle and remember indices into borders.
                                circles_idxs.emplace_back(start_idx, i);
                                circles.emplace_back(SurfaceFeature(SurfaceFeatureType::Circle, center, plane.normal, std::nullopt, radius));
                            }
                        }
                    }
                    circle = false;
                }
                // Take care of the wrap around.
                first_iter = false;
                i = offset_to_index(i, 1);
            }

            // We have the circles. Now go around again and pick edges, while jumping over circles.
            if (circles_idxs.empty()) {
                // Just add all edges.
                for (int i=1; i<int(border.size()); ++i)
                    edges.emplace_back(SurfaceFeature(SurfaceFeatureType::Edge, border[i-1], border[i]));
                edges.emplace_back(SurfaceFeature(SurfaceFeatureType::Edge, border[0], border[border.size()-1]));
            } else if (circles_idxs.size() > 1 || circles_idxs.front().first != circles_idxs.front().second) {
                // There is at least one circular segment. Start at its end and add edges until the start of the next one.
                int i = circles_idxs.front().second;
                int circle_idx = 1;
                while (true) {
                    i = offset_to_index(i, 1);
                    edges.emplace_back(SurfaceFeature(SurfaceFeatureType::Edge, border[offset_to_index(i,-1)], border[i]));
                    if (circle_idx < int(circles_idxs.size()) && i == circles_idxs[circle_idx].first) {
                        i = circles_idxs[circle_idx].second;
                        ++circle_idx;
                    }
                    if (i == circles_idxs.front().first)
                        break;
                }
            }

            // Merge adjacent edges where needed.
            assert(std::all_of(edges.begin(), edges.end(),
                            [](const SurfaceFeature& f) { return f.get_type() == SurfaceFeatureType::Edge; }));
            for (int i=edges.size()-1; i>=0; --i) {
                const auto& [first_start, first_end] = edges[i==0 ? edges.size()-1 : i-1].get_edge();
                const auto& [second_start, second_end] =   edges[i].get_edge();

                if (Slic3r::is_approx(first_end, second_start)
                    && Slic3r::is_approx((first_end-first_start).normalized().dot((second_end-second_start).normalized()), 1.)) {
                    // The edges have the same direction and share a point. Merge them.
                    edges[i==0 ? edges.size()-1 : i-1] = SurfaceFeature(SurfaceFeatureType::Edge, first_start, second_end);
                    edges.erase(edges.begin() + i);
                }
            }

            // Now move the circles and edges into the feature list for the plane.
            assert(std::all_of(circles.begin(), circles.end(), [](const SurfaceFeature& f) {
                return f.get_type() == SurfaceFeatureType::Circle;
            }));
            assert(std::all_of(edges.begin(), edges.end(), [](const SurfaceFeature& f) {
                return f.get_type() == SurfaceFeatureType::Edge;
            }));
            plane.surface_features.insert(plane.surface_features.end(), std::make_move_iterator(circles.begin()),
                std::make_move_iterator(circles.end()));
            plane.surface_features.insert(plane.surface_features.end(), std::make_move_iterator(edges.begin()),
                std::make_move_iterator(edges.end()));
        }
    }

    // The last surface feature is the plane itself.
    Vec3d cog = Vec3d::Zero();
    size_t counter = 0;
    for (const std::vector<Vec3d>& b : plane.borders) {
        for (size_t i = 0; i < b.size(); ++i) {
            cog += b[i];
            ++counter;
        }
    }
    cog /= double(counter);
    plane.surface_features.emplace_back(SurfaceFeature(SurfaceFeatureType::Plane,
        plane.normal, cog, std::optional<Vec3d>(), plane_idx + 0.0001));

    plane.borders.clear();
    plane.borders.shrink_to_fit();

    plane.features_extracted = true;
}

std::optional<SurfaceFeature> MeasuringImpl::get_feature(size_t face_idx, const Vec3d &point, const Transform3d &world_tran,bool only_select_plane)
{
    if (face_idx >= m_face_to_plane.size())
        return std::optional<SurfaceFeature>();

    const PlaneData& plane = m_planes[m_face_to_plane[face_idx]];

    if (! plane.features_extracted)
        extract_features(m_face_to_plane[face_idx]);

    size_t closest_feature_idx = size_t(-1);
    double min_dist = std::numeric_limits<double>::max();

    MeasurementResult res;
    SurfaceFeature point_sf(point);

    assert(plane.surface_features.empty() || plane.surface_features.back().get_type() == SurfaceFeatureType::Plane);

    if (!only_select_plane) {
        for (size_t i = 0; i < plane.surface_features.size() - 1; ++i) {
            // The -1 is there to prevent measuring distance to the plane itself,
            // which is needless and relatively expensive.
            res = get_measurement(plane.surface_features[i], point_sf);
            if (res.distance_strict) { // TODO: this should become an assert after all combinations are implemented.
                double dist = res.distance_strict->dist;
                if (dist < feature_hover_limit && dist < min_dist) {
                    min_dist            = std::min(dist, min_dist);
                    closest_feature_idx = i;
                }
            }
        }

        if (closest_feature_idx != size_t(-1)) {
            const SurfaceFeature &f = plane.surface_features[closest_feature_idx];
            if (f.get_type() == SurfaceFeatureType::Edge) {
                // If this is an edge, check if we are not close to the endpoint. If so,
                // we will include the endpoint as well. Close = 10% of the lenghth of
                // the edge, clamped between 0.025 and 0.5 mm.
                const auto &[sp, ep] = f.get_edge();
                double len_sq        = (ep - sp).squaredNorm();
                double limit_sq      = std::max(0.025 * 0.025, std::min(0.5 * 0.5, 0.1 * 0.1 * len_sq));
                if ((point - sp).squaredNorm() < limit_sq) {
                    SurfaceFeature local_f(sp);
                    local_f.origin_surface_feature = std::make_shared<SurfaceFeature>(local_f);
                    local_f.translate(world_tran);
                    return std::make_optional(local_f);
                }

                if ((point - ep).squaredNorm() < limit_sq) {
                    SurfaceFeature local_f(ep);
                    local_f.origin_surface_feature = std::make_shared<SurfaceFeature>(local_f);
                    local_f.translate(world_tran);
                    return std::make_optional(local_f);
                }
            }
            SurfaceFeature f_tran(f);
            f_tran.origin_surface_feature = std::make_shared<SurfaceFeature>(f);
            f_tran.translate(world_tran);
            return std::make_optional(f_tran);
        }
    }

    // Nothing detected, return the plane as a whole.
    assert(plane.surface_features.back().get_type() == SurfaceFeatureType::Plane);
    auto cur_plane = const_cast<PlaneData*>(&plane);
    SurfaceFeature f_tran(cur_plane->surface_features.back());
    f_tran.origin_surface_feature = std::make_shared<SurfaceFeature>(cur_plane->surface_features.back());
    f_tran.translate(world_tran);
    return std::make_optional(f_tran);
}





int MeasuringImpl::get_num_of_planes() const
{
    return (m_planes.size());
}



const std::vector<int>& MeasuringImpl::get_plane_triangle_indices(int idx) const
{
    assert(idx >= 0 && idx < int(m_planes.size()));
    return m_planes[idx].facets;
}

std::vector<int>* MeasuringImpl::get_plane_tri_indices(int idx)
{
    assert(idx >= 0 && idx < int(m_planes.size()));
    return &m_planes[idx].facets;
}

const std::vector<SurfaceFeature>& MeasuringImpl::get_plane_features(unsigned int plane_id)
{
    assert(plane_id < m_planes.size());
    if (! m_planes[plane_id].features_extracted)
        extract_features(plane_id);
    return m_planes[plane_id].surface_features;
}

std::vector<SurfaceFeature>* MeasuringImpl::get_plane_features_pointer(unsigned int plane_id) {
    assert(plane_id < m_planes.size());
    if (!m_planes[plane_id].features_extracted)
        extract_features(plane_id);
    return &m_planes[plane_id].surface_features;
}

const indexed_triangle_set& MeasuringImpl::get_its() const
{
    return this->m_its;
}

Measuring::Measuring(const indexed_triangle_set& its)
: priv{std::make_unique<MeasuringImpl>(its)}
{}

Measuring::~Measuring() {}



std::optional<SurfaceFeature> Measuring::get_feature(size_t face_idx, const Vec3d &point, const Transform3d &world_tran, bool only_select_plane) const
{
    if (face_idx == 7516 || face_idx == 7517) {
        std::cout << "";
    }
    return priv->get_feature(face_idx, point, world_tran, only_select_plane);
}


int Measuring::get_num_of_planes() const
{
    return priv->get_num_of_planes();
}


const std::vector<int>& Measuring::get_plane_triangle_indices(int idx) const
{
    return priv->get_plane_triangle_indices(idx);
}

const std::vector<SurfaceFeature>& Measuring::get_plane_features(unsigned int plane_id) const
{
    return priv->get_plane_features(plane_id);
}

const indexed_triangle_set& Measuring::get_its() const
{
    return priv->get_its();
}

const AngleAndEdges AngleAndEdges::Dummy = { 0.0, Vec3d::Zero(), { Vec3d::Zero(), Vec3d::Zero() }, { Vec3d::Zero(), Vec3d::Zero() }, 0.0, true };

static AngleAndEdges angle_edge_edge(const std::pair<Vec3d, Vec3d>& e1, const std::pair<Vec3d, Vec3d>& e2)
{
    if (are_parallel(e1, e2))
        return AngleAndEdges::Dummy;

    Vec3d e1_unit = edge_direction(e1.first, e1.second);
    Vec3d e2_unit = edge_direction(e2.first, e2.second);

    // project edges on the plane defined by them
    Vec3d normal = e1_unit.cross(e2_unit).normalized();
    const Eigen::Hyperplane<double, 3> plane(normal, e1.first);
    Vec3d e11_proj = plane.projection(e1.first);
    Vec3d e12_proj = plane.projection(e1.second);
    Vec3d e21_proj = plane.projection(e2.first);
    Vec3d e22_proj = plane.projection(e2.second);

    const bool coplanar = (e2.first - e21_proj).norm() < EPSILON && (e2.second - e22_proj).norm() < EPSILON;

    // rotate the plane to become the XY plane
    auto qp = Eigen::Quaternion<double>::FromTwoVectors(normal, Vec3d::UnitZ());
    auto qp_inverse = qp.inverse();
    const Vec3d e11_rot = qp * e11_proj;
    const Vec3d e12_rot = qp * e12_proj;
    const Vec3d e21_rot = qp * e21_proj;
    const Vec3d e22_rot = qp * e22_proj;

    // discard Z
    const Vec2d e11_rot_2d = Vec2d(e11_rot.x(), e11_rot.y());
    const Vec2d e12_rot_2d = Vec2d(e12_rot.x(), e12_rot.y());
    const Vec2d e21_rot_2d = Vec2d(e21_rot.x(), e21_rot.y());
    const Vec2d e22_rot_2d = Vec2d(e22_rot.x(), e22_rot.y());

    // find intersection (arc center) of edges in XY plane
    const Eigen::Hyperplane<double, 2> e1_rot_2d_line = Eigen::Hyperplane<double, 2>::Through(e11_rot_2d, e12_rot_2d);
    const Eigen::Hyperplane<double, 2> e2_rot_2d_line = Eigen::Hyperplane<double, 2>::Through(e21_rot_2d, e22_rot_2d);
    const Vec2d center_rot_2d = e1_rot_2d_line.intersection(e2_rot_2d_line);

    // arc center in original coordinate
    const Vec3d center = qp_inverse * Vec3d(center_rot_2d.x(), center_rot_2d.y(), e11_rot.z());

    // ensure the edges are pointing away from the center
    std::pair<Vec3d, Vec3d> out_e1 = e1;
    std::pair<Vec3d, Vec3d> out_e2 = e2;
    if ((center_rot_2d - e11_rot_2d).squaredNorm() > (center_rot_2d - e12_rot_2d).squaredNorm()) {
        std::swap(e11_proj, e12_proj);
        std::swap(out_e1.first, out_e1.second);
        e1_unit = -e1_unit;
    }
    if ((center_rot_2d - e21_rot_2d).squaredNorm() > (center_rot_2d - e22_rot_2d).squaredNorm()) {
        std::swap(e21_proj, e22_proj);
        std::swap(out_e2.first, out_e2.second);
        e2_unit = -e2_unit;
    }

    // arc angle
    const double angle = std::acos(std::clamp(e1_unit.dot(e2_unit), -1.0, 1.0));
    // arc radius
    const Vec3d e1_proj_mid = 0.5 * (e11_proj + e12_proj);
    const Vec3d e2_proj_mid = 0.5 * (e21_proj + e22_proj);
    const double radius = std::min((center - e1_proj_mid).norm(), (center - e2_proj_mid).norm());

    return { angle, center, out_e1, out_e2, radius, coplanar };
}

static AngleAndEdges angle_edge_plane(const std::pair<Vec3d, Vec3d>& e, const std::tuple<int, Vec3d, Vec3d>& p)
{
    const auto& [idx, normal, origin] = p;
    Vec3d e1e2_unit = edge_direction(e);
    if (are_perpendicular(e1e2_unit, normal))
        return AngleAndEdges::Dummy;

    // ensure the edge is pointing away from the intersection
    // 1st calculate instersection between edge and plane
    const Eigen::Hyperplane<double, 3> plane(normal, origin);
    const Eigen::ParametrizedLine<double, 3> line = Eigen::ParametrizedLine<double, 3>::Through(e.first, e.second);
    const Vec3d inters = line.intersectionPoint(plane);

    // then verify edge direction and revert it, if needed
    Vec3d e1 = e.first;
    Vec3d e2 = e.second;
    if ((e1 - inters).squaredNorm() > (e2 - inters).squaredNorm()) {
        std::swap(e1, e2);
        e1e2_unit = -e1e2_unit;
    }

    if (are_parallel(e1e2_unit, normal)) {
        const std::array<Vec3d, 3> basis = orthonormal_basis(e1e2_unit);
        const double radius = (0.5 * (e1 + e2) - inters).norm();
        const Vec3d edge_on_plane_dir = (basis[1].dot(origin - inters) >= 0.0) ? basis[1] : -basis[1];
        std::pair<Vec3d, Vec3d> edge_on_plane = std::make_pair(inters, inters + radius * edge_on_plane_dir);
        if (!inters.isApprox(e1)) {
            edge_on_plane.first  += radius * edge_on_plane_dir;
            edge_on_plane.second += radius * edge_on_plane_dir;
        }
        return AngleAndEdges(0.5 * double(PI), inters, std::make_pair(e1, e2), edge_on_plane, radius, inters.isApprox(e1));
    }

    const Vec3d e1e2 = e2 - e1;
    const double e1e2_len = e1e2.norm();

    // calculate 2nd edge (on the plane)
    const Vec3d temp = normal.cross(e1e2);
    const Vec3d edge_on_plane_unit = normal.cross(temp).normalized();
    std::pair<Vec3d, Vec3d> edge_on_plane = { origin, origin + e1e2_len * edge_on_plane_unit };

    // ensure the 2nd edge is pointing in the correct direction
    const Vec3d test_edge = (edge_on_plane.second - edge_on_plane.first).cross(e1e2);
    if (test_edge.dot(temp) < 0.0)
        edge_on_plane = { origin, origin - e1e2_len * edge_on_plane_unit };

    AngleAndEdges ret = angle_edge_edge({ e1, e2 }, edge_on_plane);
    ret.radius = (inters - 0.5 * (e1 + e2)).norm();
    return ret;
}

static AngleAndEdges angle_plane_plane(const std::tuple<int, Vec3d, Vec3d>& p1, const std::tuple<int, Vec3d, Vec3d>& p2)
{
    const auto& [idx1, normal1, origin1] = p1;
    const auto& [idx2, normal2, origin2] = p2;

    // are planes parallel ?
    if (are_parallel(normal1, normal2))
        return AngleAndEdges::Dummy;

    auto intersection_plane_plane = [](const Vec3d& n1, const Vec3d& o1, const Vec3d& n2, const Vec3d& o2) {
        Eigen::MatrixXd m(2, 3);
        m << n1.x(), n1.y(), n1.z(), n2.x(), n2.y(), n2.z();
        Eigen::VectorXd b(2);
        b << o1.dot(n1), o2.dot(n2);
        Eigen::VectorXd x = m.colPivHouseholderQr().solve(b);
        return std::make_pair(n1.cross(n2).normalized(), Vec3d(x(0), x(1), x(2)));
    };

    // Calculate intersection line between planes
    const auto [intersection_line_direction, intersection_line_origin] = intersection_plane_plane(normal1, origin1, normal2, origin2);

    // Project planes' origin on intersection line
    const Eigen::ParametrizedLine<double, 3> intersection_line = Eigen::ParametrizedLine<double, 3>(intersection_line_origin, intersection_line_direction);
    const Vec3d origin1_proj = intersection_line.projection(origin1);
    const Vec3d origin2_proj = intersection_line.projection(origin2);

    // Calculate edges on planes
    const Vec3d edge_on_plane1_unit = (origin1 - origin1_proj).normalized();
    const Vec3d edge_on_plane2_unit = (origin2 - origin2_proj).normalized();
    const double radius = std::max(10.0, std::max((origin1 - origin1_proj).norm(), (origin2 - origin2_proj).norm()));
    const std::pair<Vec3d, Vec3d> edge_on_plane1 = { origin1_proj + radius * edge_on_plane1_unit, origin1_proj + 2.0 * radius * edge_on_plane1_unit };
    const std::pair<Vec3d, Vec3d> edge_on_plane2 = { origin2_proj + radius * edge_on_plane2_unit, origin2_proj + 2.0 * radius * edge_on_plane2_unit };

    AngleAndEdges ret = angle_edge_edge(edge_on_plane1, edge_on_plane2);
    ret.radius = radius;
    return ret;
}

MeasurementResult get_measurement(const SurfaceFeature &a, const SurfaceFeature &b, bool deal_circle_result)
{
    assert(a.get_type() != SurfaceFeatureType::Undef && b.get_type() != SurfaceFeatureType::Undef);

    const bool swap = int(a.get_type()) > int(b.get_type());
    const SurfaceFeature& f1 = swap ? b : a;
    const SurfaceFeature& f2 = swap ? a : b;

    MeasurementResult result;

    ///////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////
    if (f1.get_type() == SurfaceFeatureType::Point) {
        if (f2.get_type() == SurfaceFeatureType::Point) {
            Vec3d diff = (f2.get_point() - f1.get_point());
            result.distance_strict = std::make_optional(DistAndPoints{diff.norm(), f1.get_point(), f2.get_point()});
            result.distance_xyz = diff;

    ///////////////////////////////////////////////////////////////////////////
        } else if (f2.get_type() == SurfaceFeatureType::Edge) {
            const auto [s,e] = f2.get_edge();
            const Eigen::ParametrizedLine<double, 3> line(s, (e-s).normalized());
            const double dist_inf = line.distance(f1.get_point());
            const Vec3d proj = line.projection(f1.get_point());
            const double len_sq = (e-s).squaredNorm();
            const double dist_start_sq = (proj-s).squaredNorm();
            const double dist_end_sq = (proj-e).squaredNorm();
            if (dist_start_sq < len_sq && dist_end_sq < len_sq) {
                // projection falls on the line - the strict distance is the same as infinite
                result.distance_strict = std::make_optional(DistAndPoints{dist_inf, f1.get_point(), proj});
            } else { // the result is the closer of the endpoints
                const bool s_is_closer = dist_start_sq < dist_end_sq;
                result.distance_strict = std::make_optional(DistAndPoints{std::sqrt(std::min(dist_start_sq, dist_end_sq) + sqr(dist_inf)), f1.get_point(), s_is_closer ? s : e});
            }
            result.distance_infinite = std::make_optional(DistAndPoints{dist_inf, f1.get_point(), proj});
    ///////////////////////////////////////////////////////////////////////////
        } else if (f2.get_type() == SurfaceFeatureType::Circle) {
            // Find a plane containing normal, center and the point.
            const auto [c, radius, n] = f2.get_circle();
            const Eigen::Hyperplane<double, 3> circle_plane(n, c);
            const Vec3d proj = circle_plane.projection(f1.get_point());
            if (proj.isApprox(c)) {
                const Vec3d p_on_circle = c + radius * get_orthogonal(n, true);
                result.distance_strict = std::make_optional(DistAndPoints{ radius, c, p_on_circle });
            }
            else {
                if (deal_circle_result == false) {
                    const Eigen::Hyperplane<double, 3> circle_plane(n, c);
                    const Vec3d                        proj = circle_plane.projection(f1.get_point());
                    const double                       dist = std::sqrt(std::pow((proj - c).norm() - radius, 2.) + (f1.get_point() - proj).squaredNorm());

                    const Vec3d p_on_circle = c + radius * (proj - c).normalized();
                    result.distance_strict  = std::make_optional(DistAndPoints{dist, f1.get_point(), p_on_circle});
                }
                else {
                    const double dist      = (f1.get_point() - c).norm();
                    result.distance_strict = std::make_optional(DistAndPoints{dist, f1.get_point(), c});
                }
            }
    ///////////////////////////////////////////////////////////////////////////
        } else if (f2.get_type() == SurfaceFeatureType::Plane) {
            const auto [idx, normal, pt] = f2.get_plane();
            Eigen::Hyperplane<double, 3> plane(normal, pt);
            result.distance_infinite = std::make_optional(DistAndPoints{plane.absDistance(f1.get_point()), f1.get_point(), plane.projection(f1.get_point())}); // TODO
            // TODO: result.distance_strict =
        }
    ///////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////
    }
    else if (f1.get_type() == SurfaceFeatureType::Edge) {
        if (f2.get_type() == SurfaceFeatureType::Edge) {
            std::vector<DistAndPoints> distances;

            auto add_point_edge_distance = [&distances](const Vec3d& v, const std::pair<Vec3d, Vec3d>& e) {
                const MeasurementResult res = get_measurement(SurfaceFeature(v), SurfaceFeature(SurfaceFeatureType::Edge, e.first, e.second));
                double distance = res.distance_strict->dist;
                Vec3d v2 = res.distance_strict->to;

                const Vec3d e1e2 = e.second - e.first;
                const Vec3d e1v2 = v2 - e.first;
                if (e1v2.dot(e1e2) >= 0.0 && e1v2.norm() < e1e2.norm())
                    distances.emplace_back(distance, v, v2);
            };

            std::pair<Vec3d, Vec3d> e1 = f1.get_edge();
            std::pair<Vec3d, Vec3d> e2 = f2.get_edge();

            distances.emplace_back((e2.first - e1.first).norm(), e1.first, e2.first);
            distances.emplace_back((e2.second - e1.first).norm(), e1.first, e2.second);
            distances.emplace_back((e2.first - e1.second).norm(), e1.second, e2.first);
            distances.emplace_back((e2.second - e1.second).norm(), e1.second, e2.second);
            add_point_edge_distance(e1.first, e2);
            add_point_edge_distance(e1.second, e2);
            add_point_edge_distance(e2.first, e1);
            add_point_edge_distance(e2.second, e1);
            auto it = std::min_element(distances.begin(), distances.end(),
                [](const DistAndPoints& item1, const DistAndPoints& item2) {
                    return item1.dist < item2.dist;
                });
            result.distance_infinite = std::make_optional(*it);

            result.angle = angle_edge_edge(f1.get_edge(), f2.get_edge());
    ///////////////////////////////////////////////////////////////////////////
        } else if (f2.get_type() == SurfaceFeatureType::Circle) {
            const std::pair<Vec3d, Vec3d> e      = f1.get_edge();
            const auto &[center, radius, normal] = f2.get_circle();
            const Vec3d e1e2                     = (e.second - e.first);
            const Vec3d e1e2_unit                = e1e2.normalized();

            std::vector<DistAndPoints> distances;
            distances.emplace_back(*get_measurement(SurfaceFeature(e.first), f2).distance_strict);
            distances.emplace_back(*get_measurement(SurfaceFeature(e.second), f2).distance_strict);

            const Eigen::Hyperplane<double, 3>       plane(e1e2_unit, center);
            const Eigen::ParametrizedLine<double, 3> line    = Eigen::ParametrizedLine<double, 3>::Through(e.first, e.second);
            const Vec3d                              inter   = line.intersectionPoint(plane);
            const Vec3d                              e1inter = inter - e.first;
            if (e1inter.dot(e1e2) >= 0.0 && e1inter.norm() < e1e2.norm()) distances.emplace_back(*get_measurement(SurfaceFeature(inter), f2).distance_strict);

            auto it = std::min_element(distances.begin(), distances.end(), [](const DistAndPoints &item1, const DistAndPoints &item2) { return item1.dist < item2.dist; });
            if (deal_circle_result == false) {
                result.distance_infinite = std::make_optional(DistAndPoints{it->dist, it->from, it->to});
            }
            else{
                const double dist      = (it->from - center).norm();
                result.distance_infinite = std::make_optional(DistAndPoints{dist, it->from, center});
            }
    ///////////////////////////////////////////////////////////////////////////
        } else if (f2.get_type() == SurfaceFeatureType::Plane) {
            const auto [from, to] = f1.get_edge();
            const auto [idx, normal, origin] = f2.get_plane();

            const Vec3d edge_unit = (to - from).normalized();
            if (are_perpendicular(edge_unit, normal)) {
                std::vector<DistAndPoints> distances;
                const Eigen::Hyperplane<double, 3> plane(normal, origin);
                distances.push_back(DistAndPoints{ plane.absDistance(from), from, plane.projection(from) });
                distances.push_back(DistAndPoints{ plane.absDistance(to), to, plane.projection(to) });
                auto it = std::min_element(distances.begin(), distances.end(),
                    [](const DistAndPoints& item1, const DistAndPoints& item2) {
                        return item1.dist < item2.dist;
                    });
                result.distance_infinite = std::make_optional(DistAndPoints{ it->dist, it->from, it->to });
            }
            else {
                auto plane_features = f2.world_plane_features;
                std::vector<DistAndPoints> distances;
                for (const SurfaceFeature& sf : *plane_features) {
                    if (sf.get_type() == SurfaceFeatureType::Edge) {
                        const auto m = get_measurement(sf, f1);
                        if (!m.distance_infinite.has_value()) {
                            distances.clear();
                            break;
                        }
                        else
                            distances.push_back(*m.distance_infinite);
                    }
                }
                if (!distances.empty()) {
                    auto it = std::min_element(distances.begin(), distances.end(),
                        [](const DistAndPoints& item1, const DistAndPoints& item2) {
                            return item1.dist < item2.dist;
                        });
                    result.distance_infinite = std::make_optional(DistAndPoints{ it->dist, it->from, it->to });
                }
            }
            result.angle = angle_edge_plane(f1.get_edge(), f2.get_plane());
        }
    ///////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////
    } else if (f1.get_type() == SurfaceFeatureType::Circle) {
        if (f2.get_type() == SurfaceFeatureType::Circle) {
            const auto [c0, r0, n0] = f1.get_circle();
            const auto [c1, r1, n1] = f2.get_circle();

            // The following code is an adaptation of the algorithm found in:
            // https://github.com/davideberly/GeometricTools/blob/master/GTE/Mathematics/DistCircle3Circle3.h
            // and described in:
            // https://www.geometrictools.com/Documentation/DistanceToCircle3.pdf

            struct ClosestInfo
            {
                double sqrDistance{ 0.0 };
                Vec3d circle0Closest{ Vec3d::Zero() };
                Vec3d circle1Closest{ Vec3d::Zero() };

                inline bool operator < (const ClosestInfo& other) const { return sqrDistance < other.sqrDistance; }
            };
            std::array<ClosestInfo, 16> candidates{};

            const double zero = 0.0;

            const Vec3d D = c1 - c0;

            if (!are_parallel(n0, n1)) {
                // Get parameters for constructing the degree-8 polynomial phi.
                const double one = 1.0;
                const double two = 2.0;
                const double  r0sqr = sqr(r0);
                const double  r1sqr = sqr(r1);

                // Compute U1 and V1 for the plane of circle1.
                const std::array<Vec3d, 3> basis = orthonormal_basis(n1);
                const Vec3d U1 = basis[0];
                const Vec3d V1 = basis[1];

                // Construct the polynomial phi(cos(theta)).
                const Vec3d N0xD = n0.cross(D);
                const Vec3d N0xU1 = n0.cross(U1);
                const Vec3d N0xV1 = n0.cross(V1);
                const double a0 = r1 * D.dot(U1);
                const double a1 = r1 * D.dot(V1);
                const double a2 = N0xD.dot(N0xD);
                const double a3 = r1 * N0xD.dot(N0xU1);
                const double a4 = r1 * N0xD.dot(N0xV1);
                const double a5 = r1sqr * N0xU1.dot(N0xU1);
                const double a6 = r1sqr * N0xU1.dot(N0xV1);
                const double a7 = r1sqr * N0xV1.dot(N0xV1);
                Polynomial1 p0{ a2 + a7, two * a3, a5 - a7 };
                Polynomial1 p1{ two * a4, two * a6 };
                Polynomial1 p2{ zero, a1 };
                Polynomial1 p3{ -a0 };
                Polynomial1 p4{ -a6, a4, two * a6 };
                Polynomial1 p5{ -a3, a7 - a5 };
                Polynomial1 tmp0{ one, zero, -one };
                Polynomial1 tmp1 = p2 * p2 + tmp0 * p3 * p3;
                Polynomial1 tmp2 = two * p2 * p3;
                Polynomial1 tmp3 = p4 * p4 + tmp0 * p5 * p5;
                Polynomial1 tmp4 = two * p4 * p5;
                Polynomial1 p6 = p0 * tmp1 + tmp0 * p1 * tmp2 - r0sqr * tmp3;
                Polynomial1 p7 = p0 * tmp2 + p1 * tmp1 - r0sqr * tmp4;

                // Parameters for polynomial root finding. The roots[] array
                // stores the roots. We need only the unique ones, which is
                // the responsibility of the set uniqueRoots. The pairs[]
                // array stores the (cosine,sine) information mentioned in the
                // PDF. TODO: Choose the maximum number of iterations for root
                // finding based on specific polynomial data?
                const uint32_t maxIterations = 128;
                int32_t degree = 0;
                size_t numRoots = 0;
                std::array<double, 8> roots{};
                std::set<double> uniqueRoots{};
                size_t numPairs = 0;
                std::array<std::pair<double, double>, 16> pairs{};
                double temp = zero;
                double sn = zero;

                if (p7.GetDegree() > 0 || p7[0] != zero) {
                    // H(cs,sn) = p6(cs) + sn * p7(cs)
                    Polynomial1 phi = p6 * p6 - tmp0 * p7 * p7;
                    degree = static_cast<int32_t>(phi.GetDegree());
                    assert(degree > 0);
                    numRoots = RootsPolynomial::Find(degree, &phi[0], maxIterations, roots.data());
                    for (size_t i = 0; i < numRoots; ++i) {
                        uniqueRoots.insert(roots[i]);
                    }

                    for (auto const& cs : uniqueRoots) {
                        if (std::fabs(cs) <= one) {
                            temp = p7(cs);
                            if (temp != zero) {
                                sn = -p6(cs) / temp;
                                pairs[numPairs++] = std::make_pair(cs, sn);
                            }
                            else {
                                temp = std::max(one - sqr(cs), zero);
                                sn = std::sqrt(temp);
                                pairs[numPairs++] = std::make_pair(cs, sn);
                                if (sn != zero)
                                    pairs[numPairs++] = std::make_pair(cs, -sn);
                            }
                        }
                    }
                }
                else {
                    // H(cs,sn) = p6(cs)
                    degree = static_cast<int32_t>(p6.GetDegree());
                    assert(degree > 0);
                    numRoots = RootsPolynomial::Find(degree, &p6[0], maxIterations, roots.data());
                    for (size_t i = 0; i < numRoots; ++i) {
                        uniqueRoots.insert(roots[i]);
                    }

                    for (auto const& cs : uniqueRoots) {
                        if (std::fabs(cs) <= one) {
                            temp = std::max(one - sqr(cs), zero);
                            sn = std::sqrt(temp);
                            pairs[numPairs++] = std::make_pair(cs, sn);
                            if (sn != zero)
                                pairs[numPairs++] = std::make_pair(cs, -sn);
                        }
                    }
                }

                for (size_t i = 0; i < numPairs; ++i) {
                    ClosestInfo& info = candidates[i];
                    Vec3d delta = D + r1 * (pairs[i].first * U1 + pairs[i].second * V1);
                    info.circle1Closest = c0 + delta;
                    const double N0dDelta = n0.dot(delta);
                    const double lenN0xDelta = n0.cross(delta).norm();
                    if (lenN0xDelta > 0.0) {
                        const double diff = lenN0xDelta - r0;
                        info.sqrDistance = sqr(N0dDelta) + sqr(diff);
                        delta -= N0dDelta * n0;
                        delta.normalize();
                        info.circle0Closest = c0 + r0 * delta;
                    }
                    else {
                        const Vec3d r0U0 = r0 * get_orthogonal(n0, true);
                        const Vec3d diff = delta - r0U0;
                        info.sqrDistance = diff.dot(diff);
                        info.circle0Closest = c0 + r0U0;
                    }
                }

                std::sort(candidates.begin(), candidates.begin() + numPairs);
            }
            else {
                ClosestInfo& info = candidates[0];

                const double N0dD = n0.dot(D);
                const Vec3d normProj = N0dD * n0;
                const Vec3d compProj = D - normProj;
                Vec3d U = compProj;
                const double d = U.norm();
                U.normalize();

                // The configuration is determined by the relative location of the
                // intervals of projection of the circles on to the D-line.
                // Circle0 projects to [-r0,r0] and circle1 projects to
                // [d-r1,d+r1].
                const double dmr1 = d - r1;
                double distance;
                if (dmr1 >= r0) {
                    // d >= r0 + r1
                    // The circles are separated (d > r0 + r1) or tangent with one
                    // outside the other (d = r0 + r1).
                    distance = dmr1 - r0;
                    info.circle0Closest = c0 + r0 * U;
                    info.circle1Closest = c1 - r1 * U;
                }
                else {
                    // d < r0 + r1
                    // The cases implicitly use the knowledge that d >= 0.
                    const double dpr1 = d + r1;
                    if (dpr1 <= r0) {
                        // Circle1 is inside circle0.
                        distance = r0 - dpr1;
                        if (d > 0.0) {
                            info.circle0Closest = c0 + r0 * U;
                            info.circle1Closest = c1 + r1 * U;
                        }
                        else {
                            // The circles are concentric, so U = (0,0,0).
                            // Construct a vector perpendicular to N0 to use for
                            // closest points.
                            U = get_orthogonal(n0, true);
                            info.circle0Closest = c0 + r0 * U;
                            info.circle1Closest = c1 + r1 * U;
                        }
                    }
                    else if (dmr1 <= -r0) {
                        // Circle0 is inside circle1.
                        distance = -r0 - dmr1;
                        if (d > 0.0) {
                            info.circle0Closest = c0 - r0 * U;
                            info.circle1Closest = c1 - r1 * U;
                        }
                        else {
                            // The circles are concentric, so U = (0,0,0).
                            // Construct a vector perpendicular to N0 to use for
                            // closest points.
                            U = get_orthogonal(n0, true);
                            info.circle0Closest = c0 + r0 * U;
                            info.circle1Closest = c1 + r1 * U;
                        }
                    }
                    else {
                        distance = (c1 - c0).norm();
                        info.circle0Closest = c0;
                        info.circle1Closest = c1;
                    }
                }

                info.sqrDistance = distance * distance;
            }
            if (deal_circle_result == false) {
                result.distance_infinite = std::make_optional(
                    DistAndPoints{std::sqrt(candidates[0].sqrDistance), candidates[0].circle0Closest, candidates[0].circle1Closest}); // TODO
            } else {
                const double dist      = (c0 - c1).norm();
                result.distance_strict = std::make_optional(DistAndPoints{dist, c0, c1});
            }

    ///////////////////////////////////////////////////////////////////////////
        } else if (f2.get_type() == SurfaceFeatureType::Plane) {
            const auto [center, radius, normal1] = f1.get_circle();
            const auto [idx2, normal2, origin2] = f2.get_plane();

            const bool coplanar = are_parallel(normal1, normal2) && Eigen::Hyperplane<double, 3>(normal1, center).absDistance(origin2) < EPSILON;
            if (!coplanar) {
                auto                       plane_features = f2.world_plane_features;
                std::vector<DistAndPoints> distances;
                for (const SurfaceFeature& sf : *plane_features) {
                    if (sf.get_type() == SurfaceFeatureType::Edge) {
                        const auto m = get_measurement(sf, f1);
                        if (!m.distance_infinite.has_value()) {
                            distances.clear();
                            break;
                        }
                        else
                            distances.push_back(*m.distance_infinite);
                    }
                }
                if (!distances.empty()) {
                    auto it = std::min_element(distances.begin(), distances.end(),
                        [](const DistAndPoints& item1, const DistAndPoints& item2) {
                            return item1.dist < item2.dist;
                        });
                    result.distance_infinite = std::make_optional(DistAndPoints{ it->dist, it->from, it->to });
                }
                else {
                    const Eigen::Hyperplane<double, 3> plane(normal2, origin2);
                    result.distance_infinite = std::make_optional(DistAndPoints{plane.absDistance(center), center, plane.projection(center)});
                }
            }
            else {
                result.distance_strict = std::make_optional(DistAndPoints{0, center, origin2});
            }
        }
    ///////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////////
    } else if (f1.get_type() == SurfaceFeatureType::Plane) {
        const auto [idx1, normal1, pt1] = f1.get_plane();
        const auto [idx2, normal2, pt2] = f2.get_plane();

        if (are_parallel(normal1, normal2)) {
            // The planes are parallel, calculate distance.
            const Eigen::Hyperplane<double, 3> plane(normal2, pt2);
            result.distance_infinite = std::make_optional(DistAndPoints{ plane.absDistance(pt1), pt1, plane.projection(pt1) });
        }
        else
            result.angle = angle_plane_plane(f1.get_plane(), f2.get_plane());
    }

    if (swap) {
        auto swap_dist_and_points = [](DistAndPoints& dp) {
            auto back   = dp.to;
            dp.to       = dp.from;
            dp.from     = back;
        };
        if (result.distance_infinite.has_value()) {
            swap_dist_and_points(*result.distance_infinite);
        }
        if (result.distance_strict.has_value()) {
            swap_dist_and_points(*result.distance_strict);
        }
    }
    return result;
}

bool can_set_xyz_distance(const SurfaceFeature &a, const SurfaceFeature &b) {
    const bool            swap = int(a.get_type()) > int(b.get_type());
    const SurfaceFeature &f1   = swap ? b : a;
    const SurfaceFeature &f2   = swap ? a : b;
    if (f1.get_type() == SurfaceFeatureType::Point){
        if (f2.get_type() == SurfaceFeatureType::Point) {
            return true;
        }
    }
    else if (f1.get_type() == SurfaceFeatureType::Circle) {
        if (f2.get_type() == SurfaceFeatureType::Circle) {
            return true;
        }
    }
    return false;
}

AssemblyAction get_assembly_action(const SurfaceFeature& a, const SurfaceFeature& b)
{
    AssemblyAction        action;
    const SurfaceFeature &f1   = a;
    const SurfaceFeature &f2   = b;
    if (f1.get_type() == SurfaceFeatureType::Plane) {
        action.can_set_feature_1_reverse_rotation = true;
        if (f2.get_type() == SurfaceFeatureType::Plane) {
            const auto [idx1, normal1, pt1] = f1.get_plane();
            const auto [idx2, normal2, pt2] = f2.get_plane();
            action.can_set_to_center_coincidence = true;
            action.can_set_feature_2_reverse_rotation = true;
            if (are_parallel(normal1, normal2)) {
                action.can_set_to_parallel = false;
                action.has_parallel_distance = true;
                action.can_around_center_of_faces = true;
                Vec3d proj_pt2;
                Measure::get_point_projection_to_plane(pt2, pt1, normal1, proj_pt2);
                action.parallel_distance = (pt2 - proj_pt2).norm();
                if ((pt2 - proj_pt2).dot(normal1) < 0) {
                    action.parallel_distance = -action.parallel_distance;
                }
                action.angle_radian          = 0;

            } else {
                action.can_set_to_parallel = true;
                action.has_parallel_distance = false;
                action.can_around_center_of_faces = false;
                action.parallel_distance     = 0;
                action.angle_radian = std::acos(std::clamp(normal2.dot(-normal1), -1.0, 1.0));
            }
        }
    }
    return action;
}

void SurfaceFeature::translate(const Vec3d& displacement) {
    switch (get_type()) {
    case Measure::SurfaceFeatureType::Point: {
        m_pt1 = m_pt1 + displacement;
        break;
    }
    case Measure::SurfaceFeatureType::Edge: {
        m_pt1 = m_pt1 + displacement;
        m_pt2 = m_pt2 + displacement;
        if (m_pt3.has_value()) { //extra_point()
            m_pt3  = *m_pt3 + displacement;
        }
        break;
    }
    case Measure::SurfaceFeatureType::Plane: {
        //m_pt1 is normal;
        m_pt2 = m_pt2 + displacement;
        break;
    }
    case Measure::SurfaceFeatureType::Circle: {
        m_pt1 = m_pt1 + displacement;
        // m_pt2 is normal;
        break;
    }
    default: break;
    }
}

void SurfaceFeature::translate(const Transform3d &tran)
{
    switch (get_type()) {
    case Measure::SurfaceFeatureType::Point: {
        m_pt1 = tran * m_pt1;
        break;
    }
    case Measure::SurfaceFeatureType::Edge: {
        m_pt1 = tran * m_pt1;
        m_pt2 = tran * m_pt2;
        if (m_pt3.has_value()) { // extra_point()
            m_pt3 = tran *  *m_pt3;
        }
        break;
    }
    case Measure::SurfaceFeatureType::Plane: {
        // m_pt1 is normal;
        Vec3d temp_pt1 = m_pt2 + m_pt1;
        temp_pt1       = tran * temp_pt1;
        m_pt2          = tran * m_pt2;
        m_pt1          = (temp_pt1 - m_pt2).normalized();
        break;
    }
    case Measure::SurfaceFeatureType::Circle: {
        // m_pt1 is center;
        // m_pt2 is normal;
        auto  local_normal = m_pt2;
        auto  local_center = m_pt1;
        Vec3d temp_pt2     = local_normal + local_center;
        temp_pt2       = tran * temp_pt2;
        m_pt1          = tran * m_pt1;
        auto world_center   = m_pt1;
        m_pt2          = (temp_pt2 - m_pt1).normalized();

        auto calc_world_radius = [&local_center, &local_normal, &tran, &world_center](const Vec3d &pt, double &value) {
            Vec3d intersection_pt;
            get_point_projection_to_plane(pt, local_center, local_normal, intersection_pt);
            Vec3d local_radius_pt = (intersection_pt - local_center).normalized() * value + local_center;
            Vec3d radius_pt       = tran * local_radius_pt;
            value                = (radius_pt - world_center).norm();
        };
        //m_value is radius
        auto  new_pt = get_one_point_in_plane(local_center, local_normal);
        calc_world_radius(new_pt, m_value);
        break;
    }
    default: break;
    }
}
   }//namespace Measure
} // namespace Slic3r

