#include "SmartSlicingFeatureHost.hpp"

#include "slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.hpp"
#include "slic3r/GUI/AI/Orca/OrcaParameterProposalAdapter.hpp"
#include "slic3r/GUI/AI/Orca/OrcaSmartSlicingAdapter.hpp"
#include "slic3r/GUI/AI/Orca/OrcaTrialSliceExecutor.hpp"
#include "slic3r/GUI/AI/Orca/OrcaWorkflowRuntimeStore.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingPanel.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingPresenter.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"

#include <wx/aui/framemanager.h>
#include <wx/button.h>
#include <wx/sizer.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>

namespace Slic3r::GUI {
namespace {

struct TransformTarget
{
    size_t object_index { 0 };
    ModelInstance* instance { nullptr };
    Transform3d matrix { Transform3d::Identity() };
};

bool collect_transform_targets(Plater& plater, const AI::SmartSlicing::SliceCandidate& candidate,
                               std::vector<TransformTarget>& targets, std::string& diagnostic)
{
    PartPlate* plate = plater.get_partplate_list().get_curr_plate();
    if (plate == nullptr) {
        diagnostic = "current_plate_unavailable";
        return false;
    }
    if (plate->is_locked()) {
        diagnostic = "current_plate_locked";
        return false;
    }

    std::set<uint64_t> seen_instances;
    targets.reserve(candidate.placement.transforms.size());
    Model& model = plater.model();
    for (const AI::SmartSlicing::ObjectTransform& requested : candidate.placement.transforms) {
        if (!seen_instances.insert(requested.instance_id).second) {
            diagnostic = "duplicate_transform_target";
            return false;
        }
        Transform3d matrix;
        for (Eigen::Index row = 0; row < matrix.rows(); ++row)
            for (Eigen::Index column = 0; column < matrix.cols(); ++column) {
                const double value = requested.matrix[static_cast<size_t>(row * matrix.cols() + column)];
                if (!std::isfinite(value)) {
                    diagnostic = "invalid_transform";
                    return false;
                }
                matrix(row, column) = value;
            }
        if (!matrix.matrix().row(3).isApprox(Eigen::RowVector4d(0.0, 0.0, 0.0, 1.0)) ||
            std::abs(matrix.linear().determinant()) < 1e-12) {
            diagnostic = "invalid_transform";
            return false;
        }

        TransformTarget target;
        bool found = false;
        for (size_t object_index = 0; object_index < model.objects.size() && !found; ++object_index) {
            ModelObject* object = model.objects[object_index];
            if (object == nullptr || object->id().id != requested.object_id)
                continue;
            for (size_t instance_index = 0; instance_index < object->instances.size(); ++instance_index) {
                ModelInstance* instance = object->instances[instance_index];
                if (instance != nullptr && instance->id().id == requested.instance_id) {
                    if (!plate->contain_instance(static_cast<int>(object_index), static_cast<int>(instance_index))) {
                        diagnostic = "transform_target_not_on_current_plate";
                        return false;
                    }
                    target = { object_index, instance, matrix };
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            diagnostic = "transform_target_missing";
            return false;
        }
        targets.push_back(std::move(target));
    }
    return true;
}

bool prepare_parameter_patch(Plater& plater, const AI::SmartSlicing::SliceCandidate& candidate,
                             DynamicPrintConfig& plate_patch, std::string& diagnostic)
{
    if (candidate.parameters.entries.empty())
        return true;
    if (wxGetApp().preset_bundle == nullptr) {
        diagnostic = "current_config_unavailable";
        return false;
    }
    PartPlate* plate = plater.get_partplate_list().get_curr_plate();
    if (plate == nullptr) {
        diagnostic = "current_plate_unavailable";
        return false;
    }

    DynamicPrintConfig current_config = wxGetApp().preset_bundle->full_config();
    current_config.apply(*plate->config(), true);
    DynamicPrintConfig patched_config;
    const OrcaParameterApplyResult result = OrcaParameterProposalAdapter().validate_and_apply(
        candidate.parameters, plate->id().id, current_config, patched_config);
    if (!result.accepted) {
        diagnostic = result.diagnostic_code;
        return false;
    }
    for (const AI::SmartSlicing::ConfigPatchEntry& entry : candidate.parameters.entries) {
        const ConfigOption* replacement = patched_config.option(entry.key);
        if (replacement == nullptr) {
            diagnostic = "parameter_native_option_unavailable";
            return false;
        }
        plate_patch.set_key_value(entry.key, replacement->clone());
    }
    return true;
}

} // namespace

struct SmartSlicingFeatureHost::Impl
{
    Impl(Plater& plater, wxAuiManager& aui_manager, Sidebar& sidebar, StartOfficialSliceFn start_official_slice)
        : plater(plater)
        , aui_manager(aui_manager)
        , sidebar(sidebar)
        , workspace(std::make_unique<OrcaSmartSlicingAdapter>(&plater))
        , trial_executor(std::make_unique<OrcaTrialSliceExecutor>([this] {
            return workspace->capture_trial_slice_input();
        }))
        , official_gateway(std::make_unique<OrcaOfficialSliceGateway>(
            [this] { return workspace->current_revision(); },
            [this](const AI::SmartSlicing::SliceCandidate& candidate) { return validate_candidate(candidate); },
            [this](const AI::SmartSlicing::SliceCandidate& candidate) { return apply_candidate(candidate); },
            std::move(start_official_slice),
            [this] {
                this->plater.select_view_3D("Preview");
                return this->plater.is_preview_shown();
            },
            [this] {
                if (!applied_snapshot_time ||
                    this->plater.undo_redo_stack_main().active_snapshot_time() != *applied_snapshot_time ||
                    this->plater.get_view3D_canvas3D()->get_gizmos_manager().is_running())
                    return false;
                this->plater.select_view_3D("3D");
                if (!this->plater.can_undo())
                    return false;
                this->plater.undo();
                return this->plater.undo_redo_stack_main().active_snapshot_time() != *applied_snapshot_time;
            }))
        , coordinator(std::make_unique<AI::SmartSlicing::SmartSlicingCoordinator>(
            *workspace, *trial_executor, *official_gateway))
        , runtime_store(std::make_unique<OrcaWorkflowRuntimeStore>(
            boost::filesystem::temp_directory_path() / "OrcaSlicer-smart-slicing-runtime-v1.json"))
        , presenter(std::make_unique<SmartSlicingPresenter>(*coordinator, [](std::function<void()> publish) {
            if (wxIsMainThread())
                publish();
            else
                wxGetApp().CallAfter(std::move(publish));
        }))
    {
        AI::SmartSlicing::WorkflowResourceBudget budget;
        trial_executor->set_resource_limits(
            budget.maximum_elapsed, budget.maximum_memory_bytes, budget.maximum_temporary_disk_bytes);
        coordinator->set_resource_budget(budget);
        coordinator->set_runtime_store(*runtime_store);

        panel = new SmartSlicingPanel(&plater, *coordinator, [this] {
            const auto& snapshot = coordinator->snapshot();
            if (!snapshot.context)
                return std::vector<AI::SmartSlicing::SliceCandidate> {};
            trial_executor->prepare_session_input(workspace->capture_trial_slice_input());
            return workspace->candidate_proposals(snapshot.context->revision);
        }, [this] {
            trial_executor->cancel_trial_slice();
        }, [this] {
            this->plater.add_file();
        }, &plater);
        presenter->set_view_changed([this](const SmartSlicingViewModel& view) { render(view); });
        entry_button = new wxButton(&sidebar, wxID_ANY, _L("智能切片：检查与优化…"));
        entry_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show(true); });
        sidebar.GetSizer()->Insert(0, entry_button, 0, wxEXPAND | wxALL, sidebar.FromDIP(8));
        sidebar.Layout();

        aui_manager.AddPane(panel, wxAuiPaneInfo()
                                       .Name("smart_slicing")
                                       .Caption(_L("智能切片"))
                                       .Right()
                                       .CloseButton(true)
                                       .TopDockable(false)
                                       .BottomDockable(false)
                                       .BestSize(wxSize(38 * wxGetApp().em_unit(), 70 * wxGetApp().em_unit()))
                                       .Hide());
        aui_manager.Update();
    }

