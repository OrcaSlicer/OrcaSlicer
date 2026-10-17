use crate::eval::collision_loss::CollisionLossEvaluator;
use crate::eval::sample_eval::{SampleEval, SampleEvaluator};
use crate::quantify::tracker::CollisionTracker;
use jagua_rs::collision_detection::hazards::collector::BasicHazardCollector;
use jagua_rs::collision_detection::hazards::{HazKey, HazardEntity};
use jagua_rs::entities::{Item, Layout, PItemKey};
use jagua_rs::geometry::geo_traits::TransformableFrom;
use jagua_rs::geometry::primitives::SPolygon;
use jagua_rs::geometry::DTransformation;

pub struct SeparationEvaluator<'a> {
    layout: &'a Layout,
    item: &'a Item,
    collector: BasicHazardCollector,
    current_hazard: (HazKey, HazardEntity),
    loss_evaluator: CollisionLossEvaluator<'a>,
    shape_buff: SPolygon,
    n_evals: usize,
}

impl<'a> SeparationEvaluator<'a> {
    pub fn new(
        layout: &'a Layout,
        item: &'a Item,
        current_pk: PItemKey,
        ct: &'a CollisionTracker,
    ) -> Self {
        let current_haz_key = layout
            .cde()
            .haz_key_from_pi_key(current_pk)
            .expect("placed item should be registered in the CDE");
        let current_hazard = (
            current_haz_key,
            layout.cde().hazards_map[current_haz_key].entity,
        );

        Self {
            layout,
            item,
            collector: BasicHazardCollector::with_capacity(layout.placed_items.len() + 1),
            current_hazard,
            loss_evaluator: CollisionLossEvaluator::new(layout, ct, current_pk),
            shape_buff: item.shape_cd.as_ref().clone(),
            n_evals: 0,
        }
    }
}

impl<'a> SampleEvaluator for SeparationEvaluator<'a> {
    /// Evaluates a transformation. An upper bound can be provided to early terminate the process.
    /// Algorithm 7 from https://doi.org/10.48550/arXiv.2509.13329
    fn evaluate_sample(&mut self, dt: DTransformation, upper_bound: Option<SampleEval>) -> SampleEval {
        self.n_evals += 1;
        let cde = self.layout.cde();

        // Calculate an upper bound of quantification, above which samples are guaranteed to be rejected (because they are dominated by previous ones).
        let loss_bound = match upper_bound {
            Some(SampleEval::Collision { loss }) => loss,
            Some(SampleEval::Clear { .. }) => 0.0,
            _ => f32::INFINITY,
        };
        let shape = self
            .shape_buff
            .transform_from(self.item.shape_cd.as_ref(), &dt.compose());
        self.collector.clear();
        // Mark the moving item's existing hazard as already collected so traversal skips it.
        self.collector.insert(self.current_hazard.0, self.current_hazard.1);
        self.loss_evaluator.reload(loss_bound);

        let mut should_stop = |hazard| self.loss_evaluator.add(hazard, shape);
        let stopped_during_surrogate_check = cde.collect_surrogate_collisions_until(
            shape,
            &mut self.collector,
            &mut should_stop,
        );

        match stopped_during_surrogate_check {
            true => SampleEval::Invalid,
            false => {
                let stopped_during_precise_check = cde.collect_poly_collisions_until(
                    shape,
                    &mut self.collector,
                    &mut should_stop,
                );

                match stopped_during_precise_check {
                    true => SampleEval::Invalid,
                    false if self.collector.len() == 1 => SampleEval::Clear { loss: 0.0 },
                    false => SampleEval::Collision {
                        loss: self.loss_evaluator.loss(),
                    },
                }
            }
        }
    }

    fn n_evals(&self) -> usize {
        self.n_evals
    }
}
