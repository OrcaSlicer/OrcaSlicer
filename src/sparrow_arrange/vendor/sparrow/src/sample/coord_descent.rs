use crate::consts::{CD_STEP_FAIL, CD_STEP_SUCCESS};
use crate::eval::sample_eval::{SampleEval, SampleEvaluator};
use jagua_rs::geometry::DTransformation;
use log::trace;
use rand::{Rng, RngExt};
use std::cmp::Ordering;
use std::fmt::Debug;

#[derive(Clone, Debug, Copy)]
pub struct CDConfig {
    /// Initial step size for the coordinate descent
    pub t_step_init: f32,
    /// Limit for the step size, below which no more candidates are generated
    pub t_step_limit: f32,
    /// Initial step size for the rotation wiggle axis
    pub r_step_init: f32,
    /// Limit for the rotation wiggle step size, below which no more candidates are generated
    pub r_step_limit: f32,
    /// Defines whether the wiggle axis (rotation) is enabled
    pub wiggle: bool,
}

/// Refines an initial 'sample' (transformation and evaluation) into a local minimum using a coordinate descent inspired algorithm.
pub fn refine_coord_desc(
    (init_dt, init_eval): (DTransformation, SampleEval),
    evaluator: &mut impl SampleEvaluator,
    cd_config: CDConfig,
    rng: &mut impl Rng,
) -> (DTransformation, SampleEval) {
    let n_evals_init = evaluator.n_evals();
    let init_pos = init_dt;
    
    // Initialize the coordinate descent.
    let mut cd = CoordinateDescent {
        pos: init_pos,
        eval: init_eval,
        axis: CDAxis::random(rng, cd_config.wiggle),
        t_steps: (cd_config.t_step_init, cd_config.t_step_init),
        t_step_limit: cd_config.t_step_limit,
        r_step: cd_config.r_step_init,
        r_step_limit: cd_config.r_step_limit,
        wiggle: cd_config.wiggle,
    };

    // From the CD state, ask for candidate positions to evaluate. If none provided, stop.
    while let Some(c) = cd.ask() {
        // Evaluate the candidates using the evaluator.
        let c_eval = c.map(|c| evaluator.evaluate_sample(c, Some(cd.eval)));
        
        let best = c.into_iter().zip(c_eval)
            .min_by_key(|(_, eval)| *eval)
            .expect("At least one candidate should be present");

        // Report the best candidate to the coordinate descent state.
        cd.tell(best, rng);
        trace!("CD: {:?}", cd);
        debug_assert!(evaluator.n_evals() - n_evals_init < 1000, "coordinate descent exceeded 1000 evals");
    }
    trace!("CD: {} evals, {} -> {}, eval: {:?}",evaluator.n_evals() - n_evals_init, init_pos, cd.pos, cd.eval);
    // Return the best transformation found by the coordinate descent.
    (cd.pos, cd.eval)
}

#[derive(Debug)]
struct CoordinateDescent {
    /// The current position in the coordinate descent
    pub pos: DTransformation,
    /// The current evaluation of the position
    pub eval: SampleEval,
    /// The current axis on which new candidates are generated
    pub axis: CDAxis,
    /// The current step size for x and y axes
    pub t_steps: (f32, f32),
    /// The current step size for the rotation wiggle axis
    pub r_step: f32,
    /// The limit for the step size, below which no more candidates are generated
    pub t_step_limit: f32,
    /// The limit for the rotation wiggle step size, below which no more candidates are generated
    pub r_step_limit: f32,
    /// Defines whether the wiggle axis is enabled
    pub wiggle: bool,
}

impl CoordinateDescent {

    /// Generates candidates to be evaluated. 
    pub fn ask(&self) -> Option<[DTransformation; 2]> {
        let (sx, sy) = self.t_steps;
        let sr = self.r_step;

        if sx < self.t_step_limit && sy < self.t_step_limit && (sr < self.r_step_limit || !self.wiggle) {
            // Stop generating candidates if both steps have reached the limit
            None
        } else {
            // Generate two candidates on either side of the current position, according to the active axis.
            let (tx, ty) = self.pos.translation();
            let r = self.pos.rotation();
            let transformations = match self.axis {
                CDAxis::Horizontal => [(tx + sx, ty, r), (tx - sx, ty, r)],
                CDAxis::Vertical => [(tx, ty + sy, r), (tx, ty - sy, r)],
                CDAxis::ForwardDiag => [(tx + sx, ty + sy, r), (tx - sx, ty - sy, r)],
                CDAxis::BackwardDiag => [(tx - sx, ty + sy, r), (tx + sx, ty - sy, r)],
                CDAxis::Wiggle => [(tx, ty, r + sr), (tx, ty, r - sr)]
            };
            
            let c = transformations.map(|(tx, ty, r)| {
                DTransformation::new(r, (tx, ty))
            });
            
            Some(c)
        }
    }
    
    /// Updates the coordinate descent state with the new position and evaluation.
    pub fn tell(&mut self, (pos, eval): (DTransformation, SampleEval), rng: &mut impl Rng) {
        // Check if the reported evaluation is better or worse than the current one.
        let eval_cmp = eval.cmp(&self.eval);
        let better = eval_cmp == Ordering::Less;
        let worse = eval_cmp == Ordering::Greater;

        if !worse {
            // Update the current position if not worse
            (self.pos, self.eval) = (pos, eval);
        }

        // Determine the step size multiplier depending on whether the new evaluation is better or worse.
        let m = if better { CD_STEP_SUCCESS } else { CD_STEP_FAIL };

        // Apply the step size multiplier to the relevant steps for the current axis
        match self.axis {
            CDAxis::Horizontal => self.t_steps.0 *= m,
            CDAxis::Vertical => self.t_steps.1 *= m,
            CDAxis::ForwardDiag | CDAxis::BackwardDiag => {
                //Since both axis are involved, adjust both steps but less severely
                self.t_steps.0 *= m.sqrt();
                self.t_steps.1 *= m.sqrt();
            }
            CDAxis::Wiggle => {
                self.r_step *= m;
            }
        }

        // Every time a state is not improved, the axis gets changed to a new random one.
        if !better {
            self.axis = CDAxis::random(rng, self.wiggle);
        }
    }
}

#[derive(Clone, Debug, Copy)]
enum CDAxis {
    /// Left and right
    Horizontal,
    /// Up and down
    Vertical,
    /// Up-right and down-left
    ForwardDiag,
    /// Up-left and down-right
    BackwardDiag,
    /// Wiggle left and right (if allowed)
    Wiggle,
}

impl CDAxis {
    fn random(rng: &mut impl Rng, rotate: bool) -> Self {
        let range = if rotate {
            0..6 // Include wiggle as a possible axis
        } else {
            0..4 // Exclude wiggle if not allowed
        };
        match rng.random_range(range) {
            0 => CDAxis::Horizontal,
            1 => CDAxis::Vertical,
            2 => CDAxis::ForwardDiag,
            3 => CDAxis::BackwardDiag,
            4..6 => CDAxis::Wiggle,
            _ => unreachable!(),
        }
    }
}