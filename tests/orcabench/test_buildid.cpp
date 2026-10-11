#include <catch2/catch_test_macros.hpp>

#include "core/BuildId.hpp"

#include <string>

using namespace Slic3r::Bench;

TEST_CASE("the build records the commit it was built from", "[OrcaBench][BuildId]")
{
    const std::string revision = build_revision;
    REQUIRE_FALSE(revision.empty());
    CHECK(revision.find_first_not_of("0123456789abcdef") == std::string::npos);

    if (!ORCABENCH_HAS_COMMIT)
        SKIP("built without git or git_commit_hash, so the placeholder is the right answer");
    // The script's placeholder when git cannot answer.
    CHECK(revision != "0000000");
}

TEST_CASE("the build records its compiler and configuration", "[OrcaBench][BuildId]")
{
    // build_flags can be empty.
    CHECK_FALSE(std::string(build_compiler).empty());
    CHECK_FALSE(std::string(build_compiler_version).empty());
    CHECK_FALSE(std::string(build_config).empty());
}
