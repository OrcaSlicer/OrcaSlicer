use crate::consts::OVERLAP_PROXY_EPSILON_DIAM_RATIO;
use crate::quantify::overlap_proxy::overlap_area_proxy;
use jagua_rs::geometry::geo_traits::DistanceTo;
use jagua_rs::geometry::primitives::{Rect, SPolygon};

pub mod overlap_proxy;
mod pair_matrix;
pub mod tracker;

/// Quantifies a collision between two simple polygons.
/// Algorithm 4 from https://doi.org/10.48550/arXiv.2509.13329
#[inline(always)]
pub fn quantify_collision_poly_poly(s1: &SPolygon, s2: &SPolygon) -> f32 {
    let epsilon = f32::max(s1.diameter, s2.diameter) * OVERLAP_PROXY_EPSILON_DIAM_RATIO;

    let overlap_proxy = overlap_area_proxy(s1.surrogate(), s2.surrogate(), epsilon) + epsilon.powi(2);

    debug_assert!(overlap_proxy.is_normal());

    let penalty = calc_shape_penalty(s1, s2);

    overlap_proxy.sqrt() * penalty
}

pub fn calc_shape_penalty(s1: &SPolygon, s2: &SPolygon) -> f32 {
    // The shape-based penalty between two shapes is defined as the geometric mean of the square roots of their convex hull areas.
    let p1 = f32::sqrt(s1.surrogate().convex_hull_area);
    let p2 = f32::sqrt(s2.surrogate().convex_hull_area);
    (p1 * p2).sqrt()
}

/// Quantifies a collision between a simple polygon and the exterior of the container.
#[inline(always)]
pub fn quantify_collision_poly_container(s: &SPolygon, c_bbox: Rect) -> f32 {
    let s_bbox = s.bbox;
    let overlap = match Rect::intersection(s_bbox, c_bbox) {
        Some(r) => {
            //intersection exist, calculate the area of the intersection (+ a small value to ensure it is never zero)
            (s_bbox.area() - r.area()) + 0.0001 * s_bbox.area()
        }
        None => {
            //no intersection, guide towards intersection with container
            s_bbox.area() + s_bbox.centroid().distance_to(&c_bbox.centroid())
        }
    };
    debug_assert!(overlap.is_normal());

    let penalty = calc_shape_penalty(s, s);

    2.0 * overlap.sqrt() * penalty
}