#include "Exception.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Line.hpp"
#include "NonplanarTopSlice.hpp"
#include "Point.hpp"
#include "Print.hpp"
#include "SLA/IndexedMesh.hpp"
#include "libslic3r.h"
#include <algorithm>
#include <Eigen/Core>
#include <cfloat>
#include <cmath>
#include <initializer_list>
#include <math.h>
#include <string>
#include <utility>
#include "Config.hpp"
#include "Polyline.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

// What a layer is contoured against: the mesh, bounded further by a cut-plane segment's clips.
struct ContourZSurface
{
    const sla::IndexedMesh                    &mesh;
    const std::vector<Layer::ContourZCeiling> &ceilings;
    // UNLAYERED_MOD: nonplanar layers -- curved shell material near the paths (nullptr: none).
    const NonplanarZaaLimits                  *np = nullptr;

    struct Hit
    {
        bool   is_hit;
        double distance;
        Vec3d  normal;
    };

    // Where an upward ray from inside the material first leaves  mesh ∩ every ceiling.
    Hit query_up(const Vec3d &origin) const
    {
        const sla::IndexedMesh::hit_result mesh_hit = mesh.query_ray_hit(origin, Vec3d::UnitZ());
        Hit hit{mesh_hit.is_hit(), mesh_hit.distance(), mesh_hit.is_hit() ? mesh_hit.normal() : Vec3d::UnitZ()};
        for (const Layer::ContourZCeiling &ceiling : ceilings) {
            // A downward-facing plane (a segment's floor) is never left through going up.
            if (ceiling.normal.z() < EPSILON)
                continue;
            const double t = std::max(0., (ceiling.offset - ceiling.normal.dot(origin)) / ceiling.normal.z());
            if (!hit.is_hit || t < hit.distance)
                hit = {true, t, ceiling.normal};
        }
        return hit;
    }
};

static void contour_extrusion_entity(LayerRegion *region, const ContourZSurface &surface, ExtrusionEntity *extr);

static double follow_slope_down(double angle_rad, double dist)
{
	return -dist * std::sin(angle_rad);
}

static double slope_from_normal(const Eigen::Vector3d& normal)
{
    // Ensure the normal is normalized
    Eigen::Vector3d n = normal.normalized();

    // Compute angle between normal and z-axis
    double angle_rad = std::acos(std::abs(n.z()));  // angle between normal and vertical
	return angle_rad;
}

