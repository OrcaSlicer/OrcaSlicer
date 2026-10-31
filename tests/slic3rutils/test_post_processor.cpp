#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <exception>
#include <fstream>
#include <string>
#include <utility>
#ifndef _WIN32
#include <cstdlib>
#include <optional>
#include <stdlib.h>
#endif

#include <wx/event.h>

#include "libslic3r/Config.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "slic3r/GUI/PostProcessor.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;

namespace {

// Runs `script` as the post-processing script on a real G-code file, so a failure comes from the script
// and not from a missing input, and returns what it threw.
std::exception_ptr post_process_with(const std::string& script)
{
    ScopedTemporaryFile gcode(".gcode");
    std::ofstream(gcode.string()) << "G1 X10 Y10\n";
    DynamicPrintConfig config;
    config.set_key_value("post_process", new ConfigOptionStrings({script}));
    std::string path        = gcode.string();
    std::string output_name = gcode.string();
    try {
        run_post_process_scripts(path, false, "File", output_name, config);
    } catch (...) {
        return std::current_exception();
    }
    return nullptr;
}

std::string message_of(const std::exception_ptr& ex)
{
    try {
        std::rethrow_exception(ex);
    } catch (const std::exception& e) {
        return e.what();
    }
}

SlicingProcessCompletedEvent completed(SlicingProcessCompletedEvent::StatusType status, std::exception_ptr ex)
{
    return SlicingProcessCompletedEvent(wxEVT_NULL, 0, status, std::move(ex));
}

#ifndef _WIN32
// Sets an environment variable for the scope, restoring its previous value.
class ScopedEnv
{
public:
    ScopedEnv(const char* name, const char* value) : m_name(name)
    {
        if (const char* old = std::getenv(name))
            m_old = old;
        ::setenv(name, value, 1);
    }
    ~ScopedEnv()
    {
        if (m_old)
            ::setenv(m_name.c_str(), m_old->c_str(), 1);
        else
            ::unsetenv(m_name.c_str());
    }

private:
    std::string                m_name;
    std::optional<std::string> m_old;
};
#endif

} // namespace

TEST_CASE("A post-processing script that cannot run fails the export, not the slice", "[PostProcessor]")
{
    ScopedTemporaryFile missing(".sh");
    // Quoted, as a configured path with spaces must be (the temp directory may have some).
    const std::exception_ptr ex = post_process_with("\"" + missing.string() + "\"");

    REQUIRE(ex);
    CHECK_THROWS_AS(std::rethrow_exception(ex), ExportError);
    CHECK_THAT(message_of(ex), ContainsSubstring(missing.path().filename().string()));
    const SlicingProcessCompletedEvent evt = completed(SlicingProcessCompletedEvent::Error, ex);
    CHECK(evt.export_failed());
    CHECK_FALSE(evt.invalidate_plater());
}

#ifndef _WIN32
TEST_CASE("A post-processing script that fails reports its exit code as an export error", "[PostProcessor]")
{
    const std::exception_ptr ex = post_process_with("sh -c 'echo \"; half edited\" >> \"$0\"; exit 3'");

    REQUIRE(ex);
    CHECK_THROWS_AS(std::rethrow_exception(ex), ExportError);
    CHECK_THAT(message_of(ex), ContainsSubstring("Error code: 3"));
}

TEST_CASE("A shell that cannot start fails the export, not the slice", "[PostProcessor]")
{
    ScopedTemporaryFile missing_shell(".sh");
    ScopedEnv           shell("SHELL", missing_shell.string().c_str());
    const std::exception_ptr ex = post_process_with("true");

    REQUIRE(ex);
    CHECK_THROWS_AS(std::rethrow_exception(ex), ExportError);
}
#endif

TEST_CASE("A completion event tells a failed export from a failed slice", "[PostProcessor]")
{
    const SlicingProcessCompletedEvent export_error =
        completed(SlicingProcessCompletedEvent::Error, std::make_exception_ptr(ExportError("copy failed")));
    CHECK(export_error.export_failed());
    CHECK(export_error.critical_error());
    CHECK_FALSE(export_error.invalidate_plater());

    const SlicingProcessCompletedEvent slice_error =
        completed(SlicingProcessCompletedEvent::Error, std::make_exception_ptr(RuntimeError("slicing failed")));
    CHECK_FALSE(slice_error.export_failed());
    CHECK(slice_error.invalidate_plater());

    // A cancel or a success carries no exception to inspect.
    for (const auto status : {SlicingProcessCompletedEvent::Cancelled, SlicingProcessCompletedEvent::Finished}) {
        const SlicingProcessCompletedEvent evt = completed(status, nullptr);
        CHECK_FALSE(evt.export_failed());
        CHECK_FALSE(evt.invalidate_plater());
    }
}
