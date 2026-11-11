use jagua_rs::probs::spp::entities::{SPInstance, SPSolution};

/// Trait for listeners that can receive solutions during the optimization process
pub trait SolutionListener {
    fn report(&mut self, report: ReportType, solution: &SPSolution, instance: &SPInstance);

    fn report_separation_progress(&mut self, _progress: SeparationProgress) {}

    fn report_separation_result(&mut self, _result: SeparationResult) {}

}

/// Progress within one call to [`crate::optimizer::separator::Separator::separate`].
/// Iteration zero describes the initial layout; later values describe completed iterations.
#[derive(Debug, Clone, Copy)]
pub struct SeparationProgress {
    pub strip_width: f32,
    pub density: f32,
    pub iteration: usize,
    pub min_loss: f32,
}

#[derive(Debug, Clone, Copy)]
pub struct SeparationResult {
    pub success: bool,
    pub elapsed_seconds: f32,
    pub total_evals: usize,
    pub total_moves: usize,
    pub iterations: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ReportType {
    /// Report contains an intermediate solution that is closer to feasibility than the previous one.
    ExplImproving,
}

/// A dummy implementation of the `SolutionListener` trait that does nothing.
pub struct DummySolListener;

impl SolutionListener for DummySolListener {
    fn report(&mut self, _report: ReportType, _solution: &SPSolution, _instance: &SPInstance) {
        // Do nothing
    }
}
