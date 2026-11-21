use crate::eval::sample_eval::SampleEval;
use itertools::Itertools;
use jagua_rs::geometry::DTransformation;
use std::f32::consts::PI;
use std::fmt::Debug;

/// Data structure to store the N best samples, automatically keeps them sorted and evicts the worst.
/// It makes sure that no two included samples are too similar.
/// Also provides an upper bound in loss value for acceptance of new samples.
#[derive(Debug, Clone)]
pub struct BestSamples {
    pub size: usize,
    pub samples: Vec<(DTransformation, SampleEval)>,
    pub unique_thresh: f32,
}

impl BestSamples {
    pub fn new(size: usize, unique_thresh: f32) -> Self {
        Self {
            size,
            samples: vec![],
            unique_thresh,
        }
    }

    pub fn report(&mut self, dt: DTransformation, eval: SampleEval) -> bool {
        let accept = match eval < self.upper_bound() {
            false => false,
            true => {
                let any_similar = self.samples.iter()
                    .any(|(d, _)| dtransfs_are_similar(*d, dt, self.unique_thresh, self.unique_thresh));

                match any_similar {
                    false => { //no similar sample found, evict worst and accept
                        if self.samples.len() == self.size {
                            self.samples.pop();
                        }
                        true
                    }
                    true => { //at least one similar sample exists
                        let better_than_all_similar = self.samples.iter()
                            .filter(|(d, _)| dtransfs_are_similar(*d, dt, self.unique_thresh, self.unique_thresh))
                            .all(|(_, sim_eval)| eval < *sim_eval);

                        if better_than_all_similar {
                            //evict all similar samples
                            self.samples.retain(|(d, _)| !dtransfs_are_similar(*d, dt, self.unique_thresh, self.unique_thresh));
                            true
                        }
                        else {
                            false
                        }
                    }
                }
            }
        };
        if accept {
            self.samples.push((dt, eval));
            self.samples.sort_by_key(|(_, eval)| *eval);
            debug_assert!(
                self.samples.iter()
                    .filter(|(_, eval)| *eval != SampleEval::Invalid)
                    .array_combinations().all(|[a, b]| {
                        !dtransfs_are_similar(a.0, b.0, self.unique_thresh, self.unique_thresh)
                    }
                ),
                "BestSamples: samples are not unique: {:?}", &self.samples
            );
            true
        }
        else{
            debug_assert!(self.samples.is_sorted_by_key(|(_, eval)| *eval));
            false
        }
    }

    pub fn best(&self) -> Option<(DTransformation, SampleEval)> {
        self.samples.first().cloned()
    }

    pub fn upper_bound(&self) -> SampleEval {
        if let Some((_, eval)) = self.samples.get(self.size - 1) {
            *eval
        } else {
            SampleEval::Invalid
        }
    }
}

pub fn dtransfs_are_similar(
    dt1: DTransformation,
    dt2: DTransformation,
    x_threshold: f32,
    y_threshold: f32,
) -> bool {
    let x_diff = f32::abs(dt1.translation().0 - dt2.translation().0);
    let y_diff = f32::abs(dt1.translation().1 - dt2.translation().1);

    if x_diff < x_threshold && y_diff < y_threshold {
        let r1 = dt1.rotation() % (2.0 * PI);
        let r2 = dt2.rotation() % (2.0 * PI);
        let angle_diff = f32::abs(r1 - r2);
        angle_diff < (1.0f32).to_radians()
    } else {
        false
    }
}
