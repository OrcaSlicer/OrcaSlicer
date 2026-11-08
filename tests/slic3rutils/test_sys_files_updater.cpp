#include <catch2/catch_all.hpp>

#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "libslic3r/miniz_extension.hpp"
#include "slic3r/Utils/DuetRrfClient.hpp"
#include "slic3r/Utils/FirmwareBundle.hpp"
#include "slic3r/Utils/SysFilesBundle.hpp"
#include "slic3r/Utils/SysFilesUpdater.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace {

std::vector<uint8_t> bytes(const std::string &s) { return std::vector<uint8_t>(s.begin(), s.end()); }
std::string          text(const std::vector<uint8_t> &b) { return std::string(b.begin(), b.end()); }

// An SD card in memory behind the DuetClient interface, recording what was asked of it.
class FakeBoard : public DuetClient
{
public:
    std::map<std::string, std::vector<uint8_t>> card; // "0:/sys/homez.g" -> bytes
    std::string                                 status_value = "idle";
    std::string                                 firmware     = "3.7.0-beta.1+unlayered.19";
    Capabilities                                caps;
    std::string                                 fail_upload_of;   // this remote path fails to upload
    bool                                        fail_corrupts = false; // ...after writing garbage to it
    std::function<void(const std::string &)>    after_upload;     // e.g. to cancel mid-run
    std::vector<std::string>                    uploads, removes, gcodes;
    bool                                        connected = false;

    Capabilities       capabilities() const override { return caps; }
    const std::string &endpoint() const override { return m_endpoint; }
    ConnectResult      connect(std::string &) override { connected = true; return ConnectResult::ok; }
    void               disconnect() override { connected = false; }

    bool object_model(const std::string &key, const std::string &, pt::ptree &out, std::string &) override
    {
        out = pt::ptree();
        if (key == "boards[0]") {
            out.put("firmwareVersion", firmware);
            out.put("firmwareFileName", "Duet3Firmware_Mini5plus.uf2");
            out.put("shortName", "Mini5plus");
            out.put("uniqueId", "TEST-BOARD-1");
            out.put("name", "Duet 3 Mini 5+");
        } else if (key == "network") {
            out.put("name", "Unlayered Test Printer");
        } else if (key == "state") {
            out.put("status", status_value);
        }
        return true;
    }
    bool gcode(const std::string &cmd, std::string &) override
    {
        gcodes.push_back(cmd);
        return true;
    }
    bool upload(const std::string &remote, const std::vector<uint8_t> &data, ProgressFn, std::string &err) override
    {
        uploads.push_back(remote);
        if (remote == fail_upload_of) {
            if (fail_corrupts)
                card[remote] = bytes("half a fi");
            err = "HTTP 500";
            return false;
        }
        card[remote] = data;
        if (after_upload)
            after_upload(remote);
        return true;
    }
    bool download(const std::string &remote, std::vector<uint8_t> &out, bool &missing, std::string &) override
    {
        auto it = card.find(remote);
        missing = it == card.end();
        if (missing)
            return false;
        out = it->second;
        return true;
    }
    bool filelist(const std::string &, std::vector<FileEntry> &, bool &missing, std::string &) override
    {
        missing = true;
        return true;
    }
    bool mkdir(const std::string &, std::string &) override { return true; }
    bool remove(const std::string &remote, std::string &) override
    {
        removes.push_back(remote);
        return card.erase(remote) > 0;
    }
    bool wait_for_reboot(int, const std::function<bool()> &, const std::function<void(int)> &, std::string &) override
    {
        return true;
    }

private:
    std::string m_endpoint = "192.0.2.19";
};

class FakeSource : public SysFileSource
{
public:
    SysFilePackage pkg;
    bool           fetch(SysFilePackage &out, SysProgressFn, std::string &) override { out = pkg; return true; }
    std::string    description() const override { return "test package"; }
};

SysFile file(const std::string &path, const std::string &content)
{
    SysFile f;
    f.path = path;
    f.data = bytes(content);
    f.sha256 = sha256_hex(f.data);
    return f;
}

// The 2026.10.21 release's shape: two homing macros, plus one file the card does not have yet.
SysFilePackage release_package()
{
    SysFilePackage p;
    p.version = "2026.10.21";
    p.date    = "2026-10-21";
    p.machine = "Unlayered Test Printer";
    p.files   = {file("sys/homez.g", "; homez.g NEW\r\nG30\r\n"), file("sys/homezstall.g", "; homezstall.g NEW\r\n"),
                 file("macros/Service/Measure Z stop", "M98 P\"0:/sys/homezstall.g\" R1\r\n")};
    return p;
}

