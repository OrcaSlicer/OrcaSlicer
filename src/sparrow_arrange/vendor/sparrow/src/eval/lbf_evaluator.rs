use crate::eval::sample_eval::{SampleEval, SampleEvaluator};
use jagua_rs::collision_detection::hazards::filter::NoFilter;
use jagua_rs::entities::Item;
use jagua_rs::entities::Layout;
use jagua_rs::geometry::geo_traits::TransformableFrom;
use jagua_rs::geometry::primitives::SPolygon;
use jagua_rs::geometry::DTransformation;

pub const X_MULTIPLIER: f32 = 10.0;
pub const Y_MULTIPLIER: f32 = 1.0;

/// Simple evaluator for the Left-Bottom-Fill constructor.
/// Basically either returns [SampleEval::Invalid] in case of any collision or [SampleEval::Clear] with a loss value
/// that rewards placements that are closer to the left-bottom corner of the container.
pub struct LBFEvaluator<'a> {
    layout: &'a Layout,
    item: &'a Item,
    shape_buff: SPolygon,
    n_evals: usize
}

impl<'a> LBFEvaluator<'a> {
    pub fn new(layout: &'a Layout, item: &'a Item) -> Self {
        Self {
            layout,
            item,
            shape_buff: item.shape_cd.as_ref().clone(),
            n_evals: 0
        }
    }
}

impl<'a> SampleEvaluator for LBFEvaluator<'a> {
    fn evaluate_sample(&mut self, dt: DTransformation, _upper_bound: Option<SampleEval>) -> SampleEval {
        self.n_evals += 1;
        let cde = self.layout.cde();
        let transf = dt.into();
        match cde.detect_surrogate_collision(self.item.shape_cd.surrogate(), &transf, &NoFilter) {
            true => SampleEval::Invalid, // Surrogate collides with something
            false => {
                self.shape_buff.transform_from(&self.item.shape_cd, &transf);
                match cde.detect_poly_collision(&self.shape_buff, &NoFilter) {
                    true => SampleEval::Invalid, // Exact shape collides with something
                    false => {
                        // No collisions
                        let poi = self.shape_buff.poi.center;
                        let bbox_corner = self.shape_buff.bbox.corners()[0];
                        let loss = X_MULTIPLIER * (poi.0 + bbox_corner.0) + Y_MULTIPLIER * (poi.1 + bbox_corner.1);
                        SampleEval::Clear{loss}
                    }
                }
            }
        }
    }

    fn n_evals(&self) -> usize {
        self.n_evals
    }
}
