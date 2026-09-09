#include "SmartSlicingCoordinator.hpp"
#include "ApplyWorkflow.hpp"
#include "TrialSlicingWorkflow.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace Slic3r::AI::SmartSlicing {

SmartSlicingCoordinator::SmartSlicingCoordinator(IOrcaWorkspace& workspace) : m_workspace(workspace) {}

SmartSlicingCoordinator::SmartSlicingCoordinator(IOrcaWorkspace& workspace, ITrialSliceExecutor& trial_slice_executor)
    : m_workspace(workspace), m_trial_slice_executor(&trial_slice_executor)
{}

SmartSlicingCoordinator::SmartSlicingCoordinator(IOrcaWorkspace& workspace, ITrialSliceExecutor& trial_slice_executor,
                                                 IOfficialSliceGateway& official_slice_gateway)
    : m_workspace(workspace)
    , m_trial_slice_executor(&trial_slice_executor)
    , m_official_slice_gateway(&official_slice_gateway)
{}

void SmartSlicingCoordinator::set_observer(Observer observer)
{
    m_observer = std::move(observer);
    if (m_observer)
        m_observer(m_snapshot);
}

bool SmartSlicingCoordinator::set_runtime_store(IWorkflowRuntimeStore& runtime_store, bool recover)
{
    m_runtime_store = &runtime_store;
    if (!recover)
        return false;
    try {
        const std::optional<WorkflowRuntimeRecord> record = m_runtime_store->load();
        if (!record)
            return false;
        m_last_workflow_id = std::max(m_last_workflow_id, record->workflow_id);
        const WorkspaceRevision current = m_workspace.current_revision();
        m_runtime_store->clear(record->workflow_id);
        if (!record->revision.valid() || current != record->revision)
            return false;
        m_snapshot = {};
        m_snapshot.workflow_id = record->workflow_id;
        m_snapshot.state = WorkflowState::Failed;
        m_snapshot.detail = "interrupted_workflow_recovered";
        return true;
    } catch (...) {
        try {
            m_runtime_store->clear(0);
        } catch (...) {
        }
        return false;
    }
}

void SmartSlicingCoordinator::set_resource_budget(WorkflowResourceBudget budget,
                                                  std::function<WorkflowResourceUsage()> usage_probe)
{
    m_resource_budget = std::move(budget);
    m_usage_probe = std::move(usage_probe);
}

std::string SmartSlicingCoordinator::resource_violation(size_t candidate_count) const
{
    const WorkflowResourceUsage usage = m_usage_probe ? m_usage_probe() : WorkflowResourceUsage{};
    return workflow_budget_violation(m_resource_budget, candidate_count,
                                     std::chrono::steady_clock::now() - m_started_at, usage);
}

void SmartSlicingCoordinator::persist_runtime_state()
{
    if (m_runtime_store == nullptr || m_snapshot.workflow_id == 0)
        return;
    const bool terminal = m_snapshot.state == WorkflowState::Idle || m_snapshot.state == WorkflowState::Completed ||
                          m_snapshot.state == WorkflowState::Canceled || m_snapshot.state == WorkflowState::Stale ||
                          m_snapshot.state == WorkflowState::Failed;
    try {
        if (terminal) {
            m_runtime_store->clear(m_snapshot.workflow_id);
            return;
        }
        if (!m_snapshot.context)
            return;
        WorkflowRuntimeRecord record;
        record.workflow_id = m_snapshot.workflow_id;
        record.state = m_snapshot.state;
        record.revision = m_snapshot.context->revision;
        record.detail = m_snapshot.detail;
        record.updated_at_epoch_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        record.candidates.reserve(m_snapshot.candidates.size());
        for (const SliceCandidate& candidate : m_snapshot.candidates)
            record.candidates.push_back({candidate.id, candidate.goal, candidate.status});
        m_runtime_store->save(record);
    } catch (...) {
        // Runtime recovery is best effort and must never break normal slicing.
    }
}

