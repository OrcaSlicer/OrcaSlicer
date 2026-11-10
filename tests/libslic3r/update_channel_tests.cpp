#include <catch2/catch_all.hpp>

#include <string>

#include "libslic3r/UpdateChannel.hpp"

using namespace Slic3r;

namespace {

// Two stable releases plus the rolling nightly one, in the order the service
// returns them: the picker must not simply take the first entry.
const char* stable_releases_json()
{
    return R"JSON([
        {
            "tag_name": "v2.4.0",
            "prerelease": false,
            "html_url": "https://example.invalid/releases/v2.4.0",
            "body": "older release",
            "updated_at": "2026-06-01T10:00:00Z",
            "assets": [
                {
                    "name": "OrcaSlicer_Windows_Installer_V2.4.0_x64.exe",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Windows_Installer_V2.4.0_x64.exe",
                    "digest": "sha256:1111",
                    "size": 100
                }
            ]
        },
        {
            "tag_name": "v2.5.0",
            "prerelease": false,
            "html_url": "https://example.invalid/releases/v2.5.0",
            "body": "newer release",
            "updated_at": "2026-10-01T10:00:00Z",
            "assets": [
                {
                    "name": "OrcaSlicer_Windows_Installer_V2.5.0_x64.exe",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Windows_Installer_V2.5.0_x64.exe",
                    "digest": "sha256:2222",
                    "size": 200
                }
            ]
        },
        {
            "tag_name": "nightly-builds",
            "prerelease": true,
            "html_url": "https://example.invalid/releases/nightly-builds",
            "body": "nightly release",
            "updated_at": "2026-11-10T08:31:54Z",
            "assets": [
                {
                    "name": "OrcaSlicer_Linux_AppImage_Ubuntu2404_nightly.AppImage",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Linux_AppImage_Ubuntu2404_nightly.AppImage",
                    "digest": "sha256:nightly2",
                    "size": 300
                }
            ]
        }
    ])JSON";
}

const char* nightly_releases_json()
{
    return R"JSON([
        {
            "tag_name": "nightly-builds",
            "prerelease": true,
            "html_url": "https://example.invalid/releases/nightly-builds",
            "body": "nightly release",
            "updated_at": "2026-11-10T08:31:54Z",
            "assets": [
                {
                    "name": "OrcaSlicer_Linux_AppImage_Ubuntu2404_nightly.AppImage",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Linux_AppImage_Ubuntu2404_nightly.AppImage",
                    "digest": "sha256:nightly2",
                    "size": 300
                }
            ]
        }
    ])JSON";
}

// A stable release whose only asset carries no digest.
const char* stable_release_without_digest_json()
{
    return R"JSON([
        {
            "tag_name": "v2.5.0",
            "prerelease": false,
            "html_url": "https://example.invalid/releases/v2.5.0",
            "body": "newer release",
            "updated_at": "2026-10-01T10:00:00Z",
            "assets": [
                {
                    "name": "OrcaSlicer_Windows_Installer_V2.5.0_x64.exe",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Windows_Installer_V2.5.0_x64.exe",
                    "size": 200
                }
            ]
        }
    ])JSON";
}

} // namespace

