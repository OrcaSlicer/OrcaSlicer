#pragma once

#include "slic3r/AI/SmartSlicing/Ports/IOfficialSliceGateway.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace Slic3r::GUI {

struct OrcaApplyMutationResult
{
    bool success{false};
    bool workspace_mutated{false};
    std::string diagnostic_code;
};

class OrcaOfficialSliceGateway final : public AI::SmartSlicing::IOfficialSliceGateway
{
public:
    using RevisionFn = std::function<AI::SmartSlicing::WorkspaceRevision()>;
    using CompatibilityFn = std::function<std::string(const AI::SmartSlicing::SliceCandidate&)>;
    using ApplyFn = std::function<OrcaApplyMutationResult(const AI::SmartSlicing::SliceCandidate&)>;
    using ActionFn = std::function<bool()>;

    OrcaOfficialSliceGateway(RevisionFn revision, CompatibilityFn compatibility, ApplyFn apply,
                             ActionFn start_slice, ActionFn show_preview, ActionFn undo)
        : m_revision(std::move(revision))
        , m_compatibility(std::move(compatibility))
        , m_apply(std::move(apply))
        , m_start_slice(std::move(start_slice))
        , m_show_preview(std::move(show_preview))
        , m_undo(std::move(undo))
    {}

    AI::SmartSlicing::OfficialSliceResult
    prepare(const AI::SmartSlicing::SliceCandidate& candidate,
            const AI::SmartSlicing::WorkspaceRevision& expected_revision) override
    {
        if (!revision_matches(candidate, expected_revision))
            return rejected("stale_revision");
        std::string diagnostic;
        try {
            diagnostic = m_compatibility ? m_compatibility(candidate) : "compatibility_check_unavailable";
        } catch (...) {
            return rejected("compatibility_check_failed");
        }
        if (!diagnostic.empty())
            return rejected(diagnostic);
        return {AI::SmartSlicing::OfficialSlicePhase::Prepared, {}, false, false};
    }

    AI::SmartSlicing::OfficialSliceResult
    commit(const AI::SmartSlicing::SliceCandidate& candidate,
           const AI::SmartSlicing::WorkspaceRevision& expected_revision) override
    {
        if (!revision_matches(candidate, expected_revision))
            return rejected("stale_revision");
        m_pending = false;
        m_preview_shown = false;
        m_workspace_mutated = false;
        m_can_undo = false;
        m_applied_revision.reset();
        try {
            OrcaApplyMutationResult applied = m_apply ? m_apply(candidate) : OrcaApplyMutationResult{};
            m_workspace_mutated = applied.workspace_mutated;
            m_can_undo          = applied.workspace_mutated;
            if (m_can_undo && m_revision)
                m_applied_revision = m_revision();
            if (!applied.success) {
                m_last = {AI::SmartSlicing::OfficialSlicePhase::Failed,
                          applied.diagnostic_code.empty() ? "candidate_apply_failed" : applied.diagnostic_code,
                          m_workspace_mutated, m_can_undo};
                return m_last;
            }
            if (!m_start_slice || !m_start_slice()) {
                m_last = {AI::SmartSlicing::OfficialSlicePhase::Failed, "official_slice_not_started",
                          m_workspace_mutated, m_can_undo};
                return m_last;
            }
            m_pending = true;
            m_last = {AI::SmartSlicing::OfficialSlicePhase::Slicing, {}, m_workspace_mutated, m_can_undo};
            return m_last;
        } catch (...) {
            if (!m_applied_revision)
                m_can_undo = false;
            m_last = {AI::SmartSlicing::OfficialSlicePhase::Failed, "candidate_apply_exception",
                      m_workspace_mutated, m_can_undo};
            return m_last;
        }
    }

    AI::SmartSlicing::OfficialSliceResult poll() override
    {
        if (m_last.phase == AI::SmartSlicing::OfficialSlicePhase::Completed && !m_preview_shown) {
            m_preview_shown = true;
            if (!m_show_preview || !m_show_preview())
                m_last = {AI::SmartSlicing::OfficialSlicePhase::Failed, "preview_navigation_failed",
                          m_workspace_mutated, m_can_undo};
        }
        return m_last;
    }

    bool undo_last_apply() override
    {
        if (!m_can_undo || !m_applied_revision || !m_revision ||
            m_revision() != *m_applied_revision || !m_undo || !m_undo())
            return false;
        m_can_undo = false;
        m_workspace_mutated = false;
        m_pending = false;
        m_preview_shown = false;
        m_last = {AI::SmartSlicing::OfficialSlicePhase::Prepared, "apply_undone", false, false};
        return true;
    }

    void notify_slice_completed(bool success, std::string diagnostic_code = {})
    {
        if (!m_pending)
            return;
        m_pending = false;
        m_last = {success ? AI::SmartSlicing::OfficialSlicePhase::Completed :
                            AI::SmartSlicing::OfficialSlicePhase::Failed,
                  success ? std::string{} : (diagnostic_code.empty() ? "official_slice_failed" : std::move(diagnostic_code)),
                  m_workspace_mutated, m_can_undo};
    }

private:
    bool revision_matches(const AI::SmartSlicing::SliceCandidate& candidate,
                          const AI::SmartSlicing::WorkspaceRevision& expected_revision) const
    {
        try {
            return candidate.base_revision == expected_revision && m_revision && m_revision() == expected_revision;
        } catch (...) {
            return false;
        }
    }

    static AI::SmartSlicing::OfficialSliceResult rejected(std::string diagnostic_code)
    {
        return {AI::SmartSlicing::OfficialSlicePhase::Rejected, std::move(diagnostic_code), false, false};
    }

    RevisionFn m_revision;
    CompatibilityFn m_compatibility;
    ApplyFn m_apply;
    ActionFn m_start_slice;
    ActionFn m_show_preview;
    ActionFn m_undo;
    AI::SmartSlicing::OfficialSliceResult m_last;
    std::optional<AI::SmartSlicing::WorkspaceRevision> m_applied_revision;
    bool m_pending{false};
    bool m_workspace_mutated{false};
    bool m_can_undo{false};
    bool m_preview_shown{false};
};

} // namespace Slic3r::GUI
