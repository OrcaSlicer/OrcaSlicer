#pragma once

#include "SmartSlicingViewModel.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <thread>
#include <wx/scrolwin.h>
#include <wx/timer.h>

class wxButton;
class wxPanel;
class wxRadioButton;
class wxStaticText;
class wxCollapsiblePane;

namespace Slic3r::AI::SmartSlicing {
class SmartSlicingCoordinator;
}

namespace Slic3r::GUI {
class Plater;

class SmartSlicingPanel final : public wxScrolledWindow
{
public:
    using PlanCandidatesFn = std::function<std::vector<AI::SmartSlicing::SliceCandidate>()>;
    using CancelTrialFn = std::function<void()>;

    SmartSlicingPanel(wxWindow* parent, AI::SmartSlicing::SmartSlicingCoordinator& coordinator,
                      PlanCandidatesFn plan_candidates = {}, CancelTrialFn cancel_trial = {},
                      std::function<void()> add_model = {}, Plater* plater = nullptr);
    ~SmartSlicingPanel() override;
    void render(const SmartSlicingViewModel& view_model);

private:
    struct CandidateControls
    {
        wxPanel* panel{nullptr};
        wxRadioButton* selector{nullptr};
        wxStaticText* metrics{nullptr};
        wxStaticText* reason{nullptr};
        wxButton* retry{nullptr};
    };

    void on_revision_timer(wxTimerEvent& event);
    bool run_in_background(std::function<void()> work);

    AI::SmartSlicing::SmartSlicingCoordinator& m_coordinator;
    PlanCandidatesFn m_plan_candidates;
    CancelTrialFn m_cancel_trial;
    std::array<wxStaticText*, 4> m_stage_labels{};
    wxStaticText* m_summary{nullptr};
    wxStaticText* m_issues{nullptr};
    wxCollapsiblePane* m_diagnostics{nullptr};
    wxStaticText* m_diagnostic_text{nullptr};
    wxButton* m_add_model{nullptr};
    wxStaticText* m_p0_notice{nullptr};
    wxPanel* m_candidate_section{nullptr};
    std::array<CandidateControls, 3> m_candidate_controls{};
    std::array<std::string, 3> m_candidate_ids{};
    wxButton* m_keep_baseline{nullptr};
    wxButton* m_apply{nullptr};
    wxButton* m_undo_apply{nullptr};
    wxButton* m_start{nullptr};
    wxButton* m_cancel{nullptr};
    wxTimer m_revision_timer;
    bool m_can_plan_candidates{false};
    bool m_can_recheck{false};
    std::atomic<bool> m_worker_running{false};
    std::thread m_worker;
};

} // namespace Slic3r::GUI
