#include <catch2/catch_all.hpp>

#include <string>

#include "catch2/catch_test_macros.hpp"
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

// The nightly release lists the discontinued _belt AppImage before the main one.
const char* nightly_belt_first_releases_json()
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
                    "name": "OrcaSlicer_Linux_AppImage_Ubuntu2404_nightly_belt.AppImage",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Linux_AppImage_Ubuntu2404_nightly_belt.AppImage",
                    "digest": "sha256:belt",
                    "size": 310
                },
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

const char* nightly_belt_only_releases_json()
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
                    "name": "OrcaSlicer_Linux_AppImage_Ubuntu2404_nightly_belt.AppImage",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Linux_AppImage_Ubuntu2404_nightly_belt.AppImage",
                    "digest": "sha256:belt",
                    "size": 310
                }
            ]
        }
    ])JSON";
}

// The nightly release with its only asset carrying no digest.
const char* nightly_release_without_digest_json()
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
                    "name": "OrcaSlicer_Windows_Installer_x64_nightly.exe",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Windows_Installer_x64_nightly.exe",
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

// The shape the service returns while 2.6.0 is in flight: the final release, the
// rolling nightly and the three prereleases, deliberately not in version order. The
// final release sorts above every prerelease, so both Stable and Pre-Release offer it.
const char* prerelease_releases_json()
{
    return R"JSON([
        {
            "tag_name": "v2.6.0",
            "prerelease": false,
            "html_url": "https://example.invalid/releases/v2.6.0",
            "body": "2.6.0",
            "updated_at": "2026-11-20T10:00:00Z",
            "assets": [
                {
                    "name": "OrcaSlicer_Windows_Installer_V2.6.0_x64.exe",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Windows_Installer_V2.6.0_x64.exe",
                    "digest": "sha256:2600",
                    "size": 400
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
                    "name": "OrcaSlicer_Windows_Installer_x64_nightly.exe",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Windows_Installer_x64_nightly.exe",
                    "digest": "sha256:nightly2",
                    "size": 350
                }
            ]
        },
        {
            "tag_name": "v2.6.0-beta",
            "prerelease": true,
            "html_url": "https://example.invalid/releases/v2.6.0-beta",
            "body": "2.6.0 beta",
            "updated_at": "2026-10-27T17:22:32Z",
            "assets": [
                {
                    "name": "OrcaSlicer_Windows_Installer_V2.6.0-beta_x64.exe",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Windows_Installer_V2.6.0-beta_x64.exe",
                    "digest": "sha256:2600beta",
                    "size": 390
                }
            ]
        },
        {
            "tag_name": "v2.6.0-rc",
            "prerelease": true,
            "html_url": "https://example.invalid/releases/v2.6.0-rc",
            "body": "2.6.0 release candidate",
            "updated_at": "2026-11-01T10:36:26Z",
            "assets": [
                {
                    "name": "OrcaSlicer_Windows_Installer_V2.6.0-rc_x64.exe",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Windows_Installer_V2.6.0-rc_x64.exe",
                    "digest": "sha256:2600rc",
                    "size": 395
                }
            ]
        },
        {
            "tag_name": "v2.6.0-alpha",
            "prerelease": true,
            "html_url": "https://example.invalid/releases/v2.6.0-alpha",
            "body": "2.6.0 alpha",
            "updated_at": "2026-10-16T14:28:11Z",
            "assets": [
                {
                    "name": "OrcaSlicer_Windows_Installer_V2.6.0-alpha_x64.exe",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Windows_Installer_V2.6.0-alpha_x64.exe",
                    "digest": "sha256:2600alpha",
                    "size": 380
                }
            ]
        }
    ])JSON";
}

// The state right after a final ships, before the next cycle opens: a stable build and a
// prerelease of the next version.
const char* newer_prerelease_release_json()
{
    return R"JSON([
        {
            "tag_name": "v2.6.0",
            "prerelease": false,
            "html_url": "https://example.invalid/releases/v2.6.0",
            "body": "2.6.0",
            "updated_at": "2026-11-20T10:00:00Z",
            "assets": [
                {
                    "name": "OrcaSlicer_Windows_Installer_V2.6.0_x64.exe",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Windows_Installer_V2.6.0_x64.exe",
                    "digest": "sha256:2600",
                    "size": 400
                }
            ]
        },
        {
            "tag_name": "v2.7.0-alpha",
            "prerelease": true,
            "html_url": "https://example.invalid/releases/v2.7.0-alpha",
            "body": "2.7.0 alpha",
            "updated_at": "2026-12-02T09:00:00Z",
            "assets": [
                {
                    "name": "OrcaSlicer_Windows_Installer_V2.7.0-alpha_x64.exe",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Windows_Installer_V2.7.0-alpha_x64.exe",
                    "digest": "sha256:2700alpha",
                    "size": 410
                }
            ]
        }
    ])JSON";
}

