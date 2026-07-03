#include <catch2/catch_all.hpp>

#include "libslic3r/AppConfig.hpp"

using namespace Slic3r;

// AppConfig::set_defaults() runs the one-shot migration that converts the
// legacy sync_user_preset / selfhost_sync_* keys into the unified
// profile_sync_* namespace. The migration must be idempotent and must
// pick the correct provider for each combination of legacy values.

TEST_CASE("Profile sync migration: WebDAV", "[AppConfig][ProfileSync]") {
    AppConfig cfg;
    cfg.set("selfhost_sync_backend",      "1");          // WebDAV
    cfg.set("selfhost_sync_webdav_url",   "https://dav.example.com/files/");
    cfg.set("selfhost_sync_webdav_user",  "alice");
    cfg.set("selfhost_sync_webdav_pass",  "s3cret");
    cfg.set("selfhost_sync_interval",     "2");          // 15 minutes
    cfg.set_bool("selfhost_sync_readonly",     true);
    cfg.set_bool("selfhost_sync_always_review", true);

    cfg.set_defaults();

    CHECK(cfg.get("profile_sync_provider")      == "webdav");
    CHECK(cfg.get_bool("profile_sync_auto")     == true);
    CHECK(cfg.get("profile_sync_interval_sec")  == "900");
    CHECK(cfg.get_bool("profile_sync_read_only")     == true);
    CHECK(cfg.get_bool("profile_sync_always_review") == true);

    CHECK(cfg.get("profile_sync_webdav_url")  == "https://dav.example.com/files/");
    CHECK(cfg.get("profile_sync_webdav_user") == "alice");
    CHECK(cfg.get("profile_sync_webdav_pass") == "s3cret");

    CHECK(cfg.get_bool("profile_sync_migrated_v1") == true);
}

TEST_CASE("Profile sync migration: Git with default branch", "[AppConfig][ProfileSync]") {
    AppConfig cfg;
    cfg.set("selfhost_sync_backend",   "2");           // Git
    cfg.set("selfhost_sync_git_url",   "git@github.com:me/dotfiles.git");
    cfg.set("selfhost_sync_git_token", "ghp_xxx");
    // branch left empty -> defaults to "main"
    cfg.set("selfhost_sync_interval",  "4");           // 1 hour

    cfg.set_defaults();

    CHECK(cfg.get("profile_sync_provider")     == "git");
    CHECK(cfg.get("profile_sync_git_url")      == "git@github.com:me/dotfiles.git");
    CHECK(cfg.get("profile_sync_git_branch")   == "main");
    CHECK(cfg.get("profile_sync_git_token")    == "ghp_xxx");
    CHECK(cfg.get("profile_sync_interval_sec") == "3600");
}

TEST_CASE("Profile sync migration: Orca via sync_user_preset", "[AppConfig][ProfileSync]") {
    AppConfig cfg;
    cfg.set_bool("sync_user_preset", true);

    cfg.set_defaults();

    CHECK(cfg.get("profile_sync_provider")  == "orca");
    CHECK(cfg.get_bool("profile_sync_auto") == true);
}

TEST_CASE("Profile sync migration: nothing set defaults to disabled", "[AppConfig][ProfileSync]") {
    AppConfig cfg;
    cfg.set_defaults();

    CHECK(cfg.get("profile_sync_provider")  == "disabled");
}

TEST_CASE("Profile sync migration is idempotent", "[AppConfig][ProfileSync]") {
    AppConfig cfg;
    cfg.set("selfhost_sync_backend",     "1");
    cfg.set("selfhost_sync_webdav_url",  "https://first.example/");
    cfg.set_defaults();
    REQUIRE(cfg.get("profile_sync_provider") == "webdav");

    // Second run with different legacy data must NOT overwrite the
    // already-migrated unified keys (guarded by profile_sync_migrated_v1).
    cfg.set("selfhost_sync_webdav_url", "https://second.example/");
    cfg.set_defaults();
    CHECK(cfg.get("profile_sync_webdav_url") == "https://first.example/");
}
