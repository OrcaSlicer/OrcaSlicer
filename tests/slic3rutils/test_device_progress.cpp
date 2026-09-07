// why: match the GUI include order to avoid rpcndr.h byte/std::byte
// ambiguity in the Windows COM headers.
// why: wx/timer.h must precede DeviceManager.hpp because
// DeviceErrorDialog.hpp uses wxTimerEvent.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include <stdexcept>

#include <wx/timer.h>

#include "slic3r/GUI/DeviceManager.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace Slic3r;

TEST_CASE("Integer progress reaches the shared subtask", "[DeviceManager][Progress]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");

    machine.update_print_progress(json(37));

    REQUIRE(machine.mc_print_percent == 37);
    BBLSubTask* subtask = machine.get_subtask();
    REQUIRE(subtask != nullptr);
    CHECK(subtask->task_progress == 37);
}

TEST_CASE("String progress reaches the shared subtask", "[DeviceManager][Progress]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");

    machine.update_print_progress(json("41"));

    REQUIRE(machine.mc_print_percent == 41);
    BBLSubTask* subtask = machine.get_subtask();
    REQUIRE(subtask != nullptr);
    CHECK(subtask->task_progress == 41);
}

TEST_CASE("Floating-point progress preserves the previous shared value", "[DeviceManager][Progress]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");

    machine.update_print_progress(json(29));
    REQUIRE(machine.mc_print_percent == 29);
    BBLSubTask* subtask = machine.get_subtask();
    REQUIRE(subtask != nullptr);
    REQUIRE(subtask->task_progress == 29);

    machine.update_print_progress(json(29.5));
    BBLSubTask* current_subtask = machine.get_subtask();
    REQUIRE(current_subtask != nullptr);
    REQUIRE(current_subtask == subtask);
    CHECK(machine.mc_print_percent == 29);
    CHECK(current_subtask->task_progress == 29);
}

TEST_CASE("Unsupported progress values leave a fresh machine unchanged", "[DeviceManager][Progress]")
{
    SECTION("boolean") {
        MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
        REQUIRE(machine.subtask_ == nullptr);

        machine.update_print_progress(json(true));

        CHECK(machine.mc_print_percent == 0);
        CHECK(machine.subtask_ == nullptr);
    }

    SECTION("null") {
        MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
        REQUIRE(machine.subtask_ == nullptr);

        machine.update_print_progress(json(nullptr));

        CHECK(machine.mc_print_percent == 0);
        CHECK(machine.subtask_ == nullptr);
    }

    SECTION("object") {
        MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
        REQUIRE(machine.subtask_ == nullptr);

        machine.update_print_progress(json::object());

        CHECK(machine.mc_print_percent == 0);
        CHECK(machine.subtask_ == nullptr);
    }

    SECTION("array") {
        MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
        REQUIRE(machine.subtask_ == nullptr);

        machine.update_print_progress(json::array());

        CHECK(machine.mc_print_percent == 0);
        CHECK(machine.subtask_ == nullptr);
    }
}

TEST_CASE("Malformed string progress leaves a fresh machine unchanged", "[DeviceManager][Progress]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");
    REQUIRE(machine.subtask_ == nullptr);

    CHECK_THROWS_AS(machine.update_print_progress(json("not-a-percent")), std::invalid_argument);
    CHECK(machine.mc_print_percent == 0);
    CHECK(machine.subtask_ == nullptr);
}

TEST_CASE("Zero progress replaces active shared progress", "[DeviceManager][Progress]")
{
    MachineObject machine(nullptr, nullptr, "test", "test-device", "127.0.0.1");

    machine.update_print_progress(json(63));
    BBLSubTask* subtask = machine.get_subtask();
    REQUIRE(subtask != nullptr);
    REQUIRE(machine.mc_print_percent == 63);
    REQUIRE(subtask->task_progress == 63);

    machine.set_print_state("FAILED");
    machine.update_print_progress(json(0));

    BBLSubTask* current_subtask = machine.get_subtask();
    REQUIRE(current_subtask != nullptr);
    REQUIRE(current_subtask == subtask);
    REQUIRE(machine.mc_print_percent == 0);
    CHECK(current_subtask->task_progress == 0);
}