struct Ctl : SysFilesUpdateCtl
{
    bool                     cancel = false;
    std::vector<std::string> messages;
    void status(int, const std::string &m) override { messages.push_back(m); }
    bool cancelled() const override { return cancel; }
};

struct TempDir
{
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / fs::unique_path("sysfiles-test-%%%%-%%%%")) { fs::create_directories(path); }
    ~TempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

FakeBoard board_with_previous_config()
{
    FakeBoard b;
    b.card["0:/sys/homez.g"]           = bytes("; homez.g OLD\r\n");
    b.card["0:/sys/homezstall.g"]      = bytes("; homezstall.g OLD\r\n");
    b.card["0:/sys/zstall.g"]          = bytes("global zStallTip = 62.8\r\n");
    b.card["0:/sys/unlayered-sys.json"] = bytes("{\"version\":\"2026.10.12\",\"installedAt\":\"2026-10-12T21:00:00\"}");
    return b;
}

SysFilesUpdateResult run(FakeBoard &board, const SysFilePackage &pkg, SysFilesUpdateOptions opts, Ctl &ctl)
{
    SysFilesUpdateResult r;
    run_sys_files_update(board, [pkg]() {
        auto s = std::make_unique<FakeSource>();
        s->pkg = pkg;
        return s;
    }, opts, ctl, r);
    return r;
}

} // namespace

TEST_CASE("An install writes the files, records the version, keeps a backup and restarts", "[SysFilesUpdater]")
{
    TempDir   tmp;
    FakeBoard board = board_with_previous_config();
    Ctl       ctl;
    SysFilesUpdateOptions opts;
    opts.backup_root = tmp.path.string();

    const SysFilesUpdateResult r = run(board, release_package(), opts, ctl);

    REQUIRE(r.outcome == SysFilesUpdateResult::Outcome::Success);
    CHECK(r.restarted);
    CHECK(board.gcodes == std::vector<std::string>{"M999"});
    CHECK(text(board.card["0:/sys/homez.g"]) == "; homez.g NEW\r\nG30\r\n");
    CHECK(text(board.card["0:/macros/Service/Measure Z stop"]) == "M98 P\"0:/sys/homezstall.g\" R1\r\n");
    // The machine's own measured value is not the package's business.
    CHECK(text(board.card["0:/sys/zstall.g"]) == "global zStallTip = 62.8\r\n");
    CHECK(r.created == std::vector<std::string>{"macros/Service/Measure Z stop"});

    SysFilesInstalled marker;
    REQUIRE(parse_sys_marker(text(board.card["0:/sys/unlayered-sys.json"]), marker));
    CHECK(marker.version == "2026.10.21");
    CHECK(marker.file_count == 3);

    // The backup holds exactly what was replaced, byte for byte, under the machine's name.
    REQUIRE_FALSE(r.backup_dir.empty());
    const fs::path dir(r.backup_dir);
    CHECK(dir.parent_path().filename().string() == "Unlayered_Test_Printer");
    std::ifstream homez((dir / "sys" / "homez.g").string(), std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(homez), {}) == "; homez.g OLD\r\n");
    CHECK_FALSE(fs::exists(dir / "macros" / "Service" / "Measure Z stop")); // it did not exist before
    pt::ptree info;
    pt::read_json((dir / ".unlayered-backup.json").string(), info);
    CHECK(info.get<std::string>("configurationBefore") == "2026.10.12");
    CHECK(info.get<std::string>("configurationInstalled") == "2026.10.21");
    CHECK(info.get_child("replaced").size() == 2);
    CHECK(info.get_child("created").size() == 1);
    CHECK(fs::exists(dir / ".unlayered-sys.before.json"));
}

TEST_CASE("The backup folder restores through From Folder without its description", "[SysFilesUpdater]")
{
    TempDir   tmp;
    FakeBoard board = board_with_previous_config();
    Ctl       ctl;
    SysFilesUpdateOptions opts;
    opts.backup_root = tmp.path.string();
    const SysFilesUpdateResult r = run(board, release_package(), opts, ctl);
    REQUIRE(r.outcome == SysFilesUpdateResult::Outcome::Success);

    LocalSysFileSource restore(r.backup_dir);
    SysFilePackage     pkg;
    std::string        err;
    REQUIRE(restore.fetch(pkg, nullptr, err));
    std::set<std::string> paths;
    for (const auto &f : pkg.files)
        paths.insert(f.path);
    CHECK(paths == std::set<std::string>{"sys/homez.g", "sys/homezstall.g"});
}

