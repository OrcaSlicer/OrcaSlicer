#include <catch2/catch_all.hpp>

#include <stdexcept>

#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingViewModel.hpp"

using namespace Slic3r::AI::SmartSlicing;

namespace {

WorkspaceContext context_with_revision(std::string fingerprint)
{
    WorkspaceContext context;
    context.revision          = {1, 2, 3, std::move(fingerprint)};
    context.plate_index       = 0;
    context.printer_preset_id = "printer";
    context.process_preset_id = "process";
    context.objects.push_back({42, "cube", 1, 12, 0, false});
    context.native_validation_available = true;
    return context;
}

class FakeWorkspace final : public IOrcaWorkspace
{
public:
    WorkspaceContext context = context_with_revision("revision-a");
    mutable size_t capture_count{0};
    bool throw_on_revision{false};
    bool throw_on_capture{false};

    WorkspaceRevision current_revision() const override
    {
        if (throw_on_revision)
            throw std::runtime_error("revision unavailable");
        return context.revision;
    }

    WorkspaceContext capture_context() const override
    {
        if (throw_on_capture)
            throw std::runtime_error("capture unavailable");
        ++capture_count;
        return context;
    }
};

} // namespace

TEST_CASE("smart slicing coordinator captures and preflights without applying workspace changes", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    SmartSlicingCoordinator coordinator(workspace);

    coordinator.start();

    CHECK(workspace.capture_count == 1);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyForCandidatePlanning);
    REQUIRE(coordinator.snapshot().report);
    CHECK(coordinator.snapshot().report->revision == workspace.context.revision);
}

TEST_CASE("blocking printability issues require a decision", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    workspace.context.objects.front().open_edge_count = 3;
    SmartSlicingCoordinator coordinator(workspace);

    coordinator.start();

    CHECK(coordinator.snapshot().state == WorkflowState::AwaitingRiskDecision);
    CHECK(coordinator.snapshot().report->has_blocking_issue());
}

TEST_CASE("smart slicing coordinator supports cancel and restart", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();

    coordinator.cancel();
    CHECK(coordinator.snapshot().state == WorkflowState::Canceled);
    CHECK(coordinator.snapshot().can_start());

    coordinator.start();
    CHECK(coordinator.snapshot().workflow_id == 2);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyForCandidatePlanning);
}

TEST_CASE("revision refresh makes an existing workflow stale", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();

    workspace.context = context_with_revision("revision-b");
    CHECK(coordinator.refresh_revision());
    CHECK(coordinator.snapshot().state == WorkflowState::Stale);
    REQUIRE(coordinator.snapshot().context);
    CHECK(coordinator.snapshot().context->revision.fingerprint == "revision-a");
}

TEST_CASE("four-stage view model and legacy projection share coordinator state", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();

    const Slic3r::GUI::SmartSlicingViewModel view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    CHECK(view.stages[0].status == Slic3r::GUI::SmartSlicingStageStatus::Complete);
    CHECK(view.stages[1].status == Slic3r::GUI::SmartSlicingStageStatus::Complete);
    CHECK(view.stages[2].status == Slic3r::GUI::SmartSlicingStageStatus::Disabled);
    CHECK(view.legacy_steps[1] == Slic3r::GUI::LegacyAIWorkflowStatus::Success);
    CHECK(view.legacy_steps[4] == Slic3r::GUI::LegacyAIWorkflowStatus::Waiting);
}

TEST_CASE("minimum printability report uses stable issue codes", "[AI][SmartSlicing]")
{
    WorkspaceContext context = context_with_revision("revision-a");
    context.materials.push_back({"material", "#FFFFFF"});
    context.validation_warnings.push_back("Existing Print validation warning.");

    const PrintabilityReport report = PrintabilityInspector().inspect(context);

    REQUIRE(report.issues.size() == 1);
    CHECK(report.issues.front().code == IssueCode::ConfigurationValidationWarning);
    CHECK(std::string(issue_code_name(report.issues.front().code)) == "configuration_validation_warning");
    CHECK(report.readiness == Readiness::NeedsAttention);
    CHECK_FALSE(report.has_blocking_issue());
}

TEST_CASE("workspace revision equality includes every revision component", "[AI][SmartSlicing]")
{
    const WorkspaceRevision baseline{1, 2, 3, "fingerprint"};
    CHECK((baseline == WorkspaceRevision{1, 2, 3, "fingerprint"}));
    CHECK((baseline != WorkspaceRevision{9, 2, 3, "fingerprint"}));
    CHECK((baseline != WorkspaceRevision{1, 9, 3, "fingerprint"}));
    CHECK((baseline != WorkspaceRevision{1, 2, 9, "fingerprint"}));
    CHECK((baseline != WorkspaceRevision{1, 2, 3, "different"}));
}