void SmartSlicingCoordinator::transition(WorkflowState state, std::string detail)
{
    m_snapshot.state  = state;
    m_snapshot.detail = std::move(detail);
    if (m_observer)
        m_observer(m_snapshot);
    persist_runtime_state();
}

void SmartSlicingCoordinator::start()
{
    if (!m_snapshot.can_start())
        return;

    m_snapshot             = {};
    m_applied_revision.reset();
    m_snapshot.workflow_id = ++m_last_workflow_id;
    m_started_at = std::chrono::steady_clock::now();

    try {
        transition(WorkflowState::CapturingContext, "capturing_workspace");
        WorkspaceContext context = m_workspace.capture_context();
        if (!context.revision.valid()) {
            transition(WorkflowState::Failed, "invalid_workspace_revision");
            return;
        }
        m_snapshot.context = std::move(context);

        transition(WorkflowState::Preflighting, "inspecting_printability");
        PrintabilityReport report = m_inspector.inspect(*m_snapshot.context);
        if (report.revision != m_snapshot.context->revision) {
            transition(WorkflowState::Failed, "preflight_revision_mismatch");
            return;
        }
        m_snapshot.report = std::move(report);

        if (m_snapshot.report->has_blocking_issue() || m_snapshot.report->readiness == Readiness::Blocked)
            transition(WorkflowState::AwaitingRiskDecision, "printability_action_required");
        else
            transition(WorkflowState::ReadyForCandidatePlanning, "preflight_complete");
    } catch (const std::exception& error) {
        transition(WorkflowState::Failed, error.what());
    } catch (...) {
        transition(WorkflowState::Failed, "unknown_preflight_error");
    }
}

void SmartSlicingCoordinator::cancel()
{
    if (!m_snapshot.can_cancel())
        return;
    const bool trial_slice_running = m_snapshot.state == WorkflowState::TrialSlicingBaseline ||
                                     m_snapshot.state == WorkflowState::TrialSlicingCandidates;
    transition(WorkflowState::Canceling, "canceling");
    if (trial_slice_running && m_trial_slice_executor != nullptr)
        m_trial_slice_executor->cancel_trial_slice();
    m_snapshot.candidates.clear();
    m_snapshot.comparison.reset();
    m_snapshot.selected_candidate_id.clear();
    transition(WorkflowState::Canceled, "canceled");
}

bool SmartSlicingCoordinator::workspace_revision_matches() const
{
    return m_snapshot.context && m_workspace.current_revision() == m_snapshot.context->revision;
}

