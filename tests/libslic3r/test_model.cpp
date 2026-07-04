#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"

using namespace Slic3r;

// Checks that get_model_backup_root composes the backup directory name: the user
// id is appended so each user gets a distinct root, and an empty id keeps the
// legacy name.
TEST_CASE("get_model_backup_root builds a per-user backup dir name", "[Model]") {
    const std::string tmp = "/tmp";

    SECTION("distinct users get distinct roots") {
        REQUIRE(Model::get_model_backup_root(tmp, "1000") != Model::get_model_backup_root(tmp, "1001"));
    }
    SECTION("the user id is embedded in the root") {
        REQUIRE_THAT(Model::get_model_backup_root(tmp, "1000"), Catch::Matchers::ContainsSubstring("1000"));
    }
    SECTION("an empty user id keeps the legacy path") {
        REQUIRE(Model::get_model_backup_root(tmp, "") == tmp + "/orcaslicer_model");
    }
}
