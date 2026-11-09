#include "SeamPlacer.hpp"
#include "SeamContour.hpp"

#include "libslic3r/Point.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "Polygon.hpp"
#include "libslic3r/PrintBase.hpp"
#include "PrintConfig.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "tbb/parallel_for.h"
#include "tbb/blocked_range.h"
#include "tbb/parallel_reduce.h"
#include <atomic>
#include <Eigen/Geometry>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstddef>
#include <igl/Hit.h>
#include <optional>
#include <cassert>
#include <functional>
#include <numeric>
#include <memory>
#include <random>
#include <algorithm>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/KDTreeIndirect.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"

#include "libslic3r/Geometry/Curves.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/ShortEdgeCollapse.hpp"
#include "libslic3r/TriangleSetSampling.hpp"

#include "libslic3r/Utils.hpp"
#include "PreciseSeam.hpp"

//#define DEBUG_FILES

#ifdef DEBUG_FILES
#include <boost/nowide/cstdio.hpp>
#include <SVG.hpp>
#endif

namespace Slic3r {

namespace SeamPlacerImpl {

template<typename T> int sgn(T val) {
  return int(T(0) < val) - int(val < T(0));
}

// base function: ((e^(((1)/(x^(2)+1)))-1)/(e-1))
// checkout e.g. here: https://www.geogebra.org/calculator
float gauss(float value, float mean_x_coord, float mean_value, float falloff_speed) {
  float shifted = value - mean_x_coord;
  float denominator = falloff_speed * shifted * shifted + 1.0f;
  float exponent = 1.0f / denominator;
  return mean_value * (std::exp(exponent) - 1.0f) / (std::exp(1.0f) - 1.0f);
}

float compute_angle_penalty(float ccw_angle) {
  // This function is used:
  // ((ℯ^(((1)/(x^(2)*3+1)))-1)/(ℯ-1))*1+((1)/(2+ℯ^(-x)))
  // looks scary, but it is gaussian combined with sigmoid,
  // so that concave points have much smaller penalty over convex ones
  // https://github.com/prusa3d/PrusaSlicer/tree/master/doc/seam_placement/corner_penalty_function.png
  return gauss(ccw_angle, 0.0f, 1.0f, 3.0f) +
         1.0f / (2 + std::exp(-ccw_angle));
}

/// Coordinate frame
class Frame {
public:
  Frame() {
    mX = Vec3f(1, 0, 0);
    mY = Vec3f(0, 1, 0);
    mZ = Vec3f(0, 0, 1);
  }

  Frame(const Vec3f &x, const Vec3f &y, const Vec3f &z) :
                                                          mX(x), mY(y), mZ(z) {
  }

  void set_from_z(const Vec3f &z) {
    mZ = z.normalized();
    Vec3f tmpZ = mZ;
    Vec3f tmpX = (std::abs(tmpZ.x()) > 0.99f) ? Vec3f(0, 1, 0) : Vec3f(1, 0, 0);
    mY = (tmpZ.cross(tmpX)).normalized();
    mX = mY.cross(tmpZ);
  }

  Vec3f to_world(const Vec3f &a) const {
    return a.x() * mX + a.y() * mY + a.z() * mZ;
  }

  Vec3f to_local(const Vec3f &a) const {
    return Vec3f(mX.dot(a), mY.dot(a), mZ.dot(a));
  }

  const Vec3f& binormal() const {
    return mX;
  }

  const Vec3f& tangent() const {
    return mY;
  }

  const Vec3f& normal() const {
    return mZ;
  }

private:
  Vec3f mX, mY, mZ;
};

Vec3f sample_sphere_uniform(const Vec2f &samples) {
  float term1 = 2.0f * float(PI) * samples.x();
  float term2 = 2.0f * sqrt(samples.y() - samples.y() * samples.y());
  return {cos(term1) * term2, sin(term1) * term2,
          1.0f - 2.0f * samples.y()};
}

Vec3f sample_hemisphere_uniform(const Vec2f &samples) {
  float term1 = 2.0f * float(PI) * samples.x();
  float term2 = 2.0f * sqrt(samples.y() - samples.y() * samples.y());
  return {cos(term1) * term2, sin(term1) * term2,
          abs(1.0f - 2.0f * samples.y())};
}

Vec3f sample_power_cosine_hemisphere(const Vec2f &samples, float power) {
  float term1 = 2.f * float(PI) * samples.x();
  float term2 = pow(samples.y(), 1.f / (power + 1.f));
  float term3 = sqrt(1.f - term2 * term2);

  return Vec3f(cos(term1) * term3, sin(term1) * term3, term2);
}

std::vector<float> raycast_visibility(const AABBTreeIndirect::Tree<3, float> &raycasting_tree,
                                      const indexed_triangle_set &triangles,
                                      const TriangleSetSamples &samples,
                                      size_t negative_volumes_start_index,
                                      SeamPosition seam_position = spAligned) {
  BOOST_LOG_TRIVIAL(debug)
      << "SeamPlacer: raycast visibility of " << samples.positions.size() << " samples over " << triangles.indices.size()
      << " triangles: end";

  //prepare uniform samples of a hemisphere
  float step_size = 1.0f / SeamPlacer::sqr_rays_per_sample_point;
  std::vector<Vec3f> precomputed_sample_directions(
      SeamPlacer::sqr_rays_per_sample_point * SeamPlacer::sqr_rays_per_sample_point);
  for (size_t x_idx = 0; x_idx < SeamPlacer::sqr_rays_per_sample_point; ++x_idx) {
    float sample_x = x_idx * step_size + step_size / 2.0;
    for (size_t y_idx = 0; y_idx < SeamPlacer::sqr_rays_per_sample_point; ++y_idx) {
      size_t dir_index = x_idx * SeamPlacer::sqr_rays_per_sample_point + y_idx;
      float sample_y = y_idx * step_size + step_size / 2.0;
      precomputed_sample_directions[dir_index] = sample_hemisphere_uniform( { sample_x, sample_y });
    }
  }

  bool model_contains_negative_parts = negative_volumes_start_index < triangles.indices.size();

  std::vector<float> result(samples.positions.size());
  tbb::parallel_for(tbb::blocked_range<size_t>(0, result.size()),
                    [&triangles, &precomputed_sample_directions, model_contains_negative_parts, negative_volumes_start_index,
                     &raycasting_tree, &result, &samples, seam_position](tbb::blocked_range<size_t> r) {
                      // Maintaining hits memory outside of the loop, so it does not have to be reallocated for each query.
                      std::vector<igl::Hit<float>> hits;
                      for (size_t s_idx = r.begin(); s_idx < r.end(); ++s_idx) {
                        result[s_idx] = 1.0f;
                        constexpr float decrease_step = 1.0f
                                                        / (SeamPlacer::sqr_rays_per_sample_point * SeamPlacer::sqr_rays_per_sample_point);

                        const Vec3f &center = samples.positions[s_idx];
                        const Vec3f &normal = samples.normals[s_idx];
                        if (seam_position == spAlignedBack) {
                            const float front_adjustment = std::clamp((normal.dot(Vec3f(0.0f, -1.0f, 0.0f)) + 1.2f) * 0.5f, 0.0f, 1.0f);
                            result[s_idx] += front_adjustment;
                        }

                        // apply the local direction via Frame struct - the local_dir is with respect to +Z being forward
                        Frame f;
                        f.set_from_z(normal);

                        for (const auto &dir : precomputed_sample_directions) {
                          Vec3f final_ray_dir = (f.to_world(dir));
                          if (!model_contains_negative_parts) {
                            igl::Hit<float> hitpoint;
                            // FIXME: This AABBTTreeIndirect query will not compile for float ray origin and
                            // direction.
                            Vec3d final_ray_dir_d = final_ray_dir.cast<double>();
                            Vec3d ray_origin_d = (center + normal * 0.01f).cast<double>(); // start above surface.
                            bool hit = AABBTreeIndirect::intersect_ray_first_hit(triangles.vertices,
                                                                                 triangles.indices, raycasting_tree, ray_origin_d, final_ray_dir_d, hitpoint);
                            if (hit && its_face_normal(triangles, hitpoint.id).dot(final_ray_dir) <= 0) {
                              result[s_idx] -= decrease_step;
                            }
                          } else { //TODO improve logic for order based boolean operations - consider order of volumes
                            bool casting_from_negative_volume = samples.triangle_indices[s_idx]
                                                                >= negative_volumes_start_index;

                            Vec3d ray_origin_d = (center + normal * 0.01f).cast<double>(); // start above surface.
                            if (casting_from_negative_volume) { // if casting from negative volume face, invert direction, change start pos
                              final_ray_dir = -1.0 * final_ray_dir;
                              ray_origin_d = (center - normal * 0.01f).cast<double>();
                            }
                            Vec3d final_ray_dir_d = final_ray_dir.cast<double>();
                            bool some_hit = AABBTreeIndirect::intersect_ray_all_hits(triangles.vertices,
                                                                                     triangles.indices, raycasting_tree,
                                                                                     ray_origin_d, final_ray_dir_d, hits);
                            if (some_hit) {
                              int counter = 0;
                              // NOTE: iterating in reverse, from the last hit for one simple reason: We know the state of the ray at that point;
                              //  It cannot be inside model, and it cannot be inside negative volume
                              for (int hit_index = int(hits.size()) - 1; hit_index >= 0; --hit_index) {
                                Vec3f face_normal = its_face_normal(triangles, hits[hit_index].id);
                                if (hits[hit_index].id >= int(negative_volumes_start_index)) { //negative volume hit
                                  counter -= sgn(face_normal.dot(final_ray_dir)); // if volume face aligns with ray dir, we are leaving negative space
                                                                                               // which in reverse hit analysis means, that we are entering negative space :) and vice versa
                                } else {
                                  counter += sgn(face_normal.dot(final_ray_dir));
                                }
                              }
                              if (counter == 0) {
                                result[s_idx] -= decrease_step;
                              }
                            }
                          }
                        }
                      }
                    });

  BOOST_LOG_TRIVIAL(debug)
      << "SeamPlacer: raycast visibility of " << samples.positions.size() << " samples over " << triangles.indices.size()
      << " triangles: end";

  return result;
}

std::vector<float> calculate_polygon_angles_at_vertices(const Polygon &polygon, const std::vector<float> &lengths,
                                                        float min_arm_length) {
  std::vector<float> result(polygon.size());

  if (polygon.size() == 1) {
    result[0] = 0.0f;
  }

  size_t idx_prev = 0;
  size_t idx_curr = 0;
  size_t idx_next = 0;

  float distance_to_prev = 0;
  float distance_to_next = 0;

  //push idx_prev far enough back as initialization
  while (distance_to_prev < min_arm_length) {
    idx_prev = Slic3r::prev_idx_modulo(idx_prev, polygon.size());
    distance_to_prev += lengths[idx_prev];
  }

  for (size_t _i = 0; _i < polygon.size(); ++_i) {
    // pull idx_prev to current as much as possible, while respecting the min_arm_length
    while (distance_to_prev - lengths[idx_prev] > min_arm_length) {
      distance_to_prev -= lengths[idx_prev];
      idx_prev = Slic3r::next_idx_modulo(idx_prev, polygon.size());
    }

    //push idx_next forward as far as needed
    while (distance_to_next < min_arm_length) {
      distance_to_next += lengths[idx_next];
      idx_next = Slic3r::next_idx_modulo(idx_next, polygon.size());
    }

    // Calculate angle between idx_prev, idx_curr, idx_next.
    const Point &p0 = polygon.points[idx_prev];
    const Point &p1 = polygon.points[idx_curr];
    const Point &p2 = polygon.points[idx_next];
    result[idx_curr] = float(angle(p1 - p0, p2 - p1));

    // increase idx_curr by one
    float curr_distance = lengths[idx_curr];
    idx_curr++;
    distance_to_prev += curr_distance;
    distance_to_next -= curr_distance;
  }

  return result;
}

struct CoordinateFunctor {
  const std::vector<Vec3f> *coordinates;
  CoordinateFunctor(const std::vector<Vec3f> *coords) :
                                                        coordinates(coords) {
  }
  CoordinateFunctor() :
                        coordinates(nullptr) {
  }

  const float& operator()(size_t idx, size_t dim) const {
    return coordinates->operator [](idx)[dim];
  }
};

// A paint boundary uses the same geometry for green preferences and red exclusions.
struct PaintedContour {
  std::vector<Linef3> edges;
  std::vector<Vec3d> inward;
  AABBTreeIndirect::Tree<3, double> tree;

  std::vector<std::pair<Vec2d, Vec2d>> crossings(double z, const Vec2d &min, const Vec2d &max) const {
    std::vector<std::pair<Vec2d, Vec2d>> result;
    const Eigen::AlignedBox<double, 3> box(Vec3d(min.x(), min.y(), z), Vec3d(max.x(), max.y(), z));
    AABBTreeIndirect::traverse(tree, AABBTreeIndirect::intersecting(box), [&](const auto &node) {
      const Linef3 &edge = edges[node.idx];
      // Half-open in z counts a contour vertex on the plane once.
      if ((edge.a.z() < z) != (edge.b.z() < z)) {
        const Vec3d p = edge.intersect_plane(z);
        result.emplace_back(p.head<2>(), inward[node.idx].template head<2>());
      }
      return true;
    });
    return result;
  }
};

// structure to store global information about the model - occlusion hits, enforcers, blockers
struct GlobalModelInfo {
  TriangleSetSamples mesh_samples;
  std::vector<float> mesh_samples_visibility;
  CoordinateFunctor mesh_samples_coordinate_functor;
  KDTreeIndirect<3, float, CoordinateFunctor> mesh_samples_tree { CoordinateFunctor { } };
  float mesh_samples_radius;

  indexed_triangle_set enforcers;
  indexed_triangle_set blockers;
  // Non-green surface proves that missing paint ends do not mean full coverage.
  indexed_triangle_set non_enforcers;
  AABBTreeIndirect::Tree<3, float> enforcers_tree;
  AABBTreeIndirect::Tree<3, float> blockers_tree;
  AABBTreeIndirect::Tree<3, float> non_enforcers_tree;
  // Contour of the painted enforcer region: edges of one enforcer triangle only, with the unit direction from
  // each edge into the paint.
  PaintedContour enforcer_contour;
  PaintedContour blocker_contour;

  // Precise Seam modifiers: strong modifiers (CENTER/LEFT/RIGHT) determine exact seam placement
  std::vector<const ModelVolume*> precise_seam_strong_volumes;
  // Precise Seam modifiers: weak modifiers (ENFORCED/BLOCKED/NEUTRAL) provide hints for seam placement
  std::vector<const ModelVolume*> precise_seam_weak_volumes;