TEST_CASE("invalid or unavailable workspace capture fails without leaving the workflow stuck", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.revision.fingerprint.clear();
    SmartSlicingCoordinator coordinator(workspace);

    coordinator.start();
    CHECK(coordinator.snapshot().state == WorkflowState::Failed);
    CHECK(coordinator.snapshot().detail == "invalid_workspace_revision");
    CHECK(coordinator.snapshot().can_start());

    workspace.context = context_with_revision("revision-b");
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    coordinator.start();
    CHECK(coordinator.snapshot().workflow_id == 2);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyForCandidatePlanning);

    coordinator.cancel();
    workspace.throw_on_capture = true;
    coordinator.start();
    CHECK(coordinator.snapshot().state == WorkflowState::Failed);
    CHECK(coordinator.snapshot().detail == "capture unavailable");
}

TEST_CASE("revision refresh is stable for equal or temporarily unavailable revisions", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();

    CHECK_FALSE(coordinator.refresh_revision());
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyForCandidatePlanning);

    workspace.throw_on_revision = true;
    CHECK_FALSE(coordinator.refresh_revision());
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyForCandidatePlanning);
}

TEST_CASE("coordinator publishes deterministic synchronous preflight transitions", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    SmartSlicingCoordinator coordinator(workspace);
    std::vector<WorkflowState> states;
    coordinator.set_observer([&states](const WorkflowSnapshot& snapshot) { states.push_back(snapshot.state); });

    coordinator.start();

    REQUIRE(states.size() == 4);
    CHECK(states[0] == WorkflowState::Idle);
    CHECK(states[1] == WorkflowState::CapturingContext);
    CHECK(states[2] == WorkflowState::Preflighting);
    CHECK(states[3] == WorkflowState::ReadyForCandidatePlanning);
}

TEST_CASE("minimum printability prerequisites use stable issue ordering", "[AI][SmartSlicing]")
{
    WorkspaceContext context;
    context.revision                    = {1, 2, 3, "revision-a"};
    context.native_validation_available = true;

    const PrintabilityReport report = PrintabilityInspector().inspect(context);

    REQUIRE(report.issues.size() == 4);
    CHECK(report.issues[0].code == IssueCode::EmptyPlate);
    CHECK(report.issues[1].code == IssueCode::MissingPrinter);
    CHECK(report.issues[2].code == IssueCode::MissingProcess);
    CHECK(report.issues[3].code == IssueCode::MissingMaterial);
    CHECK(report.readiness == Readiness::Blocked);
    CHECK(report.has_blocking_issue());
}

TEST_CASE("unavailable native validation is explicit and non-blocking", "[AI][SmartSlicing]")
{
    WorkspaceContext context = context_with_revision("revision-a");
    context.materials.push_back({"material", "#FFFFFF"});
    context.native_validation_available = false;

    const PrintabilityReport report = PrintabilityInspector().inspect(context);

    REQUIRE(report.issues.size() == 1);
    CHECK(report.issues.front().code == IssueCode::NativeValidationUnavailable);
    CHECK(report.readiness == Readiness::NeedsAttention);
    CHECK_FALSE(report.has_blocking_issue());

    WorkflowSnapshot snapshot;
    snapshot.state                                = WorkflowState::ReadyForCandidatePlanning;
    snapshot.context                              = context;
    snapshot.report                               = report;
    const Slic3r::GUI::SmartSlicingViewModel view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot);
    CHECK(view.summary_key == "preflight_complete_with_warnings");
    CHECK(view.stages[1].status == Slic3r::GUI::SmartSlicingStageStatus::NeedsAttention);
    CHECK(view.legacy_steps[1] == Slic3r::GUI::LegacyAIWorkflowStatus::Warning);
}

TEST_CASE("empty material preset entries do not satisfy printability prerequisites", "[AI][SmartSlicing]")
{
    WorkspaceContext context = context_with_revision("revision-a");
    context.materials.push_back({"", "#FFFFFF"});

    const PrintabilityReport report = PrintabilityInspector().inspect(context);

    REQUIRE(report.issues.size() == 1);
    CHECK(report.issues.front().code == IssueCode::MissingMaterial);
    CHECK(report.has_blocking_issue());
}