static bool contour_extrusion_path(LayerRegion *region, const ContourZSurface &surface, ExtrusionPath &path)
{

	    //BOOST_LOG_TRIVIAL(info) << "contour_extrusion_path: role=" << path.role();

    // UNLAYERED_MOD: a nonplanar layers shell path already follows the surface.
    // A path contoured before (nonplanar layers: the planar layer under a curved shell) keeps its Z.
    if (path.nonplanar_shell || path.z_contoured)
        return false;

    // UNLAYERED_MOD: nonplanar layers -- under a curved band every planar path of the layer reaching it is lowered onto
    // the band's bottom (below); elsewhere only these roles are contoured.
    const bool contoured_role = path.role() == erTopSolidInfill || path.role() == erIroning || path.role() == erExternalPerimeter ||
                                path.role() == erPerimeter;
    if (! contoured_role && surface.np == nullptr) {
            //BOOST_LOG_TRIVIAL(info) << "contour_extrusion_path: skipping role=" << path.role();

		return false;
	}
	
	Layer *layer = region->layer();
	coordf_t mesh_slice_z = layer->slice_z + surface.mesh.ground_level();
    BOOST_LOG_TRIVIAL(info) << "contour_extrusion_path: mesh_slice_z=" << mesh_slice_z << " ground_level=" << surface.mesh.ground_level();

	coordf_t min_z = region->region().config().zaa_min_z;

	const Points3 &points = path.polyline.points;
	double resolution_mm = 0.1;

	coordf_t height = layer->height;

	double minimize_perimeter_height_angle = region->region().config().zaa_minimize_perimeter_height;

	Pointf3s contoured_points;
	bool was_contoured = false;

    // Normally, if the new point is collinear with the last two points, we do not add it to the list of contoured
    // points. Instead we update the last point to be the new point. This is to avoid creating a large number of very
    // short segments. However, if the new point corresponds to a point in the original path (original), even if it is
    // collinear, we add it anyway. This is to avoid creating a degenerate polygon with only two points, which may cause
    // issues in downstream code.
    auto add_point = [&contoured_points](const Vec3d &new_point, bool original) {
        if (contoured_points.size() >= 2 && !original) {
            double dist = Linef3::distance_to_infinite_squared(new_point, contoured_points[contoured_points.size() - 2],
                                                               contoured_points[contoured_points.size() - 1]);
            if (dist < EPSILON * EPSILON) {
                contoured_points[contoured_points.size() - 1] = new_point;
                return;
            }
        }
        contoured_points.push_back(new_point);
    };

    if (points.size() < 2) {
        // Safety check. The loop below does not handle paths with less than two points correctly.
        return false;
    }

    for (Points3::const_iterator it = points.begin(); it != points.end()-1; ++it) {
		Vec2d p1d(unscale_(it->x()), unscale_(it->y()));
		Vec2d p2d(unscale_((it+1)->x()), unscale_((it+1)->y()));
		Linef line(p1d, p2d);

		double length_mm = line.length();
		int num_segments = int(std::ceil(length_mm / resolution_mm));
		Vec2d delta = line.vector();

        if (num_segments == 0) {
            continue;
        }

        for (int i = 0; i < num_segments + 1; i++) {
            Vec2d p = p1d + delta * i / num_segments;

            coordf_t x = p.x();
			coordf_t y = p.y();

            // UNLAYERED_MOD: nonplanar layers -- under a curved band printed with or after this layer the surface to follow
            // is the band's bottom (S - T): lower onto it, as a top surface is contoured; never raise.
            // (Only a band reaching this layer: one wholly above it -- a Benchy cabin roof 30 mm over the deck -- would only
            // block the normal contouring below, leaving the deck under the roof 0.1 mm higher than beside it.)
            if (double band_z = 0.; surface.np != nullptr && surface.np->band_bottom(layer->id(), x, y, band_z) &&
                                    band_z < layer->print_z - EPSILON) {
                double d = std::min(0., band_z - layer->print_z);
                d        = std::max(d, -(height - min_z));
                if (std::abs(d) > EPSILON)
                    was_contoured = true;
                add_point({p.x(), p.y(), d}, i == 0);
                continue;
            }
            if (! contoured_role) {
                add_point({p.x(), p.y(), 0.}, i == 0);
                continue;
            }

			// UNLAYERED_MOD -- from a hair under the slice plane: a top lying exactly on the plane (a Benchy's deck at the
			// 8.6 layer's slice z 8.5) was missed wherever floating point put the origin on or just above its face, and the
			// ray found the next surface up instead (the cabin roof's underside), so the deck printed 0.1 mm higher under
			// the roof than beside it: the roof and chimney outlines showed as steps in the deck's top and ironing.
			ContourZSurface::Hit hit_up = surface.query_up({x, y, mesh_slice_z - 1e-3});
			if (hit_up.is_hit)
				hit_up.distance -= 1e-3;
            //BOOST_LOG_TRIVIAL(info) << "contour_extrusion_path: hit=" << hit_up.is_hit << " distance=" << hit_up.distance;

            // UNLAYERED_MOD -- a top surface or ironing follows only an exit through an UPWARD-facing surface. A mesh whose
            // parts overlap (a Benchy's cabin and chimney reaching into the deck) has inner faces the ray leaves through at
            // once: the outlines of geometry far above were pressed 0.1 mm into the deck's top and ironing (2026-11-02).
            if ((path.role() == erTopSolidInfill || path.role() == erIroning) && (! hit_up.is_hit || hit_up.normal.z() < 0.2)) {
                add_point({p.x(), p.y(), 0.}, i == 0);
                continue;
            }


			double d = hit_up.distance - (layer->print_z - layer->slice_z);

			double max_up = min_z;
			double min_down = -(height - min_z);
			double half_width = path.width / 2.0;
			if (path.role() == erIroning) {
				max_up = height;
				min_down = -(height + 0.1);
			}

            if (is_perimeter(path.role()) && hit_up.is_hit) {
				const Vec3d &normal = hit_up.normal;
                double slope_rad     = slope_from_normal(normal);
                double slope_degrees = slope_rad * 180.0 / M_PI;

                if (d > min_down && minimize_perimeter_height_angle > 0 && minimize_perimeter_height_angle < slope_degrees) {
                    double adjustment = follow_slope_down(slope_rad, half_width);
                    if (adjustment > 0) {
                        throw RuntimeError("ContourZ: got positive adjustment");
                    }
                    d += adjustment;
                    if (d < min_down) {
                        d = min_down;
                    }
                }
            }

            if (d < -height || d > max_up + 0.03) {
                // this point is too far from the mesh edge, probably because this is not a top surface. Do not contour it.
                d = 0;
            }

            if (d < min_down) {
                d = min_down;
            } else if (d > max_up) {
                d = max_up;
            }

            if (is_perimeter(path.role()) && d > 0) {
                // do not increase height of perimeters as this may create an appearance of a seam
                d = 0;
            }

            // UNLAYERED_MOD: nonplanar layers -- beside a curved shell a path is never lowered into the shell's material
            // printed before it (Benchy: 0.06-0.09 mm under it), nor raised where a shell prints later.
            if (surface.np != nullptr) {
                double z_floor = 0.;
                bool   no_raise = false;
                if (surface.np->query(layer->id(), layer->print_z, x, y, z_floor, no_raise)) {
                    if (no_raise && d > 0)
                        d = 0;
                    if (d < 0 && layer->print_z + d < z_floor)
                        d = std::min(0., z_floor - layer->print_z);
                }
            }

            if (std::abs(d) > EPSILON) {
				was_contoured = true;
			}

            add_point({p.x(), p.y(), d}, i == 0);
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

static void contour_extrusion_multipath(LayerRegion *region, const ContourZSurface &surface, ExtrusionMultiPath &multipath) 
{
	for (ExtrusionPath &path : multipath.paths) {
		contour_extrusion_path(region, surface, path);
	}
}

static void contour_extrusion_loop(LayerRegion *region, const ContourZSurface &surface, ExtrusionLoop &loop) 
{
	for (ExtrusionPath &path : loop.paths) {
		contour_extrusion_path(region, surface, path);
	}
}

static void contour_extrusion_entitiy_collection(LayerRegion *region, const ContourZSurface &surface, ExtrusionEntityCollection &collection)
{
	for (ExtrusionEntity *entity : collection.entities) {
		contour_extrusion_entity(region, surface, entity);
	}
}

static void contour_extrusion_entity(LayerRegion *region, const ContourZSurface &surface, ExtrusionEntity *extr)
{
	const ExtrusionPathSloped *sloped = dynamic_cast<const ExtrusionPathSloped*>(extr);
	if (sloped != nullptr) {
		throw RuntimeError("ExtrusionPathSloped not implemented");
		return;
	}

	ExtrusionMultiPath *multipath = dynamic_cast<ExtrusionMultiPath*>(extr);
	if (multipath != nullptr) {
		contour_extrusion_multipath(region, surface, *multipath);
		return;
	}

	ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(extr);
	if (path != nullptr) {
		contour_extrusion_path(region, surface, *path);
		return;
	}

	ExtrusionLoop *loop = dynamic_cast<ExtrusionLoop*>(extr);
	if (loop != nullptr) {
		contour_extrusion_loop(region, surface, *loop);
		return;
	}

	const ExtrusionLoopSloped *loop_sloped = dynamic_cast<const ExtrusionLoopSloped*>(extr);
	if (loop_sloped != nullptr) {
		throw RuntimeError("ExtrusionLoopSloped not implemented");
		return;
	}

	ExtrusionEntityCollection *collection = dynamic_cast<ExtrusionEntityCollection*>(extr);
	if (collection != nullptr) {
		contour_extrusion_entitiy_collection(region, surface, *collection);
		return;
	}

	throw RuntimeError("ContourZ: ExtrusionEntity type not implemented: " + std::string(typeid(*extr).name()));
	return;
}

// An empty role list visits every entity (contour_extrusion_path decides per path and per point).
static void handle_extrusion_collection(LayerRegion *region, const ContourZSurface &surface, ExtrusionEntityCollection &collection, std::initializer_list<ExtrusionRole> roles) {
    for (ExtrusionEntity* extr : collection.entities) {
        if (roles.size() != 0 && !contains(roles, extr->role())) {
			continue;
		}

		contour_extrusion_entity(region, surface, extr);
    }
}

void Layer::make_contour_z(const sla::IndexedMesh &mesh, const std::vector<ContourZCeiling> &ceilings)
{
    const std::shared_ptr<const NonplanarZaaLimits> np = this->object() != nullptr ? nonplanar_top_zaa_limits(*this->object()) : nullptr;
    const ContourZSurface surface{mesh, ceilings, np.get()};
	for (LayerRegion *region : this->regions()) {
        if (!region->region().config().zaa_enabled)
            continue;

        // UNLAYERED_MOD: nonplanar layers -- with curved shells every fill and wall path is offered: the layer under a band
        // is lowered onto it whatever its role (solid infill, gap fill, bridges as well).
        if (np != nullptr) {
            handle_extrusion_collection(region, surface, region->fills, {});
            handle_extrusion_collection(region, surface, region->perimeters, {});
        } else {
            handle_extrusion_collection(region, surface, region->fills, {erTopSolidInfill, erIroning, erPerimeter, erExternalPerimeter, erMixed});
            handle_extrusion_collection(region, surface, region->perimeters, {erPerimeter, erExternalPerimeter, erMixed});
        }
    }
}
} // namespace Slic3r
