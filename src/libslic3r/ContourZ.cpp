#include "Exception.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Point.hpp"
#include "Print.hpp"
#include "SLA/IndexedMesh.hpp"
#include "libslic3r.h"
#include <cfloat>
#include <cmath>
#include <initializer_list>
#include <string>

namespace Slic3r {

static void contour_extrusion_entity(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionEntity *extr);

// static double lowest_z_within_distance(const Vec3d &normal, double dist) {
// 	const Vec3d p(0.0, 0.0, 0.0);
// 	Eigen::Vector3d n_unit = normal.normalized();
//     Eigen::Vector3d z_hat(0.0, 0.0, 1.0);

//     // Project the negative z-direction into the tangent plane
//     Eigen::Vector3d v_dir = -z_hat + (z_hat.dot(n_unit)) * n_unit;

//     double norm_v = v_dir.norm();
//     if (norm_v == 0.0) {
//         // Surface is horizontal, cannot go lower in z within tangent plane
//         return p.z();
//     }

//     Eigen::Vector3d v = dist * v_dir / norm_v;
//     Eigen::Vector3d q = p + v;
// 	return q.z();
// }

static double follow_slope_down(double angle_rad, double dist) {
	return -dist * std::sin(angle_rad);
}

static double slope_from_normal(const Eigen::Vector3d& normal) {
    // Ensure the normal is normalized
    Eigen::Vector3d n = normal.normalized();

    // Compute angle between normal and z-axis
    double angle_rad = std::acos(std::abs(n.z()));  // angle between normal and vertical
	return angle_rad;

	// calculate fall over dist
	// double dist = 0.2;
	// double z_dist = lowest_z(angle_rad, dist);
	// printf("fall %f vs %f\n", z_dist, lowest_z_within_distance(normal, dist));

    // double angle_deg = angle_rad * 180.0 / M_PI;
    // return angle_deg;
}

// const int LINE = 180;

static bool contour_extrusion_path(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionPath &path) {
	if (region->region().config().zaa_region_disable) {
		return false;
	}

	if (path.role() != erTopSolidInfill && path.role() != erIroning && path.role() != erExternalPerimeter && path.role() != erPerimeter) {
		return false;
	}
	
	Layer *layer = region->layer();
	coordf_t mesh_z = layer->print_z + mesh.ground_level();
	coordf_t min_z = layer->object()->config().zaa_min_z;

	const Points3 &points = path.polyline.points;
	double resolution_mm = 0.1;

	coordf_t height = layer->height;
	// std::cout << "LAYER " << (layer->id()+1) << std::endl;
	// std::cout << "PRINT Z " << layer->print_z << std::endl;
	// std::cout << "LAYER HEIGHT " << height << std::endl;
	// std::cout << "EXTRUSION HEIGHT " << path.height << std::endl;
	// std::cout << "EXTRUSION WIDTH " << path.width << std::endl;
	// std::cout << "EXTRUSION ROLE: " << ExtrusionEntity::role_to_string(path.role()) << std::endl;
	// std::cout << "FIRST POINT: " << path.polyline.first_point() << std::endl;

	double minimize_perimeter_height_angle = region->region().config().zaa_minimize_perimeter_height;

	Pointf3s contoured_points;
	bool was_contoured = false;
	// bool is_perimeter = path.role() == erExternalPerimeter || path.role() == erPerimeter || path.role() == erOverhangPerimeter;

	for (Points3::const_iterator it = points.begin(); it != points.end()-1; ++it) {
		Vec2d p1d(unscale_(it->x()), unscale_(it->y()));
		Vec2d p2d(unscale_((it+1)->x()), unscale_((it+1)->y()));
		Linef line(p1d, p2d);

		double length_mm = line.length();
		int num_segments = int(std::ceil(length_mm / resolution_mm));
		Vec2d delta = line.vector();

		for (int i = 0; i < num_segments+1; i++) {
			Vec2d p = p1d + delta*i/num_segments;

			coordf_t x = p.x();
			coordf_t y = p.y();

			sla::IndexedMesh::hit_result hit_up = mesh.query_ray_hit({x, y, mesh_z}, {0.0, 0.0, 1.0});
			sla::IndexedMesh::hit_result hit_down = mesh.query_ray_hit({x, y, mesh_z}, {0.0, 0.0, -1.0});

			double up = hit_up.distance();
			double down = hit_down.distance();
			double d = up < down ? up : -down;
			const Vec3d &normal = (up < down ? hit_up : hit_down).normal();
			
			double max_up = min_z;
			double min_down = -(height - min_z);
			double half_width = path.width / 2.0;
			if (path.role() == erIroning) {
				max_up = height;
				min_down = -(height + 0.1);
			}

			double slope_rad = slope_from_normal(normal);
			double slope_degrees = slope_rad * 180.0 / M_PI;

			if (d > min_down && minimize_perimeter_height_angle > 0 && minimize_perimeter_height_angle < slope_degrees && path.role() == erExternalPerimeter) {
				double adjustment = follow_slope_down(slope_rad, half_width);
				if (adjustment > 0) {
					throw RuntimeError("ContourZ: got positive adjustment");
				}
				d += adjustment;
				if (d < min_down) {
					d = min_down;
				}
			}

			if (d > max_up + 0.03 || d < min_down) {
				d = 0;
			} else {
				if (d > max_up) {
					d = max_up;
				}
			}

			if (path.role() == erExternalPerimeter && d > 0) {
				// do not increase height of external perimeters as this may create an appearance of a seam
				d = 0;
			}
			
			if (std::abs(d) > EPSILON) {
				was_contoured = true;
			}

			Vec3d new_point = {p.x(), p.y(), d};

			if (contoured_points.size() > 2) {
				double dist = Linef3::distance_to_infinite_squared(
					contoured_points[contoured_points.size() - 2], 
					contoured_points[contoured_points.size() - 1], 
					new_point);
				if (dist < EPSILON) {
					contoured_points[contoured_points.size() - 1] = new_point;
					continue;
				}
			}

			contoured_points.push_back(new_point);
		}
	}

	if (!was_contoured) {
		return false;
	}

	Polyline3 polyline;
	for (const Vec3d &point : contoured_points) {
		polyline.append(Point3(scale_(point.x()), scale_(point.y()), scale_(point.z())));
	}

	path.polyline = std::move(polyline);
	path.z_contoured = true;
	return true;
}

static void contour_extrusion_multipath(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionMultiPath &multipath) 
{
	for (ExtrusionPath &path : multipath.paths) {
		contour_extrusion_path(region, mesh, path);
	}
}

static void contour_extrusion_loop(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionLoop &loop) 
{
	for (ExtrusionPath &path : loop.paths) {
		contour_extrusion_path(region, mesh, path);
	}
}

static void contour_extrusion_entitiy_collection(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionEntityCollection &collection) {
	for (ExtrusionEntity *entity : collection.entities) {
		contour_extrusion_entity(region, mesh, entity);
	}
}

static void contour_extrusion_entity(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionEntity *extr) {
	const ExtrusionPathSloped *sloped = dynamic_cast<const ExtrusionPathSloped*>(extr);
	if (sloped != nullptr) {
		throw RuntimeError("ExtrusionPathSloped not implemented");
		return;
	}

	ExtrusionMultiPath *multipath = dynamic_cast<ExtrusionMultiPath*>(extr);
	if (multipath != nullptr) {
		contour_extrusion_multipath(region, mesh, *multipath);
		return;
	}

	ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(extr);
	if (path != nullptr) {
		contour_extrusion_path(region, mesh, *path);
		return;
	}

	ExtrusionLoop *loop = dynamic_cast<ExtrusionLoop*>(extr);
	if (loop != nullptr) {
		contour_extrusion_loop(region, mesh, *loop);
		return;
	}

	const ExtrusionLoopSloped *loop_sloped = dynamic_cast<const ExtrusionLoopSloped*>(extr);
	if (loop_sloped != nullptr) {
		throw RuntimeError("ExtrusionLoopSloped not implemented");
		return;
	}

	ExtrusionEntityCollection *collection = dynamic_cast<ExtrusionEntityCollection*>(extr);
	if (collection != nullptr) {
		contour_extrusion_entitiy_collection(region, mesh, *collection);
		return;
	}

	throw RuntimeError("ContourZ: ExtrusionEntity type not implemented: " + std::string(typeid(*extr).name()));
	return;
}

static void handle_extrusion_collection(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionEntityCollection &collection, std::initializer_list<ExtrusionRole> roles) {
	for (ExtrusionEntity *extr : collection.entities) {
		// printf("handling extrusion collection %p %p\n", &collection, extr);
		if (!contains(roles, extr->role())) {
			continue;
		}

		contour_extrusion_entity(region, mesh, extr);
	}
}

// static void find_point(ExtrusionPath &path, const std::string &path_info) { 
// 	Points3 &points = path.polyline.points;

// 	size_t i = 0;
// 	for (Points3::const_iterator it = points.begin(); it != points.end()-1; ++it) {
// 		if (it->x() == -883971 && it->y() == 979001) {
// 			std::cout << "FOUND POINT " << ExtrusionEntity::role_to_string(path.role()) << " at path " << path_info << "[" + std::to_string(i) + "]" << std::endl;
// 		}
// 		i++;
// 	}
// }

// static void find_point(ExtrusionLoop &loop, const std::string &path_info) { 
// 	size_t i = 0;
// 	for (ExtrusionPath &path : loop.paths) {
// 		find_point(path, path_info + "[" + std::to_string(i) + "]");
// 		i++;
// 	}
// }

// static void find_point(ExtrusionEntity &extr, const std::string &path);

// static void find_point(ExtrusionEntityCollection &collection, const std::string &path) {
// 	size_t i = 0;
// 	for (ExtrusionEntity *extr : collection.entities) {
// 		find_point(*extr, path + "[" + std::to_string(i) + "]");
// 		i++;
// 	}
// }

// static void find_point(ExtrusionEntity &extr, const std::string &path_info) { 
// 	const ExtrusionPathSloped *sloped = dynamic_cast<const ExtrusionPathSloped*>(&extr);
// 	if (sloped != nullptr) {
// 		throw RuntimeError("ExtrusionPathSloped not implemented");
// 		return;
// 	}

// 	ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(&extr);
// 	if (path != nullptr) {
// 		find_point(*path, path_info + " as ExtrusionPath " + ExtrusionEntity::role_to_string(extr.role()));
// 		return;
// 	}

// 	ExtrusionLoop *loop = dynamic_cast<ExtrusionLoop*>(&extr);
// 	if (loop != nullptr) {
// 		find_point(*loop, path_info + " as ExtrusionLoop " + ExtrusionEntity::role_to_string(extr.role()));
// 		return;
// 	}

// 	const ExtrusionLoopSloped *loop_sloped = dynamic_cast<const ExtrusionLoopSloped*>(&extr);
// 	if (loop_sloped != nullptr) {
// 		throw RuntimeError("ExtrusionLoopSloped not implemented");
// 		return;
// 	}

// 	ExtrusionEntityCollection *collection = dynamic_cast<ExtrusionEntityCollection*>(&extr);
// 	if (collection != nullptr) {
// 		find_point(*collection, path_info + " as ExtrusionEntityCollection " + ExtrusionEntity::role_to_string(extr.role()));
// 		return;
// 	}

// 	throw RuntimeError("ContourZ: ExtrusionEntity type not implemented");
// 	return;
// }

void Layer::make_contour_z(const sla::IndexedMesh &mesh)
{
	// printf("make_contour_z() called\n");
	for (LayerRegion *region : this->regions()) {
		// printf("processing layer region %p\n", region);
		// find_point(region->fills, "fills");
		// find_point(region->perimeters, "perimeters");

		handle_extrusion_collection(region, mesh, region->fills, {erTopSolidInfill, erIroning, erExternalPerimeter, erMixed});
		handle_extrusion_collection(region, mesh, region->perimeters, {erExternalPerimeter, erMixed});
	}
}
} // namespace Slic3r