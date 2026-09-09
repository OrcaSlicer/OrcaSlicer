#include <catch2/catch_all.hpp>
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.hpp"
#include <stdexcept>

using namespace Slic3r::AI::SmartSlicing;

namespace {
struct ApplyTestWorkspace final : IOrcaWorkspace {
    WorkspaceRevision current_revision() const override { return {1, 2, 3, "before"}; }
    WorkspaceContext capture_context() const override {
        WorkspaceContext context;
        context.revision = current_revision();
        context.plate_index = 0;
        context.printer_preset_id = "printer";
        context.process_preset_id = "process";
        context.materials.push_back({"material", "#FFFFFF"});
        context.objects.push_back({42, "cube", 1, 12, 0, false});
        context.native_validation_available = true;
        return context;
    }
};
struct ApplyTestTrial final : ITrialSliceExecutor {
    TrialSliceResult execute_trial_slice(const SliceCandidate& candidate) override {
        return {candidate.id, candidate.base_revision, TrialSliceStatus::Succeeded, SlicingMetrics{}, {}};
    }
    void cancel_trial_slice() override {}
};
} // namespace

TEST_CASE("completed applications recover from undo errors and reject obsolete undo", "[SmartSlicing][Apply]")
{
    ApplyTestWorkspace workspace;
    ApplyTestTrial trial;
    bool throw_on_undo = true;
    const bool can_undo = GENERATE(false, true);
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [&] { return workspace.current_revision(); }, [](const SliceCandidate&) { return std::string{}; },
        [](const SliceCandidate&) { return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}}; },
        [] { return true; }, [] { return true; }, [&] {
            if (throw_on_undo) throw std::runtime_error("temporary undo failure");
            return can_undo;
        });
    SmartSlicingCoordinator coordinator(workspace, trial, gateway);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());
    REQUIRE(coordinator.apply_selected_candidate());
    gateway.notify_slice_completed(true);
    REQUIRE(coordinator.poll_official_slice());
    REQUIRE(coordinator.snapshot().state == WorkflowState::Completed);
    CHECK_FALSE(coordinator.undo_applied_candidate());
    CHECK(coordinator.snapshot().state == WorkflowState::Completed);
    CHECK(coordinator.snapshot().detail == "apply_undo_failed");
    CHECK(coordinator.snapshot().can_undo_apply);
    throw_on_undo = false;
    CHECK(coordinator.undo_applied_candidate() == can_undo);
    CHECK(coordinator.snapshot().state == (can_undo ? WorkflowState::ReadyToApply : WorkflowState::Stale));
    CHECK_FALSE(coordinator.snapshot().can_undo_apply);
    CHECK_FALSE(coordinator.undo_applied_candidate());
}

TEST_CASE("undo targets the applied revision and leaves subsequent edits untouched", "[SmartSlicing][Apply]")
{
    WorkspaceRevision current{1, 2, 3, "before"};
    size_t undo_calls = 0;
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [&] { return current; }, [](const SliceCandidate&) { return std::string{}; },
        [&](const SliceCandidate&) {
            current.fingerprint = "applied";
            return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}};
        }, [] { return true; }, [] { return true; }, [&] { ++undo_calls; return true; });
    SliceCandidate candidate;
    candidate.base_revision = current;
    REQUIRE(gateway.commit(candidate, candidate.base_revision).phase == OfficialSlicePhase::Slicing);
    gateway.notify_slice_completed(true);
    REQUIRE(gateway.poll().phase == OfficialSlicePhase::Completed);
    const bool subsequently_edited = GENERATE(false, true);
    if (subsequently_edited) current.fingerprint = "user-edit";
    CHECK(gateway.undo_last_apply() == !subsequently_edited);
    CHECK(undo_calls == (subsequently_edited ? 0 : 1));
    CHECK_FALSE(gateway.undo_last_apply());
}

TEST_CASE("native undo rejection preserves later history even when the content matches", "[SmartSlicing][Apply]")
{
    const WorkspaceRevision revision{1, 2, 3, "same-content"};
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [=] { return revision; }, [](const SliceCandidate&) { return std::string{}; },
        [](const SliceCandidate&) { return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}}; },
        [] { return false; }, [] { return true; }, [] { return false; });
    SliceCandidate candidate;
    candidate.base_revision = revision;
    REQUIRE(gateway.commit(candidate, revision).phase == OfficialSlicePhase::Failed);
    CHECK_FALSE(gateway.undo_last_apply());
}
