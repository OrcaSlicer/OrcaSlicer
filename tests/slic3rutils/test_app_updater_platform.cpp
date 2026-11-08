// Platform-specific rollouts: a release published for other platforms only is not an update here
// (release_targets_platform). Runs on every platform, each against its own names.
#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <string>

#include "slic3r/Utils/AppUpdater.hpp"

using namespace Slic3r;

namespace {

AccountDownload download(const std::string &platform, const std::string &arch = "x64")
{
    AccountDownload d;
    d.platform = platform;
    d.arch     = arch;
    d.name     = "installer";
    d.url      = "https://example.invalid/d";
    d.sha256   = std::string(64, 'a');
    return d;
}

AppUpdateAsset asset(const std::string &name)
{
    return AppUpdateAsset{name, "https://example.invalid/" + name, std::string(64, 'b'), 1};
}

#if defined(_WIN32)
const char *THIS_PLATFORM = "windows";
const char *OTHER_PLATFORM = "mac";
const char *THIS_FILE = "UnlayeredSlicer_Windows_Installer_v0.1.5.exe";
const char *OTHER_FILE = "UnlayeredSlicer_Mac_universal_v0.1.5.dmg";
#elif defined(__APPLE__)
const char *THIS_PLATFORM = "mac";
const char *OTHER_PLATFORM = "windows";
const char *THIS_FILE = "UnlayeredSlicer_Mac_universal_v0.1.5.dmg";
const char *OTHER_FILE = "UnlayeredSlicer_Windows_Installer_v0.1.5.exe";
#else
const char *THIS_PLATFORM = "linux";
const char *OTHER_PLATFORM = "windows";
const char *THIS_FILE = "UnlayeredSlicer_Linux_v0.1.5.AppImage";
const char *OTHER_FILE = "UnlayeredSlicer_Windows_Installer_v0.1.5.exe";
#endif

} // namespace

TEST_CASE("release_targets_platform: other platforms only is not an update here", "[AppUpdater]")
{
    REQUIRE(std::string(updater_platform()) == THIS_PLATFORM);
    // Published for another platform only, by account download or by asset.
    CHECK_FALSE(release_targets_platform({}, {download(OTHER_PLATFORM)}));
    CHECK_FALSE(release_targets_platform({asset(OTHER_FILE)}, {}));
    CHECK_FALSE(release_targets_platform({asset(OTHER_FILE)}, {download(OTHER_PLATFORM), download("linux")}));
    // Published for this platform, either way; any architecture counts here.
    CHECK(release_targets_platform({}, {download(OTHER_PLATFORM), download(THIS_PLATFORM, "arm64")}));
    CHECK(release_targets_platform({asset(OTHER_FILE), asset(THIS_FILE)}, {}));
    // The platform name is not case sensitive in the manifest.
    std::string upper(THIS_PLATFORM);
    for (auto &c : upper)
        c = char(std::toupper(c));
    CHECK(release_targets_platform({}, {download(upper)}));
}

TEST_CASE("release_targets_platform: a page-only release is still an update", "[AppUpdater]")
{
    // No installers listed at all: the old behaviour (offer it, open the download page).
    CHECK(release_targets_platform({}, {}));
}

TEST_CASE("release_targets_platform: side files are not installers", "[AppUpdater]")
{
#if defined(__APPLE__)
    CHECK_FALSE(release_targets_platform({asset("UnlayeredSlicer_profile_validator_Mac_v0.1.5.dmg")}, {download(OTHER_PLATFORM)}));
#elif defined(_WIN32)
    CHECK_FALSE(release_targets_platform({asset("UnlayeredSlicer_profile_validator_v0.1.5.exe")}, {download(OTHER_PLATFORM)}));
#endif
    CHECK_FALSE(release_targets_platform({asset("UnlayeredSlicer_v0.1.5_pdb.zip")}, {download(OTHER_PLATFORM)}));
}