TEST_CASE("A failed write puts back what was replaced and removes what was created", "[SysFilesUpdater]")
{
    TempDir   tmp;
    FakeBoard board = board_with_previous_config();
    const auto before = board.card;
    board.fail_upload_of = "0:/macros/Service/Measure Z stop"; // the third file, after two were written
    board.fail_corrupts  = true;
    Ctl ctl;
    SysFilesUpdateOptions opts;
    opts.backup_root = tmp.path.string();

    const SysFilesUpdateResult r = run(board, release_package(), opts, ctl);

    CHECK(r.outcome == SysFilesUpdateResult::Outcome::Failed);
    CHECK(r.rolled_back);
    CHECK(r.rollback_failed.empty());
    CHECK(board.card == before); // marker included: the old version is still what the card claims
    CHECK(board.gcodes.empty()); // never restarted onto a half-written configuration
    CHECK(boost::contains(r.message, "put back"));
}

TEST_CASE("Cancelling part way puts the card back too", "[SysFilesUpdater]")
{
    FakeBoard board = board_with_previous_config();
    const auto before = board.card;
    Ctl ctl;
    board.after_upload = [&ctl](const std::string &remote) {
        if (remote == "0:/sys/homez.g")
            ctl.cancel = true;
    };
    SysFilesUpdateOptions opts;
    opts.backup = false; // rollback does not depend on the backup folder

    const SysFilesUpdateResult r = run(board, release_package(), opts, ctl);

    CHECK(r.outcome == SysFilesUpdateResult::Outcome::Cancelled);
    CHECK(r.rolled_back);
    CHECK(r.backup_dir.empty());
    CHECK(board.card == before);
}

TEST_CASE("Without exact read-back there is no backup and no rollback, and the message says so", "[SysFilesUpdater]")
{
    FakeBoard board = board_with_previous_config();
    board.caps.read_files_exact = false;
    board.fail_upload_of = "0:/sys/homezstall.g";
    Ctl ctl;
    SysFilesUpdateOptions opts;

    const SysFilesUpdateResult r = run(board, release_package(), opts, ctl);

    CHECK(r.outcome == SysFilesUpdateResult::Outcome::Failed);
    CHECK_FALSE(r.rolled_back);
    CHECK(r.backup_dir.empty());
    CHECK(boost::contains(r.message, "part way"));
    CHECK(text(board.card["0:/sys/homez.g"]) == "; homez.g NEW\r\nG30\r\n");
}

TEST_CASE("A busy printer is not touched", "[SysFilesUpdater]")
{
    FakeBoard board = board_with_previous_config();
    board.status_value = "processing";
    const auto before = board.card;
    Ctl ctl;
    const SysFilesUpdateResult r = run(board, release_package(), {}, ctl);
    CHECK(r.outcome == SysFilesUpdateResult::Outcome::Failed);
    CHECK(board.uploads.empty());
    CHECK(board.card == before);
}

TEST_CASE("Files that need newer firmware are refused before anything is read or written", "[SysFilesUpdater]")
{
    FakeBoard board = board_with_previous_config();
    board.firmware  = "3.7.0-beta.1+unlayered.9";
    SysFilePackage pkg = release_package();
    pkg.min_firmware   = "3.7.0-beta.1+unlayered.19";
    Ctl ctl;
    const SysFilesUpdateResult r = run(board, pkg, {}, ctl);
    CHECK(r.outcome == SysFilesUpdateResult::Outcome::Failed);
    CHECK(boost::contains(r.message, "Update the firmware first"));
    CHECK(board.uploads.empty());
}

TEST_CASE("The installed version is not installed again unless forced", "[SysFilesUpdater]")
{
    FakeBoard board = board_with_previous_config();
    board.card["0:/sys/unlayered-sys.json"] = bytes("{\"version\":\"2026.10.21\",\"date\":\"2026-10-21\"}");
    Ctl ctl;
    CHECK(run(board, release_package(), {}, ctl).outcome == SysFilesUpdateResult::Outcome::UpToDate);
    CHECK(board.uploads.empty());

    SysFilesUpdateOptions force;
    force.backup = false;
    force.force  = true;
    CHECK(run(board, release_package(), force, ctl).outcome == SysFilesUpdateResult::Outcome::Success);
}

