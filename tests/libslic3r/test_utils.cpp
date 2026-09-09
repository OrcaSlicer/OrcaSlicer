#include <catch2/catch_all.hpp>

#include "libslic3r/Utils.hpp"

#include "test_utils.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

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

TEST_CASE("copy_file reports the OS error when the destination cannot be written", "[utils]") {
    ScopedTemporaryFile source(".txt");
    {
        std::ofstream ofs(source.string(), std::ios::binary);
        ofs << "orca";
    }
    REQUIRE(boost::filesystem::exists(source.path()));

    // A directory that was never created, so the copy fails on every platform.
    const boost::filesystem::path destination = source.path().parent_path() / "orca-missing-dir" / "copy.txt";
    REQUIRE_FALSE(boost::filesystem::exists(destination.parent_path()));

    std::string error_message;
    REQUIRE(copy_file(source.string(), destination.string(), error_message) == FAIL_COPY_FILE);
    REQUIRE_FALSE(error_message.empty());

#ifdef _WIN32
    // The Windows branch formats GetLastError() itself. Writing that as
    // "Error: " + errCode adds an integer to a string literal, which indexes into the
    // literal instead of appending and runs off its end for any code above 7.
    const std::string prefix = "Error: ";
    REQUIRE(error_message.rfind(prefix, 0) == 0);

    const std::string code = error_message.substr(prefix.size());
    REQUIRE_FALSE(code.empty());
    REQUIRE(std::all_of(code.begin(), code.end(), [](unsigned char c) { return std::isdigit(c) != 0; }));
#endif // _WIN32
}