bool SmartSlicingCoordinator::plan_and_slice_candidates(std::vector<SliceCandidate> proposals, CandidateGoal goal,
                                                        bool defer_revision_checks)
{
    if (m_snapshot.state != WorkflowState::ReadyForCandidatePlanning || !m_snapshot.context ||
        m_trial_slice_executor == nullptr)
        return false;

    try {
        if (!defer_revision_checks && !workspace_revision_matches()) {
            transition(WorkflowState::Stale, "workspace_changed");
            return false;
        }

        transition(WorkflowState::PlanningCandidates, "planning_candidates");
        if (m_snapshot.state != WorkflowState::PlanningCandidates)
            return false;
        m_snapshot.goal       = goal;
        m_snapshot.candidates = m_candidate_planner.plan(*m_snapshot.context, std::move(proposals), goal);
        m_snapshot.comparison.reset();
        m_snapshot.selected_candidate_id.clear();
        if (m_snapshot.candidates.empty()) {
            transition(WorkflowState::Failed, "no_candidates");
            return false;
        }
        if (const std::string violation = resource_violation(m_snapshot.candidates.size()); !violation.empty()) {
            transition(WorkflowState::Failed, violation);
            return false;
        }

        for (size_t index = 0; index < m_snapshot.candidates.size(); ++index) {
            if (const std::string violation = resource_violation(m_snapshot.candidates.size()); !violation.empty()) {
                if (index == 0) {
                    transition(WorkflowState::Failed, violation);
                    return false;
                }
                for (size_t skipped = index; skipped < m_snapshot.candidates.size(); ++skipped) {
                    m_snapshot.candidates[skipped].status = CandidateStatus::Failed;
                    m_snapshot.candidates[skipped].diagnostic_code = violation;
                }
                break;
            }
            transition(index == 0 ? WorkflowState::TrialSlicingBaseline : WorkflowState::TrialSlicingCandidates,
                       index == 0 ? "trial_slicing_baseline" : "trial_slicing_candidate");
            const WorkflowState expected_state =
                index == 0 ? WorkflowState::TrialSlicingBaseline : WorkflowState::TrialSlicingCandidates;
            if (m_snapshot.state != expected_state)
                return false;

            SliceCandidate& candidate = m_snapshot.candidates[index];
            candidate.status          = CandidateStatus::TrialSlicing;
            TrialSliceResult result   = m_trial_slice_executor->execute_trial_slice(candidate);

            if (!defer_revision_checks && !workspace_revision_matches()) {
                for (SliceCandidate& planned : m_snapshot.candidates)
                    planned.status = CandidateStatus::Stale;
                m_snapshot.comparison.reset();
                transition(WorkflowState::Stale, "workspace_changed");
                return false;
            }
            if (result.status == TrialSliceStatus::Canceled && TrialSlicingWorkflow::result_matches(candidate, result)) {
                m_snapshot.candidates.clear();
                m_snapshot.comparison.reset();
                transition(WorkflowState::Canceled, "trial_slice_canceled");
                return false;
            }

            const bool accepted = TrialSlicingWorkflow::accept_result(candidate, std::move(result));
            if (index == 0 && !accepted) {
                transition(WorkflowState::Failed, "baseline_trial_failed");
                return false;
            }
            if (const std::string violation = resource_violation(m_snapshot.candidates.size()); !violation.empty()) {
                if (index == 0) {
                    for (size_t skipped = 1; skipped < m_snapshot.candidates.size(); ++skipped) {
                        m_snapshot.candidates[skipped].status = CandidateStatus::Failed;
                        m_snapshot.candidates[skipped].diagnostic_code = violation;
                    }
                    break;
                }
                for (size_t skipped = index + 1; skipped < m_snapshot.candidates.size(); ++skipped) {
                    m_snapshot.candidates[skipped].status = CandidateStatus::Failed;
                    m_snapshot.candidates[skipped].diagnostic_code = violation;
                }
                break;
            }
        }

        m_snapshot.comparison = compare_candidates(m_snapshot.candidates, goal);
        if (m_snapshot.comparison->recommended_candidate_id.empty()) {
            transition(WorkflowState::Failed, "no_comparable_candidate");
            return false;
        }
        m_snapshot.selected_candidate_id = m_snapshot.comparison->recommended_candidate_id;
        transition(WorkflowState::ReadyToApply, "candidates_ready");
        return true;
    } catch (const std::exception& error) {
        transition(WorkflowState::Failed, error.what());
    } catch (...) {
        transition(WorkflowState::Failed, "unknown_candidate_error");
    }
    return false;
}

bool SmartSlicingCoordinator::select_candidate(const CandidateId& candidate_id)
{
    if (m_snapshot.state != WorkflowState::ReadyToApply || candidate_id.empty())
        return false;
    if (!workspace_revision_matches()) {
        transition(WorkflowState::Stale, "workspace_changed");
        return false;
    }
    const auto candidate = std::find_if(m_snapshot.candidates.begin(), m_snapshot.candidates.end(),
                                        [&candidate_id](const SliceCandidate& item) {
                                            return item.id == candidate_id && item.status == CandidateStatus::Ready;
                                        });
    if (candidate == m_snapshot.candidates.end())
        return false;
    m_snapshot.selected_candidate_id = candidate_id;
    transition(WorkflowState::ReadyToApply, "candidate_selected");
    return true;
}

