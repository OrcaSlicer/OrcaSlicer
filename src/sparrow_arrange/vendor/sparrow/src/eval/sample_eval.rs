use jagua_rs::geometry::DTransformation;
use jagua_rs::util::FPA;
use std::cmp::Ordering;

use SampleEval::{Clear, Collision, Invalid};

#[derive(Clone, Debug, PartialEq, Copy)]
pub enum SampleEval {
    /// No collisions occur
    Clear { loss: f32 },
    Collision{ loss: f32 },
    Invalid,
}

impl PartialOrd for SampleEval {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for SampleEval {
    fn cmp(&self, other: &Self) -> Ordering {
        match (self, other) {
            (Invalid, Invalid) => Ordering::Equal,
            (Invalid, _) => Ordering::Greater,
            (_, Invalid) => Ordering::Less,
            (Collision{..}, Clear{..}) => Ordering::Greater,
            (Clear{..}, Collision{..}) => Ordering::Less,
            (Collision{loss: l1}, Collision{loss: l2}) |
            (Clear{loss: l1}, Clear { loss: l2 }) => {
                FPA(*l1).partial_cmp(&FPA(*l2)).unwrap()
            }
        }
    }
}

impl Eq for SampleEval {}

/// Simple trait for types that can evaluate samples
pub trait SampleEvaluator {
    fn evaluate_sample(&mut self, dt: DTransformation, upper_bound: Option<SampleEval>) -> SampleEval;

    fn n_evals(&self) -> usize;
}