  // Slice each modifier once; both consumers share structured regions and source provenance.
  PreciseSeam::ModifierRegionsCache precise_seam_slices;

  bool is_enforced(const Vec3f &position, float radius) const {
    if (enforcers.empty()) {
      return false;
    }
    float radius_sqr = radius * radius;
    return AABBTreeIndirect::is_any_triangle_in_radius(enforcers.vertices, enforcers.indices,
                                                       enforcers_tree, position, radius_sqr);
  }

  bool is_blocked(const Vec3f &position, float radius) const {
    if (blockers.empty()) {
      return false;
    }
    float radius_sqr = radius * radius;
    return AABBTreeIndirect::is_any_triangle_in_radius(blockers.vertices, blockers.indices,
                                                       blockers_tree, position, radius_sqr);
  }

  float calculate_point_visibility(const Vec3f &position) const {
    std::vector<size_t> points = find_nearby_points(mesh_samples_tree, position, mesh_samples_radius);
    if (points.empty()) {
      return 1.0f;
    }

    auto compute_dist_to_plane = [](const Vec3f &position, const Vec3f &plane_origin, const Vec3f &plane_normal) {
      Vec3f orig_to_point = position - plane_origin;
      return std::abs(orig_to_point.dot(plane_normal));
    };

    float total_weight = 0;
    float total_visibility = 0;
    for (size_t i = 0; i < points.size(); ++i) {
      size_t sample_idx = points[i];

      Vec3f sample_point = this->mesh_samples.positions[sample_idx];
      Vec3f sample_normal = this->mesh_samples.normals[sample_idx];

      float weight = mesh_samples_radius - compute_dist_to_plane(position, sample_point, sample_normal);
      weight += (mesh_samples_radius - (position - sample_point).norm());
      total_visibility += weight * mesh_samples_visibility[sample_idx];
      total_weight += weight;
    }

    return total_visibility / total_weight;

  }

#ifdef DEBUG_FILES
  void debug_export(const indexed_triangle_set &obj_mesh) const {

    indexed_triangle_set divided_mesh = obj_mesh;
    Slic3r::CNumericLocalesSetter locales_setter;

    {
      auto filename = debug_out_path("visiblity.obj");
      FILE *fp = boost::nowide::fopen(filename.c_str(), "w");
      if (fp == nullptr) {
        BOOST_LOG_TRIVIAL(error)
            << "stl_write_obj: Couldn't open " << filename << " for writing";
        return;
      }

      for (size_t i = 0; i < divided_mesh.vertices.size(); ++i) {
        float visibility = calculate_point_visibility(divided_mesh.vertices[i]);
        Vec3f color = value_to_rgbf(0.0f, 1.0f, visibility);
        fprintf(fp, "v %f %f %f  %f %f %f\n",
                divided_mesh.vertices[i](0), divided_mesh.vertices[i](1), divided_mesh.vertices[i](2),
                color(0), color(1), color(2));
      }
      for (size_t i = 0; i < divided_mesh.indices.size(); ++i)
        fprintf(fp, "f %d %d %d\n", divided_mesh.indices[i][0] + 1, divided_mesh.indices[i][1] + 1,
                divided_mesh.indices[i][2] + 1);
      fclose(fp);
    }

    {
      auto filename = debug_out_path("visiblity_samples.obj");
      FILE *fp = boost::nowide::fopen(filename.c_str(), "w");
      if (fp == nullptr) {
        BOOST_LOG_TRIVIAL(error)
            << "stl_write_obj: Couldn't open " << filename << " for writing";
        return;
      }

      for (size_t i = 0; i < mesh_samples.positions.size(); ++i) {
        float visibility = mesh_samples_visibility[i];
        Vec3f color = value_to_rgbf(0.0f, 1.0f, visibility);
        fprintf(fp, "v %f %f %f  %f %f %f\n",
                mesh_samples.positions[i](0), mesh_samples.positions[i](1), mesh_samples.positions[i](2),
                color(0), color(1), color(2));
      }
      fclose(fp);
    }

  }
#endif
}
;

//Extract perimeter polygons of the given layer
Polygons extract_perimeter_polygons(const Layer *layer, std::vector<const LayerRegion*> &corresponding_regions_out,
                                   std::vector<std::optional<bool>> &boundary_types_out,
                                   bool has_precise_seam_modifiers) {
  Polygons polygons;
  const auto boundary_type = [](const ExtrusionEntity *entity) -> std::optional<bool> {
    if (!entity->is_loop()) return std::nullopt;
    return (static_cast<const ExtrusionLoop*>(entity)->loop_role() & elrHole) != 0;
  };
  for (const LayerRegion *layer_region : layer->regions()) {
    for (const ExtrusionEntity *ex_entity : layer_region->perimeters.entities) {
      if (ex_entity->is_collection()) { //collection of inner, outer, and overhang perimeters
        const size_t collection_begin = polygons.size(); // The fallback below applies per collection, not per layer.
        for (const ExtrusionEntity *perimeter : static_cast<const ExtrusionEntityCollection*>(ex_entity)->entities) {
          ExtrusionRole role = perimeter->role();
          if (perimeter->is_loop()) {
            for (const ExtrusionPath &path : static_cast<const ExtrusionLoop*>(perimeter)->paths) {
              if (path.role() == ExtrusionRole::erExternalPerimeter) {
                role = ExtrusionRole::erExternalPerimeter;
              }
            }
            // A fully overhanging outer loop has no external paths but keeps inset_idx 0.
            if (perimeter->inset_idx == 0)
              role = ExtrusionRole::erExternalPerimeter;
          }

          if (role == ExtrusionRole::erExternalPerimeter) {
            Points p;
            perimeter->collect_points(p);
            polygons.emplace_back(std::move(p));
            corresponding_regions_out.push_back(layer_region);
            boundary_types_out.push_back(boundary_type(perimeter));
          }
        }
        if (polygons.size() == collection_begin) {
          Points p;
          ex_entity->collect_points(p);
          polygons.emplace_back(std::move(p));
          corresponding_regions_out.push_back(layer_region);
          boundary_types_out.push_back(boundary_type(ex_entity));
        }
      } else {
        Points p;
        ex_entity->collect_points(p);
        polygons.emplace_back(std::move(p));
        corresponding_regions_out.push_back(layer_region);
        boundary_types_out.push_back(boundary_type(ex_entity));
      }
    }
  }

  if (has_precise_seam_modifiers) {
    // Extrusion loops repeat their start point; Polygon closes the contour implicitly.
    // Normalize here for Precise Seam without changing ordinary seam candidates.
    for (Polygon &polygon : polygons) {
      // Adjacent extrusion paths share endpoints; zero-length edges would prevent refinement at their junctions.
      // Remove only consecutive duplicates, preserving distinct visits to a self-touching contour point.
      polygon.points.erase(std::unique(polygon.points.begin(), polygon.points.end()), polygon.points.end());
      while (polygon.size() > 1 && polygon.points.front() == polygon.points.back())
        polygon.points.pop_back();
    }
  }

  if (polygons.empty()) { // If there are no perimeter polygons for whatever reason (disabled perimeters .. ) insert dummy point
    // it is easier than checking everywhere if the layer is not emtpy, no seam will be placed to this layer anyway
    polygons.emplace_back(Points{ { 0, 0 } });
    corresponding_regions_out.push_back(nullptr);
    boundary_types_out.push_back(std::nullopt);
  }

  return polygons;
}

// Painted range of a loop along its length from its first vertex; `end` exceeds the loop length if the range
// passes the first vertex.
struct PaintedInterval {
  double begin;
  double end;
};

// Two loop positions closer than this (1 um) are one point, mm.
constexpr double enforced_end_tolerance = 1e-3;

// Prove complete coverage before sampling candidates; proximity at vertices alone can miss paint gaps.
bool fully_enforced_loop(const std::vector<Vec2d> &loop, double z, double reach, const GlobalModelInfo &info) {
  if (loop.size() < 3 || info.enforcers.empty())
    return false;
  if (info.non_enforcers.empty())
    return true; // Every relevant model surface is green, so paint adds no seam information.
  if (reach <= enforced_end_tolerance)
    return false;
  Vec2d min = loop.front(), max = loop.front();
  for (const Vec2d &point : loop) {
    min = min.cwiseMin(point);
    max = max.cwiseMax(point);
  }
  // A conservative box also rejects tiny gaps and a nearby opposite unpainted wall.
  const Eigen::AlignedBox<float, 3> box(Vec3f(float(min.x() - reach), float(min.y() - reach), float(z - reach)),
                                       Vec3f(float(max.x() + reach), float(max.y() + reach), float(z + reach)));
  bool non_green_nearby = false;
  AABBTreeIndirect::traverse(info.non_enforcers_tree, AABBTreeIndirect::intersecting(box), [&](const auto &) {
    non_green_nearby = true;
    return false; // Stop on the first uncertain surface rather than infer a full painted ring.
  });
  if (non_green_nearby)
    return false;
  const auto covered = [&](const auto &self, const Vec3d &a, const Vec3d &b, unsigned depth) -> bool {
    const Vec3d middle = 0.5 * (a + b);
    // A paint point this close to the midpoint covers the entire segment by the triangle inequality.
    const double radius = reach - 0.5 * (b - a).norm() - enforced_end_tolerance;
    if (radius > 0.0 && info.is_enforced(middle.cast<float>(), float(radius)))
      return true;
    if (depth == 8)
      return false; // A bounded proof failure preserves the existing conservative paint behavior.
    return self(self, a, middle, depth + 1) && self(self, middle, b, depth + 1);
  };
  for (size_t i = 0; i < loop.size(); ++i) {
    const Vec2d &a = loop[i], &b = loop[(i + 1) % loop.size()];
    if (!covered(covered, Vec3d(a.x(), a.y(), z), Vec3d(b.x(), b.y(), z), 0))
      return false;
  }
  return true;
}

// Painted ranges of a counterclockwise loop at height z, bounded exactly where the paint contour crosses the
// layer: each crossing within `reach` of the loop is moved to its closest loop point, and paint lies ahead of
// it or behind it along the loop. `arc` holds the length from the first vertex to each vertex, the loop length
// last. Empty if the contour does not cross near the loop or its crossings do not pair up into ranges.
std::vector<PaintedInterval> exact_painted_intervals(const std::vector<Vec2d> &loop, const std::vector<double> &arc,
                                                   double z, double reach, const PaintedContour &contour) {
  const size_t count = loop.size();
  if (count < 3 || contour.edges.empty())
    return {};
  Vec2d min = loop.front(), max = loop.front();
  for (const Vec2d &p : loop) {
    min = min.cwiseMin(p);
    max = max.cwiseMax(p);
  }
  const auto crossings = contour.crossings(z, min - Vec2d(reach, reach), max + Vec2d(reach, reach));
  // Direction of the first edge from edge i on, forward or backward, that is longer than the tolerance: loops
  // repeat their first point and the shared ends of their paths.
  const auto direction = [&](size_t i, bool forward) {
    for (size_t step = 0; step < count; ++step, i = forward ? (i + 1) % count : (i + count - 1) % count) {
      const Vec2d d = loop[(i + 1) % count] - loop[i];
      if (d.norm() >= enforced_end_tolerance)
        return Vec2d(d.normalized());
    }
    return Vec2d(Vec2d::Zero());
  };
  std::vector<std::pair<double, bool>> ends; // Position along the loop; true if paint lies ahead.
  for (const auto &[point, inward] : crossings) {
    double best = std::numeric_limits<double>::max(), position = 0.0;
    Vec2d tangent = Vec2d::Zero();
    for (size_t i = 0; i < count; ++i) {
      const Vec2d a = loop[i], ab = loop[(i + 1) % count] - a;
      const double length2 = ab.squaredNorm();
      const double t = length2 > 0.0 ? std::clamp((point - a).dot(ab) / length2, 0.0, 1.0) : 0.0;
      const double distance2 = (a + t * ab - point).squaredNorm();
      if (distance2 < best) {
        best = distance2;
        const double along = t * (arc[i + 1] - arc[i]);
        position = arc[i] + along;
        // At a vertex the loop turns, so its direction there is the mean of the two edges; a crossing at a
        // corner may land a rounding error off the vertex, on either edge.
        const size_t vertex = along < enforced_end_tolerance                 ? i :
                              arc[i + 1] - position < enforced_end_tolerance ? (i + 1) % count :
                                                                               count;
        tangent = vertex < count ? direction((vertex + count - 1) % count, false) + direction(vertex, true) :
                                   direction(i, true);
      }
    }
    if (best > reach * reach)
      continue;
    const double side = inward.dot(tangent);
    if (std::abs(side) < 1e-6)
      return {}; // The contour runs along the loop here: no clear end.
    ends.emplace_back(position, side > 0.0);
  }
  if (ends.empty() || ends.size() % 2 != 0)
    return {};
  std::sort(ends.begin(), ends.end());
  for (size_t i = 0; i < ends.size(); ++i)
    if (ends[i].second == ends[(i + 1) % ends.size()].second)
      return {}; // Paint must start and stop in turn along the loop.
  const size_t first = ends.front().second ? 0 : 1;
  std::vector<PaintedInterval> intervals;
  for (size_t i = first; i < first + ends.size(); i += 2) {
    const double begin = ends[i % ends.size()].first;
    double end = ends[(i + 1) % ends.size()].first;
    if (end < begin)
      end += arc.back();
    intervals.push_back({begin, end});
  }
  return intervals;
}

// Build SeamCandidates for each vertex of the perimeter polygon and attach them to a shared Perimeter.
// For each vertex: computes position, angle, and type (Enforcer / Blocker / Neutral).
// When Precise Seam modifiers are present: inserts strong seam point,
// oversamples enforcer edges, applies weak modifiers, marks one enforced point as central for alignment.
void process_perimeter_polygon(const Polygon &orig_polygon, float z_coord, const LayerRegion *region,
                               const GlobalModelInfo &global_model_info, PrintObjectSeamData::LayerSeams &result,
                               PreciseSeam::PreciseSeamWarnings* warnings = nullptr,
                               std::optional<bool> boundary_hole = std::nullopt) {
  if (orig_polygon.size() == 0) {
    return;
  }
  Polygon polygon = orig_polygon;
  bool was_clockwise = polygon.make_counter_clockwise();

  // Process Precise Seam modifiers to find seam placement
  const Layer* layer = region ? region->layer() : nullptr;

  // Use pre-computed Precise Seam volumes from global_model_info (computed once in init)
  const auto& strong_volumes = global_model_info.precise_seam_strong_volumes;
  const auto& weak_volumes = global_model_info.precise_seam_weak_volumes;

  std::optional<Point> seam_point;
  std::vector<PreciseSeam::WeakModifierSegment> weak_segments;
  if (layer != nullptr && (!strong_volumes.empty() || !weak_volumes.empty())) {
    // Share validation, bounds and clipping line across all modifiers while the polygon is unchanged.
    // A strong insertion ends processing; otherwise weak reads the same preparation before inserting.
    const PreciseSeam::PreparedPerimeter prepared(polygon);
    seam_point = PreciseSeam::insert_strong_seam_point(
        strong_volumes, polygon, prepared, layer, global_model_info.precise_seam_slices, warnings);
    if (!seam_point.has_value())
      weak_segments = PreciseSeam::collect_weak_modifier_segments(
          weak_volumes, polygon, prepared, layer, global_model_info.precise_seam_slices, warnings);
  }

  // Store the inserted point position for marking as central_enforcer later.
  std::optional<Vec3f> inserted_seam_position;
  if (seam_point.has_value()) {
    Vec2f unscaled_p = unscale(seam_point.value()).cast<float>();
    inserted_seam_position = Vec3f(unscaled_p.x(), unscaled_p.y(), z_coord);
  }

  float angle_arm_len = region != nullptr ? region->flow(FlowRole::frExternalPerimeter).nozzle_diameter() : 0.5f;

  std::vector<float> lengths { };
  for (size_t point_idx = 0; point_idx < polygon.size() - 1; ++point_idx) {
    lengths.push_back((unscale(polygon[point_idx]) - unscale(polygon[point_idx + 1])).norm());
  }
  lengths.push_back(std::max((unscale(polygon[0]) - unscale(polygon[polygon.size() - 1])).norm(), 0.1));
  std::vector<float> polygon_angles = calculate_polygon_angles_at_vertices(polygon, lengths,
                                                                           angle_arm_len);

  result.perimeters.push_back( { });
  Perimeter &perimeter = result.perimeters.back();
  // The loop role is independent of the configured printing direction.
  perimeter.is_hole = boundary_hole.value_or(was_clockwise);
  perimeter.contour_reliable = boundary_hole.has_value(); // Keep unknown roles out of geometric matching.
  perimeter.contour_index = result.perimeters.size() - 1;

  std::queue<Vec3f> orig_polygon_points { };
  for (size_t index = 0; index < polygon.size(); ++index) {
    Vec2f unscaled_p = unscale(polygon[index]).cast<float>();
    orig_polygon_points.emplace(unscaled_p.x(), unscaled_p.y(), z_coord);
  }
  Vec3f first = orig_polygon_points.front();
  std::queue<Vec3f> oversampled_points { };
  size_t orig_angle_index = 0;
  perimeter.start_index = result.points.size();
  perimeter.flow_width = region != nullptr ? region->flow(FlowRole::frExternalPerimeter).width() : 0.0f;

  // Painted ranges bounded exactly by the paint contour crossing this layer; without them, points within
  // flow_width of paint are Enforced.
  std::vector<Vec2d> loop_xy;
  std::vector<double> loop_arc { 0.0 };
  for (const Point &point : polygon.points)
    loop_xy.push_back(unscale(point));
  for (size_t i = 0; i < loop_xy.size(); ++i)
    loop_arc.push_back(loop_arc.back() + (loop_xy[(i + 1) % loop_xy.size()] - loop_xy[i]).norm());
  const double loop_length = loop_arc.back();
  const auto intervals = exact_painted_intervals(loop_xy, loop_arc, z_coord, perimeter.flow_width,
                                                 global_model_info.enforcer_contour);
  const auto blocked_intervals = exact_painted_intervals(loop_xy, loop_arc, z_coord, perimeter.flow_width,
                                                         global_model_info.blocker_contour);
  perimeter.exact_blocked_ends = !blocked_intervals.empty();
  const bool exact_marks = !intervals.empty() || perimeter.exact_blocked_ends;
  // Keep active Precise Seam modifiers authoritative; normalize ordinary full paint only.
  const bool ignore_full_paint = intervals.empty() && !inserted_seam_position && weak_segments.empty() &&
      fully_enforced_loop(loop_xy, z_coord, perimeter.flow_width, global_model_info);
  if (ignore_full_paint)
    BOOST_LOG_TRIVIAL(debug) << "[SeamPaint] full_perimeter z=" << z_coord << " normalized=true";
  perimeter.exact_enforced_ends = !intervals.empty();
  const auto in_interval = [&](double at, const std::vector<PaintedInterval> &ranges) {
    for (const PaintedInterval &interval : ranges)
      for (const double s : { at, at + loop_length })
        if (s >= interval.begin - enforced_end_tolerance && s <= interval.end + enforced_end_tolerance)
          return true;
    return false;
  };
  const auto point_at = [&](double s) {
    const size_t i = std::min(size_t(std::upper_bound(loop_arc.begin(), loop_arc.end(), s) - loop_arc.begin()),
                              loop_xy.size()) - 1;
    const double length = loop_arc[i + 1] - loop_arc[i];
    const Vec2d p = loop_xy[i] + (length > 0.0 ? (s - loop_arc[i]) / length : 0.0) *
                                     (loop_xy[(i + 1) % loop_xy.size()] - loop_xy[i]);
    return Vec3f(float(p.x()), float(p.y()), z_coord);
  };
  // Green paint without exact ends: an edge whose start is within the edge length of paint gets a point every
  // enforcer_oversampling_distance, passed to emit as its distance from the start.
  const auto oversample_green = [&](const Vec3f &start, float length, const auto &emit) {
    if (ignore_full_paint || !global_model_info.is_enforced(start, length))
      return;
    for (float step = SeamPlacer::enforcer_oversampling_distance; step < length;
         step += SeamPlacer::enforcer_oversampling_distance)
      emit(step);
  };
  // Points to insert: the range ends, middles and every enforcer_oversampling_distance from their begin, and the
  // middle of each gap between ranges, which keeps neighboring ranges apart as patches; as sorted loop positions
  // in [0, loop_length).
  std::vector<double> marks;
  std::vector<std::pair<Vec3f, double>> interval_middles; // Middle point and length of each range.
  for (size_t k = 0; k < intervals.size(); ++k) {
    const PaintedInterval &interval = intervals[k];
    const double middle = 0.5 * (interval.begin + interval.end);
    // Ranges come in loop order from the first vertex.
    const double next_begin = k + 1 < intervals.size() ? intervals[k + 1].begin : intervals.front().begin + loop_length;
    marks.insert(marks.end(), { interval.begin, interval.end, middle, 0.5 * (interval.end + next_begin) });
    for (double s = interval.begin + SeamPlacer::enforcer_oversampling_distance; s < interval.end - enforced_end_tolerance;
         s += SeamPlacer::enforcer_oversampling_distance)
      marks.push_back(s);
    interval_middles.emplace_back(point_at(std::fmod(middle, loop_length)), interval.end - interval.begin);
  }
  for (size_t k = 0; k < blocked_intervals.size(); ++k) {
    const auto &interval = blocked_intervals[k];
    const double next_begin = k + 1 < blocked_intervals.size() ? blocked_intervals[k + 1].begin :
                                                              blocked_intervals.front().begin + loop_length;
    const double gap = next_begin - interval.end;
    // Keep the boundary itself blocked, with a permitted sample just outside it. Both alignment and
    // the iterative solvers stop at these samples, within 2 um of the geometric paint edge.
    const double guard = std::min(2.0 * enforced_end_tolerance, 0.25 * gap);
    marks.insert(marks.end(), {interval.begin, interval.end, 0.5 * (interval.begin + interval.end),
                               interval.end + guard, next_begin - guard, 0.5 * (interval.end + next_begin)});
  }
  if (perimeter.exact_blocked_ends && !perimeter.exact_enforced_ends) {
    // Red marks must not suppress legacy green oversampling when its boundary is uncertain.
    for (size_t i = 0; i < loop_xy.size(); ++i)
      oversample_green(point_at(loop_arc[i]), float(loop_arc[i + 1] - loop_arc[i]),
                       [&](float step) { marks.push_back(loop_arc[i] + step); });
  }
  for (double &s : marks)
    s = std::fmod(s, loop_length);
  std::sort(marks.begin(), marks.end());
  // A mark closer than the tolerance to the last one kept is the same point.
  size_t kept = 0;
  for (size_t i = 0; i < marks.size(); ++i)
    if (kept == 0 || marks[i] - marks[kept - 1] >= enforced_end_tolerance)
      marks[kept++] = marks[i];
  marks.resize(kept);

  std::queue<double> oversampled_arcs { }; // Loop positions of the inserted points with exact ranges.
  bool some_point_enforced = false;
  while (!orig_polygon_points.empty() || !oversampled_points.empty()) {
    EnforcedBlockedSeamPoint type = EnforcedBlockedSeamPoint::Neutral;
    Vec3f position;
    float local_ccw_angle = 0;
    bool orig_point = false;
    double at = 0.0; // Loop position, used with exact ranges.
    if (!oversampled_points.empty()) {
      position = oversampled_points.front();
      oversampled_points.pop();
      if (exact_marks) {
        at = oversampled_arcs.front();
        oversampled_arcs.pop();
      }
    } else {
      position = orig_polygon_points.front();
      orig_polygon_points.pop();
      local_ccw_angle = was_clockwise ? -polygon_angles[orig_angle_index] : polygon_angles[orig_angle_index];
      at = loop_arc[orig_angle_index];
      orig_angle_index++;
      orig_point = true;
    }

    // Projected ends bound paint but cannot prove that a detour between them stays near its surface.
    if (!ignore_full_paint && (!perimeter.exact_enforced_ends || in_interval(at, intervals)) &&
        global_model_info.is_enforced(position, perimeter.flow_width)) {
      type = EnforcedBlockedSeamPoint::Enforced;
    }

    if ((!perimeter.exact_blocked_ends || in_interval(at, blocked_intervals)) &&
        global_model_info.is_blocked(position, perimeter.flow_width)) {
      type = EnforcedBlockedSeamPoint::Blocked;
    }
    some_point_enforced = some_point_enforced || type == EnforcedBlockedSeamPoint::Enforced;

    if (orig_point && exact_marks) {
      // Marks strictly inside the edge from this vertex; one at a vertex is the vertex itself.
      const double from = loop_arc[orig_angle_index - 1], to = loop_arc[orig_angle_index];
      for (auto it = std::upper_bound(marks.begin(), marks.end(), from + enforced_end_tolerance);
           it != marks.end() && *it < to - enforced_end_tolerance; ++it) {
        oversampled_points.push(point_at(*it));
        oversampled_arcs.push(*it);
      }
    } else if (orig_point) {
      const Vec3f pos_of_next = orig_polygon_points.empty() ? first : orig_polygon_points.front();
      const Vec3f vec_to_next = (pos_of_next - position).normalized();
      oversample_green(position, (position - pos_of_next).norm(),
                       [&](float step) { oversampled_points.push(position + vec_to_next * step); });
    }

    result.points.emplace_back(position, perimeter, local_ccw_angle, type);
  }

  perimeter.end_index = result.points.size();

  // Apply weak modifiers if no strong modifier was inserted
  if (!inserted_seam_position.has_value() && !weak_segments.empty()) {
    PreciseSeam::apply_weak_modifiers_to_perimeter(
        weak_segments, result, perimeter, some_point_enforced, warnings);
  }

  if (some_point_enforced) {
    // Choose the continuous enforced patch with the most candidates, then select its middle
    // candidate or a sharp corner. Patch length here is a point count, not geometric distance.
    size_t perimeter_size = perimeter.end_index - perimeter.start_index;
    const auto next_index = [&](size_t idx) {
      return perimeter.start_index + Slic3r::next_idx_modulo(idx - perimeter.start_index, perimeter_size);
    };

    std::vector<size_t> patches_starts_ends;
    for (size_t i = perimeter.start_index; i < perimeter.end_index; ++i) {
      if (result.points[i].type != EnforcedBlockedSeamPoint::Enforced &&
          result.points[next_index(i)].type == EnforcedBlockedSeamPoint::Enforced) {
        patches_starts_ends.push_back(next_index(i));
      }
      if (result.points[i].type == EnforcedBlockedSeamPoint::Enforced &&
          result.points[next_index(i)].type != EnforcedBlockedSeamPoint::Enforced) {
        patches_starts_ends.push_back(next_index(i));
      }
    }
    // If patches_starts_ends are empty, the whole perimeter is enforced, or no point is enforced any more
    // (Precise Seam weak zones retyped every painted enforcer); don't do anything in either case.
    if (!patches_starts_ends.empty()) {
      //if the first point in the patches is not enforced, it marks a patch end. in that case, put it to the end and start on next
      // to simplify the processing
      assert(patches_starts_ends.size() % 2 == 0);
      bool start_on_second = false;
      if (result.points[patches_starts_ends[0]].type != EnforcedBlockedSeamPoint::Enforced) {
        start_on_second = true;
        patches_starts_ends.push_back(patches_starts_ends[0]);
      }
      //now pick the longest patch
      std::pair<size_t, size_t> longest_patch { 0, 0 };
      auto patch_len = [perimeter_size](const std::pair<size_t, size_t> &start_end) {
        if (start_end.second < start_end.first) {
          // Count [start, end) across the closing edge, independently of the contour's start.
          // Subtract indices first: they are offsets in the layer, not local perimeter indices.
          return perimeter_size - (start_end.first - start_end.second);
        } else {
          return start_end.second - start_end.first;
        }
      };
      for (size_t patch_idx = start_on_second ? 1 : 0; patch_idx < patches_starts_ends.size(); patch_idx += 2) {
        std::pair<size_t, size_t> current_patch { patches_starts_ends[patch_idx], patches_starts_ends[patch_idx
                                                                                                    + 1] };
        if (patch_len(longest_patch) < patch_len(current_patch)) {
          longest_patch = current_patch;
        }
      }
      std::vector<size_t> viable_points_indices;
      std::vector<size_t> large_angle_points_indices;
      for (size_t point_idx = longest_patch.first; point_idx != longest_patch.second;
           point_idx = next_index(point_idx)) {
        viable_points_indices.push_back(point_idx);
        if (std::abs(result.points[point_idx].local_ccw_angle)
            > SeamPlacer::sharp_angle_snapping_threshold) {
          large_angle_points_indices.push_back(point_idx);
        }
      }
      assert(viable_points_indices.size() > 0);
      if (large_angle_points_indices.empty()) {
        size_t central_idx = viable_points_indices[viable_points_indices.size() / 2];
        // With exact ranges, the middle of the longest range in the patch.
        double longest = -1.0;
        for (const size_t idx : viable_points_indices)
          for (const auto &[middle, length] : interval_middles)
            if (length > longest && (result.points[idx].position - middle).norm() <= enforced_end_tolerance) {
              central_idx = idx;
              longest = length;
            }
        result.points[central_idx].central_enforcer = true;
      } else {
        size_t central_idx = large_angle_points_indices.size() / 2;
        result.points[large_angle_points_indices[central_idx]].central_enforcer = true;
      }
    }
  }

  // Apply precise seam point if it was inserted
  if (inserted_seam_position.has_value()) {
    // Set single point as Enforced, block all others
    for (size_t i = perimeter.start_index; i < perimeter.end_index; ++i) {
      if (result.points[i].position == inserted_seam_position.value()) {
        // Mark as the single enforced point with highest priority
        result.points[i].type = EnforcedBlockedSeamPoint::Enforced;
        result.points[i].central_enforcer = true;
        perimeter.precise_seam_point = inserted_seam_position;
        perimeter.precise_seam_index = i;
      } else {
        // Block all other points
        result.points[i].type = EnforcedBlockedSeamPoint::Blocked;
        result.points[i].central_enforcer = false;
      }
    }
  }

}

// Computes all global model info - transforms object, performs raycasting
void compute_global_occlusion(GlobalModelInfo &result, const PrintObject *po,
                              std::function<void(void)> throw_if_canceled,
                              SeamPosition seam_position = spAligned) {
  BOOST_LOG_TRIVIAL(debug)
      << "SeamPlacer: gather occlusion meshes: start";
  auto obj_transform = po->trafo_sliced();
  indexed_triangle_set triangle_set;
  indexed_triangle_set negative_volumes_set;
  //add all parts
  for (const ModelVolume *model_volume : po->model_object()->volumes) {
    if (model_volume->type() == ModelVolumeType::MODEL_PART
        || model_volume->type() == ModelVolumeType::NEGATIVE_VOLUME) {
      auto model_transformation = model_volume->get_matrix();
      indexed_triangle_set model_its = model_volume->mesh().its;
      // ORCA fix (not related to Precise Seam, discovered during its development):
      // Mirror transforms have negative determinant which flips triangle winding.
      // fix_left_handed=true swaps indices to keep normals pointing outward.
      its_transform(model_its, model_transformation, true);
      if (model_volume->type() == ModelVolumeType::MODEL_PART) {
        its_merge(triangle_set, model_its);
      } else {
        its_merge(negative_volumes_set, model_its);
      }
    }
  }
  throw_if_canceled();

  BOOST_LOG_TRIVIAL(debug)
      << "SeamPlacer: gather occlusion meshes: end";

  BOOST_LOG_TRIVIAL(debug)
      << "SeamPlacer: decimate: start";
  its_short_edge_collpase(triangle_set, SeamPlacer::fast_decimation_triangle_count_target);
  its_short_edge_collpase(negative_volumes_set, SeamPlacer::fast_decimation_triangle_count_target);

  size_t negative_volumes_start_index = triangle_set.indices.size();
  its_merge(triangle_set, negative_volumes_set);
  // ORCA fix (not related to Precise Seam, discovered during its development):
  // Object-level transform may include mirroring (negative determinant),
  // which inverts triangle winding. fix_left_handed=true corrects this
  // so visibility ray sampling sees outward-facing normals.
  its_transform(triangle_set, obj_transform, true);
  BOOST_LOG_TRIVIAL(debug)
      << "SeamPlacer: decimate: end";

  BOOST_LOG_TRIVIAL(debug)
      << "SeamPlacer: Compute visibility sample points: start";

  result.mesh_samples = sample_its_uniform_parallel(SeamPlacer::raycasting_visibility_samples_count,
                                                    triangle_set);
  result.mesh_samples_coordinate_functor = CoordinateFunctor(&result.mesh_samples.positions);
  result.mesh_samples_tree = KDTreeIndirect<3, float, CoordinateFunctor>(result.mesh_samples_coordinate_functor,
                                                                         result.mesh_samples.positions.size());

  // The following code determines search area for random visibility samples on the mesh when calculating visibility of each perimeter point
  // number of random samples in the given radius (area) is approximately poisson distribution
  // to compute ideal search radius (area), we use exponential distribution (complementary distr to poisson)
  // parameters of exponential distribution to compute area that will have with probability="probability" more than given number of samples="samples"
  float probability = 0.9f;
  float samples = 4;
  float density = SeamPlacer::raycasting_visibility_samples_count / result.mesh_samples.total_area;
  // exponential probability distrubtion function is : f(x) = P(X > x) = e^(l*x) where l is the rate parameter (computed as 1/u where u is mean value)
  // probability that sampled area A with S samples contains more than samples count:
  //  P(S > samples in A) = e^-(samples/(density*A));   express A:
  float search_area = samples / (-logf(probability) * density);
  float search_radius = sqrt(search_area / PI);
  result.mesh_samples_radius = search_radius;

  BOOST_LOG_TRIVIAL(debug)
      << "SeamPlacer: Compute visiblity sample points: end";
  throw_if_canceled();

  BOOST_LOG_TRIVIAL(debug)
      << "SeamPlacer: Mesh sample raidus: " << result.mesh_samples_radius;

  BOOST_LOG_TRIVIAL(debug)
      << "SeamPlacer: build AABB tree: start";
  auto raycasting_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(triangle_set.vertices,
                                                                                     triangle_set.indices);

  throw_if_canceled();
  BOOST_LOG_TRIVIAL(debug)
      << "SeamPlacer: build AABB tree: end";
  result.mesh_samples_visibility = raycast_visibility(raycasting_tree, triangle_set, result.mesh_samples,
                                                      negative_volumes_start_index, seam_position);
  throw_if_canceled();
#ifdef DEBUG_FILES
  result.debug_export(triangle_set);
#endif
}

// Appends the contour of painted triangles without T-joints (shared vertices): their edges used by one
// triangle only. Paint lies to the left of an edge in the triangle's counterclockwise order.
void append_painted_contour(PaintedContour &result, const indexed_triangle_set &painted) {
  const auto edge_key = [](int a, int b) { return (uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b)); };
  std::unordered_map<uint64_t, int> uses;
  for (const stl_triangle_vertex_indices &face : painted.indices)
    for (int i = 0; i < 3; ++i)
      ++uses[edge_key(face[i], face[(i + 1) % 3])];
  for (const stl_triangle_vertex_indices &face : painted.indices) {
    const Vec3d v[3] = {painted.vertices[face[0]].cast<double>(), painted.vertices[face[1]].cast<double>(),
                        painted.vertices[face[2]].cast<double>()};
    const Vec3d normal = (v[1] - v[0]).cross(v[2] - v[0]);
    for (int i = 0; i < 3; ++i) {
      const Vec3d inward = normal.cross(v[(i + 1) % 3] - v[i]);
      if (uses[edge_key(face[i], face[(i + 1) % 3])] == 1 && inward.squaredNorm() > 0.0) {
        result.edges.emplace_back(v[i], v[(i + 1) % 3]);
        result.inward.emplace_back(inward.normalized());
      }
    }
  }
}

void gather_enforcers_blockers(GlobalModelInfo &result, const PrintObject *po) {
  BOOST_LOG_TRIVIAL(debug)
      << "SeamPlacer: build AABB trees for raycasting enforcers/blockers: start";

  auto obj_transform = po->trafo_sliced();

  indexed_triangle_set painted_non_green; // Used only if the object has green paint.
  for (const ModelVolume *mv : po->model_object()->volumes) {
    // Collect painting only from model parts (what the gizmo edits) and negative volumes (the only way
    // to paint a hole's wall); painting left on modifiers and helpers after a type change is ignored.
    // TODO: painting on negative volumes still affects the seam, but the gizmo neither shows nor edits it;
    // making it editable also needs model_custom_seam_data_changed() to track it.
    if (!mv->is_model_part() && !mv->is_negative_volume())
      continue;
    const auto model_transformation = obj_transform * mv->get_matrix();
    if (mv->is_seam_painted()) {
      // One selector serves every paint state; each FacetsAnnotation query would rebuild it from the mesh.
      TriangleSelector selector(mv->mesh());
      selector.deserialize(mv->seam_facets.get_data(), false);
      indexed_triangle_set enforcers = selector.get_facets(EnforcerBlockerType::ENFORCER);
      // ORCA fix (not related to Precise Seam): fix winding for mirrored transforms
      its_transform(enforcers, model_transformation, true);
      its_merge(result.enforcers, enforcers);

      indexed_triangle_set painted = selector.get_facets_strict(EnforcerBlockerType::ENFORCER);
      its_transform(painted, model_transformation, true);
      append_painted_contour(result.enforcer_contour, painted);

      indexed_triangle_set blockers = selector.get_facets(EnforcerBlockerType::BLOCKER);
      // ORCA fix (not related to Precise Seam): fix winding for mirrored transforms
      its_transform(blockers, model_transformation, true);
      its_merge(result.blockers, blockers);
      painted = selector.get_facets_strict(EnforcerBlockerType::BLOCKER);
      its_transform(painted, model_transformation, true);
      append_painted_contour(result.blocker_contour, painted);

      indexed_triangle_set non_green = selector.get_facets(EnforcerBlockerType::NONE);
      its_transform(non_green, model_transformation, true);
      its_merge(painted_non_green, non_green);
    }
  }

  // Unpainted objects need no extra surface copy or tree; only green paint requires a coverage proof.
  if (!result.enforcers.empty()) {
    result.non_enforcers = std::move(painted_non_green);
    for (const ModelVolume *mv : po->model_object()->volumes) {
      if ((!mv->is_model_part() && !mv->is_negative_volume()) || mv->is_seam_painted())
        continue;
      indexed_triangle_set non_green = mv->mesh().its;
      its_transform(non_green, obj_transform * mv->get_matrix(), true);
      its_merge(result.non_enforcers, non_green);
    }
    its_merge(result.non_enforcers, result.blockers); // Red paint must also prevent full-green normalization.
    result.non_enforcers_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(result.non_enforcers.vertices,
                                                                                          result.non_enforcers.indices);
  }

  result.enforcers_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(result.enforcers.vertices,
                                                                                      result.enforcers.indices);
  result.blockers_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(result.blockers.vertices,
                                                                                     result.blockers.indices);
  result.enforcer_contour.tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(result.enforcer_contour.edges);
  result.blocker_contour.tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(result.blocker_contour.edges);

  BOOST_LOG_TRIVIAL(debug)
      << "SeamPlacer: build AABB trees for raycasting enforcers/blockers: end";
}

struct SeamComparator {
  SeamPosition setup;
  float angle_importance;
  explicit SeamComparator(SeamPosition setup) :
                                                setup(setup) {
    angle_importance =
        setup == spNearest ? SeamPlacer::angle_importance_nearest : SeamPlacer::angle_importance_aligned;
  }