    std::string validate_candidate(const AI::SmartSlicing::SliceCandidate& candidate)
    {
        if (plater.get_view3D_canvas3D()->get_gizmos_manager().is_running())
            return "close_active_model_tool";
        std::vector<TransformTarget> targets;
        DynamicPrintConfig parameter_patch;
        std::string diagnostic;
        if (!collect_transform_targets(plater, candidate, targets, diagnostic) ||
            !prepare_parameter_patch(plater, candidate, parameter_patch, diagnostic))
            return diagnostic;
        return {};
    }

    OrcaApplyMutationResult apply_candidate(const AI::SmartSlicing::SliceCandidate& candidate)
    {
        std::vector<TransformTarget> targets;
        DynamicPrintConfig parameter_patch;
        std::string diagnostic;
        if (!collect_transform_targets(plater, candidate, targets, diagnostic) ||
            !prepare_parameter_patch(plater, candidate, parameter_patch, diagnostic))
            return { false, false, std::move(diagnostic) };

        std::vector<TransformTarget> changed;
        std::vector<size_t> changed_object_indices;
        for (const TransformTarget& target : targets) {
            if (!target.instance->get_matrix().isApprox(target.matrix)) {
                changed.push_back(target);
                changed_object_indices.push_back(target.object_index);
            }
        }
        if (changed.empty() && candidate.parameters.entries.empty())
            return { true, false, {} };
        std::sort(changed_object_indices.begin(), changed_object_indices.end());
        changed_object_indices.erase(
            std::unique(changed_object_indices.begin(), changed_object_indices.end()), changed_object_indices.end());

        bool transaction_started = false;
        try {
            plater.select_view_3D("3D");
            {
                Plater::TakeSnapshot transaction(&plater, "Apply Smart Slicing Candidate");
                transaction_started = true;
                for (const TransformTarget& target : changed)
                    target.instance->set_transformation(Geometry::Transformation(target.matrix));
                PartPlate* plate = plater.get_partplate_list().get_curr_plate();
                if (plate == nullptr)
                    throw std::runtime_error("Current plate disappeared while applying a smart-slicing candidate.");
                for (const AI::SmartSlicing::ConfigPatchEntry& entry : candidate.parameters.entries) {
                    const ConfigOption* replacement = parameter_patch.option(entry.key);
                    if (replacement == nullptr)
                        throw std::runtime_error("Validated smart-slicing parameter disappeared before apply.");
                    plate->config()->set_key_value(entry.key, replacement->clone());
                }
                if (!changed_object_indices.empty())
                    plater.changed_objects(changed_object_indices);
                if (!candidate.parameters.entries.empty())
                    plate->update_slice_result_valid_state(false);
                plater.update_title_dirty_status();
            }
            applied_snapshot_time = plater.undo_redo_stack_main().active_snapshot_time();
            return { true, true, {} };
        } catch (...) {
            if (transaction_started && plater.can_undo())
                plater.undo();
            return { false, false, "candidate_apply_rolled_back" };
        }
    }

