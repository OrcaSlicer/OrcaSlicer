#ifndef slic3r_UpdateChannel_hpp_
#define slic3r_UpdateChannel_hpp_

// Resolving an update from the release list served by check-version.orcaslicer.com.
//
// Everything in here is pure: no network, no wx, no globals, no config access.
// The caller decides which channel it wants and what the running install is;
// this decides whether there is something to offer and which file it is.

#include <cstdint>
#include <string>

namespace Slic3r {

enum class UpdateChannel {
    Stable,      // the newest published release
    PreRelease,  // the newest release of any kind, alpha, beta, rc or final
    Nightly,     // the rolling nightly-builds release
};

// "stable" / "prerelease" / "nightly"; anything else resolves to Stable.
std::string   update_channel_key(UpdateChannel channel);
UpdateChannel update_channel_from_key(const std::string& key);

// How this copy of the application was installed. Decides which artifact to
// pick and whether an in-app install is possible at all.
enum class InstallFlavour {
    WindowsInstaller,
    WindowsPortable,
    MacOSBundle,
    LinuxAppImage,
    Msix,       // Microsoft Store: updates come from the Store
    Flatpak,    // updates come from flatpak
    Other,      // distro package, source build, unknown
};

std::string   install_flavour_key(InstallFlavour flavour);
bool          in_app_install_supported(InstallFlavour flavour);
// The app cannot replace itself, but the matching asset is downloaded directly (WindowsPortable, LinuxAppImage).
bool          is_download_only(InstallFlavour flavour);

struct UpdateAsset {
    std::string name;    // e.g. OrcaSlicer_Windows_Installer_V2.5.0_x64.exe
    std::string url;     // browser_download_url
    std::string digest;  // "sha256:<hex>", empty when the release list omits it
    int64_t     size = 0;
};

struct UpdateCandidate {
    // There is something to install right now.
    bool          available = false;
    UpdateChannel channel   = UpdateChannel::Stable;
    // Release version, e.g. "2.5.0"; for the nightly channel the release tag is
    // not a version, so this is the build label derived from the release time.
    std::string   version;
    // What the dialog shows: "2.5.0" or "Nightly, 10 Nov 2026 08:16".
    std::string   version_label;
    std::string   release_url;    // html_url, for release notes and fallbacks
    std::string   release_notes;  // markdown body
    UpdateAsset   asset;
    // Why nothing is offered, for the log. Empty when available.
    std::string   reason;

    // The identity of the artifact, used for skip and for "already installed". Falls back to
    // the version when the release list carries no digest, so a skip still sticks.
    std::string   identity() const { return asset.digest.empty() ? version : asset.digest; }
};

// True when the candidate names an artifact the in-app updater can fetch and verify.
bool can_install_in_app(const UpdateCandidate& candidate, InstallFlavour flavour);

// releases_json is the array returned by /latest (GitHub releases shape).
// running_version is SoftFever_VERSION; build_timestamp is SLIC3R_BUILD_TIMESTAMP
// (empty on source builds). installed_artifact and installed_channel describe the
// build that is running now (empty on the first run after this feature ships).
UpdateCandidate resolve_update(const std::string& releases_json,
                               UpdateChannel       channel,
                               const std::string&  installed_artifact,
                               const std::string&  installed_channel,
                               const std::string&  running_version,
                               const std::string&  build_timestamp,
                               InstallFlavour      flavour,
                               const std::string&  arch);

// True when this candidate is the one the user asked not to see again.
bool update_is_skipped(const UpdateCandidate& candidate,
                       const std::string&     skip_version,
                       const std::string&     skip_artifact);

// Values to persist when the user skips: the version for Stable and Pre-Release,
// the artifact digest for Nightly. The other one is left untouched.
void update_skip_values(const UpdateCandidate& candidate,
                        std::string&           skip_version,
                        std::string&           skip_artifact);

} // namespace Slic3r

#endif // slic3r_UpdateChannel_hpp_
