#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Mcp/McpPaths.hpp"
#include "test_utils.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

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