TEST_CASE("Only the ticked files are installed", "[SysFilesUpdater]")
{
    FakeBoard board = board_with_previous_config();
    Ctl ctl;
    SysFilesUpdateOptions opts;
    opts.backup   = false;
    opts.selected = {"sys/homez.g"};
    const SysFilesUpdateResult r = run(board, release_package(), opts, ctl);
    REQUIRE(r.outcome == SysFilesUpdateResult::Outcome::Success);
    CHECK(r.written == std::vector<std::string>{"sys/homez.g"});
    CHECK(text(board.card["0:/sys/homezstall.g"]) == "; homezstall.g OLD\r\n");
}

TEST_CASE("A rollback undoes the newest write first, and deletes only what it created", "[SysFilesUpdater]")
{
    const SysFilesRollbackPlan plan =
        plan_sys_files_rollback({"sys/a.g", "macros/New", "sys/b.g"}, {"macros/New"});
    CHECK(plan.restore == std::vector<std::string>{"sys/b.g", "sys/a.g"});
    CHECK(plan.remove == std::vector<std::string>{"macros/New"});
}

TEST_CASE("Machine-specific files are never part of a package, whatever the manifest says", "[SysFilesUpdater]")
{
    TempDir tmp;
    auto write = [&tmp](const std::string &rel, const std::string &content) {
        const fs::path p = tmp.path / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p.string(), std::ios::binary) << content;
    };
    write("sys/homez.g", "G30\r\n");
    write("sys/zstall.g", "global zStallTip = 22.46\r\n");
    write("sys/heightmap.csv", "RepRapFirmware height map\r\n");
    write("sys/Config-Override.g", "M307 H0\r\n");
    write("manifest.json", "{\"version\":\"1\",\"files\":[{\"path\":\"sys/homez.g\"},{\"path\":\"sys/zstall.g\"}]}");

    LocalSysFileSource source(tmp.path.string());
    SysFilePackage     pkg;
    std::string        err;
    REQUIRE(source.fetch(pkg, nullptr, err));
    REQUIRE(pkg.files.size() == 1);
    CHECK(pkg.files[0].path == "sys/homez.g");
    std::set<std::string> skipped;
    for (const auto &s : pkg.skipped)
        if (boost::contains(s.second, "belongs to one printer"))
            skipped.insert(s.first);
    CHECK(skipped.count("sys/zstall.g") == 1);

    // Without a manifest the same files are dropped from "everything that was found".
    fs::remove(tmp.path / "manifest.json");
    REQUIRE(LocalSysFileSource(tmp.path.string()).fetch(pkg, nullptr, err));
    REQUIRE(pkg.files.size() == 1);
    CHECK(pkg.files[0].path == "sys/homez.g");

    // A manifest that lists only such files does not fall back to installing everything.
    write("manifest.json", "{\"version\":\"1\",\"files\":[{\"path\":\"sys/zstall.g\"}]}");
    CHECK_FALSE(LocalSysFileSource(tmp.path.string()).fetch(pkg, nullptr, err));
}

namespace {

// Writes a zip to disk, the way From Zip is handed one.
void write_zip(const fs::path &where, const std::vector<std::pair<std::string, std::string>> &entries)
{
    mz_zip_archive arch;
    mz_zip_zero_struct(&arch);
    REQUIRE(mz_zip_writer_init_heap(&arch, 0, 64 * 1024));
    for (const auto &e : entries)
        REQUIRE(mz_zip_writer_add_mem(&arch, e.first.c_str(), e.second.data(), e.second.size(), MZ_BEST_SPEED));
    void  *buf  = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&arch, &buf, &size));
    std::ofstream(where.string(), std::ios::binary).write(static_cast<const char *>(buf), std::streamsize(size));
    mz_free(buf);
    mz_zip_writer_end(&arch);
}