  // Standard comparator, must respect the requirements of comparators (e.g. give same result on same inputs) for sorting usage
  // should return if a is better seamCandidate than b
  bool is_first_better(const SeamCandidate &a, const SeamCandidate &b, const Vec2f &preffered_location = Vec2f { 0.0f,
                                                                                                               0.0f }) const {
    if ((setup == SeamPosition::spAligned || setup == SeamPosition::spAlignedBack) && a.central_enforcer != b.central_enforcer) {
      return a.central_enforcer;
    }

    // Blockers/Enforcers discrimination, top priority
    if (a.type != b.type) {
      return a.type > b.type;
    }

    //avoid overhangs
    if (a.overhang > 0.0f || b.overhang > 0.0f) {
      return a.overhang < b.overhang;
    }

    // prefer hidden points (more than 0.5 mm inside)
    if (a.embedded_distance < -0.5f && b.embedded_distance > -0.5f) {
      return true;
    }
    if (b.embedded_distance < -0.5f && a.embedded_distance > -0.5f) {
      return false;
    }

    if (setup == SeamPosition::spRear && a.position.y() != b.position.y()) {
      return a.position.y() > b.position.y();
    }

    float distance_penalty_a = 0.0f;
    float distance_penalty_b = 0.0f;
    if (setup == spNearest) {
      distance_penalty_a = 1.0f - gauss((a.position.head<2>() - preffered_location).norm(), 0.0f, 1.0f, 0.005f);
      distance_penalty_b = 1.0f - gauss((b.position.head<2>() - preffered_location).norm(), 0.0f, 1.0f, 0.005f);
    }

    // the penalites are kept close to range [0-1.x] however, it should not be relied upon
    float penalty_a = a.overhang + a.visibility +
                      angle_importance * compute_angle_penalty(a.local_ccw_angle)
                      + distance_penalty_a;
    float penalty_b = b.overhang + b.visibility +
                      angle_importance * compute_angle_penalty(b.local_ccw_angle)
                      + distance_penalty_b;

    return penalty_a < penalty_b;
  }

