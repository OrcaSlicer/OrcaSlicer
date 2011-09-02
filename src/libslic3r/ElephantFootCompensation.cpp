#include "clipper/clipper_z.hpp"

#include "libslic3r.h"
#include "ClipperUtils.hpp"
#include "EdgeGrid.hpp"
#include "ExPolygon.hpp"
#include "ElephantFootCompensation.hpp"
#include "Flow.hpp"
#include "Geometry.hpp"
#include "SVG.hpp"
#include "Utils.hpp"

#include <cmath>
#include <cassert>

// #define CONTOUR_DISTANCE_DEBUG_SVG

namespace Slic3r {

struct ResampledPoint {
	ResampledPoint(size_t idx_src, bool interpolated, double curve_parameter) : idx_src(idx_src), interpolated(interpolated), curve_parameter(curve_parameter) {}

	size_t		idx_src;
	// Is this point interpolated or initial?
	bool 		interpolated;
	// Euclidean distance along the curve from the 0th point.
	double		curve_parameter;
};

// Distance calculated using SDF (Shape Diameter Function).
// The distance is calculated by casting a fan of rays and measuring the intersection distance.
// Thus the calculation is relatively slow. For the Elephant foot compensation purpose, this distance metric does not avoid 
// pinching off small pieces of a contour, thus this function has been superseded by contour_distance2().
std::vector<float> contour_distance(const EdgeGrid::Grid &grid, const size_t idx_contour, const Slic3r::Points &contour, const std::vector<ResampledPoint> &resampled_point_parameters, double search_radius)
{
	assert(! contour.empty());
	assert(contour.size() >= 2);

	std::vector<float> out;

	if (contour.size() > 2) 
	{
#ifdef CONTOUR_DISTANCE_DEBUG_SVG
		static int iRun = 0;
		++ iRun;
		BoundingBox bbox = get_extents(contour);
		bbox.merge(grid.bbox());
		ExPolygon expoly_grid;
		expoly_grid.contour = Polygon(*grid.contours().front());
		for (size_t i = 1; i < grid.contours().size(); ++ i)
			expoly_grid.holes.emplace_back(Polygon(*grid.contours()[i]));
#endif
		struct Visitor {
			Visitor(const EdgeGrid::Grid &grid, const size_t idx_contour, const std::vector<ResampledPoint> &resampled_point_parameters, double dist_same_contour_reject) :
				grid(grid), idx_contour(idx_contour), resampled_point_parameters(resampled_point_parameters), dist_same_contour_reject(dist_same_contour_reject) {}

			void init(const size_t aidx_point_start, const Point &apt_start, Vec2d dir, const double radius) {
				this->idx_point_start = aidx_point_start;
				this->pt       = apt_start.cast<double>() + SCALED_EPSILON * dir;
				dir *= radius;
				this->pt_start = this->pt.cast<coord_t>();
				// Trim the vector by the grid's bounding box.
				const BoundingBox &bbox = this->grid.bbox();
				double t = 1.;
				for (size_t axis = 0; axis < 2; ++ axis) {
					double dx = std::abs(dir(axis));
					if (dx >= EPSILON) {
						double tedge = (dir(axis) > 0) ? (double(bbox.max(axis)) - SCALED_EPSILON - this->pt(axis)) : (this->pt(axis) - double(bbox.min(axis)) - SCALED_EPSILON);
						if (tedge < dx)
							t = std::min(t, tedge / dx);
					}
				}
				this->dir      = dir;
				if (t < 1.)
					dir *= t;
				this->pt_end   = (this->pt + dir).cast<coord_t>();
				this->t_min    = 1.;
				assert(this->grid.bbox().contains(this->pt_start) && this->grid.bbox().contains(this->pt_end));
			}

			bool operator()(coord_t iy, coord_t ix) {
				// Called with a row and colum of the grid cell, which is intersected by a line.
				auto cell_data_range = this->grid.cell_data_range(iy, ix);
				bool valid = true;
				for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second; ++ it_contour_and_segment) {
					// End points of the line segment and their vector.
					auto segment = this->grid.segment(*it_contour_and_segment);
					if (Geometry::segments_intersect(segment.first, segment.second, this->pt_start, this->pt_end)) {
						// The two segments intersect. Calculate the intersection.
						Vec2d pt2 = segment.first.cast<double>();
						Vec2d dir2 = segment.second.cast<double>() - pt2;
						Vec2d vptpt2 = pt - pt2;
						double denom = dir(0) * dir2(1) - dir2(0) * dir(1);

						if (std::abs(denom) >= EPSILON) {
							double t = cross2(dir2, vptpt2) / denom;
							assert(t > - EPSILON && t < 1. + EPSILON);
							bool this_valid = true;
							if (it_contour_and_segment->first == idx_contour) {
								// The intersected segment originates from the same contour as the starting point.
								// Reject the intersection if it is close to the starting point.
								// Find the start and end points of this segment
								double param_lo = resampled_point_parameters[idx_point_start].curve_parameter;
								double param_hi;
								double param_end = resampled_point_parameters.back().curve_parameter;
								{
									const EdgeGrid::Contour &contour = grid.contours()[it_contour_and_segment->first];
									size_t ipt = it_contour_and_segment->second;
									ResampledPoint key(ipt, false, 0.);
									auto lower = [](const ResampledPoint& l, const ResampledPoint r) { return l.idx_src < r.idx_src || (l.idx_src == r.idx_src && int(l.interpolated) > int(r.interpolated)); };
									auto it = std::lower_bound(resampled_point_parameters.begin(), resampled_point_parameters.end(), key, lower);
									assert(it != resampled_point_parameters.end() && it->idx_src == ipt && ! it->interpolated);
									double t2 = cross2(dir, vptpt2) / denom;
									assert(t2 > - EPSILON && t2 < 1. + EPSILON);
									if (contour.begin() + (++ ipt) == contour.end())
										param_hi = t2 * dir2.norm();
									else
										param_hi = it->curve_parameter + t2 * dir2.norm();
								}
								if (param_lo > param_hi)
									std::swap(param_lo, param_hi);
								assert(param_lo >= 0. && param_lo <= param_end);
								assert(param_hi >= 0. && param_hi <= param_end);
								this_valid = param_hi > param_lo + dist_same_contour_reject && param_hi - param_end < param_lo - dist_same_contour_reject;
							}
							if (t < this->t_min) {
								this->t_min = t;
								valid = this_valid;
							}
						}
					}
					if (! valid)
						this->t_min = 1.;
				}
				// Continue traversing the grid along the edge.
				return true;
			}

			const EdgeGrid::Grid 			   &grid;
			const size_t 		  				idx_contour;
			const std::vector<ResampledPoint>  &resampled_point_parameters;
			const double 						dist_same_contour_reject;

			size_t 								idx_point_start;
			Point			      				pt_start;
			Point				  				pt_end;
			Vec2d				  				pt;
			Vec2d                 				dir;
			// Minium parameter along the vector (pt_end - pt_start).
			double				  				t_min;
		} visitor(grid, idx_contour, resampled_point_parameters, search_radius);

		const Point *pt_this = &contour.back();
		size_t       idx_pt_this = contour.size() - 1;
		const Point *pt_prev = pt_this - 1;
		// perpenduclar vector
		auto		 perp    = [](const Vec2d& v) -> Vec2d { return Vec2d(v.y(), -v.x()); };
		Vec2d        vprev   = (*pt_this - *pt_prev).cast<double>().normalized();
		out.reserve(contour.size() + 1);
		for (const Point &pt_next : contour) {
			Vec2d    vnext   = (pt_next - *pt_this).cast<double>().normalized();
			Vec2d    dir     = - (perp(vprev) + perp(vnext)).normalized();
			Vec2d    dir_perp = perp(dir);
			double   cross   = cross2(vprev, vnext);
			double   dot     = vprev.dot(vnext);
			double   a       = (cross < 0 || dot > 0.5) ? (M_PI / 3.) : (0.48 * acos(std::min(1., - dot)));
			// Throw rays, collect distances.
			std::vector<double> distances;
			int num_rays = 15;

#ifdef CONTOUR_DISTANCE_DEBUG_SVG
			SVG svg(debug_out_path("contour_distance_raycasted-%d-%d.svg", iRun, &pt_next - contour.data()).c_str(), bbox);
			svg.draw(expoly_grid);
			svg.draw_outline(Polygon(contour), "blue", scale_(0.01));
			svg.draw(*pt_this, "red", coord_t(scale_(0.1)));
#endif /* CONTOUR_DISTANCE_DEBUG_SVG */

			for (int i = - num_rays + 1; i < num_rays; ++ i) {
				double angle = a * i / (int)num_rays;
				double c = cos(angle);
				double s = sin(angle);
				Vec2d  v = c * dir + s * dir_perp;
				visitor.init(idx_pt_this, *pt_this, v, search_radius);
				grid.visit_cells_intersecting_line(visitor.pt_start, visitor.pt_end, visitor);
				distances.emplace_back(visitor.t_min);
#ifdef CONTOUR_DISTANCE_DEBUG_SVG
				svg.draw(Line(visitor.pt_start, visitor.pt_end), "yellow", scale_(0.01));
				if (visitor.t_min < 1.) {
					Vec2d pt = visitor.pt + visitor.dir * visitor.t_min;
					svg.draw(Point(pt), "red", coord_t(scale_(0.1)));
				}
#endif /* CONTOUR_DISTANCE_DEBUG_SVG */
			}
#ifdef CONTOUR_DISTANCE_DEBUG_SVG
			svg.Close();
#endif /* CONTOUR_DISTANCE_DEBUG_SVG */
			std::sort(distances.begin(), distances.end());
#if 0
			double median = distances[distances.size() / 2];
			double standard_deviation = 0;
			for (double d : distances)
				standard_deviation += (d - median) * (d - median);
			standard_deviation = sqrt(standard_deviation / (distances.size() - 1));
			double avg = 0;
			size_t cnt = 0;
			for (double d : distances)
				if (d > median - standard_deviation - EPSILON && d < median + standard_deviation + EPSILON) {
					avg += d;
					++ cnt;
				}
			avg /= double(cnt);
			out.emplace_back(float(avg * search_radius));
#else
			out.emplace_back(float(distances.front() * search_radius));
#endif
#ifdef CONTOUR_DISTANCE_DEBUG_SVG
			printf("contour_distance_raycasted-%d-%d.svg - distance %lf\n", iRun, int(&pt_next - contour.data()), unscale<double>(out.back()));
#endif /* CONTOUR_DISTANCE_DEBUG_SVG */
			pt_this = &pt_next;
			idx_pt_this = &pt_next - contour.data();
			vprev   = vnext;
		}
		// Rotate the vector by one item.
		out.emplace_back(out.front());
		out.erase(out.begin());
	}

