#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    #include <CommCtrl.h>
#endif

#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SlicingAdmission.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"

#include "../fff_print/test_helpers.hpp"

using namespace Slic3r;

TEST_CASE("Background slicing rejects busy admission and remains stoppable", "[BackgroundSlicingProcess][SlicingAdmission]")
{
    Print print;
    Model model;
    Test::init_print({ Test::cube(10) }, print, model);
    REQUIRE_FALSE(print.empty());

    GCodeProcessorResult result;
    auto isolated = try_acquire_slicing_admission(SlicingAdmissionMode::IsolatedSlicing);
    REQUIRE(isolated);

    {
        BackgroundSlicingProcess process;
        process.set_fff_print(&print);
        process.set_gcode_result(&result);
        process.select_technology(ptFFF);

        CHECK_FALSE(process.start());
        CHECK(process.idle());
        CHECK(process.stop());
        CHECK(process.idle());
    }

    CHECK_FALSE(try_acquire_slicing_admission(SlicingAdmissionMode::LiveSlicing));
    isolated.reset();
    CHECK(try_acquire_slicing_admission(SlicingAdmissionMode::LiveSlicing));
}