bool SmartSlicingCoordinator::retry_candidate(const CandidateId& candidate_id, bool defer_revision_checks)
{
    if (m_snapshot.state != WorkflowState::ReadyToApply || m_trial_slice_executor == nullptr || !m_snapshot.context)
        return false;
    auto candidate = std::find_if(m_snapshot.candidates.begin(), m_snapshot.candidates.end(),
                                  [&candidate_id](const SliceCandidate& item) {
                                      return item.id == candidate_id && item.status == CandidateStatus::Failed;
                                  });
    if (candidate == m_snapshot.candidates.end())
        return false;
    if (!defer_revision_checks && !workspace_revision_matches()) {
        transition(WorkflowState::Stale, "workspace_changed");
        return false;
    }

    transition(WorkflowState::TrialSlicingCandidates, "retrying_trial_slice");
    candidate->status = CandidateStatus::TrialSlicing;
    TrialSliceResult result = m_trial_slice_executor->execute_trial_slice(*candidate);
    if (!defer_revision_checks && !workspace_revision_matches()) {
        for (SliceCandidate& planned : m_snapshot.candidates)
            planned.status = CandidateStatus::Stale;
        m_snapshot.comparison.reset();
        m_snapshot.selected_candidate_id.clear();
        transition(WorkflowState::Stale, "workspace_changed");
        return false;
    }
    if (result.status == TrialSliceStatus::Canceled && TrialSlicingWorkflow::result_matches(*candidate, result)) {
        candidate->status          = CandidateStatus::Failed;
        candidate->metrics.reset();
        candidate->diagnostic_code = "retry_canceled";
        transition(WorkflowState::ReadyToApply, "retry_canceled");
        return false;
    }

    const bool accepted = TrialSlicingWorkflow::accept_result(*candidate, std::move(result));
    m_snapshot.comparison = compare_candidates(m_snapshot.candidates, m_snapshot.goal);
    if (m_snapshot.selected_candidate_id.empty() ||
        std::none_of(m_snapshot.candidates.begin(), m_snapshot.candidates.end(), [this](const SliceCandidate& item) {
            return item.id == m_snapshot.selected_candidate_id && item.status == CandidateStatus::Ready;
        }))
        m_snapshot.selected_candidate_id = m_snapshot.comparison->recommended_candidate_id;
    transition(WorkflowState::ReadyToApply, accepted ? "candidate_retry_succeeded" : "candidate_retry_failed");
    return accepted;
}

bool SmartSlicingCoordinator::apply_selected_candidate()
{
    if (m_snapshot.state != WorkflowState::ReadyToApply || m_official_slice_gateway == nullptr ||
        !m_snapshot.context || m_snapshot.selected_candidate_id.empty())
        return false;

    const auto candidate = std::find_if(m_snapshot.candidates.begin(), m_snapshot.candidates.end(), [this](const SliceCandidate& item) {
        return item.id == m_snapshot.selected_candidate_id && item.status == CandidateStatus::Ready;
    });
    if (candidate == m_snapshot.candidates.end())
        return false;

    m_applied_revision = m_snapshot.context->revision;
    WorkspaceRevision current_revision;
    try {
        current_revision = m_workspace.current_revision();
    } catch (...) {
        transition(WorkflowState::ApplyFailed, "apply_revision_unavailable");
        return false;
    }
    if (current_revision != m_snapshot.context->revision) {
        transition(WorkflowState::Stale, "workspace_changed");
        return false;
    }

    transition(WorkflowState::Applying, "applying_candidate");
    OfficialSliceResult result;
    try {
        result = ApplyWorkflow().start(*candidate, m_snapshot.context->revision, current_revision,
                                       *m_official_slice_gateway);
    } catch (...) {
        m_applied_revision.reset();
        transition(WorkflowState::ApplyFailed, "apply_gateway_exception");
        return false;
    }
    m_snapshot.can_undo_apply = result.can_undo;
    // The applied project may differ from the trial baseline. Monitor that new
    // revision after slicing, so later edits cannot retain a "completed" result.
    m_applied_revision.reset();
    if (!result.workspace_mutated)
        m_applied_revision = current_revision;
    else
        try { m_applied_revision = m_workspace.current_revision(); } catch (...) {}
    switch (result.phase) {
    case OfficialSlicePhase::Slicing:
        transition(WorkflowState::OfficialSlicing, "official_slicing");
        return true;
    case OfficialSlicePhase::Completed:
        transition(WorkflowState::Completed, "official_slice_complete");
        return true;
    case OfficialSlicePhase::Rejected:
        transition(result.diagnostic_code == "stale_revision" ? WorkflowState::Stale : WorkflowState::ApplyFailed,
                   result.diagnostic_code.empty() ? "apply_rejected" : result.diagnostic_code);
        return false;
    case OfficialSlicePhase::Failed:
        transition(WorkflowState::ApplyFailed,
                   result.diagnostic_code.empty() ? "official_slice_failed" : result.diagnostic_code);
        return false;
    case OfficialSlicePhase::Prepared:
        transition(WorkflowState::ApplyFailed, "commit_not_started");
        return false;
    }
    return false;
}