	return out;
}

// Contour distance by measuring the closest point of an ExPolygon stored inside the EdgeGrid, while filtering out points of the same contour
// at concave regions, or convex regions with low curvature (curvature is estimated as a ratio between contour length and chordal distance crossing the contour ends).
std::vector<float> contour_distance2(const EdgeGrid::Grid &grid, const size_t idx_contour, const Slic3r::Points &contour, const std::vector<ResampledPoint> &resampled_point_parameters, double compensation, double search_radius)
{
	assert(! contour.empty());
	assert(contour.size() >= 2);

	std::vector<float> out;

	if (contour.size() > 2) 
	{
#ifdef CONTOUR_DISTANCE_DEBUG_SVG
		static int iRun = 0;
		++ iRun;
		BoundingBox bbox = get_extents(contour);
		bbox.merge(grid.bbox());
		ExPolygon expoly_grid;
		expoly_grid.contour = Polygon(*grid.contours().front());
		for (size_t i = 1; i < grid.contours().size(); ++ i)
			expoly_grid.holes.emplace_back(Polygon(*grid.contours()[i]));
#endif
		struct Visitor {
			Visitor(const EdgeGrid::Grid &grid, const size_t idx_contour, const std::vector<ResampledPoint> &resampled_point_parameters, double dist_same_contour_accept, double dist_same_contour_reject) :
				grid(grid), idx_contour(idx_contour), contour(grid.contours()[idx_contour]), resampled_point_parameters(resampled_point_parameters), dist_same_contour_accept(dist_same_contour_accept), dist_same_contour_reject(dist_same_contour_reject) {}

			void init(const Points &contour, const Point &apoint) {
                this->idx_point  = &apoint - contour.data();
                this->point      = apoint;
                this->found      = false;
                this->dir_inside = this->dir_inside_at_point(contour, this->idx_point);
                this->distance   = std::numeric_limits<double>::max();
            }

			bool operator()(coord_t iy, coord_t ix) {
				// Called with a row and colum of the grid cell, which is intersected by a line.
				auto cell_data_range = this->grid.cell_data_range(iy, ix);
				for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second; ++ it_contour_and_segment) {
					// End points of the line segment and their vector.
					std::pair<const Point&, const Point&> segment = this->grid.segment(*it_contour_and_segment);
				    const Vec2d   v  = (segment.second - segment.first).cast<double>();
				    const Vec2d   va = (this->point - segment.first).cast<double>();
				    const double  l2 = v.squaredNorm();  // avoid a sqrt
				    const double  t  = (l2 == 0.0) ? 0. : std::clamp(va.dot(v) / l2, 0., 1.);
				    // Closest point from this->point to the segment.
				    const Vec2d   foot = segment.first.cast<double>() + t * v;
					const Vec2d   bisector = foot - this->point.cast<double>();
					const double  dist = bisector.norm();
				    if ((! this->found || dist < this->distance) && this->dir_inside.dot(bisector) > 0) {
						bool	accept = true;
					    if (it_contour_and_segment->first == idx_contour) {
							// Complex case: The closest segment originates from the same contour as the starting point.
							// Reject the closest point if its distance along the contour is reasonable compared to the current contour bisector (this->pt, foot).
							double param_lo = resampled_point_parameters[this->idx_point].curve_parameter;
							double param_hi;
							double param_end = resampled_point_parameters.back().curve_parameter;
							const EdgeGrid::Contour &contour = grid.contours()[it_contour_and_segment->first];
							const size_t		     ipt     = it_contour_and_segment->second;
							{
								ResampledPoint key(ipt, false, 0.);
								auto lower = [](const ResampledPoint& l, const ResampledPoint r) { return l.idx_src < r.idx_src || (l.idx_src == r.idx_src && int(l.interpolated) > int(r.interpolated)); };
								auto it = std::lower_bound(resampled_point_parameters.begin(), resampled_point_parameters.end(), key, lower);
								assert(it != resampled_point_parameters.end() && it->idx_src == ipt && ! it->interpolated);
								param_hi = t * sqrt(l2);
								if (contour.begin() + ipt + 1 < contour.end())
									param_hi += it->curve_parameter;
							}
							if (param_lo > param_hi)
								std::swap(param_lo, param_hi);
							assert(param_lo > - SCALED_EPSILON && param_lo <= param_end + SCALED_EPSILON);
							assert(param_hi > - SCALED_EPSILON && param_hi <= param_end + SCALED_EPSILON);
							double dist_along_contour = std::min(param_hi - param_lo, param_lo + param_end - param_hi);
							if (dist_along_contour < dist_same_contour_accept)
								accept = false;
							else if (dist < dist_same_contour_reject + SCALED_EPSILON) {
								// this->point is close to foot. This point will only be accepted if the path along the contour is significantly 
								// longer than the bisector. That is, the path shall not bulge away from the bisector too much.
								// Bulge is estimated by 0.6 of the circle circumference drawn around the bisector.
								// Test whether the contour is convex or concave.
								bool inside = 
									(t == 0.) ? this->inside_corner(contour, ipt, this->point) :
									(t == 1.) ? this->inside_corner(contour, contour.segment_idx_next(ipt), this->point) :
									this->left_of_segment(contour, ipt, this->point);
								accept = inside && dist_along_contour > 0.6 * M_PI * dist;
							}
					    }
					    if (accept && (! this->found || dist < this->distance)) {
					    	// Simple case: Just measure the shortest distance.
							this->distance = dist;
#ifdef CONTOUR_DISTANCE_DEBUG_SVG
							this->closest_point = foot.cast<coord_t>();
#endif /* CONTOUR_DISTANCE_DEBUG_SVG */
							this->found    = true;
					    }
					}
				}
				// Continue traversing the grid.
				return true;
			}

			const EdgeGrid::Grid 			   &grid;
			const size_t 		  				idx_contour;
			const EdgeGrid::Contour			   &contour;
			const std::vector<ResampledPoint>  &resampled_point_parameters;
			const double                        dist_same_contour_accept;
			const double 						dist_same_contour_reject;

			size_t 								idx_point;
			Point			      				point;
			// Direction inside the contour from idx_point, not normalized.
			Vec2d								dir_inside;
			bool 								found;
			double 								distance;
#ifdef CONTOUR_DISTANCE_DEBUG_SVG
			Point 								closest_point;
#endif /* CONTOUR_DISTANCE_DEBUG_SVG */

		private:
			static Vec2d dir_inside_at_point(const Points &contour, size_t i) {
				size_t iprev = prev_idx_modulo(i, contour);
				size_t inext = next_idx_modulo(i, contour);
				Vec2d v1 = (contour[i] - contour[iprev]).cast<double>();
				Vec2d v2 = (contour[inext] - contour[i]).cast<double>();
				return Vec2d(- v1.y() - v2.y(), v1.x() + v2.x());
			}
			static Vec2d dir_inside_at_segment(const Points& contour, size_t i) {
				size_t inext = next_idx_modulo(i, contour);
				Vec2d v = (contour[inext] - contour[i]).cast<double>();
				return Vec2d(- v.y(), v.x());
			}

            static bool inside_corner(const EdgeGrid::Contour &contour, size_t i, const Point &pt_oposite)
            {
                const Vec2d pt         = pt_oposite.cast<double>();
                const Point &pt_prev   = contour.segment_prev(i);
                const Point &pt_this   = contour.segment_start(i);
                const Point &pt_next   = contour.segment_end(i);
                Vec2d       v1         = (pt_this - pt_prev).cast<double>();
                Vec2d       v2         = (pt_next - pt_this).cast<double>();
                bool        left_of_v1 = cross2(v1, pt - pt_prev.cast<double>()) > 0.;
                bool        left_of_v2 = cross2(v2, pt - pt_this.cast<double>()) > 0.;
                return cross2(v1, v2) > 0 ? left_of_v1 && left_of_v2 : // convex corner
                                            left_of_v1 || left_of_v2;                   // concave corner
            }

            static bool left_of_segment(const EdgeGrid::Contour &contour, size_t i, const Point &pt_oposite)
            {
                const Vec2d  pt      = pt_oposite.cast<double>();
                const Point &pt_this = contour.segment_start(i);
                const Point &pt_next = contour.segment_end(i);
                Vec2d        v       = (pt_next - pt_this).cast<double>();
                return cross2(v, pt - pt_this.cast<double>()) > 0.;
            }
		} visitor(grid, idx_contour, resampled_point_parameters, 0.5 * compensation * M_PI, search_radius);

		out.reserve(contour.size());
		Point radius_vector(search_radius, search_radius);
		for (const Point &pt : contour) {
			visitor.init(contour, pt);
			grid.visit_cells_intersecting_box(BoundingBox(pt - radius_vector, pt + radius_vector), visitor);
			out.emplace_back(float(visitor.found ? std::min(visitor.distance, search_radius) : search_radius));

#if 0
//#ifdef CONTOUR_DISTANCE_DEBUG_SVG
			if (out.back() < search_radius) {
				SVG svg(debug_out_path("contour_distance_filtered-%d-%d.svg", iRun, int(&pt - contour.data())).c_str(), bbox);
				svg.draw(expoly_grid);
				svg.draw_outline(Polygon(contour), "blue", scale_(0.01));
				svg.draw(pt, "green", coord_t(scale_(0.1)));
				svg.draw(visitor.closest_point, "red", coord_t(scale_(0.1)));
				printf("contour_distance_filtered-%d-%d.svg - distance %lf\n", iRun, int(&pt - contour.data()), unscale<double>(out.back()));
			}
#endif /* CONTOUR_DISTANCE_DEBUG_SVG */
		}
#ifdef CONTOUR_DISTANCE_DEBUG_SVG
		if (out.back() < search_radius) {
			SVG svg(debug_out_path("contour_distance_filtered-final-%d.svg", iRun).c_str(), bbox);
			svg.draw(expoly_grid);
			svg.draw_outline(Polygon(contour), "blue", scale_(0.01));
			for (size_t i = 0; i < contour.size(); ++ i)
				svg.draw(contour[i], out[i] < float(search_radius - SCALED_EPSILON) ? "red" : "green", coord_t(scale_(0.1)));
		}
#endif /* CONTOUR_DISTANCE_DEBUG_SVG */
	}

	return out;
}

