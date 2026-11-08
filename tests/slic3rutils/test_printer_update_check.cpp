#include <catch2/catch_all.hpp>

#include <map>
#include <string>
#include <utility>

#include "slic3r/Utils/PrinterUpdateCheck.hpp"

using namespace Slic3r;

namespace {

// PrinterUpdateSkips over a plain map, standing in for AppConfig.
struct MemoryStore
{
    std::map<std::pair<std::string, std::string>, std::string> values;

    PrinterUpdateSkips skips()
    {
        return PrinterUpdateSkips(
            [this](const std::string &section, const std::string &key) {
                auto it = values.find({section, key});
                return it == values.end() ? std::string() : it->second;
            },
            [this](const std::string &section, const std::string &key, const std::string &value) {
                values[{section, key}] = value;
            });
    }
};

const std::string SITE = "https://unlayered3d.com";

} // namespace

// ---- Token host gating ---------------------------------------------------------------------------

TEST_CASE("The account token goes to the Unlayered site's feed and assets", "[PrinterUpdateCheck]")
{
    CHECK(feed_token_allowed("https://unlayered3d.com/printer-updates-api/repos/Unlayered3D/printer-config/releases?per_page=20", SITE));
    CHECK(feed_token_allowed("https://unlayered3d.com/printer-updates-api/assets/abc123", SITE));
    CHECK(feed_token_allowed("https://UNLAYERED3D.com:443/printer-updates-api/assets/abc123", SITE));
}

TEST_CASE("The account token never goes to GitHub or any other host", "[PrinterUpdateCheck]")
{
    CHECK_FALSE(feed_token_allowed("https://api.github.com/repos/Unlayered3D/printer-config/releases?per_page=20", SITE));
    CHECK_FALSE(feed_token_allowed("https://github.com/Unlayered3D/printer-config/releases/download/v1/sys.zip", SITE));
    CHECK_FALSE(feed_token_allowed("https://cdn.example.net/files/sys.zip", SITE));
    // Look-alikes: a subdomain, a suffix, a userinfo trick.
    CHECK_FALSE(feed_token_allowed("https://evil.unlayered3d.com/printer-updates-api/", SITE));
    CHECK_FALSE(feed_token_allowed("https://unlayered3d.com.evil.example/printer-updates-api/", SITE));
    CHECK_FALSE(feed_token_allowed("https://unlayered3d.com@evil.example/printer-updates-api/", SITE));
}

TEST_CASE("The account token never travels over plain http or to another port", "[PrinterUpdateCheck]")
{
    CHECK_FALSE(feed_token_allowed("http://unlayered3d.com/printer-updates-api/", SITE));
    CHECK_FALSE(feed_token_allowed("https://unlayered3d.com:8443/printer-updates-api/", SITE));
    CHECK_FALSE(feed_token_allowed("unlayered3d.com/printer-updates-api/", SITE));
    CHECK_FALSE(feed_token_allowed("ftp://unlayered3d.com/x", SITE));
}

TEST_CASE("A local test site gets its own token and nothing else does", "[PrinterUpdateCheck]")
{
    const std::string local = "http://127.0.0.1:3000";
    CHECK(feed_token_allowed("http://127.0.0.1:3000/printer-updates-api/repos/x/y/releases", local));
    CHECK_FALSE(feed_token_allowed("http://127.0.0.1:8000/printer-updates-api/repos/x/y/releases", local));
    CHECK_FALSE(feed_token_allowed("https://unlayered3d.com/printer-updates-api/", local));
}

TEST_CASE("No site means the token goes nowhere", "[PrinterUpdateCheck]")
{
    CHECK_FALSE(feed_token_allowed("https://unlayered3d.com/printer-updates-api/", ""));
}