  // Comparator used during alignment. If there is close potential aligned point, it is compared to the current
  // seam point of the perimeter, to find out if the aligned point is not much worse than the current seam
  // Also used by the random seam generator.
  bool is_first_not_much_worse(const SeamCandidate &a, const SeamCandidate &b) const {
    // Blockers/Enforcers discrimination, top priority
    if ((setup == SeamPosition::spAligned || setup == SeamPosition::spAlignedBack) && a.central_enforcer != b.central_enforcer) {
      // Prefer centers of enforcers.
      return a.central_enforcer;
    }

    if (a.type != b.type) {
      return a.type > b.type;
    }

    // Two Blocked points fall through like Neutral ones, so a perimeter blocked all round still gets a proper seam.
    if (a.type == EnforcedBlockedSeamPoint::Enforced) {
      return true;
    }

    //avoid overhangs
    if ((a.overhang > 0.0f || b.overhang > 0.0f)
        && abs(a.overhang - b.overhang) > (0.1f * a.perimeter.flow_width)) {
      return a.overhang < b.overhang;
    }

    // prefer hidden points (more than 0.5 mm inside)
    if (a.embedded_distance < -0.5f && b.embedded_distance > -0.5f) {
      return true;
    }
    if (b.embedded_distance < -0.5f && a.embedded_distance > -0.5f) {
      return false;
    }

    if (setup == SeamPosition::spRandom) {
      return true;
    }

    if (setup == SeamPosition::spRear) {
      return a.position.y() + SeamPlacer::seam_align_score_tolerance * 5.0f > b.position.y();
    }

    float penalty_a = a.overhang + a.visibility
                      + angle_importance * compute_angle_penalty(a.local_ccw_angle);
    float penalty_b = b.overhang + b.visibility +
                      angle_importance * compute_angle_penalty(b.local_ccw_angle);

    return penalty_a <= penalty_b || penalty_a - penalty_b < SeamPlacer::seam_align_score_tolerance;
  }