Points resample_polygon(const Points &contour, double dist, std::vector<ResampledPoint> &resampled_point_parameters)
{
	Points out;
	out.reserve(contour.size());
	resampled_point_parameters.reserve(contour.size());
    if (contour.size() > 2) {
    	Vec2d  pt_prev  = contour.back().cast<double>();
    	for (const Point &pt : contour) {
			size_t idx_this = &pt - contour.data();
    		const Vec2d  pt_this = pt.cast<double>();
    		const Vec2d  v		 = pt_this - pt_prev;
			const double l		 = v.norm();
    		const size_t n		 = size_t(ceil(l / dist));
			const double l_step  = l / n;
    		for (size_t i = 1; i < n; ++ i) {
				double interpolation_parameter = double(i) / n;
    			Vec2d new_pt = pt_prev + v * interpolation_parameter;
    			out.emplace_back(new_pt.cast<coord_t>());
				resampled_point_parameters.emplace_back(idx_this, true, l_step);
			}
    		out.emplace_back(pt);
			resampled_point_parameters.emplace_back(idx_this, false, l_step);
    		pt_prev = pt_this;
    	}
		for (size_t i = 1; i < resampled_point_parameters.size(); ++i)
			resampled_point_parameters[i].curve_parameter += resampled_point_parameters[i - 1].curve_parameter;
    }
    return out;
}

