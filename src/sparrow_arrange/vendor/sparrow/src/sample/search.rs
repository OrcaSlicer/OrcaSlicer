use crate::consts::{PRE_REFINE_CD_R_STEPS, PRE_REFINE_CD_TL_RATIOS, SND_REFINE_CD_R_STEPS, SND_REFINE_CD_TL_RATIOS, UNIQUE_SAMPLE_THRESHOLD};
use crate::eval::sample_eval::{SampleEval, SampleEvaluator};
use crate::sample::best_samples::BestSamples;
use crate::sample::coord_descent::{refine_coord_desc, CDConfig};
use crate::sample::uniform_sampler::UniformBBoxSampler;
use jagua_rs::entities::{Item, Layout, PItemKey};
use jagua_rs::geometry::geo_enums::RotationRange;
use jagua_rs::geometry::DTransformation;
use log::debug;
use rand::Rng;

#[derive(Debug, Clone, Copy)]
pub struct SampleConfig {
    pub n_container_samples: usize,
    pub n_focussed_samples: usize,
    pub n_coord_descents: usize,
}

/// Algorithm 6 and Figure 7 from https://doi.org/10.48550/arXiv.2509.13329
pub fn search_placement(l: &Layout, item: &Item, ref_pk: Option<PItemKey>, mut evaluator: impl SampleEvaluator, sample_config: SampleConfig, rng: &mut impl Rng) -> (Option<(DTransformation, SampleEval)>, usize) {
    let item_min_dim = f32::min(item.shape_cd.bbox.width(), item.shape_cd.bbox.height());

    let mut best_samples = BestSamples::new(sample_config.n_coord_descents, item_min_dim * UNIQUE_SAMPLE_THRESHOLD);

    //Create the two uniform samplers, one focussed around the reference placement, one for the whole container
    let focussed_sampler = match ref_pk {
        Some(ref_pk) => {
            //Add the current placement (and evaluation) as a candidate
            let dt = l.placed_items[ref_pk].d_transf;
            let eval = evaluator.evaluate_sample(dt, Some(best_samples.upper_bound()));

            debug!("[S] Starting from: {:?}", (dt, eval));
            best_samples.report(dt, eval);

            //Create a uniform sampler focussed around the current placement
            let pi_bbox = l.placed_items[ref_pk].shape.bbox;
            UniformBBoxSampler::new(pi_bbox, item, l.container.outer_cd.bbox)
        }
        None => None,
    };
    let container_sampler = UniformBBoxSampler::new(l.container.outer_cd.bbox, item, l.container.outer_cd.bbox);

    //Perform the focussed sampling
    if let Some(focussed_sampler) = focussed_sampler {
        for _ in 0..sample_config.n_focussed_samples {
            let dt = focussed_sampler.sample(rng);
            let eval = evaluator.evaluate_sample(dt, Some(best_samples.upper_bound()));
            best_samples.report(dt, eval);
        }
    }

    //Perform the container-wide sampling
    if let Some(container_sampler) = container_sampler {
        for _ in 0..sample_config.n_container_samples {
            let dt = container_sampler.sample(rng);
            let eval = evaluator.evaluate_sample(dt, Some(best_samples.upper_bound()));
            best_samples.report(dt, eval);
        }
    }

    //Refine some of the best random samples to a local minimum in two steps:

    //1. Do a first refinement of all 'best samples' using coordinate descent
    for start in best_samples.samples.clone() {
        let descended = refine_coord_desc(start, &mut evaluator, prerefine_cd_config(item), rng);
        best_samples.report(descended.0, descended.1);
    }


    //2. Take the best one and do an even finer coordinate descent refinement
    let final_sample = best_samples.best().map(|s|
        refine_coord_desc(s, &mut evaluator, final_refine_cd_config(item), rng)
    );

    debug!("[S] {} samples evaluated, final: {:?}",evaluator.n_evals(),final_sample);
    (final_sample, evaluator.n_evals())
}

fn prerefine_cd_config(item: &Item) -> CDConfig {
    let item_min_dim = f32::min(item.shape_cd.bbox.width(), item.shape_cd.bbox.height());
    let wiggle = item.allowed_rotation == RotationRange::Continuous;
    CDConfig {
        t_step_init: item_min_dim * PRE_REFINE_CD_TL_RATIOS.0,
        t_step_limit: item_min_dim * PRE_REFINE_CD_TL_RATIOS.1,
        r_step_init: PRE_REFINE_CD_R_STEPS.0,
        r_step_limit: PRE_REFINE_CD_R_STEPS.1,
        wiggle,
    }
}

fn final_refine_cd_config(item: &Item) -> CDConfig {
    let item_min_dim = f32::min(item.shape_cd.bbox.width(), item.shape_cd.bbox.height());
    let wiggle = item.allowed_rotation == RotationRange::Continuous;
    CDConfig {
        t_step_init: item_min_dim * SND_REFINE_CD_TL_RATIOS.0,
        t_step_limit: item_min_dim * SND_REFINE_CD_TL_RATIOS.1,
        r_step_init: SND_REFINE_CD_R_STEPS.0,
        r_step_limit: SND_REFINE_CD_R_STEPS.1,
        wiggle
    }
}
