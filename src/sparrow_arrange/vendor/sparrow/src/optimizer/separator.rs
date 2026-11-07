use crate::optimizer::worker::{SepStats, SeparatorWorker};
use crate::util::terminator::Terminator;
use crate::quantify::tracker::{CTSnapshot, CollisionTracker};
use crate::sample::search::SampleConfig;
use crate::util::listener::{ReportType, SeparationProgress, SeparationResult, SolutionListener};
use crate::FMT;
use itertools::Itertools;
use jagua_rs::entities::PItemKey;
use jagua_rs::probs::spp::entities::{SPInstance, SPProblem, SPSolution};
use jagua_rs::Instant;
use log::{debug, log, Level};
use ordered_float::OrderedFloat;
use rand::{RngExt, SeedableRng};
use rand::rngs::Xoshiro256PlusPlus;
use rayon::iter::IntoParallelRefMutIterator;
use rayon::iter::ParallelIterator;
use rayon::ThreadPool;

#[derive(Debug, Clone, Copy)]
pub struct SeparatorConfig {
    pub iter_no_imprv_limit: usize,
    pub strike_limit: usize,
    pub n_workers: usize,
    pub log_level: Level,
    pub sample_config: SampleConfig,
    // SPARROW_ARRANGE PATCH: number of movable items; ids at or above it are pinned obstacles.
    pub n_movable: usize,
}

pub struct Separator {
    pub instance: SPInstance,
    pub rng: Xoshiro256PlusPlus,
    pub prob: SPProblem,
    pub ct: CollisionTracker,
    pub workers: Vec<SeparatorWorker>,
    pub config: SeparatorConfig,
    pub thread_pool: Option<ThreadPool>,
}

impl Separator {
    pub fn new(instance: SPInstance, prob: SPProblem, mut rng: Xoshiro256PlusPlus, config: SeparatorConfig) -> Self {
        let ct = CollisionTracker::new(&prob.layout, config.n_movable); // SPARROW_ARRANGE PATCH
        let workers = (0..config.n_workers).map(|_|
            SeparatorWorker {
                instance: instance.clone(),
                prob: prob.clone(),
                ct: ct.clone(),
                rng: Xoshiro256PlusPlus::seed_from_u64(rng.random()),
                sample_config: config.sample_config,
            }).collect();

        let pool = if cfg!(target_arch = "wasm32") {
            // On wasm32, only the global thread pool is available
            None
        } else {
            // Create a local thread pool to keep using the same threads for the same optimization (helps the OS scheduler)
            Some(rayon::ThreadPoolBuilder::new().num_threads(config.n_workers).build().unwrap())
        };

        Self {
            prob,
            instance,
            rng,
            ct,
            workers,
            config,
            thread_pool: pool,
        }
    }

    /// Algorithm 9 from https://doi.org/10.48550/arXiv.2509.13329
    pub fn separate(&mut self, term: &impl Terminator, sol_listener: &mut impl SolutionListener) -> (SPSolution, CTSnapshot) {
        let mut min_loss_sol = (self.prob.save(), self.ct.save());
        let mut min_loss = self.ct.get_total_loss();
        let strip_width = self.prob.strip_width();
        let density = self.prob.density() * 100.0;
        let progress = |iteration, min_loss| SeparationProgress { strip_width, density, iteration, min_loss };
        sol_listener.report_separation_progress(progress(0, min_loss));
        log!(self.config.log_level,"[SEP] separating at width: {:.3} and loss: {} ", self.prob.strip_width(), FMT().fmt2(min_loss));

        let mut n_strikes = 0;
        let mut n_iter = 0;
        let mut sep_stats = SepStats { total_moves: 0, total_evals: 0 };
        let start = Instant::now();

        // As long as the strike limit is not reached, and the solution is not yet separated.
        'outer: while n_strikes < self.config.strike_limit && !term.kill() {
            let mut n_iter_no_improvement = 0;

            let initial_strike_loss = self.ct.get_total_loss();
            debug!("[SEP] [s:{n_strikes},i:{n_iter}]     init_l: {}",FMT().fmt2(initial_strike_loss));

            // SPARROW_ARRANGE PATCH: also poll the terminator here; a strike runs `iter_no_imprv_limit`
            // iterations, which is far longer than the caller's cancellation granularity.
            while n_iter_no_improvement < self.config.iter_no_imprv_limit && !term.kill() {
                let (loss_before, w_loss_before) = (self.ct.get_total_loss(), self.ct.get_total_weighted_loss(),);
                sep_stats += self.move_items_multi();
                let (loss, w_loss) = (self.ct.get_total_loss(), self.ct.get_total_weighted_loss(),);

                debug!("[SEP] [s:{n_strikes},i:{n_iter}] ( ) l: {} -> {}, wl: {} -> {}, (min l: {})", FMT().fmt2(loss_before), FMT().fmt2(loss), FMT().fmt2(w_loss_before), FMT().fmt2(w_loss), FMT().fmt2(min_loss));
                debug_assert!(w_loss <= w_loss_before * 1.001, "weighted loss should not increase: {} -> {}", FMT().fmt2(w_loss), FMT().fmt2(w_loss_before));

                if loss == 0.0 {
                    //All collisions are resolved
                    log!(self.config.log_level,"[SEP] [s:{n_strikes},i:{n_iter}] (S)  min_l: {}",FMT().fmt2(loss));
                    min_loss_sol = (self.prob.save(), self.ct.save());
                    sol_listener.report_separation_progress(progress(n_iter + 1, loss));
                    break 'outer;
                } else if loss < min_loss {
                    //Not all collisions are resolved, but we found a new 'best' solution
                    log!(self.config.log_level,"[SEP] [s:{n_strikes},i:{n_iter}] (*) min_l: {}",FMT().fmt2(loss));
                    sol_listener.report(ReportType::ExplImproving, &self.prob.save(), &self.instance);
                    if loss < min_loss * 0.98 {
                        //Reset the `iter_no_improvement` counter if the best solution is a substantial improvement
                        n_iter_no_improvement = 0;
                    }
                    min_loss_sol = (self.prob.save(), self.ct.save());
                    min_loss = loss;
                } else {
                    // No improvement this iteration
                    n_iter_no_improvement += 1;
                }

                sol_listener.report_separation_progress(progress(n_iter + 1, min_loss));
                // Update the GLS weights
                self.ct.update_weights();
                n_iter += 1;
            }

            if initial_strike_loss * 0.98 <= min_loss {
                // No substantial improvement during this attempt, add a strike
                n_strikes += 1;
            } else {
                // Substantial improvement, reset strike counter
                n_strikes = 0;
            }
            self.rollback(&min_loss_sol.0, Some(&min_loss_sol.1));
        }
        let secs = start.elapsed().as_secs_f32();
        log!(self.config.log_level, "[SEP] finished, evals/s: {} K, evals/move: {}, moves/s: {}, iter/s: {}, #workers: {}, total {:.3}s",
            (sep_stats.total_evals as f32/ (1000.0 * secs)) as usize,
            FMT().fmt2(sep_stats.total_evals as f32 / sep_stats.total_moves as f32),
            FMT().fmt2(sep_stats.total_moves as f32 / secs),
            FMT().fmt2(n_iter as f32 / secs),
            self.workers.len(),
            FMT().fmt2(secs),
        );
        sol_listener.report_separation_result(SeparationResult {
            success: self.ct.get_total_loss() == 0.0,
            elapsed_seconds: secs,
            total_evals: sep_stats.total_evals,
            total_moves: sep_stats.total_moves,
            iterations: n_iter,
        });