#if 0
static inline void smooth_compensation(std::vector<float> &compensation, float strength, size_t num_iterations)
{
	std::vector<float> out(compensation);
	for (size_t iter = 0; iter < num_iterations; ++ iter) {
		for (size_t i = 0; i < compensation.size(); ++ i) {
			float prev = prev_value_modulo(i, compensation);
			float next = next_value_modulo(i, compensation);
			float laplacian = compensation[i] * (1.f - strength) + 0.5f * strength * (prev + next);
			// Compensations are negative. Only apply the laplacian if it leads to lower compensation.
			out[i] = std::max(laplacian, compensation[i]);
		}
		out.swap(compensation);
	}
}
#endif

static inline void smooth_compensation_banded(const Points &contour, float band, std::vector<float> &compensation, float strength, size_t num_iterations)
{
	assert(contour.size() == compensation.size());
	assert(contour.size() > 2);
	std::vector<float> out(compensation);
	float dist_min2 = band * band;
	static constexpr bool use_min = false;
	for (size_t iter = 0; iter < num_iterations; ++ iter) {
		for (int i = 0; i < int(compensation.size()); ++ i) {
			const Vec2f  pthis = contour[i].cast<float>();
			
			int		j     = prev_idx_modulo(i, contour);
			Vec2f	pprev = contour[j].cast<float>();
			float	prev  = compensation[j];
			float	l2    = (pthis - pprev).squaredNorm();
			if (l2 < dist_min2) {
				float l = sqrt(l2);
				int jprev = std::exchange(j, prev_idx_modulo(j, contour));
				while (j != i) {
					const Vec2f pp = contour[j].cast<float>();
					const float lthis = (pp - pprev).norm();
					const float lnext = l + lthis;
					if (lnext > band) {
						// Interpolate the compensation value.
						prev = use_min ?
							std::min(prev, lerp(compensation[jprev], compensation[j], (band - l) / lthis)) :
							lerp(compensation[jprev], compensation[j], (band - l) / lthis);
						break;
					}
					prev  = use_min ? std::min(prev, compensation[j]) : compensation[j];
					pprev = pp;
					l     = lnext;
					jprev = std::exchange(j, prev_idx_modulo(j, contour));
				}
			}

			j = next_idx_modulo(i, contour);
			pprev = contour[j].cast<float>();
			float next = compensation[j];
			l2 = (pprev - pthis).squaredNorm();
			if (l2 < dist_min2) {
				float l = sqrt(l2);
				int jprev = std::exchange(j, next_idx_modulo(j, contour));
				while (j != i) {
					const Vec2f pp = contour[j].cast<float>();
					const float lthis = (pp - pprev).norm();
					const float lnext = l + lthis;
					if (lnext > band) {
						// Interpolate the compensation value.
						next = use_min ?
							std::min(next, lerp(compensation[jprev], compensation[j], (band - l) / lthis)) :
							lerp(compensation[jprev], compensation[j], (band - l) / lthis);
						break;
					}
					next  = use_min ? std::min(next, compensation[j]) : compensation[j];
					pprev = pp;
					l     = lnext;
					jprev = std::exchange(j, next_idx_modulo(j, contour));
				}
			}

			float laplacian = compensation[i] * (1.f - strength) + 0.5f * strength * (prev + next);
			// Compensations are negative. Only apply the laplacian if it leads to lower compensation.
			out[i] = std::max(laplacian, compensation[i]);
		}
		out.swap(compensation);
	}
}