  bool are_similar(const SeamCandidate &a, const SeamCandidate &b) const {
    return is_first_not_much_worse(a, b) && is_first_not_much_worse(b, a);
  }
};

#ifdef DEBUG_FILES
void debug_export_points(const std::vector<PrintObjectSeamData::LayerSeams> &layers,
                         const BoundingBox &bounding_box, const SeamComparator &comparator) {
  for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
    std::string angles_file_name = debug_out_path(
        ("angles_" + std::to_string(layer_idx) + ".svg").c_str());
    SVG angles_svg { angles_file_name, bounding_box };
    float min_vis = 0;
    float max_vis = min_vis;

    float min_weight = std::numeric_limits<float>::min();
    float max_weight = min_weight;

    for (const SeamCandidate &point : layers[layer_idx].points) {
      Vec3i32 color = value_to_rgbi(-PI, PI, point.local_ccw_angle);
      std::string fill = "rgb(" + std::to_string(color.x()) + "," + std::to_string(color.y()) + ","
                         + std::to_string(color.z()) + ")";
      angles_svg.draw(scaled(Vec2f(point.position.head<2>())), fill);
      min_vis = std::min(min_vis, point.visibility);
      max_vis = std::max(max_vis, point.visibility);

      min_weight = std::min(min_weight, -compute_angle_penalty(point.local_ccw_angle));
      max_weight = std::max(max_weight, -compute_angle_penalty(point.local_ccw_angle));

    }

    std::string visiblity_file_name = debug_out_path(
        ("visibility_" + std::to_string(layer_idx) + ".svg").c_str());
    SVG visibility_svg { visiblity_file_name, bounding_box };
    std::string weights_file_name = debug_out_path(
        ("weight_" + std::to_string(layer_idx) + ".svg").c_str());
    SVG weight_svg { weights_file_name, bounding_box };
    std::string overhangs_file_name = debug_out_path(
        ("overhang_" + std::to_string(layer_idx) + ".svg").c_str());
    SVG overhangs_svg { overhangs_file_name, bounding_box };

    for (const SeamCandidate &point : layers[layer_idx].points) {
      Vec3i32 color = value_to_rgbi(min_vis, max_vis, point.visibility);
      std::string visibility_fill = "rgb(" + std::to_string(color.x()) + "," + std::to_string(color.y()) + ","
                                    + std::to_string(color.z()) + ")";
      visibility_svg.draw(scaled(Vec2f(point.position.head<2>())), visibility_fill);

      Vec3i32 weight_color = value_to_rgbi(min_weight, max_weight,
                                         -compute_angle_penalty(point.local_ccw_angle));
      std::string weight_fill = "rgb(" + std::to_string(weight_color.x()) + "," + std::to_string(weight_color.y())
                                + ","
                                + std::to_string(weight_color.z()) + ")";
      weight_svg.draw(scaled(Vec2f(point.position.head<2>())), weight_fill);

      Vec3i32 overhang_color = value_to_rgbi(-0.5, 0.5, std::clamp(point.overhang, -0.5f, 0.5f));
      std::string overhang_fill = "rgb(" + std::to_string(overhang_color.x()) + ","
                                  + std::to_string(overhang_color.y())
                                  + ","
                                  + std::to_string(overhang_color.z()) + ")";
      overhangs_svg.draw(scaled(Vec2f(point.position.head<2>())), overhang_fill);
    }
  }
}
#endif

// Pick best seam point based on the given comparator
void pick_seam_point(std::vector<SeamCandidate> &perimeter_points, size_t start_index,
                     const SeamComparator &comparator) {
  size_t end_index = perimeter_points[start_index].perimeter.end_index;

  size_t seam_index = start_index;
  for (size_t index = start_index; index < end_index; ++index) {
    if (comparator.is_first_better(perimeter_points[index], perimeter_points[seam_index])) {
      seam_index = index;
    }
  }
  perimeter_points[start_index].perimeter.seam_index = seam_index;
}

size_t pick_nearest_seam_point_index(const std::vector<SeamCandidate> &perimeter_points, size_t start_index,
                                     const Vec2f &preffered_location) {
  size_t end_index = perimeter_points[start_index].perimeter.end_index;
  SeamComparator comparator { spNearest };

  size_t seam_index = start_index;
  for (size_t index = start_index; index < end_index; ++index) {
    if (comparator.is_first_better(perimeter_points[index], perimeter_points[seam_index], preffered_location)) {
      seam_index = index;
    }
  }
  return seam_index;
}

// picks random seam point uniformly, respecting enforcers blockers and overhang avoidance.
void pick_random_seam_point(const std::vector<SeamCandidate> &perimeter_points, size_t start_index) {
  SeamComparator comparator { spRandom };

  // algorithm keeps a list of viable points and their lengths. If it finds a point
  // that is much better than the viable_example_index (e.g. better type, no overhang; see is_first_not_much_worse)
  // then it throws away stored lists and starts from start
  // in the end, the list should contain points with same type (Enforced > Neutral > Blocked) and also only those which are not
  // big overhang.
  size_t viable_example_index = start_index;
  size_t end_index = perimeter_points[start_index].perimeter.end_index;
  struct Viable {
    // Candidate seam point index.
    size_t index;
    float edge_length;
    Vec3f edge;
  };
  std::vector<Viable> viables;

  const auto edge_from = [&](size_t index) -> Vec3f {
    const size_t next = index == end_index - 1 ? start_index : index + 1;
    // The exact green endpoint is Enforced, but its outgoing edge leaves the permitted painted run.
    if (perimeter_points[index].perimeter.exact_enforced_ends &&
        perimeter_points[index].type == EnforcedBlockedSeamPoint::Enforced &&
        perimeter_points[next].type != EnforcedBlockedSeamPoint::Enforced)
      return Vec3f::Zero();
    // Random samples the outgoing edge, not just the candidate; do not sample toward a closed red boundary.
    if (perimeter_points[index].perimeter.exact_blocked_ends &&
        perimeter_points[index].type != EnforcedBlockedSeamPoint::Blocked &&
        perimeter_points[next].type == EnforcedBlockedSeamPoint::Blocked)
      return Vec3f::Zero();
    return perimeter_points[next].position - perimeter_points[index].position;
  };

  const Vec3f pseudornd_seed = perimeter_points[viable_example_index].position;
  float rand = std::abs(sin(pseudornd_seed.dot(Vec3f(12.9898f,78.233f, 133.3333f))) * 43758.5453f);
  rand = rand - (int) rand;

  for (size_t index = start_index; index < end_index; ++index) {
    if (comparator.are_similar(perimeter_points[index], perimeter_points[viable_example_index])) {
      // index ok, push info into viables
      Vec3f edge_to_next = edge_from(index);
      float dist_to_next = edge_to_next.norm();
      viables.push_back( { index, dist_to_next, edge_to_next });
    } else if (comparator.is_first_not_much_worse(perimeter_points[viable_example_index],
                                                  perimeter_points[index])) {
      // index is worse then viable_example_index, skip this point
    } else {
      // index is better than viable example index, update example, clear gathered info, start again
      // clear up all gathered info, start from scratch, update example index
      viable_example_index = index;
      viables.clear();

      Vec3f edge_to_next = edge_from(index);
      float dist_to_next = edge_to_next.norm();
      viables.push_back( { index, dist_to_next, edge_to_next });
    }
  }

  // now pick random point from the stored options
  float len_sum = std::accumulate(viables.begin(), viables.end(), 0.0f, [](const float acc, const Viable &v) {
    return acc + v.edge_length;
  });
  float picked_len = len_sum * rand;

  size_t point_idx = 0;
  // Float accumulation can leave a positive residual after the final edge, including a zero-length edge.
  while (point_idx + 1 < viables.size() && picked_len - viables[point_idx].edge_length > 0) {
    picked_len = picked_len - viables[point_idx].edge_length;
    point_idx++;
  }
  picked_len = std::clamp(picked_len, 0.0f, viables[point_idx].edge_length);

  Perimeter &perimeter = perimeter_points[start_index].perimeter;
  perimeter.seam_index = viables[point_idx].index;
  perimeter.final_seam_position = perimeter_points[perimeter.seam_index].position
                                  + (viables[point_idx].edge_length > 0.f ?
                                     Vec3f(viables[point_idx].edge.normalized() * picked_len) : Vec3f::Zero());
  perimeter.finalized = true;
}

// Seam alignment on the loop, see docs/HLSD/seam-alignment.md.
// A painted seam keeps its pull (t >= 0.4) toward the discrete candidate only near a measured corner;
// on a smooth contour that pull makes the seam jump between candidates from layer to layer.
constexpr float enforced_corner_angle_min = 15.0f * float(PI) / 180.0f;
constexpr float enforced_corner_angle_max = 30.0f * float(PI) / 180.0f;
// The corner influence fades out along the perimeter between R0 and this multiple of R0.
constexpr float enforced_corner_fade_factor = 2.0f;

float smoothstep01(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

// Strength in [0, 1] of the measured corners around the candidate: a corner of at least
// enforced_corner_angle_max within `protect` along the perimeter counts fully, then fades out by `fade_end`.
float enforced_corner_influence(const std::vector<SeamCandidate> &points, size_t index, float protect, float fade_end) {
  const Perimeter &perimeter = points[index].perimeter;
  const size_t start = perimeter.start_index;
  const size_t count = perimeter.end_index - start;
  const auto contribution = [&](size_t i, float distance) {
    const float corner = smoothstep01((std::abs(points[i].local_ccw_angle) - enforced_corner_angle_min) /
                                      (enforced_corner_angle_max - enforced_corner_angle_min));
    const float weight = distance <= protect ? 1.0f : 1.0f - smoothstep01((distance - protect) / (fade_end - protect));
    return corner * weight;
  };
  float influence = contribution(index, 0.0f);
  for (const bool forward : { false, true }) {
    size_t i = index;
    float distance = 0.0f;
    // At most count - 1 steps per direction, so short loops and zero-length edges terminate.
    for (size_t step = 1; step < count && influence < 1.0f; ++step) {
      const size_t j = start + (forward ? next_idx_modulo(i - start, count) : prev_idx_modulo(i - start, count));
      distance += (points[j].position - points[i].position).head<2>().norm();
      if (distance >= fade_end)
        break;
      influence = std::max(influence, contribution(j, distance));
      i = j;
    }
  }
  return influence;
}

// Unit normal of the edge from a to b, zero for a zero-length edge.
Vec2d edge_normal(const Vec2d &a, const Vec2d &b) {
  const Vec2d d = b - a;
  const double length = d.norm();
  return length > 0.0 ? Vec2d(-d.y() / length, d.x() / length) : Vec2d::Zero();
}

// Seam position on the loop for a `target`: the closest loop point to it whose normal passes through it,
// or, if that lies outside the arc allowed around the candidate at `index`, the arc end closer to that point.
Vec3f place_on_loop(const std::vector<SeamCandidate> &points, size_t index, const Vec3f &target) {
  const Perimeter &perimeter = points[index].perimeter;
  const size_t start = perimeter.start_index;
  const size_t count = perimeter.end_index - start;
  if (count < 2)
    return points[index].position;
  const auto next = [&](size_t i) { return start + next_idx_modulo(i - start, count); };
  const auto prev = [&](size_t i) { return start + prev_idx_modulo(i - start, count); };
  const auto xy = [&](size_t i) { return Vec2d(points[i].position.head<2>().cast<double>()); };
  // Nearest neighbor at a different position, so that duplicated points share one normal.
  const auto distinct = [&](size_t i, bool forward) {
    size_t j = i;
    for (size_t step = 1; step < count; ++step) {
      j = forward ? next(j) : prev(j);
      if (xy(j) != xy(i))
        return j;
    }
    return i;
  };
  const auto vertex_normal = [&](size_t i) {
    const Vec2d sum = edge_normal(xy(distinct(i, false)), xy(i)) + edge_normal(xy(i), xy(distinct(i, true)));
    return sum.squaredNorm() > 0.0 ? Vec2d(sum.normalized()) : Vec2d::Zero();
  };
  const auto cross = [](const Vec2d &a, const Vec2d &b) { return a.x() * b.y() - a.y() * b.x(); };

  // Allowed arc on one side: the Enforced run for an Enforced candidate, plus the oversampling distance unless
  // the run ends exactly at the paint; up to a Blocked candidate otherwise. Returns its length and end point;
  // infinite length if unlimited.
  const bool enforced = points[index].type == EnforcedBlockedSeamPoint::Enforced;
  const bool blocked = points[index].type == EnforcedBlockedSeamPoint::Blocked;
  const auto allowed_arc = [&](bool forward) {
    double arc = 0.0;
    double extension = enforced && !perimeter.exact_enforced_ends ? SeamPlacer::enforcer_oversampling_distance : 0.0;
    bool in_run = true;
    size_t i = index;
    // All count edges, the closing one back to the candidate included, before the arc is unlimited.
    for (size_t visited = 1; visited <= count; ++visited) {
      const size_t j = forward ? next(i) : prev(i);
      const EnforcedBlockedSeamPoint type = points[j].type;
      if (in_run && (enforced ? type != EnforcedBlockedSeamPoint::Enforced :
                                !blocked && type == EnforcedBlockedSeamPoint::Blocked))
        in_run = false;
      if (!in_run && (!enforced || type == EnforcedBlockedSeamPoint::Blocked))
        return std::make_pair(arc, xy(i)); // Only an Enforced run is extended, and never into a Blocked candidate.
      const double length = (xy(j) - xy(i)).norm();
      if (!in_run) {
        const double reach = std::min(length, extension);
        extension -= reach;
        if (reach < length)
          return std::make_pair(arc + reach, Vec2d(xy(i) + (reach / length) * (xy(j) - xy(i))));
      }
      arc += length;
      i = j;
    }
    return std::make_pair(std::numeric_limits<double>::infinity(), xy(index));
  };

  // Closest solution over the whole loop, walking outward on the shorter side first so that solutions near
  // the candidate prune the edges farther from the target than them.
  const Vec2d p = target.head<2>().cast<double>();
  std::optional<Vec2d> best;
  double best_distance = std::numeric_limits<double>::infinity();
  bool best_forward = true;
  double best_arc = 0.0;
  double arc[2] = { 0.0, 0.0 }; // Walked arc behind and ahead of the candidate.
  size_t cursor[2] = { index, index };
  for (size_t visited = 0; visited < count; ++visited) {
    const bool forward = arc[1] <= arc[0];
    const size_t i = cursor[forward];
    const size_t j = forward ? next(i) : prev(i);
    // The edge in index order; P(u) = xy(a) + u * d.
    const size_t a = forward ? i : j;
    const size_t b = forward ? j : i;
    const Vec2d d = xy(b) - xy(a);
    const double length = d.norm();
    const Vec2d g = p - xy(a);
    const double u_closest = length > 0.0 ? std::clamp(g.dot(d) / (length * length), 0.0, 1.0) : 0.0;
    if (length > 0.0 && (g - u_closest * d).squaredNorm() < best_distance) {
      // Solve cross(p - P(u), N(u)) = 0 with the normal N blended linearly between the vertex normals.
      Vec2d na = vertex_normal(a);
      Vec2d nb = vertex_normal(b);
      if (na.squaredNorm() == 0.0 || nb.squaredNorm() == 0.0)
        na = nb = edge_normal(xy(a), xy(b));
      const Vec2d m = nb - na;
      const double c0 = cross(g, na);
      const double c1 = cross(g, m) - cross(d, na);
      const double c2 = -cross(d, m);
      const auto consider = [&](double u) {
        if (u < 0.0 || u > 1.0)
          return;
        const double distance = (g - u * d).squaredNorm();
        if (distance < best_distance) {
          best = Vec2d(xy(a) + u * d);
          best_distance = distance;
          best_forward = forward;
          best_arc = arc[forward] + (forward ? u : 1.0 - u) * length;
        }
      };
      // Absolute threshold on the coefficients (mm), only to tell the degenerate equations apart.
      constexpr double eps = 1e-12;
      if (std::abs(c2) < eps && std::abs(c1) < eps) {
        if (std::abs(c0) < eps)
          consider(u_closest); // Every point is a solution.
      } else if (std::abs(c2) < eps) {
        consider(-c0 / c1);
      } else {
        const double discriminant = c1 * c1 - 4.0 * c2 * c0;
        if (discriminant >= 0.0) {
          const double root = std::sqrt(discriminant);
          consider((-c1 - root) / (2.0 * c2));
          consider((-c1 + root) / (2.0 * c2));
        }
      }
    }
    arc[forward] += length;
    cursor[forward] = j;
  }
  if (!best)
    return points[index].position;

  // A solution outside the allowed arc gives way to the arc end closer to it, so that a solution leaving
  // the arc stays at the end it left through.
  Vec2d result = *best;
  const double loop_length = arc[0] + arc[1];
  const double ahead = best_forward ? best_arc : loop_length - best_arc;
  const auto [ahead_limit, ahead_end] = allowed_arc(true);
  const auto [behind_limit, behind_end] = allowed_arc(false);
  if (ahead > ahead_limit && loop_length - ahead > behind_limit)
    result = (ahead_end - *best).squaredNorm() <= (behind_end - *best).squaredNorm() ? ahead_end : behind_end;
  return to_3d(Vec2f(result.cast<float>()), points[index].position.z());
}

} // namespace SeamPlacerImpl