// The layout of unlayered-1.9-sd-card-*.zip: the whole card, no manifest, and a print file larger
// than any configuration file may be.
std::vector<std::pair<std::string, std::string>> full_sd_card(const std::string &wrapper)
{
    return {
        {wrapper + "README-UNLAYERED-SETUP.md", "# Setup\n"},
        {wrapper + "START HERE.txt", "Copy everything to the card.\r\n"},
        {wrapper + "firmware/Duet3Firmware_Mini5plus.uf2", std::string(64 * 1024, 'F')},
        {wrapper + "gcodes/demo print_PLA_51m23s.gcode", std::string(4789252, 'G')},
        {wrapper + "www/index.html.gz", std::string(1024, 'W')},
        {wrapper + "menu/main", "button\n"},
        {wrapper + "sys/config.g", "; config.g\r\nM550 P\"Unlayered\"\r\n"},
        {wrapper + "sys/homez.g", "G30\r\n"},
        {wrapper + "sys/zstall.g", "global zStallTip = 75\r\n"},
        {wrapper + "macros/Load Filament", "M98 P\"load.g\"\r\n"},
    };
}

std::vector<std::string> package_paths(const SysFilePackage &pkg)
{
    std::vector<std::string> out;
    for (const auto &f : pkg.files)
        out.push_back(f.path);
    return out;
}

} // namespace

TEST_CASE("A full SD card is recognised by its card-only folders and notes", "[SysFilesUpdater]")
{
    CHECK(sys_looks_like_full_sd_card({"sys/config.g", "gcodes/a.gcode"}));
    CHECK(sys_looks_like_full_sd_card({"macros/Load", "www/index.html"}));
    CHECK(sys_looks_like_full_sd_card({"sys/config.g", "firmware/x.uf2"}));
    CHECK(sys_looks_like_full_sd_card({"sys/config.g", "START HERE.txt"}));
    CHECK(sys_looks_like_full_sd_card({"SYS/config.g", "README.md"}));
    // A configuration package, with or without a manifest or loose files, is not one.
    CHECK_FALSE(sys_looks_like_full_sd_card({"manifest.json", "sys/config.g", "macros/Load"}));
    CHECK_FALSE(sys_looks_like_full_sd_card({"config.g", "homez.g", "notes.txt"}));
    CHECK_FALSE(sys_looks_like_full_sd_card({"sys/config.g", "filaments/PLA/load.g"}));
    // Card folders without any configuration are not one either: nothing would be offered.
    CHECK_FALSE(sys_looks_like_full_sd_card({"gcodes/a.gcode", "www/index.html"}));
}

TEST_CASE("From Zip on a full SD card offers sys and macros and skips the rest", "[SysFilesUpdater]")
{
    for (const std::string &wrapper : {std::string(), std::string("unlayered-1.9-sd-card-2026.10.26/")}) {
        DYNAMIC_SECTION("wrapper '" << wrapper << "'") {
            TempDir        tmp;
            const fs::path zip = tmp.path / "unlayered-1.9-sd-card.zip";
            write_zip(zip, full_sd_card(wrapper));

            SysFilePackage pkg;
            std::string    err;
            REQUIRE(LocalSysFileSource(zip.string()).fetch(pkg, nullptr, err));
            CHECK(err.empty());
            CHECK(pkg.from_full_sd_card);
            // config.g and zstall.g are kept, and nothing outside sys/ and macros/ came along -
            // not even mapped into sys/, the way a loose file in a configuration package is.
            CHECK(package_paths(pkg) == std::vector<std::string>{"macros/Load Filament", "sys/homez.g"});

            std::map<std::string, std::string> skipped(pkg.skipped.begin(), pkg.skipped.end());
            for (const char *s : {"gcodes/", "firmware/", "www/", "menu/", "README-UNLAYERED-SETUP.md", "START HERE.txt"}) {
                INFO(s);
                REQUIRE(skipped.count(s) == 1);
                CHECK(boost::contains(skipped[s], "part of the SD card"));
            }
            CHECK(boost::contains(skipped["firmware/"], "Firmware button"));
            CHECK(boost::contains(skipped["sys/zstall.g"], "belongs to one printer"));
            // The card's config.g is the generic ship one; the printer's own is kept.
            REQUIRE(skipped.count("sys/config.g") == 1);
            CHECK(skipped["sys/config.g"] == "this printer's own setup, kept");
            CHECK(validate_sys_package(pkg, err));
        }
    }
}

TEST_CASE("A full SD card with an oversized file in sys is still refused", "[SysFilesUpdater]")
{
    TempDir tmp;
    auto    entries = full_sd_card("");
    entries.emplace_back("sys/heightmap-huge.g", std::string(5 * 1024 * 1024, 'H'));
    const fs::path zip = tmp.path / "card.zip";
    write_zip(zip, entries);

    SysFilePackage pkg;
    std::string    err;
    CHECK_FALSE(LocalSysFileSource(zip.string()).fetch(pkg, nullptr, err));
    CHECK(boost::contains(err, "sys/heightmap-huge.g"));
    CHECK(boost::contains(err, "far too large"));
}