TEST_CASE("multicolor preflight makes compatibility and mapping evidence explicit", "[AI][SmartSlicing][Multicolor]")
{
    WorkspaceContext context = context_with_revision("revision-a");
    context.materials.push_back({"pla", "#FFFFFF"});
    context.materials.push_back({"petg", "#000000"});
    context.multicolor.used_logical_filament_ids = {1, 2};
    context.multicolor.filament_to_physical_slot = {1, 1};
    context.multicolor.first_layer_tool_sequence = {1, 2};
    context.multicolor.other_layer_tool_sequences.push_back({1, 25, {2, 1}});
    context.multicolor.physical_slot_compatibility = PhysicalSlotCompatibility::Incompatible;
    context.multicolor.color_mapping_degraded = true;

    const PrintabilityReport blocked = PrintabilityInspector().inspect(context);
    REQUIRE(blocked.issues.size() == 2);
    CHECK(blocked.issues[0].code == IssueCode::IncompatiblePhysicalSlots);
    CHECK(blocked.issues[1].code == IssueCode::ColorMappingDegraded);
    CHECK(blocked.has_blocking_issue());
    CHECK(blocked.readiness == Readiness::Blocked);
    CHECK(context.multicolor.first_layer_tool_sequence == std::vector<int>{1, 2});
    CHECK(context.multicolor.other_layer_tool_sequences.front().logical_filament_ids == std::vector<int>{2, 1});

    context.multicolor.color_mapping_degraded = false;
    context.multicolor.physical_slot_compatibility = PhysicalSlotCompatibility::Unavailable;
    const PrintabilityReport unavailable = PrintabilityInspector().inspect(context);
    REQUIRE(unavailable.issues.size() == 1);
    CHECK(unavailable.issues.front().code == IssueCode::MulticolorEvidenceUnavailable);
    CHECK_FALSE(unavailable.has_blocking_issue());
    CHECK(unavailable.readiness == Readiness::NeedsAttention);
}

TEST_CASE("idle view model initializes every stage and legacy step", "[AI][SmartSlicing]")
{
    const Slic3r::GUI::SmartSlicingViewModel view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(WorkflowSnapshot{});

    for (const Slic3r::GUI::SmartSlicingStageView& stage : view.stages)
        CHECK(stage.status == Slic3r::GUI::SmartSlicingStageStatus::Waiting);
    for (const Slic3r::GUI::LegacyAIWorkflowStatus status : view.legacy_steps)
        CHECK(status == Slic3r::GUI::LegacyAIWorkflowStatus::Waiting);
    CHECK(view.can_start);
    CHECK_FALSE(view.can_cancel);
    CHECK_FALSE(view.has_report);
    CHECK(view.can_add_model);
}

TEST_CASE("empty plates offer model import without claiming model preparation is complete", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.objects.clear();
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();
    const auto view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    CHECK(view.has_report);
    CHECK(view.can_add_model);
    CHECK(view.can_recheck);
    CHECK(view.stages[0].status == Slic3r::GUI::SmartSlicingStageStatus::NeedsAttention);
    CHECK_FALSE(view.can_plan_candidates);
}

TEST_CASE("candidate cards preserve the proposed old and new values", "[AI][SmartSlicing]")
{
    WorkflowSnapshot snapshot;
    snapshot.state = WorkflowState::ReadyToApply;
    SliceCandidate candidate;
    candidate.id = "wider-brim";
    candidate.parameters.entries.push_back({ConfigScope::Plate, PresetOwner::Process, 12,
        "brim_width", 2.0, 5.0, "adhesion"});
    candidate.placement.transforms.resize(2);
    snapshot.candidates.push_back(candidate);
    const auto view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot);
    REQUIRE(view.candidates.size() == 1);
    REQUIRE(view.candidates.front().parameter_changes.size() == 1);
    const auto& change = view.candidates.front().parameter_changes.front();
    CHECK(change.key == "brim_width");
    CHECK(change.target_id == 12);
    CHECK_THAT(std::get<double>(change.expected_value), Catch::Matchers::WithinAbs(2.0, 0.001));
    CHECK_THAT(std::get<double>(change.new_value), Catch::Matchers::WithinAbs(5.0, 0.001));
    CHECK(view.candidates.front().placement_change_count == 2);
}

TEST_CASE("completed and failed applications expose the same undo action", "[AI][SmartSlicing]")
{
    WorkflowSnapshot snapshot;
    snapshot.state = GENERATE(WorkflowState::Completed, WorkflowState::ApplyFailed);
    snapshot.can_undo_apply = true;
    CHECK(Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot).can_undo_apply);
    snapshot.can_undo_apply = false;
    CHECK_FALSE(Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot).can_undo_apply);
}

TEST_CASE("undo errors do not relabel completed slicing as failed", "[AI][SmartSlicing]")
{
    WorkflowSnapshot snapshot;
    snapshot.state = WorkflowState::Completed;
    snapshot.can_undo_apply = true;
    snapshot.detail = "apply_undo_failed";
    const auto view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot);
    CHECK(view.summary_key == "apply_undo_failed");
    CHECK(view.stages.back().status == Slic3r::GUI::SmartSlicingStageStatus::Complete);
    CHECK(view.can_undo_apply);
}

TEST_CASE("canceled view model does not expose a report from an obsolete workspace", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    workspace.context.objects.front().open_edge_count = 3;
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();
    REQUIRE(coordinator.snapshot().report);
    REQUIRE_FALSE(coordinator.snapshot().report->issues.empty());

    coordinator.cancel();

    const Slic3r::GUI::SmartSlicingViewModel view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    CHECK(view.summary_key == "canceled");
    CHECK(view.issue_count == 0);
    CHECK(view.issues.empty());
}
