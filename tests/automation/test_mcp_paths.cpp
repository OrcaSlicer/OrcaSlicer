#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Mcp/McpPaths.hpp"
#include "test_utils.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <ctime>
#include <string>

using namespace Slic3r::GUI::Mcp;
namespace fs = boost::filesystem;

TEST_CASE("files saved without a path go to the run's folder when the command line gave one", "[Mcp]")
{
    ScopedTemporaryDir datadir("orca-mcp");
    // The data folder and the run's folder are process-wide: both go back however the case ends.
    struct Restore
    {
        std::string data_dir = Slic3r::data_dir();
        ~Restore()
        {
            set_run_output_dir({});
            Slic3r::set_data_dir(data_dir);
        }
    } restore;
    Slic3r::set_data_dir(datadir.string());

    const fs::path default_dir = output_dir();
    CHECK(default_dir.filename() == "out");
    CHECK(default_dir.parent_path().filename() == "automation");
    CHECK(default_dir.parent_path().parent_path() == datadir.path());
    CHECK(fs::is_directory(default_dir));

    const fs::path run_dir = datadir.path() / "x";
    REQUIRE_FALSE(fs::exists(run_dir));
    set_run_output_dir(run_dir);
    CHECK(output_dir() == run_dir);
    CHECK(fs::is_directory(run_dir));

    set_run_output_dir({});
    CHECK(output_dir() == default_dir);
}

TEST_CASE("a replay with no out_dir writes into a new folder of the temp folder, or of the run's folder", "[Mcp]")
{
    ScopedTemporaryDir temp("orca-mcp");
    // The temporary folder and the run's folder are process-wide: both go back however the case ends.
    struct Restore
    {
        std::string temporary_dir = Slic3r::temporary_dir();
        ~Restore()
        {
            set_run_output_dir({});
            Slic3r::set_temporary_dir(temporary_dir);
        }
    } restore;
    Slic3r::set_temporary_dir(temp.string());

    std::tm local{};
    local.tm_year      = 2026 - 1900;
    local.tm_mon       = 10;
    local.tm_mday      = 21;
    local.tm_hour      = 9;
    local.tm_min       = 5;
    local.tm_sec       = 7;
    local.tm_isdst     = -1;
    const std::time_t when = std::mktime(&local);

    const fs::path first = replay_output_dir(when);
    CHECK(first == temp.path() / "orca_action_replay" / "replay-20261121-090507");
    CHECK_FALSE(fs::exists(first));

    // A run creates its folder; one started in the same second, as a replay that the first runs as a step, gets another.
    fs::create_directories(first);
    CHECK(replay_output_dir(when).filename() == "replay-20261121-090507-2");
    fs::create_directories(first.parent_path() / "replay-20261121-090507-2");
    CHECK(replay_output_dir(when).filename() == "replay-20261121-090507-3");

    const fs::path run_dir = temp.path() / "run";
    set_run_output_dir(run_dir);
    CHECK(replay_output_dir(when) == run_dir / "replay-20261121-090507");
}