TEST_CASE("A configuration zip is read as before: loose files go to sys, big files refuse", "[SysFilesUpdater]")
{
    TempDir        tmp;
    const fs::path zip = tmp.path / "config.zip";
    write_zip(zip, {{"homez.g", "G30\r\n"}, {"config.g", "M550 P\"x\"\r\n"}, {"macros/Load", "M83\r\n"}});
    SysFilePackage pkg;
    std::string    err;
    REQUIRE(LocalSysFileSource(zip.string()).fetch(pkg, nullptr, err));
    CHECK_FALSE(pkg.from_full_sd_card);
    // Outside a card image config.g is still offered, as before.
    CHECK(package_paths(pkg) == std::vector<std::string>{"macros/Load", "sys/config.g", "sys/homez.g"});

    // Not a card (no sys/ or macros/ folder beside the print file), so the print file still refuses.
    write_zip(zip, {{"homez.g", "G30\r\n"}, {"gcodes/big.gcode", std::string(4789252, 'G')}});
    CHECK_FALSE(LocalSysFileSource(zip.string()).fetch(pkg, nullptr, err));
    CHECK(boost::contains(err, "far too large to be a configuration file"));
}

TEST_CASE("From Folder on a copy of a full SD card reads only its configuration", "[SysFilesUpdater]")
{
    TempDir tmp;
    for (const auto &e : full_sd_card("")) {
        const fs::path p = tmp.path / e.first;
        fs::create_directories(p.parent_path());
        std::ofstream(p.string(), std::ios::binary) << e.second;
    }
    SysFilePackage pkg;
    std::string    err;
    REQUIRE(LocalSysFileSource(tmp.path.string()).fetch(pkg, nullptr, err));
    CHECK(pkg.from_full_sd_card);
    CHECK(package_paths(pkg) == std::vector<std::string>{"macros/Load Filament", "sys/homez.g"});
    CHECK(pkg.find("sys/config.g") == nullptr);
}

TEST_CASE("The protected names cover the printer's factory-reset list", "[SysFilesUpdater]")
{
    for (const char *name : {"setupdone.g", "setupskip.g", "setupprog.g", "zstall.g", "display.g", "plates.g",
                             "machinename.g", "piezopol.g", "config-override.g", "heightmap.csv", "filaments.csv",
                             "resurrect.g", "piezolog.csv", "dwc-settings.json", "dwc-plugins.json", "dwc-cache.json",
                             "configtool.json"})
        CHECK(sys_is_machine_specific(std::string("sys/") + name));
    CHECK_FALSE(sys_is_machine_specific("sys/homez.g"));
    CHECK_FALSE(sys_is_machine_specific("sys/config.g"));
}

// ------------------------------------------------------------------------------------------------
// End to end, against a real feed and a board on the network. Hidden ("[.]"): it runs only when
// asked for by tag, with the environment below. Point it at a feed and a board, e.g.
// tools/fake_duet.py; nothing here ever talks to a real printer unless pointed at one.
//
//   OTA_FEED_BASE   e.g. https://feed.example/printer-updates-api
//   OTA_SITE_URL    e.g. https://feed.example (the site the device token belongs to)
//   OTA_TOKEN       an Unlayered account device token for that site
//   OTA_BOARD       host[:port] of the board to install onto
//   OTA_BACKUP_DIR  where the backup goes
//   OTA_EXPECT      the configuration version the feed should offer
//   OTA_BAD_FEED    optional: a feed base whose sys.zip does not match its digest
// ------------------------------------------------------------------------------------------------

namespace {
std::string env(const char *name)
{
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}
} // namespace