// Parallel process and extract each perimeter polygon of the given print object.
// Gather SeamCandidates of each layer into vector and build KDtree over them
// Store results in the SeamPlacer variables m_seam_per_object
void SeamPlacer::gather_seam_candidates(const PrintObject *po, const SeamPlacerImpl::GlobalModelInfo &global_model_info,
                                        PreciseSeam::PreciseSeamWarnings* warnings) {
  using namespace SeamPlacerImpl;
  PrintObjectSeamData &seam_data = m_seam_per_object.emplace(po, PrintObjectSeamData { }).first->second;
  seam_data.layers.resize(po->layer_count());

  tbb::parallel_for(tbb::blocked_range<size_t>(0, po->layers().size()),
                    [po, &global_model_info, &seam_data, warnings]
                    (tbb::blocked_range<size_t> r) {
                      for (size_t layer_idx = r.begin(); layer_idx < r.end(); ++layer_idx) {
                        PrintObjectSeamData::LayerSeams &layer_seams = seam_data.layers[layer_idx];
                        const Layer *layer = po->get_layer(layer_idx);
                        auto unscaled_z = layer->slice_z;
                        std::vector<const LayerRegion*> regions;
                        std::vector<std::optional<bool>> boundary_types;
                        //NOTE corresponding region ptr may be null, if the layer has zero perimeters
                        const bool has_precise_seam_modifiers = !global_model_info.precise_seam_strong_volumes.empty() ||
                                                               !global_model_info.precise_seam_weak_volumes.empty();
                        Polygons polygons = extract_perimeter_polygons(layer, regions, boundary_types, has_precise_seam_modifiers);
                        for (size_t poly_index = 0; poly_index < polygons.size(); ++poly_index) {
                          process_perimeter_polygon(polygons[poly_index], unscaled_z,
                                                    regions[poly_index], global_model_info, layer_seams,
                                                    warnings, boundary_types[poly_index]);
                        }
                        auto functor = SeamCandidateCoordinateFunctor { layer_seams.points };
                        seam_data.layers[layer_idx].points_tree =
                            std::make_unique<PrintObjectSeamData::SeamCandidatesTree>(functor,
                                                                                      layer_seams.points.size());
                      }
                    }
  );
}

void SeamPlacer::calculate_candidates_visibility(const PrintObject *po,
                                                 const SeamPlacerImpl::GlobalModelInfo &global_model_info) {

  std::vector<PrintObjectSeamData::LayerSeams> &layers = m_seam_per_object[po].layers;
  tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()),
                    [&layers, &global_model_info](tbb::blocked_range<size_t> r) {
                      for (size_t layer_idx = r.begin(); layer_idx < r.end(); ++layer_idx) {
                        for (auto &perimeter_point : layers[layer_idx].points) {
                          perimeter_point.visibility = global_model_info.calculate_point_visibility(
                              perimeter_point.position);
                        }
                      }
                    });
}

void SeamPlacer::calculate_overhangs_and_layer_embedding(const PrintObject *po) {
  using namespace SeamPlacerImpl;
  using PerimeterDistancer = AABBTreeLines::LinesDistancer<Linef>;

  std::vector<PrintObjectSeamData::LayerSeams> &layers = m_seam_per_object[po].layers;
  tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()),
                    [po, &layers](tbb::blocked_range<size_t> r) {
                      std::unique_ptr<PerimeterDistancer> prev_layer_distancer;
                      if (r.begin() > 0) { // previous layer exists
                        prev_layer_distancer = std::make_unique<PerimeterDistancer>(to_unscaled_linesf(po->layers()[r.begin() - 1]->lslices));
                      }

                      for (size_t layer_idx = r.begin(); layer_idx < r.end(); ++layer_idx) {
                        size_t regions_with_perimeter = 0;
                        for (const LayerRegion *region : po->layers()[layer_idx]->regions()) {
                          if (region->perimeters.entities.size() > 0) {
                            regions_with_perimeter++;
                          }
                        };
                        bool should_compute_layer_embedding = regions_with_perimeter > 1;
                        std::unique_ptr<PerimeterDistancer> current_layer_distancer        = std::make_unique<PerimeterDistancer>(
                            to_unscaled_linesf(po->layers()[layer_idx]->lslices));

                        auto& layer_seams = layers[layer_idx];
                        for (SeamCandidate &perimeter_point : layer_seams.points) {
                          Vec2f point = Vec2f { perimeter_point.position.head<2>() };
                          if (prev_layer_distancer.get() != nullptr) {
                            const auto _dist = prev_layer_distancer->distance_from_lines<true>(point.cast<double>());
                            perimeter_point.overhang = _dist
                                                       + 0.65f * perimeter_point.perimeter.flow_width
                                                       - tan(SeamPlacer::overhang_angle_threshold)
                                                             * po->layers()[layer_idx]->height;
                            perimeter_point.overhang =
                                perimeter_point.overhang < 0.0f ? 0.0f : perimeter_point.overhang;
                            perimeter_point.unsupported_dist = _dist + 0.4f * perimeter_point.perimeter.flow_width;
                          }

                          if (should_compute_layer_embedding) { // search for embedded perimeter points (points hidden inside the print ,e.g. multimaterial join, best position for seam)
                            perimeter_point.embedded_distance = current_layer_distancer->distance_from_lines<true>(point.cast<double>())
                                                                + 0.65f * perimeter_point.perimeter.flow_width;
                          }
                        }

                        prev_layer_distancer.swap(current_layer_distancer);
                      }
                    }
  );
}

// Estimates, if there is good seam point in the layer_idx which is close to last_point_pos
// uses comparator.is_first_not_much_worse method to compare current seam with the closest point
// (if current seam is too far away )
// If the current chosen stream is close enough, it is stored in seam_string. returns true and updates last_point_pos
// If the closest point is good enough to replace current chosen seam, it is stored in potential_string_seams, returns true and updates last_point_pos
// Otherwise does nothing, returns false
// Used by align_seam_points().
std::optional<std::pair<size_t, size_t>> SeamPlacer::find_next_seam_in_layer(
    const std::vector<PrintObjectSeamData::LayerSeams> &layers,
    const Vec3f &projected_position,
    const size_t layer_idx, const float max_distance,
    const SeamPlacerImpl::SeamComparator &comparator,
    const SeamContour::Resolver *contours,
    std::pair<size_t, size_t> previous) const {
  using namespace SeamPlacerImpl;
  // Contours of the geometric continuation. Empty when there is no geometric match, which preserves the legacy radius
  // search; occupancy never expands a known set.
  std::vector<size_t> continuation;
  if (contours) {
    const Perimeter &source = layers[previous.first].points[previous.second].perimeter;
    const int direction = layer_idx > previous.first ? 1 : -1;
    continuation = contours->resolve(previous.first, source.contour_index,
                                     previous.second - source.start_index, direction, max_distance).contours;
  }

  // Find the best nearby point and the nearest one. A layer of a fine relief has tens of thousands of candidates within
  // the radius, so they are looked at as the search finds them rather than collected into a vector first.
  constexpr size_t none = std::numeric_limits<size_t>::max();
  size_t best_nearby_point_index = none;
  size_t nearest_point_index = none;
  visit_nearby_points(*layers[layer_idx].points_tree, projected_position, max_distance,
                      [&layers, &comparator, &projected_position, &continuation, layer_idx, &best_nearby_point_index,
                       &nearest_point_index](size_t nearby_point_index) {
    // Points off the continuation are skipped before the first point is taken, as they were removed before the scan.
    if (!continuation.empty() &&
        std::find(continuation.begin(), continuation.end(),
                  layers[layer_idx].points[nearby_point_index].perimeter.contour_index) == continuation.end())
      return;
    if (best_nearby_point_index == none) {
      // The first point found starts both, as the first of the collected ones did.
      best_nearby_point_index = nearest_point_index = nearby_point_index;
    }
    const SeamCandidate &point = layers[layer_idx].points[nearby_point_index];
    if (point.perimeter.finalized) {
      return; // skip over finalized perimeters, try to find some that is not finalized
    }
    if (comparator.is_first_better(point, layers[layer_idx].points[best_nearby_point_index],
                                   projected_position.head<2>())
        || layers[layer_idx].points[best_nearby_point_index].perimeter.finalized) {
      best_nearby_point_index = nearby_point_index;
    }
    if ((point.position - projected_position).squaredNorm()
            < (layers[layer_idx].points[nearest_point_index].position - projected_position).squaredNorm()
        || layers[layer_idx].points[nearest_point_index].perimeter.finalized) {
      nearest_point_index = nearby_point_index;
    }
  });

  if (best_nearby_point_index == none) {
    return {};
  }

  const SeamCandidate &best_nearby_point = layers[layer_idx].points[best_nearby_point_index];
  const SeamCandidate &nearest_point = layers[layer_idx].points[nearest_point_index];

  if (nearest_point.perimeter.finalized) {
    //all points are from already finalized perimeter, skip
    return {};
  }

  //from the nearest_point, deduce index of seam in the next layer
  const SeamCandidate &next_layer_seam = layers[layer_idx].points[nearest_point.perimeter.seam_index];

  // First try to pick central enforcer if any present
  if (next_layer_seam.central_enforcer
      && (next_layer_seam.position - projected_position).squaredNorm()
             < sqr(3 * max_distance)) {
    return {std::pair<size_t, size_t> {layer_idx, nearest_point.perimeter.seam_index}};
  }

  // First try to align the nearest, then try the best nearby
  if (comparator.is_first_not_much_worse(nearest_point, next_layer_seam)) {
    return {std::pair<size_t, size_t> {layer_idx, nearest_point_index}};
  }
  // If nearest point is not good enough, try it with the best nearby point.
  if (comparator.is_first_not_much_worse(best_nearby_point, next_layer_seam)) {
    return {std::pair<size_t, size_t> {layer_idx, best_nearby_point_index}};
  }

  return {};
}

std::vector<std::pair<size_t, size_t>> SeamPlacer::find_seam_string(const PrintObject *po,
                                                                    std::pair<size_t, size_t> start_seam, const SeamPlacerImpl::SeamComparator &comparator,
                                                                    const SeamContour::Resolver *contours) const {
  const std::vector<PrintObjectSeamData::LayerSeams> &layers = m_seam_per_object.find(po)->second.layers;
  int layer_idx = start_seam.first;

  //initialize searching for seam string - cluster of nearby seams on previous and next layers
  int next_layer = layer_idx + 1;
  int step = 1;
  std::pair<size_t, size_t> prev_point_index = start_seam;
  std::vector<std::pair<size_t, size_t>> seam_string { start_seam };

  auto reverse_lookup_direction = [&]() {
    step = -1;
    prev_point_index = start_seam;
    next_layer = layer_idx - 1;
  };

  while (next_layer >= 0) {
    if (next_layer >= int(layers.size())) {
      reverse_lookup_direction();
      if (next_layer < 0) {
        break;
      }
    }
    float max_distance = SeamPlacer::seam_align_tolerable_dist_factor *
                         layers[start_seam.first].points[start_seam.second].perimeter.flow_width;
    Vec3f prev_position = layers[prev_point_index.first].points[prev_point_index.second].position;
    Vec3f projected_position = prev_position;
    projected_position.z() = float(po->get_layer(next_layer)->slice_z);

    std::optional<std::pair<size_t, size_t>> maybe_next_seam = find_next_seam_in_layer(layers, projected_position,
                                                                                       next_layer,
                                                                                       max_distance, comparator, contours, prev_point_index);

    if (maybe_next_seam.has_value()) {
      // For old macOS (pre 10.14), std::optional does not have .value() method, so the code is using operator*() instead.
      seam_string.push_back(maybe_next_seam.operator*());
      prev_point_index = seam_string.back();
      //String added, prev_point_index updated
    } else {
      if (step == 1) {
        // Continue right below the start seam; stepping now would skip that layer.
        reverse_lookup_direction();
        continue;
      } else {
        break;
      }
    }
    next_layer += step;
  }
  return seam_string;
}