TEST_CASE("A credential hands out its token only for its own site", "[PrinterUpdateCheck]")
{
    FeedCredential cred{"test-token", SITE};
    CHECK(cred.token_for("https://unlayered3d.com/printer-updates-api/assets/1") == "test-token");
    CHECK(cred.token_for("https://api.github.com/repos/Unlayered3D/printer-config/releases").empty());

    FeedCredential none{"", SITE};
    CHECK(none.empty());
    CHECK(none.token_for("https://unlayered3d.com/printer-updates-api/assets/1").empty());
}

// ---- Firmware ------------------------------------------------------------------------------------

TEST_CASE("A higher fork number is newer firmware", "[PrinterUpdateCheck]")
{
    CHECK(firmware_is_newer("3.7.0-beta.1+unlayered.15", "3.7.0-beta.1+unlayered.16"));
    // Semver would call these equal; the fork number decides.
    CHECK(firmware_is_newer("3.7.0-beta.1+unlayered.9", "3.7.0-beta.1+unlayered.12"));
}

TEST_CASE("Older, equal and rebuilt firmware is not offered", "[PrinterUpdateCheck]")
{
    // A bench board ahead of the release.
    CHECK_FALSE(firmware_is_newer("3.7.0-beta.1+unlayered.18", "3.7.0-beta.1+unlayered.16"));
    CHECK_FALSE(firmware_is_newer("3.7.0-beta.1+unlayered.16", "3.7.0-beta.1+unlayered.16"));
    // Same fork number, different text: not ordered, so not offered automatically.
    CHECK_FALSE(firmware_is_newer("3.7.0-beta.1+unlayered.16", "3.7.0-beta.2+unlayered.16"));
}

TEST_CASE("Fork firmware is offered over stock RRF but never the reverse", "[PrinterUpdateCheck]")
{
    CHECK(firmware_is_newer("3.6.2", "3.7.0-beta.1+unlayered.16"));
    CHECK_FALSE(firmware_is_newer("3.7.0-beta.1+unlayered.16", "3.7.1"));
}

TEST_CASE("Two stock versions use semver", "[PrinterUpdateCheck]")
{
    CHECK(firmware_is_newer("3.6.2", "3.7.0"));
    CHECK_FALSE(firmware_is_newer("3.7.0", "3.6.2"));
}

TEST_CASE("Unknown firmware on either side is not offered", "[PrinterUpdateCheck]")
{
    CHECK_FALSE(firmware_is_newer("", "3.7.0-beta.1+unlayered.16"));
    CHECK_FALSE(firmware_is_newer("3.7.0-beta.1+unlayered.16", ""));
}

// ---- Configuration -------------------------------------------------------------------------------

TEST_CASE("Configuration versions compare digit runs by value", "[PrinterUpdateCheck]")
{
    CHECK(natural_version_compare("2026.9-1", "2026.10-1") < 0);
    CHECK(natural_version_compare("2026.10-2", "2026.10-1") > 0);
    CHECK(natural_version_compare("2026.08-1", "2026.8-1") == 0);
    CHECK(natural_version_compare("v1.2", "V1.2") == 0);
    CHECK(natural_version_compare("1.2", "1.2.1") < 0);
}

TEST_CASE("Newer configuration is offered, older and equal are not", "[PrinterUpdateCheck]")
{
    CHECK(config_is_newer(true, "2026.09-1", "2026.10-1"));
    CHECK_FALSE(config_is_newer(true, "2026.10-1", "2026.10-1"));
    CHECK_FALSE(config_is_newer(true, "2026.10-3", "2026.10-1"));
}

TEST_CASE("A card the slicer never wrote is offered the published configuration", "[PrinterUpdateCheck]")
{
    CHECK(config_is_newer(false, "", "2026.10-1"));
    CHECK(config_is_newer(true, "", "2026.10-1"));
}

TEST_CASE("No published configuration means no offer", "[PrinterUpdateCheck]")
{
    CHECK_FALSE(config_is_newer(false, "", ""));
    CHECK_FALSE(config_is_newer(true, "2026.10-1", ""));
}

// ---- Skip this version ---------------------------------------------------------------------------

