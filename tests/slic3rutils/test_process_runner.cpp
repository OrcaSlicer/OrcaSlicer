#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include "slic3r/GUI/ProcessRunner.hpp"
#include "slic3r/GUI/GUI.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <wx/init.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

TEST_CASE("Stopping a worker does not wait for a concurrently launched worker to exit", "[ProcessRunner][Regression]")
{
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        SKIP("wx initialization requires a display (for example, xvfb-run).");
#endif
    wxInitializer init;
    REQUIRE(init.IsOk());
    wxFileName child(wxStandardPaths::Get().GetExecutablePath());
#ifdef _WIN32
    child.SetFullName("process_runner_test_child.exe");
#else
    child.SetFullName("process_runner_test_child");
#endif
    REQUIRE(child.FileExists());
    const std::string command = "\"" + Slic3r::GUI::into_u8(child.GetFullPath()) + "\"";
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    for (int attempt = 0; attempt < 8; ++attempt) {
        INFO("launch attempt " << attempt);
        Slic3r::GUI::ProcessRunner first, second;
        REQUIRE(first.run_command_line_async(command, {}, {}));
        REQUIRE(second.run_command_line_async(command, {}, {}));
        const auto deadline = Clock::now() + 2s;
        while ((!first.can_write_stdin() || !second.can_write_stdin()) && Clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        REQUIRE(first.can_write_stdin());
        REQUIRE(second.can_write_stdin());
        const auto began = Clock::now();
        first.terminate();
        const auto elapsed = Clock::now() - began;
        const bool peer_running = second.is_running();
        second.terminate();
        CHECK(elapsed < 1s);
        CHECK(peer_running);
        CHECK_FALSE(first.is_running());
    }
}