        // Return the best solution found: a feasible one if separation was successful, otherwise the 'least' infeasible one
        (min_loss_sol.0, min_loss_sol.1)
    }

    /// Algorithm 10 from https://doi.org/10.48550/arXiv.2509.13329
    fn move_items_multi(&mut self) -> SepStats {
        let master_sol = self.prob.save();

        // Define the parallel execution closure
        let mut separate_multi = || -> SepStats {
            self.workers.par_iter_mut().map(|worker| {
                // Sync the workers with the master
                worker.load(&master_sol, &self.ct);
                // Let all of them run `move_items` with unique random orderings in which the items are moved
                worker.move_items()
            }).sum()
        };

        // Execute the parallel separation either using the local thread pool or the global one
        let sep_report = match self.thread_pool.as_mut() {
            Some(pool) => pool.install(&mut separate_multi),
            None => separate_multi(),
        };

        debug!("[MOD] optimizers w_o's: {:?}",self.workers.iter().map(|opt| opt.ct.get_total_weighted_loss()).collect_vec());

        // Check what run yielded the best solution (lowest collision quantification)
        let (best_sol, best_ct) = self.workers.iter_mut()
            .min_by_key(|opt| OrderedFloat(opt.ct.get_total_weighted_loss()))
            .map(|opt| (opt.prob.save(), &opt.ct))
            .unwrap();

        // Load this 'best' solution into the master, effectively throwing away all other work.
        self.prob.restore(&best_sol);
        self.ct = best_ct.clone();

        sep_report
    }

    // SPARROW_ARRANGE PATCH: remove a placed item and resynchronise the tracker and the workers.
    /// Stock sparrow never removes items -- in strip packing the strip simply grows until
    /// everything fits. Packing into a fixed container needs the opposite move: when a set
    /// cannot be separated, shed an item and try again.
    pub fn evict_item(&mut self, pk: PItemKey) {
        self.prob.remove_item(pk);
        self.resync();
    }

    // SPARROW_ARRANGE PATCH: rebuild tracker and workers after the caller has changed the placed set
    /// directly (used when adding an item back into the layout).
    pub fn resync(&mut self) {
        self.ct = CollisionTracker::new(&self.prob.layout, self.config.n_movable);
        self.resync_workers();
    }

    // SPARROW_ARRANGE PATCH
    fn resync_workers(&mut self) {
        self.workers.iter_mut().for_each(|w| {
            *w = SeparatorWorker {
                instance: self.instance.clone(),
                prob: self.prob.clone(),
                ct: self.ct.clone(),
                rng: Xoshiro256PlusPlus::seed_from_u64(self.rng.random()),
                sample_config: self.config.sample_config,
            };
        });
    }

    pub fn rollback(&mut self, sol: &SPSolution, ots: Option<&CTSnapshot>) {
        debug_assert!(sol.strip_width() == self.prob.strip_width());
        self.prob.restore(sol);

        match ots {
            Some(ots) => {
                //if a snapshot of the tracker was provided, restore it
                self.ct.restore_but_keep_weights(ots, &self.prob.layout);
            }
            None => {
                //otherwise, rebuild it
                self.ct = CollisionTracker::new(&self.prob.layout, self.config.n_movable); // SPARROW_ARRANGE PATCH
            }
        }
    }
}
