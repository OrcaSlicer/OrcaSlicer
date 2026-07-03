#include <catch2/catch_all.hpp>

#include "slic3r/Utils/GitSync.hpp"

#include <boost/filesystem.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;

// ============================================================
// GitSync is a real libgit2 backend: upload_file writes+stages,
// flush() commits+pushes, download/list read the working tree,
// refresh() fetches+merges. These tests drive it against a local
// bare repository standing in for the remote -- no network, but a
// full clone/commit/push/fetch round-trip. The bare remote is
// created with the `git` CLI; everything else goes through GitSync
// (libgit2). Tests SKIP when git is unavailable.
// ============================================================
namespace {

bool git_cli_available()
{
    return std::system("git --version > /dev/null 2>&1") == 0;
}

// Unique scratch area holding a bare "remote" repo; hands out GitSyncConfig
// objects that clone into fresh sibling dirs. Everything is removed on teardown.
struct GitScratch {
    fs::path root;
    fs::path remote;
    int      clone_counter{0};

    GitScratch()
    {
        static std::atomic<unsigned> seq{0};
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root   = fs::temp_directory_path() /
                 ("orca-gitsync-" + std::to_string(stamp) + "-" + std::to_string(seq++));
        remote = root / "remote.git";
        fs::create_directories(remote);

        // Bare remote with default branch 'main' so the first clone/push line up.
        const std::string r = remote.string();
        if (std::system(("git init --bare --initial-branch=main '" + r + "' > /dev/null 2>&1").c_str()) != 0) {
            // Older git without --initial-branch: init, then point HEAD at main.
            std::system(("git init --bare '" + r + "' > /dev/null 2>&1").c_str());
            std::system(("git --git-dir='" + r + "' symbolic-ref HEAD refs/heads/main > /dev/null 2>&1").c_str());
        }
    }

    ~GitScratch()
    {
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }

    GitSyncConfig make_config()
    {
        GitSyncConfig cfg;
        cfg.repo_url         = remote.string();
        cfg.branch           = "main";
        cfg.local_clone_path = (root / ("clone" + std::to_string(++clone_counter))).string();
        cfg.author_name      = "Test";
        cfg.author_email     = "test@localhost";
        return cfg;
    }
};

} // namespace

TEST_CASE("GitSync connect clones an empty remote and starts the branch", "[Git][ProfileSync]") {
    if (!git_cli_available()) { SKIP("git CLI not available"); }
    GitScratch  scratch;
    GitSync     git(scratch.make_config());
    std::string err;

    REQUIRE(git.connect(err));
    CHECK(git.is_connected());
}

TEST_CASE("GitSync flush pushes and a fresh clone sees the file", "[Git][ProfileSync]") {
    if (!git_cli_available()) { SKIP("git CLI not available"); }
    GitScratch  scratch;
    std::string err;

    GitSync a(scratch.make_config());
    REQUIRE(a.connect(err));
    REQUIRE(a.ensure_directory("presets/print", err));

    std::string etag;
    REQUIRE(a.upload_file("presets/print/P.json", R"({"v":"1"})", "", etag, err));
    CHECK_FALSE(etag.empty());
    REQUIRE(a.flush(err));

    // A brand-new clone of the same remote must contain the pushed preset.
    GitSync b(scratch.make_config());
    REQUIRE(b.connect(err));

    std::string    content;
    RemoteFileInfo info;
    REQUIRE(b.download_file("presets/print/P.json", content, info, err));
    CHECK(content == R"({"v":"1"})");
    CHECK_FALSE(info.etag.empty());
}