bool SmartSlicingCoordinator::poll_official_slice()
{
    if (m_snapshot.state != WorkflowState::OfficialSlicing || m_official_slice_gateway == nullptr)
        return false;
    OfficialSliceResult result;
    try {
        result = m_official_slice_gateway->poll();
    } catch (...) {
        transition(WorkflowState::ApplyFailed, "official_slice_poll_failed");
        return true;
    }
    m_snapshot.can_undo_apply        = result.can_undo;
    if (result.phase == OfficialSlicePhase::Completed) {
        transition(WorkflowState::Completed, "official_slice_complete");
        return true;
    }
    if (result.phase == OfficialSlicePhase::Failed || result.phase == OfficialSlicePhase::Rejected) {
        transition(WorkflowState::ApplyFailed,
                   result.diagnostic_code.empty() ? "official_slice_failed" : result.diagnostic_code);
        return true;
    }
    return false;
}

bool SmartSlicingCoordinator::undo_applied_candidate()
{
    if ((m_snapshot.state != WorkflowState::ApplyFailed && m_snapshot.state != WorkflowState::Completed) || !m_snapshot.can_undo_apply ||
        m_official_slice_gateway == nullptr)
        return false;
    try {
        if (!m_official_slice_gateway->undo_last_apply()) {
            m_snapshot.can_undo_apply = false;
            transition(WorkflowState::Stale, "apply_undo_unavailable");
            return false;
        }
    } catch (...) {
        transition(m_snapshot.state, "apply_undo_failed");
        return false;
    }
    m_snapshot.can_undo_apply = false;
    transition(WorkflowState::ReadyToApply, "apply_undone");
    return true;
}

bool SmartSlicingCoordinator::refresh_revision()
{
    if (m_snapshot.state == WorkflowState::OfficialSlicing)
        return poll_official_slice();
    const bool after_apply = m_snapshot.state == WorkflowState::Completed || m_snapshot.state == WorkflowState::ApplyFailed;
    if (!m_snapshot.context || (!m_snapshot.can_cancel() && !after_apply))
        return false;

    try {
        const auto current = m_workspace.current_revision();
        if (after_apply ? (m_applied_revision && current == *m_applied_revision) : current == m_snapshot.context->revision)
            return false;
    } catch (...) {
        // A transient capture failure must not turn a previously valid candidate
        // into stale. The next refresh will retry.
        return false;
    }

    for (SliceCandidate& candidate : m_snapshot.candidates)
        candidate.status = CandidateStatus::Stale;
    m_snapshot.comparison.reset();
    m_snapshot.selected_candidate_id.clear();
    m_snapshot.can_undo_apply = false;
    transition(WorkflowState::Stale, after_apply && !m_applied_revision ? "applied_revision_unavailable" : "workspace_changed");
    return true;
}

} // namespace Slic3r::AI::SmartSlicing