// clusters already chosen seam points into strings across multiple layers, and then
// aligns the strings via polynomial fit
// Does not change the positions of the SeamCandidates themselves, instead stores
// the new aligned position into the shared Perimeter structure of each perimeter
// This position lies on the perimeter polygon; placement on the printed loop may still move it.
void SeamPlacer::align_seam_points(const PrintObject *po, const SeamPlacerImpl::SeamComparator &comparator,
                                  const std::function<void()> &throw_if_canceled) {
  using namespace SeamPlacerImpl;

  // Prepares Debug files for writing.
#ifdef DEBUG_FILES
  Slic3r::CNumericLocalesSetter locales_setter;
  auto clusters_f = debug_out_path("seam_clusters.obj");
  FILE *clusters = boost::nowide::fopen(clusters_f.c_str(), "w");
  if (clusters == nullptr) {
    BOOST_LOG_TRIVIAL(error)
        << "stl_write_obj: Couldn't open " << clusters_f << " for writing";
    return;
  }
  auto aligned_f = debug_out_path("aligned_clusters.obj");
  FILE *aligns = boost::nowide::fopen(aligned_f.c_str(), "w");
  if (aligns == nullptr) {
    BOOST_LOG_TRIVIAL(error)
        << "stl_write_obj: Couldn't open " << clusters_f << " for writing";
    return;
  }
#endif

  //gather vector of all seams on the print_object - pair of layer_index and seam__index within that layer
  const std::vector<PrintObjectSeamData::LayerSeams> &layers = m_seam_per_object[po].layers;
  // Share the largest nozzle diameter between contour bounds and corner protection.
  double max_nozzle_diameter = 0.0;
  for (double diameter : po->print()->config().nozzle_diameter.values)
    max_nozzle_diameter = std::max(max_nozzle_diameter, diameter);
  // Reuse immutable contour geometry across all alternative string starts.
  std::unique_ptr<SeamContour::Resolver> contour_resolver;
  {
    SeamContour::Layers geometry(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
      throw_if_canceled(); // Geometry copies on dense multi-island objects must remain interruptible.
      for (const Perimeter &perimeter : layers[i].perimeters) {
        SeamContour::Contour contour;
        contour.hole = perimeter.is_hole;
        contour.reliable = perimeter.contour_reliable;
        contour.width = perimeter.flow_width;
        contour.nozzle = max_nozzle_diameter;
        for (size_t j = perimeter.start_index; j < perimeter.end_index; ++j)
          contour.points.push_back(layers[i].points[j].position.head<2>().cast<double>());
        geometry[i].push_back(std::move(contour));
      }
    }
    contour_resolver = std::make_unique<SeamContour::Resolver>(std::move(geometry), SeamContour::Options{}, throw_if_canceled);
  }
  std::vector<std::pair<size_t, size_t>> seams;
  for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
    const std::vector<SeamCandidate> &layer_perimeter_points = layers[layer_idx].points;
    size_t current_point_index = 0;
    while (current_point_index < layer_perimeter_points.size()) {
      seams.emplace_back(layer_idx, layer_perimeter_points[current_point_index].perimeter.seam_index);
      current_point_index = layer_perimeter_points[current_point_index].perimeter.end_index;
    }
  }

  //sort them before alignment. Alignment is sensitive to initializaion, this gives it better chance to choose something nice
  std::stable_sort(seams.begin(), seams.end(),
                   [&comparator, &layers](const std::pair<size_t, size_t> &left,
                                          const std::pair<size_t, size_t> &right) {
                     return comparator.is_first_better(layers[left.first].points[left.second],
                                                       layers[right.first].points[right.second]);
                   }
  );

  //align the seam points - start with the best, and check if they are aligned, if yes, skip, else start alignment
  // Keeping the vectors outside, so with a bit of luck they will not get reallocated after couple of for loop iterations.
  std::vector<std::pair<size_t, size_t>> seam_string;
  std::vector<std::pair<size_t, size_t>> alternative_seam_string;
  std::vector<Vec2f> observations;
  std::vector<float> observation_points;
  std::vector<float> weights;

  int global_index = 0;
  while (global_index < int(seams.size())) {
    throw_if_canceled();
    size_t layer_idx = seams[global_index].first;
    size_t seam_index = seams[global_index].second;
    global_index++;
    const std::vector<SeamCandidate> &layer_perimeter_points = layers[layer_idx].points;
    if (layer_perimeter_points[seam_index].perimeter.finalized) {
      // This perimeter is already aligned, skip seam
      continue;
    } else {
      seam_string = this->find_seam_string(po, { layer_idx, seam_index }, comparator, contour_resolver.get());
      size_t step_size = 1 + seam_string.size() / 20;
      for (size_t alternative_start = 0; alternative_start < seam_string.size(); alternative_start += step_size) {
        throw_if_canceled();
        size_t start_layer_idx = seam_string[alternative_start].first;
        size_t seam_idx =
            layers[start_layer_idx].points[seam_string[alternative_start].second].perimeter.seam_index;
        alternative_seam_string = this->find_seam_string(po,
                                                         std::pair<size_t, size_t>(start_layer_idx, seam_idx), comparator,
                                                         contour_resolver.get());
        if (alternative_seam_string.size() > seam_string.size()) {
          seam_string = std::move(alternative_seam_string);
        }
      }
      if (seam_string.size() < seam_align_minimum_string_seams) {
        //string NOT long enough to be worth aligning, skip
        continue;
      }

      // String is long enough, all string seams and potential string seams gathered, now do the alignment
      //sort by layer index
      std::sort(seam_string.begin(), seam_string.end(),
                [](const std::pair<size_t, size_t> &left, const std::pair<size_t, size_t> &right) {
                  return left.first < right.first;
                });

      //repeat the alignment for the current seam, since it could be skipped due to alternative path being aligned.
      global_index--;

      // gather all positions of seams and their weights
      observations.resize(seam_string.size());
      observation_points.resize(seam_string.size());
      weights.resize(seam_string.size());

      auto angle_3d = [](const Vec3f& a, const Vec3f& b){
        return std::abs(acosf(a.normalized().dot(b.normalized())));
      };

      auto angle_weight = [](float angle){
        return 1.0f / (0.1f + compute_angle_penalty(angle));
      };

      //gather points positions and weights
      float total_length = 0.0f;
      Vec3f last_point_pos = layers[seam_string[0].first].points[seam_string[0].second].position;
      for (size_t index = 0; index < seam_string.size(); ++index) {
        const SeamCandidate &current = layers[seam_string[index].first].points[seam_string[index].second];
        float layer_angle = 0.0f;
        if (index > 0 && index < seam_string.size() - 1) {
          layer_angle = angle_3d(
              current.position
                  - layers[seam_string[index - 1].first].points[seam_string[index - 1].second].position,
              layers[seam_string[index + 1].first].points[seam_string[index + 1].second].position
                  - current.position
          );
        }
        observations[index] = current.position.head<2>();
        observation_points[index] = current.position.z();
        weights[index] = angle_weight(current.local_ccw_angle);
        float curling_influence = layer_angle > 2.0 * std::abs(current.local_ccw_angle) ? -0.8f : 1.0f;
        if (current.type == EnforcedBlockedSeamPoint::Enforced) {
          curling_influence = 1.0f;
          weights[index] += 3.0f;
        }
        total_length += curling_influence * (last_point_pos - current.position).norm();
        last_point_pos = current.position;
      }

      if (comparator.setup == spRear) {
        total_length *= 0.3f;
      }

      // Curve Fitting
      size_t number_of_segments = std::max(size_t(1),
                                           size_t(std::max(0.0f,total_length) / SeamPlacer::seam_align_mm_per_segment));
      auto curve = Geometry::fit_cubic_bspline(observations, observation_points, weights, number_of_segments);

      // Do alignment - compute fitted point for each point in the string from its Z coord, and store the position into
      // Perimeter structure of the point; also set flag aligned to true
      for (size_t index = 0; index < seam_string.size(); ++index) {
        const auto &pair = seam_string[index];
        const std::vector<SeamCandidate> &points = layers[pair.first].points;
        const SeamCandidate &candidate = points[pair.second];
        float t = std::min(1.0f, std::pow(std::abs(candidate.local_ccw_angle)
                                              / SeamPlacer::sharp_angle_snapping_threshold, 3.0f));

        Vec3f current_pos = candidate.position;
        Vec2f fitted_pos = curve.get_fitted_value(current_pos.z());

        //interpolate between current and fitted position, prefer current pos for large weights.
        Vec3f final_position;
        if (candidate.type != EnforcedBlockedSeamPoint::Enforced) {
          final_position = t >= 1.0f ? current_pos :
              place_on_loop(points, pair.second, t * current_pos + (1.0f - t) * to_3d(fitted_pos, current_pos.z()));
        } else if (t >= 1.0f) {
          final_position = current_pos;
        } else {
          // Keep the 0.4 pull toward the candidate near a measured corner only, then keep the seam on the paint.
          float protect = std::max(candidate.perimeter.flow_width, float(max_nozzle_diameter));
          if (protect <= 0.0f)
            protect = 0.5f;
          const float corner = std::abs(candidate.local_ccw_angle) >= enforced_corner_angle_max ?
              1.0f : enforced_corner_influence(points, pair.second, protect, enforced_corner_fade_factor * protect);
          t = std::max(t, 0.4f * corner);
          final_position = place_on_loop(points, pair.second,
                                         t * current_pos + (1.0f - t) * to_3d(fitted_pos, current_pos.z()));
        }

        Perimeter &perimeter = layers[pair.first].points[pair.second].perimeter;
        perimeter.seam_index = pair.second;
        perimeter.final_seam_position = final_position;
        perimeter.finalized = true;
      }

#ifdef DEBUG_FILES
      auto randf = []() {
        return float(rand()) / float(RAND_MAX);
      };
      Vec3f color { randf(), randf(), randf() };
      for (size_t i = 0; i < seam_string.size(); ++i) {
        auto orig_seam = layers[seam_string[i].first].points[seam_string[i].second];
        fprintf(clusters, "v %f %f %f %f %f %f \n", orig_seam.position[0],
                orig_seam.position[1],
                orig_seam.position[2], color[0], color[1],
                color[2]);
      }

      color = Vec3f { randf(), randf(), randf() };
      for (size_t i = 0; i < seam_string.size(); ++i) {
        const Perimeter &perimeter = layers[seam_string[i].first].points[seam_string[i].second].perimeter;
        fprintf(aligns, "v %f %f %f %f %f %f \n", perimeter.final_seam_position[0],
                perimeter.final_seam_position[1],
                perimeter.final_seam_position[2], color[0], color[1],
                color[2]);
      }
#endif
    }
  }

#ifdef DEBUG_FILES
  fclose(clusters);
  fclose(aligns);
#endif

}

void SeamPlacer::init(const Print &print, std::function<void(void)> throw_if_canceled_func) {
  using namespace SeamPlacerImpl;
  m_seam_per_object.clear();
  m_precise_seam_warning.clear();

  // Warning flags for Precise Seam processing — shared across all objects
  PreciseSeam::PreciseSeamWarnings precise_seam_warnings;

  for (const PrintObject *po : print.objects()) {
    throw_if_canceled_func();
    SeamPosition configured_seam_preference = po->config().seam_position.value;
    SeamComparator comparator { configured_seam_preference };

    {
      GlobalModelInfo global_model_info { };
      gather_enforcers_blockers(global_model_info, po);
      PreciseSeam::init_precise_seam_data(
          global_model_info.precise_seam_strong_volumes,
          global_model_info.precise_seam_weak_volumes,
          m_seam_per_object[po].has_precise_seam_strong_volumes,
          po->model_object());

      // Slice each Precise Seam modifier once per object; both consumers read the cache.
      for (const ModelVolume* vol : global_model_info.precise_seam_strong_volumes)
          global_model_info.precise_seam_slices[vol] = PreciseSeam::prepare_modifier_slices(po->slice_single_volume_regions(vol));
      for (const ModelVolume* vol : global_model_info.precise_seam_weak_volumes)
          global_model_info.precise_seam_slices[vol] = PreciseSeam::prepare_modifier_slices(po->slice_single_volume_regions(vol));
      // Register usage tracking before the parallel phase; workers only set its flags. Several print
      // objects of one model object share volumes, and try_emplace keeps what earlier ones recorded.
      for (const ModelVolume* vol : global_model_info.precise_seam_strong_volumes)
          precise_seam_warnings.modifier_usage.try_emplace(vol);
      for (const ModelVolume* vol : global_model_info.precise_seam_weak_volumes)
          precise_seam_warnings.modifier_usage.try_emplace(vol);

      throw_if_canceled_func();
      if (configured_seam_preference == spAligned || configured_seam_preference == spNearest || configured_seam_preference == spAlignedBack) {
        compute_global_occlusion(global_model_info, po, throw_if_canceled_func, configured_seam_preference);
      }
      throw_if_canceled_func();

      BOOST_LOG_TRIVIAL(debug)
          << "SeamPlacer: gather_seam_candidates: start";
      gather_seam_candidates(po, global_model_info, &precise_seam_warnings);
      BOOST_LOG_TRIVIAL(debug)
          << "SeamPlacer: gather_seam_candidates: end";
      throw_if_canceled_func();
      if (configured_seam_preference == spAligned || configured_seam_preference == spNearest || configured_seam_preference == spAlignedBack) {
        BOOST_LOG_TRIVIAL(debug)
            << "SeamPlacer: calculate_candidates_visibility : start";
        calculate_candidates_visibility(po, global_model_info);
        BOOST_LOG_TRIVIAL(debug)
            << "SeamPlacer: calculate_candidates_visibility : end";
      }
    } // destruction of global_model_info (large structure, no longer needed)
    throw_if_canceled_func();
    BOOST_LOG_TRIVIAL(debug)
        << "SeamPlacer: calculate_overhangs and layer embdedding : start";
    calculate_overhangs_and_layer_embedding(po);
    BOOST_LOG_TRIVIAL(debug)
        << "SeamPlacer: calculate_overhangs and layer embdedding: end";
    throw_if_canceled_func();
    if (configured_seam_preference != spNearest) { // For spNearest, the seam is picked in the place_seam method with actual nozzle position information
      BOOST_LOG_TRIVIAL(debug)
          << "SeamPlacer: pick_seam_point : start";
      //pick seam point
      std::vector<PrintObjectSeamData::LayerSeams> &layers = m_seam_per_object[po].layers;
      tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()),
                        [&layers, configured_seam_preference, comparator](tbb::blocked_range<size_t> r) {
                          for (size_t layer_idx = r.begin(); layer_idx < r.end(); ++layer_idx) {
                            std::vector<SeamCandidate> &layer_perimeter_points = layers[layer_idx].points;
                            for (size_t current = 0; current < layer_perimeter_points.size();
                                 current = layer_perimeter_points[current].perimeter.end_index)
                              if (configured_seam_preference == spRandom)
                                pick_random_seam_point(layer_perimeter_points, current);
                              else
                                pick_seam_point(layer_perimeter_points, current, comparator);
                          }
                        });
      BOOST_LOG_TRIVIAL(debug)
          << "SeamPlacer: pick_seam_point : end";
    }
    throw_if_canceled_func();
    if (configured_seam_preference == spAligned || configured_seam_preference == spRear || configured_seam_preference == spAlignedBack) {
      BOOST_LOG_TRIVIAL(debug)
          << "SeamPlacer: align_seam_points : start";
      align_seam_points(po, comparator, throw_if_canceled_func);
      BOOST_LOG_TRIVIAL(debug)
          << "SeamPlacer: align_seam_points : end";
    }

    // Restore precise seam positions that were potentially modified
    if (m_seam_per_object[po].has_precise_seam_strong_volumes) {
      PreciseSeam::restore_precise_seam_positions(m_seam_per_object[po].layers);
    }