TEST_CASE("A stable release newer than the running version is offered", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(stable_releases_json(), UpdateChannel::Stable, "", "",
                                                     "2.4.9", "", InstallFlavour::WindowsInstaller, "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.reason.empty());
    CHECK(candidate.channel == UpdateChannel::Stable);
    CHECK(candidate.version == "2.5.0");
    CHECK(candidate.version_label == "2.5.0");
    CHECK(candidate.release_url == "https://example.invalid/releases/v2.5.0");
    CHECK(candidate.release_notes == "newer release");
    CHECK(candidate.asset.name == "OrcaSlicer_Windows_Installer_V2.5.0_x64.exe");
    CHECK(candidate.asset.digest == "sha256:2222");
    CHECK(candidate.asset.size == 200);
}

TEST_CASE("A stable release equal to the running version is not offered", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(stable_releases_json(), UpdateChannel::Stable, "", "",
                                                     "2.5.0", "", InstallFlavour::WindowsInstaller, "x86_64");

    CHECK_FALSE(candidate.available);
    CHECK(candidate.reason == "up_to_date");
    CHECK(candidate.version == "2.5.0");
}

TEST_CASE("A nightly with a different digest than the installed build is offered", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(nightly_releases_json(), UpdateChannel::Nightly,
                                                     "sha256:nightly1", "nightly", "2.6.0-dev",
                                                     "2026-11-01T00:00:00Z", InstallFlavour::LinuxAppImage,
                                                     "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.reason.empty());
    CHECK(candidate.channel == UpdateChannel::Nightly);
    CHECK(candidate.version == "2026-11-10T08:31:54Z");
    CHECK(candidate.version_label == "Nightly, 10 Nov 2026 08:31 UTC");
    CHECK(candidate.asset.digest == "sha256:nightly2");
}

TEST_CASE("A nightly with the same digest as the installed build is up to date", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(nightly_releases_json(), UpdateChannel::Nightly,
                                                     "sha256:nightly2", "nightly", "2.6.0-dev",
                                                     "2026-11-01T00:00:00Z", InstallFlavour::LinuxAppImage,
                                                     "x86_64");

    CHECK_FALSE(candidate.available);
    CHECK(candidate.reason == "up_to_date");
}

TEST_CASE("Switching from nightly to stable offers the stable release", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(stable_releases_json(), UpdateChannel::Stable,
                                                     "sha256:nightly2", "nightly", "2.5.0", "",
                                                     InstallFlavour::WindowsInstaller, "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.reason == "channel_switch");
    CHECK(candidate.version == "2.5.0");
}

TEST_CASE("Switching from stable to nightly offers the nightly release", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(nightly_releases_json(), UpdateChannel::Nightly,
                                                     "sha256:2222", "stable", "2.5.0",
                                                     "2026-11-01T00:00:00Z", InstallFlavour::LinuxAppImage,
                                                     "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.reason == "channel_switch");
}

TEST_CASE("A release asset without a digest is not offered", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(stable_release_without_digest_json(), UpdateChannel::Stable,
                                                     "", "", "2.4.9", "", InstallFlavour::WindowsInstaller,
                                                     "x86_64");

    CHECK_FALSE(candidate.available);
    CHECK(candidate.reason == "no_digest");
    CHECK(candidate.asset.name == "OrcaSlicer_Windows_Installer_V2.5.0_x64.exe");
}

TEST_CASE("A supported flavour with no matching asset is not offered", "[UpdateChannel]")
{
    // An AppImage has nothing to download from a release that only ships a Windows
    // installer: offering it would leave the updater with an empty URL.
    const UpdateCandidate candidate = resolve_update(stable_releases_json(), UpdateChannel::Stable, "", "",
                                                     "2.4.9", "", InstallFlavour::LinuxAppImage, "x86_64");

    CHECK_FALSE(candidate.available);
    CHECK(candidate.reason == "no_asset_for_platform");
    CHECK(candidate.asset.name.empty());
    CHECK(candidate.version == "2.5.0");
}

TEST_CASE("A Store flavour with no matching asset is offered so the Store or release page can open", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(stable_releases_json(), UpdateChannel::Stable, "", "",
                                                     "2.4.9", "", InstallFlavour::Msix, "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.reason == "no_asset_for_platform");
    CHECK(candidate.asset.name.empty());
    CHECK(candidate.version == "2.5.0");
    CHECK(candidate.release_url == "https://example.invalid/releases/v2.5.0");
    CHECK(candidate.release_notes == "newer release");
}

TEST_CASE("A stable release that is not newer is not offered even with a stale installed digest", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(stable_releases_json(), UpdateChannel::Stable,
                                                     "sha256:9999", "stable", "2.5.0", "",
                                                     InstallFlavour::WindowsInstaller, "x86_64");

    CHECK_FALSE(candidate.available);
    CHECK(candidate.reason == "up_to_date");
    CHECK(candidate.version == "2.5.0");
}

TEST_CASE("A release that is not newer is not offered when its flavour has no asset either", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(stable_releases_json(), UpdateChannel::Stable, "", "",
                                                     "2.5.0", "", InstallFlavour::Msix, "x86_64");

    CHECK_FALSE(candidate.available);
}

TEST_CASE("A source build has no nightly identity to compare against", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(nightly_releases_json(), UpdateChannel::Nightly, "", "",
                                                     "2.6.0-dev", "", InstallFlavour::LinuxAppImage, "x86_64");

    CHECK_FALSE(candidate.available);
    CHECK(candidate.reason == "source_build");
}

TEST_CASE("A skipped stable version stays skipped until a newer one appears", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(stable_releases_json(), UpdateChannel::Stable, "", "",
                                                     "2.4.9", "", InstallFlavour::WindowsInstaller, "x86_64");

    CHECK(update_is_skipped(candidate, "2.5.0", ""));
    CHECK_FALSE(update_is_skipped(candidate, "2.4.0", ""));
    CHECK_FALSE(update_is_skipped(candidate, "", ""));
}

TEST_CASE("A skipped nightly digest stays skipped until the build changes", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(nightly_releases_json(), UpdateChannel::Nightly,
                                                     "sha256:nightly1", "nightly", "2.6.0-dev",
                                                     "2026-11-01T00:00:00Z", InstallFlavour::LinuxAppImage,
                                                     "x86_64");

    CHECK(update_is_skipped(candidate, "", "sha256:nightly2"));
    CHECK_FALSE(update_is_skipped(candidate, "", "sha256:nightly1"));
    CHECK_FALSE(update_is_skipped(candidate, "", ""));
}

TEST_CASE("Skipping records the version for stable and the digest for nightly", "[UpdateChannel]")
{
    const UpdateCandidate stable = resolve_update(stable_releases_json(), UpdateChannel::Stable, "", "",
                                                  "2.4.9", "", InstallFlavour::WindowsInstaller, "x86_64");
    std::string skip_version;
    std::string skip_artifact = "keep";
    update_skip_values(stable, skip_version, skip_artifact);
    CHECK(skip_version == "2.5.0");
    CHECK(skip_artifact == "keep");

    const UpdateCandidate nightly = resolve_update(nightly_releases_json(), UpdateChannel::Nightly,
                                                   "sha256:nightly1", "nightly", "2.6.0-dev",
                                                   "2026-11-01T00:00:00Z", InstallFlavour::LinuxAppImage,
                                                   "x86_64");
    skip_version  = "keep";
    skip_artifact = "";
    update_skip_values(nightly, skip_version, skip_artifact);
    CHECK(skip_version == "keep");
    CHECK(skip_artifact == "sha256:nightly2");
}

TEST_CASE("Channel and flavour keys round-trip through their strings", "[UpdateChannel]")
{
    CHECK(update_channel_key(UpdateChannel::Stable) == "stable");
    CHECK(update_channel_key(UpdateChannel::Nightly) == "nightly");
    CHECK(update_channel_from_key("nightly") == UpdateChannel::Nightly);
    CHECK(update_channel_from_key("stable") == UpdateChannel::Stable);
    CHECK(update_channel_from_key("") == UpdateChannel::Stable);
    CHECK(update_channel_from_key("unknown") == UpdateChannel::Stable);

    CHECK(install_flavour_key(InstallFlavour::WindowsInstaller) == "windows_installer");
    CHECK(install_flavour_key(InstallFlavour::WindowsPortable) == "windows_portable");
    CHECK(install_flavour_key(InstallFlavour::MacOSBundle) == "macos_bundle");
    CHECK(install_flavour_key(InstallFlavour::LinuxAppImage) == "linux_appimage");
    CHECK(install_flavour_key(InstallFlavour::Msix) == "msix");
    CHECK(install_flavour_key(InstallFlavour::Flatpak) == "flatpak");
    CHECK(install_flavour_key(InstallFlavour::Other) == "other");

    CHECK(in_app_install_supported(InstallFlavour::WindowsInstaller));
    CHECK(in_app_install_supported(InstallFlavour::WindowsPortable));
    CHECK(in_app_install_supported(InstallFlavour::MacOSBundle));
    CHECK(in_app_install_supported(InstallFlavour::LinuxAppImage));
    CHECK_FALSE(in_app_install_supported(InstallFlavour::Msix));
    CHECK_FALSE(in_app_install_supported(InstallFlavour::Flatpak));
    CHECK_FALSE(in_app_install_supported(InstallFlavour::Other));
}
