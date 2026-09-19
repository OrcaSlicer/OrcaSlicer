#include <catch2/catch_all.hpp>

#include "libslic3r/SlicingAdmission.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace Slic3r;

static_assert(! std::is_copy_constructible_v<SlicingAdmissionToken>);
static_assert(! std::is_copy_assignable_v<SlicingAdmissionToken>);
static_assert(std::is_nothrow_move_constructible_v<SlicingAdmissionToken>);
static_assert(std::is_nothrow_move_assignable_v<SlicingAdmissionToken>);

TEST_CASE("Slicing admission rejects conflicting jobs without waiting", "[SlicingAdmission]")
{
    auto live = try_acquire_slicing_admission(SlicingAdmissionMode::LiveSlicing);
    REQUIRE(live);

    CHECK_FALSE(try_acquire_slicing_admission(SlicingAdmissionMode::LiveSlicing));
    CHECK_FALSE(try_acquire_slicing_admission(SlicingAdmissionMode::IsolatedSlicing));

    // Preserve existing GUI behavior: a live slice does not freeze bed changes.
    auto scale_change = try_acquire_slicing_admission(SlicingAdmissionMode::ScaleChanging);
    REQUIRE(scale_change);

    live.reset();
    CHECK_FALSE(try_acquire_slicing_admission(SlicingAdmissionMode::IsolatedSlicing));

    scale_change.reset();
    CHECK(try_acquire_slicing_admission(SlicingAdmissionMode::IsolatedSlicing));
}

TEST_CASE("Slicing admission token releases on terminal paths", "[SlicingAdmission]")
{
    SECTION("success") {
        {
            auto live = try_acquire_slicing_admission(SlicingAdmissionMode::LiveSlicing);
            REQUIRE(live);
        }
        CHECK(try_acquire_slicing_admission(SlicingAdmissionMode::IsolatedSlicing));
    }

    SECTION("exception") {
        try {
            auto live = try_acquire_slicing_admission(SlicingAdmissionMode::LiveSlicing);
            REQUIRE(live);
            throw std::runtime_error("test worker failure");
        } catch (const std::runtime_error &) {
        }
        CHECK(try_acquire_slicing_admission(SlicingAdmissionMode::IsolatedSlicing));
    }

    SECTION("cancellation") {
        auto live = try_acquire_slicing_admission(SlicingAdmissionMode::LiveSlicing);
        REQUIRE(live);
        live.reset();
        CHECK(try_acquire_slicing_admission(SlicingAdmissionMode::IsolatedSlicing));
    }

    SECTION("move transfers ownership") {
        auto live = try_acquire_slicing_admission(SlicingAdmissionMode::LiveSlicing);
        REQUIRE(live);
        SlicingAdmissionToken worker_token(std::move(live));
        CHECK_FALSE(live);
        CHECK(worker_token);
        CHECK_FALSE(try_acquire_slicing_admission(SlicingAdmissionMode::IsolatedSlicing));
    }
}

TEST_CASE("Isolated slicing freezes bed-shape scale changes", "[SlicingAdmission][BedShape]")
{
    auto isolated = try_acquire_slicing_admission(SlicingAdmissionMode::IsolatedSlicing);
    REQUIRE(isolated);

    CHECK_FALSE(try_acquire_slicing_admission(SlicingAdmissionMode::ScaleChanging));

    isolated.reset();
    CHECK(try_acquire_slicing_admission(SlicingAdmissionMode::ScaleChanging));
}