TEST_CASE("A skipped version is remembered per board", "[PrinterUpdateCheck]")
{
    MemoryStore store;
    auto        skips = store.skips();
    skips.skip("W7W5M-KA6AA-Q65J0-401L8-K602Z-Z7245", "3.7.0-beta.1+unlayered.16", "2026.10-1");

    CHECK(skips.firmware_skipped("W7W5M-KA6AA-Q65J0-401L8-K602Z-Z7245", "3.7.0-beta.1+unlayered.16"));
    CHECK(skips.config_skipped("W7W5M-KA6AA-Q65J0-401L8-K602Z-Z7245", "2026.10-1"));
    // Another machine with the same name is a different board.
    CHECK_FALSE(skips.firmware_skipped("AAAAA-BBBBB-CCCCC-DDDDD-EEEEE-FFFFF", "3.7.0-beta.1+unlayered.16"));
}

TEST_CASE("Skipping one version does not silence the next", "[PrinterUpdateCheck]")
{
    MemoryStore store;
    auto        skips = store.skips();
    skips.skip("board", "3.7.0-beta.1+unlayered.16", "");
    CHECK_FALSE(skips.firmware_skipped("board", "3.7.0-beta.1+unlayered.17"));
}

TEST_CASE("Skipping only firmware keeps an earlier configuration skip", "[PrinterUpdateCheck]")
{
    MemoryStore store;
    auto        skips = store.skips();
    skips.skip("board", "", "2026.10-1");
    skips.skip("board", "3.7.0-beta.1+unlayered.16", "");
    CHECK(skips.config_skipped("board", "2026.10-1"));
    CHECK(skips.firmware_skipped("board", "3.7.0-beta.1+unlayered.16"));
}

TEST_CASE("A board id cannot break the config store's keys", "[PrinterUpdateCheck]")
{
    const std::string key = PrinterUpdateSkips::key_for("a=b;c.d[e]\nf");
    CHECK(key.find_first_of("=;.[]\n\r") == std::string::npos);
}

TEST_CASE("Nothing is remembered without a board id", "[PrinterUpdateCheck]")
{
    MemoryStore store;
    auto        skips = store.skips();
    skips.skip("", "3.7.0-beta.1+unlayered.16", "2026.10-1");
    CHECK(store.values.empty());
}

// ---- The decision --------------------------------------------------------------------------------

TEST_CASE("Both components are offered when both are newer", "[PrinterUpdateCheck]")
{
    MemoryStore        store;
    PrinterUpdateFacts f;
    f.board_id           = "board";
    f.running_firmware   = "3.7.0-beta.1+unlayered.15";
    f.available_firmware = "3.7.0-beta.1+unlayered.16";
    f.config_known       = true;
    f.installed_config   = "2026.09-1";
    f.available_config   = "2026.10-1";

    const auto d = decide_printer_update(f, store.skips());
    CHECK(d.firmware);
    CHECK(d.config);
}

TEST_CASE("Signed out, only firmware is decided", "[PrinterUpdateCheck]")
{
    MemoryStore        store;
    PrinterUpdateFacts f;
    f.board_id           = "board";
    f.running_firmware   = "3.7.0-beta.1+unlayered.15";
    f.available_firmware = "3.7.0-beta.1+unlayered.16";
    // available_config stays empty: the configuration feed was not asked.

    const auto d = decide_printer_update(f, store.skips());
    CHECK(d.firmware);
    CHECK_FALSE(d.config);
}

TEST_CASE("A skipped version is not offered again, a newer one is", "[PrinterUpdateCheck]")
{
    MemoryStore        store;
    PrinterUpdateFacts f;
    f.board_id           = "board";
    f.running_firmware   = "3.7.0-beta.1+unlayered.15";
    f.available_firmware = "3.7.0-beta.1+unlayered.16";
    f.config_known       = true;
    f.installed_config   = "2026.09-1";
    f.available_config   = "2026.10-1";

    store.skips().skip("board", f.available_firmware, f.available_config);
    CHECK_FALSE(decide_printer_update(f, store.skips()).any());

    f.available_firmware = "3.7.0-beta.1+unlayered.17";
    const auto d = decide_printer_update(f, store.skips());
    CHECK(d.firmware);
    CHECK_FALSE(d.config);
}