#ifndef NDEBUG
static bool validate_expoly_orientation(const ExPolygon &expoly)
{
	bool valid = expoly.contour.is_counter_clockwise();
    for (auto &h : expoly.holes) 
		valid &= h.is_clockwise();
	return valid;
}
#endif /* NDEBUG */

ExPolygon elephant_foot_compensation(const ExPolygon &input_expoly, double min_contour_width, const double compensation)
{	
	assert(validate_expoly_orientation(input_expoly));

	double scaled_compensation = scale_(compensation);
    min_contour_width = scale_(min_contour_width);
	double min_contour_width_compensated = min_contour_width + 2. * scaled_compensation;
	// Make the search radius a bit larger for the averaging in contour_distance over a fan of rays to work.
	double search_radius = min_contour_width_compensated + min_contour_width * 0.5;

	BoundingBox bbox = get_extents(input_expoly.contour);
	Point 		bbox_size = bbox.size();
	ExPolygon   out;
	if (bbox_size.x() < min_contour_width_compensated + SCALED_EPSILON ||
		bbox_size.y() < min_contour_width_compensated + SCALED_EPSILON ||
		input_expoly.area() < min_contour_width_compensated * min_contour_width_compensated * 5.)
	{
		// The contour is tiny. Don't correct it.
		out = input_expoly;
	}
	else
	{
		EdgeGrid::Grid grid;
		ExPolygon simplified = input_expoly.simplify(SCALED_EPSILON).front();
		assert(validate_expoly_orientation(simplified));
		BoundingBox bbox = get_extents(simplified.contour);
		bbox.offset(SCALED_EPSILON);
		grid.set_bbox(bbox);
		grid.create(simplified, coord_t(0.7 * search_radius));
		std::vector<std::vector<float>> deltas;
		deltas.reserve(simplified.holes.size() + 1);
		ExPolygon resampled(simplified);
		double resample_interval = scale_(0.5);
		for (size_t idx_contour = 0; idx_contour <= simplified.holes.size(); ++ idx_contour) {
			Polygon &poly = (idx_contour == 0) ? resampled.contour : resampled.holes[idx_contour - 1];
			std::vector<ResampledPoint> resampled_point_parameters;
			poly.points = resample_polygon(poly.points, resample_interval, resampled_point_parameters);
			assert(poly.is_counter_clockwise() == (idx_contour == 0));
			std::vector<float> dists = contour_distance2(grid, idx_contour, poly.points, resampled_point_parameters, scaled_compensation, search_radius);
			for (float &d : dists) {
	//			printf("Point %d, Distance: %lf\n", int(&d - dists.data()), unscale<double>(d));
				// Convert contour width to available compensation distance.
				if (d < min_contour_width)
					d = 0.f;
				else if (d > min_contour_width_compensated)
					d = - float(scaled_compensation);
				else
					d = - (d - float(min_contour_width)) / 2.f;
				assert(d >= - float(scaled_compensation) && d <= 0.f);
			}
	//		smooth_compensation(dists, 0.4f, 10);
			smooth_compensation_banded(poly.points, float(0.8 * resample_interval), dists, 0.3f, 3);
			deltas.emplace_back(dists);
		}

		ExPolygons out_vec = variable_offset_inner_ex(resampled, deltas, 2.);
		if (out_vec.size() == 1)
			out = std::move(out_vec.front());
		else {
			// Something went wrong, don't compensate.
			out = input_expoly;
#ifdef TESTS_EXPORT_SVGS
			if (out_vec.size() > 1) {
				static int iRun = 0;
				SVG::export_expolygons(debug_out_path("elephant_foot_compensation-many_contours-%d.svg", iRun ++).c_str(),
					{ { { input_expoly },   { "gray", "black", "blue", coord_t(scale_(0.02)), 0.5f, "black", coord_t(scale_(0.05)) } },
					  { { out_vec },		{ "gray", "black", "blue", coord_t(scale_(0.02)), 0.5f, "black", coord_t(scale_(0.05)) } } });
			}
#endif /* TESTS_EXPORT_SVGS */
			assert(out_vec.size() == 1);
		}
	}
    
	assert(validate_expoly_orientation(out));
	return out;
}

ExPolygon  elephant_foot_compensation(const ExPolygon  &input, const Flow &external_perimeter_flow, const double compensation)
{
    // The contour shall be wide enough to apply the external perimeter plus compensation on both sides.
    double min_contour_width = double(external_perimeter_flow.width() + external_perimeter_flow.spacing());
    return elephant_foot_compensation(input, min_contour_width, compensation);
}

ExPolygons elephant_foot_compensation(const ExPolygons &input, const Flow &external_perimeter_flow, const double compensation)
{
	ExPolygons out;
	out.reserve(input.size());
	for (const ExPolygon &expoly : input)
		out.emplace_back(elephant_foot_compensation(expoly, external_perimeter_flow, compensation));
	return out;
}

ExPolygons elephant_foot_compensation(const ExPolygons &input, double min_contour_width, const double compensation)
{
	ExPolygons out;
	out.reserve(input.size());
	for (const ExPolygon &expoly : input)
		out.emplace_back(elephant_foot_compensation(expoly, min_contour_width, compensation));
	return out;
}

} // namespace Slic3r
