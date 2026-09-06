#include <catch2/catch_all.hpp>

#include "libslic3r/Utils.hpp"
#include "test_utils.hpp"

#include <boost/filesystem.hpp>
#include <fstream>

#ifndef _WIN32
#include <unistd.h>     // getuid
#endif

using namespace Slic3r;

TEST_CASE("per_user_temp_dir composes a per-user temp root", "[utils]") {
    const std::string base = "/tmp";

    SECTION("an empty id returns base unchanged") {
        REQUIRE(per_user_temp_dir(base, "") == base);
    }
    SECTION("a non-empty id is appended at the top level") {
        REQUIRE(per_user_temp_dir(base, "1000") == base + "/orcaslicer_1000");
    }
    SECTION("distinct ids produce distinct roots") {
        REQUIRE(per_user_temp_dir(base, "1000") != per_user_temp_dir(base, "1001"));
    }
}

TEST_CASE("per_user_temp_id follows the platform contract", "[utils]") {
    const std::string id = per_user_temp_id();

    SECTION("stable across calls") {
        REQUIRE(per_user_temp_id() == id);
    }
#ifdef _WIN32
    SECTION("empty on Windows (its temp dir is already per-user)") {
        REQUIRE(id.empty());
    }
#else
    SECTION("the current uid on Linux/macOS") {
        REQUIRE_FALSE(id.empty());
        REQUIRE(id == std::to_string(static_cast<unsigned long>(::getuid())));
    }
#endif
}

// The end-to-end contract callers depend on: the temp root is left alone on
// Windows and isolated per user on Linux/macOS.
TEST_CASE("per-user temp root is unchanged on Windows, isolated elsewhere", "[utils]") {
    const std::string base = "/tmp";
    const std::string root = per_user_temp_dir(base, per_user_temp_id());
#ifdef _WIN32
    REQUIRE(root == base);
#else
    REQUIRE(root != base);
    REQUIRE_THAT(root, Catch::Matchers::StartsWith(base + "/orcaslicer_"));
#endif
}

TEST_CASE("A resolved input path still names the same file after the working directory changes", "[utils]") {
    ScopedTemporaryFile model(".3mf");
    { std::ofstream out(model.string()); out << "3mf"; }
    const std::string name = model.path().filename().string();

    // Resolve the bare name from the directory holding the file, then move away from it. The guard
    // restores the directory the test started in, wherever this leaves it.
    ScopedWorkingDirectory cwd(model.path().parent_path());
    const std::string resolved = resolve_cli_input_path(name);
    boost::filesystem::current_path(boost::filesystem::path(TEST_DATA_DIR));

    REQUIRE(boost::filesystem::exists(resolved));
    REQUIRE(boost::filesystem::equivalent(resolved, model.path()));
    // Control: the bare name finds nothing from here, so resolving it this late would have failed.
    REQUIRE_FALSE(boost::filesystem::exists(name));
}

TEST_CASE("resolve_cli_input_path completes a relative path against the working directory", "[utils]") {
    ScopedWorkingDirectory cwd(boost::filesystem::temp_directory_path());
    // Read back rather than reusing temp_directory_path(): changing to it resolves any symlink.
    const boost::filesystem::path here = boost::filesystem::current_path();

    SECTION("a bare name") {
        REQUIRE(resolve_cli_input_path("model.3mf") == (here / "model.3mf").make_preferred().string());
    }
    SECTION("a ./ prefix is dropped") {
        REQUIRE(resolve_cli_input_path("./model.3mf") == (here / "model.3mf").make_preferred().string());
    }
    SECTION("a ../ traversal is collapsed") {
        REQUIRE(resolve_cli_input_path("../model.3mf") == (here.parent_path() / "model.3mf").make_preferred().string());
    }
}

TEST_CASE("resolve_cli_input_path leaves inputs that must not be completed unchanged", "[utils]") {
    SECTION("an absolute path") {
        const boost::filesystem::path absolute = (boost::filesystem::temp_directory_path() / "model.3mf").make_preferred();
        REQUIRE(resolve_cli_input_path(absolute.string()) == absolute.string());
    }
#ifdef _WIN32
    // Every absolute form Windows accepts opens today, so each must come back byte for byte:
    // normalizing them would rewrite the forward slashes and rebuild the \\?\ and UNC prefixes.
    SECTION("an absolute Windows path of any form") {
        for (const std::string absolute : {R"(C:\models\model.3mf)",
                                           R"(C:/models/model.3mf)",
                                           R"(\\server\share\model.3mf)",
                                           R"(\\?\C:\models\model.3mf)"})
            REQUIRE(resolve_cli_input_path(absolute) == absolute);
    }
#endif
    // These are downloaded rather than opened, and completing one would produce a path, not a URL.
    SECTION("a custom open protocol URL") {
        for (const std::string url : {"orcaslicer://open/?file=https://example.com/model.3mf",
                                      "prusaslicer://open/?file=https://example.com/model.3mf",
                                      "bambustudio://open/?file=https://example.com/model.3mf",
                                      "cura://open/?file=https://example.com/model.3mf"})
            REQUIRE(resolve_cli_input_path(url) == url);
    }
    SECTION("an empty argument") {
        REQUIRE(resolve_cli_input_path("").empty());
    }
}