TEST_CASE("An up to date printer gets no offer", "[PrinterUpdateCheck]")
{
    MemoryStore        store;
    PrinterUpdateFacts f;
    f.board_id           = "board";
    f.running_firmware   = "3.7.0-beta.1+unlayered.16";
    f.available_firmware = "3.7.0-beta.1+unlayered.16";
    f.config_known       = true;
    f.installed_config   = "2026.10-1";
    f.available_config   = "2026.10-1";
    CHECK_FALSE(decide_printer_update(f, store.skips()).any());
}

// ---- The setup wizard's "Check for updates" (guide 21update) -------------------------------------

namespace {
PrinterUpdateFacts wizard_facts()
{
    PrinterUpdateFacts f;
    f.board_id           = "board";
    f.running_firmware   = "3.7.0-beta.1+unlayered.16";
    f.available_firmware = "3.7.0-beta.1+unlayered.16";
    f.config_known       = true;
    f.installed_config   = "2026.10-1";
    f.available_config   = "2026.10-1";
    return f;
}
} // namespace

TEST_CASE("The wizard reports a printer that did not answer", "[PrinterUpdateCheck]")
{
    CHECK(classify_printer_update_check(false, wizard_facts()) == PrinterUpdateCheckOutcome::Unreachable);
    CHECK(printer_update_check_after_setup(PrinterUpdateCheckOutcome::Unreachable));
}

TEST_CASE("The wizard reports an up to date printer and leaves nothing pending", "[PrinterUpdateCheck]")
{
    CHECK(classify_printer_update_check(true, wizard_facts()) == PrinterUpdateCheckOutcome::UpToDate);
    CHECK_FALSE(printer_update_check_after_setup(PrinterUpdateCheckOutcome::UpToDate));

    // Signed out: no configuration was asked, the firmware alone decides.
    PrinterUpdateFacts f = wizard_facts();
    f.available_config.clear();
    CHECK(classify_printer_update_check(true, f) == PrinterUpdateCheckOutcome::UpToDate);
}

TEST_CASE("The wizard reports newer firmware or configuration as available", "[PrinterUpdateCheck]")
{
    PrinterUpdateFacts f = wizard_facts();
    f.available_firmware = "3.7.0-beta.1+unlayered.17";
    CHECK(classify_printer_update_check(true, f) == PrinterUpdateCheckOutcome::Available);
    CHECK(printer_update_check_after_setup(PrinterUpdateCheckOutcome::Available));

    f                    = wizard_facts();
    f.config_known       = false; // a card this slicer never wrote
    CHECK(classify_printer_update_check(true, f) == PrinterUpdateCheckOutcome::Available);
}

TEST_CASE("The wizard's check ignores Skip this version", "[PrinterUpdateCheck]")
{
    MemoryStore        store;
    PrinterUpdateFacts f = wizard_facts();
    f.available_firmware = "3.7.0-beta.1+unlayered.17";
    store.skips().skip("board", f.available_firmware, "");
    CHECK_FALSE(decide_printer_update(f, store.skips()).any());
    CHECK(decide_printer_update(f, no_printer_update_skips()).firmware);
    CHECK(classify_printer_update_check(true, f) == PrinterUpdateCheckOutcome::Available);
}

TEST_CASE("The wizard says so when no feed answered", "[PrinterUpdateCheck]")
{
    PrinterUpdateFacts f = wizard_facts();
    f.available_firmware.clear();
    f.available_config.clear();
    CHECK(classify_printer_update_check(true, f) == PrinterUpdateCheckOutcome::NoFeed);
    CHECK(printer_update_check_after_setup(PrinterUpdateCheckOutcome::NoFeed));
}