// v2.3.2-rc shipped its assets with the suffix spelled in upper case.
const char* mixed_case_asset_release_json()
{
    return R"JSON([
        {
            "tag_name": "v2.3.2-rc",
            "prerelease": true,
            "html_url": "https://example.invalid/releases/v2.3.2-rc",
            "body": "2.3.2 rc",
            "updated_at": "2026-03-01T13:59:35Z",
            "assets": [
                {
                    "name": "OrcaSlicer_Windows_Installer_V2.3.2-RC_x64.exe",
                    "browser_download_url": "https://example.invalid/OrcaSlicer_Windows_Installer_V2.3.2-RC_x64.exe",
                    "digest": "sha256:232rc",
                    "size": 300
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

TEST_CASE("The pre-release channel offers the final release when it is newer than every prerelease", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(prerelease_releases_json(), UpdateChannel::PreRelease, "",
                                                     "", "2.5.0", "", InstallFlavour::WindowsInstaller, "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.channel == UpdateChannel::PreRelease);
    CHECK(candidate.version == "2.6.0");
    CHECK(candidate.asset.name == "OrcaSlicer_Windows_Installer_V2.6.0_x64.exe");
    CHECK(candidate.asset.digest == "sha256:2600");
}

TEST_CASE("The pre-release channel offers a prerelease newer than the newest final release", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(newer_prerelease_release_json(), UpdateChannel::PreRelease, "",
                                                     "", "2.5.0", "", InstallFlavour::WindowsInstaller, "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.version == "2.7.0-alpha");
}

TEST_CASE("The pre-release channel offers the newest final release when no prerelease exists", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(stable_releases_json(), UpdateChannel::PreRelease, "", "",
                                                     "2.4.9", "", InstallFlavour::WindowsInstaller, "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.version == "2.5.0");
}

TEST_CASE("Stable never offers a prerelease that outranks every final release", "[UpdateChannel]")
{
    // 2.7.0-alpha sorts above the newest final, so a Stable check that saw it would offer it.
    const UpdateCandidate candidate = resolve_update(newer_prerelease_release_json(), UpdateChannel::Stable, "",
                                                     "", "2.5.0", "", InstallFlavour::WindowsInstaller, "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.version == "2.6.0");
}

TEST_CASE("The pre-release channel offers nothing when the running build is the final release", "[UpdateChannel]")
{
    // The newest release is the 2.6.0 final itself, so nothing is newer than the running build.
    const UpdateCandidate candidate = resolve_update(prerelease_releases_json(), UpdateChannel::PreRelease, "",
                                                     "", "2.6.0", "", InstallFlavour::WindowsInstaller, "x86_64");

    CHECK_FALSE(candidate.available);
    CHECK(candidate.reason == "up_to_date");
    CHECK(candidate.version == "2.6.0");
}

TEST_CASE("A beta is offered the final release of its version", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(prerelease_releases_json(), UpdateChannel::PreRelease, "",
                                                     "", "2.6.0-beta", "", InstallFlavour::WindowsInstaller, "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.version == "2.6.0");
}

TEST_CASE("A dev build is offered the final release of its own version", "[UpdateChannel]")
{
    // Semver ranks the final release above the -dev build of the same version.
    const UpdateCandidate candidate = resolve_update(prerelease_releases_json(), UpdateChannel::PreRelease, "",
                                                     "", "2.6.0-dev", "", InstallFlavour::WindowsInstaller, "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.version == "2.6.0");
}

TEST_CASE("Switching to pre-release does not offer a prerelease older than the running build", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(prerelease_releases_json(), UpdateChannel::PreRelease,
                                                     "sha256:2600", "stable", "2.6.0", "",
                                                     InstallFlavour::WindowsInstaller, "x86_64");

    CHECK_FALSE(candidate.available);
    CHECK(candidate.reason == "up_to_date");
}

TEST_CASE("Switching to pre-release offers a prerelease newer than the running build", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(newer_prerelease_release_json(), UpdateChannel::PreRelease,
                                                     "sha256:2600", "stable", "2.6.0", "",
                                                     InstallFlavour::WindowsInstaller, "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.version == "2.7.0-alpha");
}

TEST_CASE("A skipped pre-release stays skipped until a newer one appears", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(newer_prerelease_release_json(), UpdateChannel::PreRelease, "",
                                                     "", "2.6.0", "", InstallFlavour::WindowsInstaller, "x86_64");
    REQUIRE(candidate.available);

    CHECK(update_is_skipped(candidate, "2.7.0-alpha", ""));
    CHECK(update_is_skipped(candidate, "2.7.0", ""));
    CHECK_FALSE(update_is_skipped(candidate, "2.6.0", ""));

    std::string skip_version  = "2.6.0";
    std::string skip_artifact = "sha256:nightly2";
    update_skip_values(candidate, skip_version, skip_artifact);
    CHECK(skip_version == "2.7.0-alpha");
    CHECK(skip_artifact == "sha256:nightly2");
}

TEST_CASE("An asset version matches regardless of case", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(mixed_case_asset_release_json(), UpdateChannel::PreRelease, "",
                                                     "", "2.3.1", "", InstallFlavour::WindowsInstaller, "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.version == "2.3.2-rc");
    CHECK(candidate.asset.name == "OrcaSlicer_Windows_Installer_V2.3.2-RC_x64.exe");
}

TEST_CASE("A release asset without a digest is offered but cannot be installed in-app", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(stable_release_without_digest_json(), UpdateChannel::Stable,
                                                     "", "", "2.4.9", "", InstallFlavour::WindowsInstaller,
                                                     "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.reason == "no_digest");
    CHECK(candidate.asset.name == "OrcaSlicer_Windows_Installer_V2.5.0_x64.exe");
    CHECK_FALSE(can_install_in_app(candidate, InstallFlavour::WindowsInstaller));
}

TEST_CASE("A supported flavour with no matching asset is offered so the release page can open", "[UpdateChannel]")
{
    // An AppImage has nothing to download from a release that only ships a Windows
    // installer, so the caller falls back to the release page.
    const UpdateCandidate candidate = resolve_update(stable_releases_json(), UpdateChannel::Stable, "", "",
                                                     "2.4.9", "", InstallFlavour::LinuxAppImage, "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.reason == "no_asset_for_platform");
    CHECK(candidate.asset.name.empty());
    CHECK(candidate.version == "2.5.0");
    CHECK(candidate.release_url == "https://example.invalid/releases/v2.5.0");
    CHECK_FALSE(can_install_in_app(candidate, InstallFlavour::LinuxAppImage));
}

TEST_CASE("Only a candidate with an asset, URL and digest is installed in-app", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(stable_releases_json(), UpdateChannel::Stable, "", "",
                                                     "2.4.9", "", InstallFlavour::WindowsInstaller, "x86_64");

    CHECK(can_install_in_app(candidate, InstallFlavour::WindowsInstaller));
    CHECK_FALSE(can_install_in_app(candidate, InstallFlavour::Msix));
}

TEST_CASE("The Store offers only the stable channel", "[UpdateChannel]")
{
    const UpdateCandidate nightly = resolve_update(nightly_releases_json(), UpdateChannel::Nightly,
                                                   "sha256:nightly1", "nightly", "2.6.0-dev",
                                                   "2026-11-01T00:00:00Z", InstallFlavour::Msix, "x86_64");
    const UpdateCandidate prerelease = resolve_update(prerelease_releases_json(), UpdateChannel::PreRelease, "",
                                                      "", "2.5.0", "", InstallFlavour::Msix, "x86_64");

    CHECK_FALSE(nightly.available);
    CHECK(nightly.reason == "store_channel");
    CHECK_FALSE(prerelease.available);
    CHECK(prerelease.reason == "store_channel");
}

TEST_CASE("An AppImage update takes the main build over a discontinued _belt build", "[UpdateChannel]")
{
    // The _belt asset is listed first, as it is on the nightly release.
    const UpdateCandidate candidate = resolve_update(nightly_belt_first_releases_json(), UpdateChannel::Nightly,
                                                     "sha256:nightly1", "nightly", "2.6.0-dev",
                                                     "2026-11-01T00:00:00Z", InstallFlavour::LinuxAppImage, "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.asset.name == "OrcaSlicer_Linux_AppImage_Ubuntu2404_nightly.AppImage");
    CHECK(candidate.asset.digest == "sha256:nightly2");
}

TEST_CASE("A _belt AppImage alone leaves the release offered without an in-app asset", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(nightly_belt_only_releases_json(), UpdateChannel::Nightly,
                                                     "sha256:nightly1", "nightly", "2.6.0-dev",
                                                     "2026-11-01T00:00:00Z", InstallFlavour::LinuxAppImage, "x86_64");

    CHECK(candidate.available);
    CHECK(candidate.reason == "no_asset_for_platform");
    CHECK(candidate.asset.name.empty());
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

TEST_CASE("A nightly without a digest is skipped by its build time", "[UpdateChannel]")
{
    const UpdateCandidate candidate = resolve_update(nightly_release_without_digest_json(), UpdateChannel::Nightly,
                                                     "sha256:nightly1", "nightly", "2.6.0-dev",
                                                     "2026-11-01T00:00:00Z", InstallFlavour::WindowsInstaller,
                                                     "x86_64");
    REQUIRE(candidate.available);

    std::string skip_version;
    std::string skip_artifact;
    update_skip_values(candidate, skip_version, skip_artifact);
    CHECK(skip_artifact == "2026-11-10T08:31:54Z");
    CHECK(update_is_skipped(candidate, "", skip_artifact));
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
    CHECK(update_channel_key(UpdateChannel::PreRelease) == "prerelease");
    CHECK(update_channel_from_key("nightly") == UpdateChannel::Nightly);
    CHECK(update_channel_from_key("prerelease") == UpdateChannel::PreRelease);
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