TEST_CASE("GitSync upload_file flags a stale-etag conflict", "[Git][ProfileSync]") {
    if (!git_cli_available()) { SKIP("git CLI not available"); }
    GitScratch  scratch;
    std::string err;

    GitSync a(scratch.make_config());
    REQUIRE(a.connect(err));
    REQUIRE(a.ensure_directory("presets/print", err));

    std::string etag1;
    REQUIRE(a.upload_file("presets/print/P.json", R"({"v":"1"})", "", etag1, err));

    // Wrong expected etag -> conflict; the file must not change.
    std::string etag2;
    SyncError   code = SyncError::None;
    CHECK_FALSE(a.upload_file("presets/print/P.json", R"({"v":"2"})", "deadbeef", etag2, err, &code));
    CHECK(code == SyncError::Conflict);

    // Correct expected etag -> overwrite succeeds.
    REQUIRE(a.upload_file("presets/print/P.json", R"({"v":"2"})", etag1, etag2, err));
    std::string    content;
    RemoteFileInfo info;
    REQUIRE(a.download_file("presets/print/P.json", content, info, err));
    CHECK(content == R"({"v":"2"})");
}

TEST_CASE("GitSync delete_file propagates through flush", "[Git][ProfileSync]") {
    if (!git_cli_available()) { SKIP("git CLI not available"); }
    GitScratch  scratch;
    std::string err;

    GitSync a(scratch.make_config());
    REQUIRE(a.connect(err));
    REQUIRE(a.ensure_directory("presets/print", err));
    std::string etag;
    REQUIRE(a.upload_file("presets/print/P.json", R"({"v":"1"})", "", etag, err));
    REQUIRE(a.flush(err));

    REQUIRE(a.delete_file("presets/print/P.json", err));
    REQUIRE(a.flush(err));

    GitSync b(scratch.make_config());
    REQUIRE(b.connect(err));
    std::string    content;
    RemoteFileInfo info;
    SyncError      code = SyncError::None;
    CHECK_FALSE(b.download_file("presets/print/P.json", content, info, err, &code));
    CHECK(code == SyncError::NotFound);
}

TEST_CASE("GitSync list_files returns uploaded presets and skips .gitkeep", "[Git][ProfileSync]") {
    if (!git_cli_available()) { SKIP("git CLI not available"); }
    GitScratch  scratch;
    std::string err;

    GitSync a(scratch.make_config());
    REQUIRE(a.connect(err));
    REQUIRE(a.ensure_directory("presets/filament", err));
    std::string etag;
    REQUIRE(a.upload_file("presets/filament/F.json", R"({"v":"1"})", "", etag, err));

    std::vector<RemoteFileInfo> out;
    REQUIRE(a.list_files("presets/filament", out, err));

    bool found_preset = false;
    for (const auto& f : out) {
        CHECK(f.path.find(".gitkeep") == std::string::npos);
        if (f.path.find("F.json") != std::string::npos) {
            found_preset = true;
            CHECK_FALSE(f.is_directory);
            CHECK_FALSE(f.etag.empty());
        }
    }
    CHECK(found_preset);
}

TEST_CASE("GitSync refresh fast-forwards a clone to new remote commits", "[Git][ProfileSync]") {
    if (!git_cli_available()) { SKIP("git CLI not available"); }
    GitScratch  scratch;
    std::string err;

    GitSync a(scratch.make_config());
    REQUIRE(a.connect(err));
    REQUIRE(a.ensure_directory("presets/print", err));
    std::string etag_v1;
    REQUIRE(a.upload_file("presets/print/P.json", R"({"v":"1"})", "", etag_v1, err));
    REQUIRE(a.flush(err));

    // Second clone starts at v1.
    GitSync b(scratch.make_config());
    REQUIRE(b.connect(err));

    // A pushes v2 on top of v1.
    std::string etag_v2;
    REQUIRE(a.upload_file("presets/print/P.json", R"({"v":"2"})", etag_v1, etag_v2, err));
    REQUIRE(a.flush(err));

    // B fast-forwards to it via refresh().
    REQUIRE(b.refresh(err));
    std::string    content;
    RemoteFileInfo info;
    REQUIRE(b.download_file("presets/print/P.json", content, info, err));
    CHECK(content == R"({"v":"2"})");
}
