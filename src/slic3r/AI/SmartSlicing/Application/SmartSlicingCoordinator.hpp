#pragma once

#include "slic3r/AI/SmartSlicing/Domain/WorkflowState.hpp"
#include "slic3r/AI/SmartSlicing/Ports/IOrcaWorkspace.hpp"
#include "slic3r/AI/SmartSlicing/Ports/ITrialSliceExecutor.hpp"
#include "slic3r/AI/SmartSlicing/Ports/IOfficialSliceGateway.hpp"
#include "slic3r/AI/SmartSlicing/Ports/IWorkflowRuntimeStore.hpp"
#include "slic3r/AI/SmartSlicing/Domain/WorkflowResourceBudget.hpp"
#include "CandidatePlanningWorkflow.hpp"
#include "PrintabilityInspector.hpp"

#include <functional>
#include <chrono>

namespace Slic3r::AI::SmartSlicing {

class SmartSlicingCoordinator
{
public:
    using Observer = std::function<void(const WorkflowSnapshot&)>;

    explicit SmartSlicingCoordinator(IOrcaWorkspace& workspace);
    SmartSlicingCoordinator(IOrcaWorkspace& workspace, ITrialSliceExecutor& trial_slice_executor);
    SmartSlicingCoordinator(IOrcaWorkspace& workspace, ITrialSliceExecutor& trial_slice_executor,
                            IOfficialSliceGateway& official_slice_gateway);

    const WorkflowSnapshot& snapshot() const { return m_snapshot; }
    void set_observer(Observer observer);
    bool set_runtime_store(IWorkflowRuntimeStore& runtime_store, bool recover = true);
    void set_resource_budget(WorkflowResourceBudget budget,
                             std::function<WorkflowResourceUsage()> usage_probe = {});

    void start();
    void cancel();
    bool refresh_revision();
    bool plan_and_slice_candidates(std::vector<SliceCandidate> proposals = {},
                                   CandidateGoal goal = CandidateGoal::Stability,
                                   bool defer_revision_checks = false);
    bool select_candidate(const CandidateId& candidate_id);
    bool retry_candidate(const CandidateId& candidate_id, bool defer_revision_checks = false);
    bool apply_selected_candidate();
    bool poll_official_slice();
    bool undo_applied_candidate();

private:
    void transition(WorkflowState state, std::string detail = {});
    bool workspace_revision_matches() const;
    void persist_runtime_state();
    std::string resource_violation(size_t candidate_count) const;

    IOrcaWorkspace& m_workspace;
    ITrialSliceExecutor* m_trial_slice_executor{nullptr};
    IOfficialSliceGateway* m_official_slice_gateway{nullptr};
    PrintabilityInspector m_inspector;
    CandidatePlanningWorkflow m_candidate_planner;
    WorkflowSnapshot m_snapshot;
    std::optional<WorkspaceRevision> m_applied_revision;
    Observer m_observer;
    WorkflowId m_last_workflow_id{0};
    IWorkflowRuntimeStore* m_runtime_store{nullptr};
    WorkflowResourceBudget m_resource_budget;
    std::function<WorkflowResourceUsage()> m_usage_probe;
    std::chrono::steady_clock::time_point m_started_at{std::chrono::steady_clock::now()};
};

} // namespace Slic3r::AI::SmartSlicing
