#include <catch2/catch_all.hpp>

#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"

using Slic3r::GUI::MmuSegmentationColorShortcutResult;
using Slic3r::GUI::resolve_mmu_segmentation_color_shortcut;

TEST_CASE("MMU segmentation color shortcuts use the available filament count", "[GLGizmosManager][MmSegmentation]")
{
    SECTION("single digit colors select immediately when no two digit color is possible") {
        const MmuSegmentationColorShortcutResult shortcut = resolve_mmu_segmentation_color_shortcut(1, 4, 0);
        CHECK(shortcut.select_now == 1);
        CHECK(shortcut.pending_tens == 0);
        CHECK_FALSE(shortcut.start_timer);
        CHECK_FALSE(shortcut.stop_timer);
        CHECK(shortcut.processed);
    }

    SECTION("valid two digit colors defer the first digit and select on the second") {
        const MmuSegmentationColorShortcutResult first = resolve_mmu_segmentation_color_shortcut(1, 12, 0);
        CHECK(first.select_now == 0);
        CHECK(first.pending_tens == 1);
        CHECK(first.start_timer);

        const MmuSegmentationColorShortcutResult second = resolve_mmu_segmentation_color_shortcut(2, 12, first.pending_tens);
        CHECK(second.select_now == 12);
        CHECK(second.pending_tens == 0);
        CHECK_FALSE(second.start_timer);
        CHECK(second.stop_timer);
    }

    SECTION("invalid two digit colors fall back to the fresh single digit") {
        const MmuSegmentationColorShortcutResult shortcut = resolve_mmu_segmentation_color_shortcut(7, 16, 1);
        CHECK(shortcut.select_now == 7);
        CHECK(shortcut.pending_tens == 0);
        CHECK_FALSE(shortcut.start_timer);
        CHECK(shortcut.stop_timer);
    }

    SECTION("21 and 32 are reachable when mixed virtual filaments make them available") {
        const MmuSegmentationColorShortcutResult twenty_first = resolve_mmu_segmentation_color_shortcut(1, 21, 2);
        CHECK(twenty_first.select_now == 21);
        CHECK(twenty_first.pending_tens == 0);
        CHECK(twenty_first.stop_timer);

        const MmuSegmentationColorShortcutResult thirty_first = resolve_mmu_segmentation_color_shortcut(3, 32, 0);
        CHECK(thirty_first.select_now == 0);
        CHECK(thirty_first.pending_tens == 3);
        CHECK(thirty_first.start_timer);

        const MmuSegmentationColorShortcutResult thirty_second =
            resolve_mmu_segmentation_color_shortcut(2, 32, thirty_first.pending_tens);
        CHECK(thirty_second.select_now == 32);
        CHECK(thirty_second.pending_tens == 0);
        CHECK(thirty_second.stop_timer);
    }

    SECTION("zero is consumed but never selects a filament") {
        const MmuSegmentationColorShortcutResult shortcut = resolve_mmu_segmentation_color_shortcut(0, 9, 0);
        CHECK(shortcut.select_now == 0);
        CHECK(shortcut.pending_tens == 0);
        CHECK_FALSE(shortcut.start_timer);
        CHECK(shortcut.processed);
    }
}