#ifdef DEBUG_FILES
    debug_export_points(m_seam_per_object[po].layers, po->bounding_box(), comparator);
#endif
  }

  // Prepare one combined Precise Seam warning; G-code export issues it. Keep it single: separate
  // warnings would each re-push all warnings and duplicate text in the notification.
  {
      const unsigned failed_types = precise_seam_warnings.failed_types.load(std::memory_order_relaxed);
      const unsigned mi = precise_seam_warnings.multiple_intersections.load(std::memory_order_relaxed);
      const unsigned fc = precise_seam_warnings.full_containment.load(std::memory_order_relaxed);
      const size_t failed = precise_seam_warnings.failed_fragments.load(std::memory_order_relaxed);
      // All workers have finished; cancellation before this point may omit the summary.
      if (failed > PreciseSeam::failed_fragment_log_limit)
          BOOST_LOG_TRIVIAL(warning) << "[PreciseSeamIntersectionFailed] " << failed
              << " fragments discarded; first " << PreciseSeam::failed_fragment_log_limit
              << " logged (parallel processing order), " << (failed - PreciseSeam::failed_fragment_log_limit)
              << " omitted";
      // Recoveries are log-only: no user warning, but the same bounded detail and a total.
      const size_t recovered = precise_seam_warnings.recovered_fragments.load(std::memory_order_relaxed);
      if (recovered > PreciseSeam::failed_fragment_log_limit)
          BOOST_LOG_TRIVIAL(warning) << "[PreciseSeamFragmentRecovered] " << recovered
              << " fragments recovered; first " << PreciseSeam::failed_fragment_log_limit
              << " logged (parallel processing order), " << (recovered - PreciseSeam::failed_fragment_log_limit)
              << " omitted";
      // Reasons name the modifier types, as the menu does, not individual modifiers: "Seam Left, Seam
      // Enforced" in menu order, each type once. The same msgids as the menu share its translations.
      const auto type_list = [](unsigned mask) {
          const std::pair<ModelVolumeType, std::string> types[] = {
              {ModelVolumeType::PRECISE_SEAM_CENTER, _u8L("Seam Center")},
              {ModelVolumeType::PRECISE_SEAM_LEFT, _u8L("Seam Left")},
              {ModelVolumeType::PRECISE_SEAM_RIGHT, _u8L("Seam Right")},
              {ModelVolumeType::PRECISE_SEAM_ENFORCED, _u8L("Seam Enforced")},
              {ModelVolumeType::PRECISE_SEAM_BLOCKED, _u8L("Seam Blocked")},
              {ModelVolumeType::PRECISE_SEAM_NEUTRAL, _u8L("Seam Neutral")}};
          std::string list;
          for (const auto &[type, name] : types)
              if (mask & PreciseSeam::PreciseSeamWarnings::type_bit(type))
                  list += (list.empty() ? "" : ", ") + name;
          return list;
      };
      std::vector<std::string> parts;
      if (failed_types != 0)
          parts.push_back((boost::format(_u8L("failed to process some intersections (%1%)")) % type_list(failed_types)).str());
      if (mi != 0)
          parts.push_back((boost::format(_u8L("multiple intersections with a perimeter, only one was used (%1%)")) % type_list(mi)).str());
      if (fc != 0)
          parts.push_back((boost::format(_u8L("a perimeter is fully inside a modifier, the modifier was not applied to it (%1%)")) % type_list(fc)).str());
      // Modifiers whose usage flags match, in print and volume order, so the named one is deterministic.
      const auto collect_modifiers = [&print, &precise_seam_warnings](const auto &matches) {
          std::vector<const ModelVolume*> found;
          for (const PrintObject *po : print.objects())
              for (const ModelVolume *volume : po->model_object()->volumes) {
                  const auto it = precise_seam_warnings.modifier_usage.find(volume);
                  if (it != precise_seam_warnings.modifier_usage.end() && matches(it->second) &&
                      std::find(found.begin(), found.end(), volume) == found.end())
                      found.push_back(volume);
              }
          return found;
      };
      // Seam Blocked modifiers whose whole perimeter stayed Blocked; the user warning names only the
      // first one, the log lists them all.
      const std::vector<const ModelVolume*> fully_blocked = collect_modifiers(
          [](const PreciseSeam::PreciseSeamWarnings::ModifierUsage &usage) { return usage.fully_blocked.load(std::memory_order_relaxed); });
      for (const ModelVolume *volume : fully_blocked)
          BOOST_LOG_TRIVIAL(warning) << "[PreciseSeamFullyBlocked] object=\"" << volume->get_object()->name
              << "\" modifier=\"" << volume->name << "\"";
      if (!fully_blocked.empty()) {
          const ModelVolume *first = fully_blocked.front();
          if (fully_blocked.size() == 1) {
              // TRN: %1% is the modifier type "Seam Blocked" as in the menu, %2% the modifier name, %3% the object name.
              parts.push_back((boost::format(_u8L("%1% modifier \"%2%\" of \"%3%\" covers an entire perimeter"))
                  % _u8L("Seam Blocked") % first->name % first->get_object()->name).str());
          } else {
              // TRN: %1% is the modifier type "Seam Blocked" as in the menu, %2% the modifier name, %3% the object name, %4% the number of modifiers.
              parts.push_back((boost::format(_u8L("%1% modifier \"%2%\" of \"%3%\" (%4% in total) covers an entire perimeter"))
                  % _u8L("Seam Blocked") % first->name % first->get_object()->name % fully_blocked.size()).str());
          }
      }
      // Modifiers evaluated somewhere that never reached a perimeter; never-evaluated ones are not reported.
      const std::vector<const ModelVolume*> no_effect = collect_modifiers(
          [](const PreciseSeam::PreciseSeamWarnings::ModifierUsage &usage) {
              return usage.checked.load(std::memory_order_relaxed) && !usage.reached.load(std::memory_order_relaxed);
          });
      // The user warning names only the first one; the log lists them all.
      for (const ModelVolume *volume : no_effect)
          BOOST_LOG_TRIVIAL(warning) << "[PreciseSeamNoEffect] object=\"" << volume->get_object()->name
              << "\" modifier=\"" << volume->name << "\"";
      if (!no_effect.empty()) {
          const ModelVolume *first = no_effect.front();
          if (no_effect.size() == 1)
              parts.push_back((boost::format(_u8L("modifier \"%1%\" of \"%2%\" had no effect on the seam (it might not reach the centerline of the printed perimeter)"))
                  % first->name % first->get_object()->name).str());
          else
              parts.push_back((boost::format(_u8L("modifier \"%1%\" of \"%2%\" (%3% in total) had no effect on the seam (it might not reach the centerline of the printed perimeter)"))
                  % first->name % first->get_object()->name % no_effect.size()).str());
      }
      if (!parts.empty()) {
          // One line: the export warnings dialog shows only the first line of each warning.
          std::string warning_text = _u8L("Precise Seam") + ": ";
          for (size_t i = 0; i < parts.size(); ++i) {
              if (i > 0) warning_text += "; ";
              warning_text += parts[i];
          }
          warning_text += ". ";
          warning_text += _u8L("Seam placement may differ from expected.");
          m_precise_seam_warning = std::move(warning_text);
      }
  }
}

void SeamPlacer::place_seam(const Layer *layer, ExtrusionLoop &loop,
                            const Point &last_pos, float& overhang) const {
  using namespace SeamPlacerImpl;
  const PrintObject *po = layer->object();
  // Must not be called with supprot layer.
  assert(dynamic_cast<const SupportLayer*>(layer) == nullptr);
  // Object layer IDs are incremented by the number of raft layers.
  assert(layer->id() >= po->slicing_parameters().raft_layers());
  const size_t layer_index = layer->id() - po->slicing_parameters().raft_layers();
  const double unscaled_z = layer->slice_z;

  auto get_next_loop_point = [loop](ExtrusionLoop::ClosestPathPoint current) {
    current.segment_idx += 1;
    if (current.segment_idx >= loop.paths[current.path_idx].polyline.points.size()) {
      current.path_idx = next_idx_modulo(current.path_idx, loop.paths.size());
      current.segment_idx = 0;
    }
    current.foot_pt = loop.paths[current.path_idx].polyline.points[current.segment_idx].to_point();
    return current;
  };

  const PrintObjectSeamData::LayerSeams &layer_perimeters =
      m_seam_per_object.find(layer->object())->second.layers[layer_index];

  // Find the closest perimeter in the SeamPlacer to this loop.
  // Repeat search until two consecutive points of the loop are found, that result in the same closest_perimeter
  // This is beacuse with arachne, T-Junctions may exist and sometimes the wrong perimeter was chosen
  size_t closest_perimeter_point_index = 0;
  { // local space for the closest_perimeter_point_index
    Perimeter *closest_perimeter = nullptr;
    ExtrusionLoop::ClosestPathPoint closest_point{0, 0, loop.paths[0].polyline.points[0].to_point()};
    size_t points_count = std::accumulate(loop.paths.begin(), loop.paths.end(), 0, [](size_t acc,const ExtrusionPath& p) {
      return acc + p.polyline.points.size();
    });
    for (size_t i = 0; i < points_count; ++i) {
      Vec2f unscaled_p = unscaled<float>(closest_point.foot_pt);
      closest_perimeter_point_index = find_closest_point(*layer_perimeters.points_tree.get(),
                                                         to_3d(unscaled_p, float(unscaled_z)));
      if (closest_perimeter != &layer_perimeters.points[closest_perimeter_point_index].perimeter) {
        closest_perimeter = &layer_perimeters.points[closest_perimeter_point_index].perimeter;
        closest_point = get_next_loop_point(closest_point);
      } else {
        break;
      }
    }
  }

  Vec3f seam_position;
  size_t seam_index;
  if (const Perimeter &perimeter = layer_perimeters.points[closest_perimeter_point_index].perimeter;
      perimeter.finalized) {
    seam_position = perimeter.final_seam_position;
    seam_index = perimeter.seam_index;
  } else {
    seam_index =
        po->config().seam_position == spNearest ?
                                                pick_nearest_seam_point_index(layer_perimeters.points, perimeter.start_index,
                                                                              unscaled<float>(last_pos)) :
                                                perimeter.seam_index;
    seam_position = layer_perimeters.points[seam_index].position;
  }

  Point seam_point = Point::new_scale(seam_position.x(), seam_position.y());
  overhang = layer_perimeters.points[seam_index].unsupported_dist;

  if (loop.role() == ExtrusionRole::erPerimeter) { //Hopefully inner perimeter
    const SeamCandidate &perimeter_point = layer_perimeters.points[seam_index];
    ExtrusionLoop::ClosestPathPoint projected_point = loop.get_closest_path_and_point(seam_point, false);
    // determine depth of the seam point.
    float depth = (float) unscale(Point(seam_point - projected_point.foot_pt)).norm();
    float beta_angle = cos(perimeter_point.local_ccw_angle / 2.0f);
    size_t index_of_prev =
        seam_index == perimeter_point.perimeter.start_index ?
                                                            perimeter_point.perimeter.end_index - 1 :
                                                            seam_index - 1;
    size_t index_of_next =
        seam_index == perimeter_point.perimeter.end_index - 1 ?
                                                              perimeter_point.perimeter.start_index :
                                                              seam_index + 1;

    if ((seam_position - perimeter_point.position).squaredNorm() < depth && // seam is on perimeter point
        perimeter_point.local_ccw_angle < -EPSILON // In concave angles
    ) { // In this case, we are at internal perimeter, where the external perimeter has seam in concave angle. We want to align
                                                                            // the internal seam into the concave corner, and not on the perpendicular projection on the closest edge (which is what the split_at function does)
      Vec2f dir_to_middle =
          ((perimeter_point.position - layer_perimeters.points[index_of_prev].position).head<2>().normalized()
           + (perimeter_point.position - layer_perimeters.points[index_of_next].position).head<2>().normalized())
          * 0.5;
      depth = 1.4142 * depth / beta_angle;
      // There are some nice geometric identities in determination of the correct depth of new seam point.
      //overshoot the target depth, in concave angles it will correctly snap to the corner; TODO: find out why such big overshoot is needed.
      Vec2f final_pos = perimeter_point.position.head<2>() + depth * dir_to_middle;
      projected_point = loop.get_closest_path_and_point(Point::new_scale(final_pos.x(), final_pos.y()), false);
    } else { // not concave angle, in that case the nearest point is the good candidate
      // but for staggering, we also need to recompute depth of the inner perimter, because in convex corners, the distance is larger than layer width
      // we want the perpendicular depth, not distance to nearest point
      depth = depth * beta_angle / 1.4142;
    }

    seam_point = projected_point.foot_pt;

    //lastly, for internal perimeters, do the staggering if requested
    if (po->config().staggered_inner_seams && loop.length() > 0.0) {
      //fix depth, it is sometimes strongly underestimated
      depth = std::max(loop.paths[projected_point.path_idx].width, depth);

      while (depth > 0.0f) {
        auto next_point = get_next_loop_point(projected_point);
        Vec2f a = unscale(projected_point.foot_pt).cast<float>();
        Vec2f b = unscale(next_point.foot_pt).cast<float>();
        float dist = (a - b).norm();
        if (dist > depth) {
          Vec2f final_pos = a + (b - a) * depth / dist;
          next_point.foot_pt = Point::new_scale(final_pos.x(), final_pos.y());
        }
        depth -= dist;
        projected_point = next_point;
      }
      seam_point = projected_point.foot_pt;
    }
  }

  // Because the G-code export has 1um resolution, don't generate segments shorter than 1.5 microns,
  // thus empty path segments will not be produced by G-code export.
  if (!loop.split_at_vertex(seam_point, scaled<double>(0.0015))) {
    // The point is not in the original loop.
    // Insert it.
    loop.split_at(seam_point, true);
  }

}

} // namespace Slic3r