    void render(const SmartSlicingViewModel& view)
    {
        if (panel != nullptr)
            panel->render(view);
        if (view.is_stale || view.summary_key == "official_slice_complete" || view.summary_key == "canceled" ||
            view.summary_key == "preflight_failed")
            trial_executor->clear_session_input();
    }

    bool is_shown() const
    {
        return panel != nullptr && aui_manager.GetPane(panel).IsShown();
    }

    void show(bool should_show)
    {
        if (panel == nullptr)
            return;
        auto& pane = aui_manager.GetPane(panel);
        if (!pane.IsOk())
            return;
        pane.Show(should_show);
        aui_manager.Update();
    }

    Plater& plater;
    wxAuiManager& aui_manager;
    Sidebar& sidebar;
    std::unique_ptr<OrcaSmartSlicingAdapter> workspace;
    std::unique_ptr<OrcaTrialSliceExecutor> trial_executor;
    std::unique_ptr<OrcaOfficialSliceGateway> official_gateway;
    std::unique_ptr<AI::SmartSlicing::SmartSlicingCoordinator> coordinator;
    std::unique_ptr<OrcaWorkflowRuntimeStore> runtime_store;
    std::unique_ptr<SmartSlicingPresenter> presenter;
    SmartSlicingPanel* panel { nullptr };
    wxButton* entry_button { nullptr };
    std::optional<size_t> applied_snapshot_time;
};

SmartSlicingFeatureHost::SmartSlicingFeatureHost(Plater& plater, wxAuiManager& aui_manager, Sidebar& sidebar,
                                                 StartOfficialSliceFn start_official_slice)
    : m_impl(std::make_unique<Impl>(plater, aui_manager, sidebar, std::move(start_official_slice)))
{}

SmartSlicingFeatureHost::~SmartSlicingFeatureHost() = default;

bool SmartSlicingFeatureHost::is_shown() const
{
    return m_impl->is_shown();
}

void SmartSlicingFeatureHost::show(bool show)
{
    m_impl->show(show);
}

void SmartSlicingFeatureHost::notify_slice_completed(bool success, const std::string& failure_code)
{
    m_impl->official_gateway->notify_slice_completed(success, failure_code);
}

} // namespace Slic3r::GUI