TEST_CASE("OTA end to end: feed -> download -> verify -> install onto the board", "[.][ota-e2e]")
{
    const std::string feed = env("OTA_FEED_BASE"), site = env("OTA_SITE_URL"), board_host = env("OTA_BOARD");
    REQUIRE_FALSE(feed.empty());
    REQUIRE_FALSE(board_host.empty());

    GitHubSysFileSource::Options base;
    base.api_base             = feed;
    base.credential.site_url  = site;
    base.credential.token     = env("OTA_TOKEN");

    SECTION("signed out, the configuration feed is refused with a sign-in message")
    {
        GitHubSysFileSource::Options opts = base;
        opts.credential                   = FeedCredential();
        GitHubSysFileSource src(opts);
        GitHubSysFileSource::Release rel;
        std::string err;
        CHECK_FALSE(src.fetch_release(rel, err));
        CHECK(src.last_http_status() == 401);
        CHECK(boost::contains(err, "Sign in"));
    }

    SECTION("signed in, the release resolves with a digest, downloads, verifies and installs")
    {
        GitHubSysFileSource          probe(base);
        GitHubSysFileSource::Release rel;
        std::string                  err;
        REQUIRE(probe.fetch_release(rel, err));
        CHECK(rel.asset_name == "sys.zip");
        CHECK(rel.asset_sha256.size() == 64);

        // The offer, decided the way PrinterUpdateOffer::check does: the board's marker against the
        // version in the feed's archive, with no skips remembered.
        {
            DuetRrfClient c(board_host, "");
            REQUIRE(c.connect(err) == DuetClient::ConnectResult::ok);
            DuetClient::BoardInfo bi;
            REQUIRE(c.read_board_info(bi, err));
            std::vector<uint8_t> m;
            bool                 missing = false;
            SysFilesInstalled    inst;
            if (c.download(std::string("0:/") + SYS_MARKER_PATH, m, missing, err) && !missing)
                parse_sys_marker(text(m), inst);
            c.disconnect();

            GitHubSysFileSource feed_src(base);
            SysFilePackage      offered;
            REQUIRE(feed_src.fetch(offered, nullptr, err));

            PrinterUpdateFacts facts;
            facts.board_id         = bi.unique_id;
            facts.running_firmware = bi.firmware_version;
            facts.config_known     = inst.known;
            facts.installed_config = inst.version;
            facts.available_config = offered.version;
            std::map<std::string, std::string> remembered;
            PrinterUpdateSkips skips([&](const std::string &s, const std::string &k) { return remembered[s + "/" + k]; },
                                     [&](const std::string &s, const std::string &k, const std::string &v) { remembered[s + "/" + k] = v; });
            const PrinterUpdateDecision decision = decide_printer_update(facts, skips);
            CHECK(decision.config);
            WARN("offer for " << bi.unique_id << ": configuration " << (inst.known ? inst.version : std::string("unknown"))
                              << " -> " << offered.version << (decision.config ? " (offered)" : " (not offered)"));
        }

        DuetRrfClient         client(board_host, "");
        Ctl                   ctl;
        SysFilesUpdateOptions opts;
        opts.backup_root             = env("OTA_BACKUP_DIR");
        opts.restart_timeout_seconds = 30;
        SysFilesUpdateResult r;
        run_sys_files_update(client, [base]() { return std::make_unique<GitHubSysFileSource>(base); }, opts, ctl, r);
        for (const auto &m : ctl.messages)
            WARN(m);
        CHECK(r.message.size() > 0);
        REQUIRE(r.outcome == SysFilesUpdateResult::Outcome::Success);
        CHECK(r.available_version == env("OTA_EXPECT"));
        CHECK(r.restarted);
        CHECK_FALSE(r.backup_dir.empty());
        WARN("backup: " << r.backup_dir);
        for (const auto &w : r.written)
            WARN("written: " << w);
    }

    SECTION("a second run finds the version installed and writes nothing")
    {
        DuetRrfClient         client(board_host, "");
        Ctl                   ctl;
        SysFilesUpdateOptions opts;
        opts.backup_root = env("OTA_BACKUP_DIR");
        SysFilesUpdateResult r;
        run_sys_files_update(client, [base]() { return std::make_unique<GitHubSysFileSource>(base); }, opts, ctl, r);
        CHECK(r.outcome == SysFilesUpdateResult::Outcome::UpToDate);
    }

    SECTION("a download that does not match the feed's digest is refused before anything is written")
    {
        const std::string bad = env("OTA_BAD_FEED");
        if (bad.empty())
            SKIP("OTA_BAD_FEED not set");
        GitHubSysFileSource::Options opts = base;
        opts.api_base                     = bad;
        GitHubSysFileSource src(opts);
        SysFilePackage      pkg;
        std::string         err;
        CHECK_FALSE(src.fetch(pkg, nullptr, err));
        CHECK(boost::contains(err, "did not arrive intact"));
    }
}
