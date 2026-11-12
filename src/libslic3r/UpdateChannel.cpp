#include "UpdateChannel.hpp"

#include "Semver.hpp"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <string>

#include <nlohmann/json.hpp>

namespace Slic3r {

namespace {

using nlohmann::json;

std::string string_field(const json& object, const char* key)
{
    auto it = object.find(key);
    if (it == object.end() || !it->is_string())
        return std::string();
    return it->get<std::string>();
}

bool bool_field(const json& object, const char* key)
{
    auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

int64_t integer_field(const json& object, const char* key)
{
    auto it = object.find(key);
    if (it == object.end() || !it->is_number())
        return 0;
    return it->get<int64_t>();
}

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

bool ends_with(const std::string& text, const char* suffix)
{
    const std::string tail(suffix);
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

std::string lower_ascii(std::string text)
{
    for (char& ch : text)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return text;
}

std::string strip_version_prefix(const std::string& tag)
{
    if (!tag.empty() && (tag.front() == 'v' || tag.front() == 'V'))
        return tag.substr(1);
    return tag;
}

bool parse_tag_version(const std::string& tag, Semver& out)
{
    auto parsed = Semver::parse(strip_version_prefix(tag));
    if (!parsed)
        return false;
    out = *parsed;
    return true;
}

std::string nightly_label(const std::string& updated_at)
{
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    if (std::sscanf(updated_at.c_str(), "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) != 5)
        return updated_at;
    if (month < 1 || month > 12)
        return updated_at;

    static const char* const month_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::tm release_time = {};
    release_time.tm_year = year - 1900;
    release_time.tm_mon  = month - 1;
    release_time.tm_mday = day;
    release_time.tm_hour = hour;
    release_time.tm_min  = minute;

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "Nightly, %d %s %d %02d:%02d UTC",
                  release_time.tm_mday, month_names[release_time.tm_mon], release_time.tm_year + 1900,
                  release_time.tm_hour, release_time.tm_min);
    return std::string(buffer);
}

bool asset_is_excluded(const std::string& name)
{
    return contains(name, "profile_validator") || contains(name, "Debug_PDB") || contains(name, "preset_tests");
}

bool asset_matches_flavour(const std::string& name, InstallFlavour flavour, const std::string& arch)
{
    const bool is_x64 = arch == "x86_64";
    const bool is_arm = arch == "arm64";

    switch (flavour) {
    case InstallFlavour::WindowsInstaller:
        if (!contains(name, "Windows_Installer") || !ends_with(name, ".exe"))
            return false;
        return is_x64 ? contains(name, "_x64") : (is_arm ? contains(name, "_arm64") : false);
    case InstallFlavour::WindowsPortable:
        if (!contains(name, "portable") || !ends_with(name, ".zip"))
            return false;
        return is_x64 ? contains(name, "_x64") : (is_arm ? contains(name, "_arm64") : false);
    case InstallFlavour::MacOSBundle:
        return contains(name, "Mac_universal") && ends_with(name, ".dmg");
    case InstallFlavour::LinuxAppImage:
        if (!contains(name, "AppImage") || !ends_with(name, ".AppImage"))
            return false;
        // _belt builds are discontinued; their assets are never refreshed.
        if (contains(name, "_belt"))
            return false;
        return is_arm ? contains(name, "aarch64") : (is_x64 ? !contains(name, "aarch64") : false);
    case InstallFlavour::Msix:
    case InstallFlavour::Flatpak:
    case InstallFlavour::Other:
        return false;
    }
    return false;
}

// Stable and Pre-Release assets name the release, nightly assets are the rolling build.
bool asset_matches_channel(const std::string& name, UpdateChannel channel, const std::string& version)
{
    if (channel == UpdateChannel::Nightly)
        return contains(name, "_nightly");
    if (version.empty())
        return false;
    // Case-insensitive: the v2.3.2-rc release shipped its assets spelled V2.3.2-RC.
    return contains(lower_ascii(name), "v" + lower_ascii(version));
}

UpdateAsset pick_asset(const json& release, UpdateChannel channel, const std::string& version,
                       InstallFlavour flavour, const std::string& arch)
{
    auto assets = release.find("assets");
    if (assets == release.end() || !assets->is_array())
        return UpdateAsset();

    for (const auto& entry : *assets) {
        if (!entry.is_object())
            continue;
        const std::string name = string_field(entry, "name");
        if (name.empty() || asset_is_excluded(name))
            continue;
        if (!asset_matches_flavour(name, flavour, arch))
            continue;
        if (!asset_matches_channel(name, channel, version))
            continue;

        UpdateAsset asset;
        asset.name   = name;
        asset.url    = string_field(entry, "browser_download_url");
        asset.digest = string_field(entry, "digest");
        asset.size   = integer_field(entry, "size");
        return asset;
    }
    return UpdateAsset();
}

bool is_newer_version(const std::string& candidate_version, const std::string& running_version)
{
    auto candidate = Semver::parse(candidate_version);
    auto running   = Semver::parse(running_version);
    if (!candidate || !running)
        return false;
    return *candidate > *running;
}

// CI uploads nightly assets up to about an hour after the build starts, and the release's
// updated_at follows the last upload, so a release only counts as newer past this margin.
constexpr int64_t kNightlyBuildMargin = 6 * 60 * 60;

// Seconds since the Unix epoch for a UTC "YYYY-MM-DDTHH:MM:SSZ" stamp.
bool parse_utc_timestamp(const std::string& text, int64_t& seconds)
{
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    char zone = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d%c", &year, &month, &day, &hour, &minute, &second, &zone) != 7 ||
        zone != 'Z')
        return false;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
        return false;

    // Days since the epoch from the civil date (proleptic Gregorian calendar).
    const int64_t y   = year - (month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - 719468;

    seconds = ((days * 24 + hour) * 60 + minute) * 60 + second;
    return true;
}

// False when either stamp does not parse, so an unknown build is never offered a nightly.
bool nightly_is_newer_than_build(const std::string& release_time, const std::string& build_timestamp)
{
    int64_t release_seconds = 0;
    int64_t build_seconds   = 0;
    if (!parse_utc_timestamp(release_time, release_seconds) || !parse_utc_timestamp(build_timestamp, build_seconds))
        return false;
    return release_seconds > build_seconds + kNightlyBuildMargin;
}

} // namespace

std::string update_channel_key(UpdateChannel channel)
{
    switch (channel) {
    case UpdateChannel::Nightly:    return "nightly";
    case UpdateChannel::PreRelease: return "prerelease";
    case UpdateChannel::Stable:     return "stable";
    }
    return "stable";
}

UpdateChannel update_channel_from_key(const std::string& key)
{
    if (key == "nightly")
        return UpdateChannel::Nightly;
    if (key == "prerelease")
        return UpdateChannel::PreRelease;
    return UpdateChannel::Stable;
}

std::string install_flavour_key(InstallFlavour flavour)
{
    switch (flavour) {
    case InstallFlavour::WindowsInstaller: return "windows_installer";
    case InstallFlavour::WindowsPortable:  return "windows_portable";
    case InstallFlavour::MacOSBundle:      return "macos_bundle";
    case InstallFlavour::LinuxAppImage:    return "linux_appimage";
    case InstallFlavour::Msix:             return "msix";
    case InstallFlavour::Flatpak:          return "flatpak";
    case InstallFlavour::Other:            return "other";
    }
    return "other";
}

bool in_app_install_supported(InstallFlavour flavour)
{
    switch (flavour) {
    case InstallFlavour::WindowsInstaller:
    case InstallFlavour::MacOSBundle:
        return true;
    case InstallFlavour::WindowsPortable:
    case InstallFlavour::LinuxAppImage:
    case InstallFlavour::Msix:
    case InstallFlavour::Flatpak:
    case InstallFlavour::Other:
        return false;
    }
    return false;
}

bool is_download_only(InstallFlavour flavour)
{
    switch (flavour) {
    case InstallFlavour::WindowsPortable:
    case InstallFlavour::LinuxAppImage:
        return true;
    case InstallFlavour::WindowsInstaller:
    case InstallFlavour::MacOSBundle:
    case InstallFlavour::Msix:
    case InstallFlavour::Flatpak:
    case InstallFlavour::Other:
        return false;
    }
    return false;
}

bool can_install_in_app(const UpdateCandidate& candidate, InstallFlavour flavour)
{
    return in_app_install_supported(flavour) && !candidate.asset.name.empty() && !candidate.asset.url.empty() &&
           !candidate.asset.digest.empty();
}

UpdateCandidate resolve_update(const std::string& releases_json,
                               UpdateChannel       channel,
                               const std::string&  installed_artifact,
                               const std::string&  installed_channel,
                               const std::string&  running_version,
                               const std::string&  build_timestamp,
                               InstallFlavour      flavour,
                               const std::string&  arch)
{
    UpdateCandidate candidate;
    candidate.channel = channel;

    // The Store only ships the stable channel.
    if (flavour == InstallFlavour::Msix && channel != UpdateChannel::Stable) {
        candidate.reason = "store_channel";
        return candidate;
    }

    json releases;
    try {
        releases = json::parse(releases_json);
    } catch (const std::exception&) {
        candidate.reason = "no_release";
        return candidate;
    }
    if (!releases.is_array()) {
        candidate.reason = "no_release";
        return candidate;
    }

    const json* release = nullptr;
    if (channel == UpdateChannel::Nightly) {
        for (const auto& entry : releases) {
            if (entry.is_object() && string_field(entry, "tag_name") == "nightly-builds") {
                release = &entry;
                break;
            }
        }
        if (release == nullptr) {
            candidate.reason = "no_release";
            return candidate;
        }
        candidate.version       = string_field(*release, "updated_at");
        candidate.version_label = nightly_label(candidate.version);
    } else {
        // Stable never offers a prerelease. Pre-Release offers the newest release of
        // either kind. The nightly release is not semver, so neither channel picks it up.
        Semver best;
        bool   have_best = false;
        for (const auto& entry : releases) {
            if (!entry.is_object())
                continue;
            if (channel == UpdateChannel::Stable && bool_field(entry, "prerelease"))
                continue;
            Semver parsed;
            if (!parse_tag_version(string_field(entry, "tag_name"), parsed))
                continue;
            if (!have_best || parsed > best) {
                best      = parsed;
                have_best = true;
                release   = &entry;
            }
        }
        if (!have_best || release == nullptr) {
            candidate.reason = "no_release";
            return candidate;
        }
        candidate.version       = strip_version_prefix(string_field(*release, "tag_name"));
        candidate.version_label = candidate.version;
    }

    candidate.release_url   = string_field(*release, "html_url");
    candidate.release_notes = string_field(*release, "body");
    candidate.asset         = pick_asset(*release, channel, candidate.version, flavour, arch);

    // Switching channel normally offers that channel's current artifact even when it sorts
    // lower. Pre-Release is the exception: its candidate is the newest release of any kind,
    // so the override could only hand back something older than what is running.
    const bool channel_switch = channel != UpdateChannel::PreRelease &&
                                !installed_channel.empty() && installed_channel != update_channel_key(channel);

    // Whether the release itself is worth offering, whether or not an asset fits this flavour.
    bool release_is_newer = false;
    if (channel != UpdateChannel::Nightly) {
        release_is_newer = is_newer_version(candidate.version, running_version);
    } else if (!installed_artifact.empty()) {
        release_is_newer = true;
    } else if (nightly_is_newer_than_build(candidate.version, build_timestamp)) {
        release_is_newer = true;
    }

    if (candidate.asset.name.empty() || candidate.asset.digest.empty()) {
        // Offer the release even without an installable asset: the caller opens the Store or
        // the release page instead, so the update is not hidden forever.
        candidate.reason    = candidate.asset.name.empty() ? "no_asset_for_platform" : "no_digest";
        candidate.available = release_is_newer || channel_switch;
        return candidate;
    }
    if (!installed_artifact.empty() && installed_artifact == candidate.asset.digest) {
        candidate.reason = "up_to_date";
        return candidate;
    }
    if (channel_switch) {
        candidate.available = true;
        candidate.reason    = "channel_switch";
        return candidate;
    }

    if (channel != UpdateChannel::Nightly) {
        if (!release_is_newer) {
            candidate.reason = "up_to_date";
            return candidate;
        }
    } else if (installed_artifact.empty()) {
        if (build_timestamp.empty()) {
            candidate.reason = "source_build";
            return candidate;
        }
        if (!nightly_is_newer_than_build(candidate.version, build_timestamp)) {
            candidate.reason = "up_to_date";
            return candidate;
        }
    }

    candidate.available = true;
    candidate.reason.clear();
    return candidate;
}

bool update_is_skipped(const UpdateCandidate& candidate,
                       const std::string&     skip_version,
                       const std::string&     skip_artifact)
{
    if (candidate.channel == UpdateChannel::Nightly) {
        const std::string identity = candidate.identity();
        return !identity.empty() && identity == skip_artifact;
    }

    if (skip_version.empty() || candidate.version.empty())
        return false;
    auto candidate_semver = Semver::parse(candidate.version);
    auto skipped_semver   = Semver::parse(skip_version);
    if (candidate_semver && skipped_semver)
        return *candidate_semver <= *skipped_semver;
    return candidate.version <= skip_version;
}

void update_skip_values(const UpdateCandidate& candidate,
                        std::string&           skip_version,
                        std::string&           skip_artifact)
{
    if (candidate.channel == UpdateChannel::Nightly)
        skip_artifact = candidate.identity();
    else
        skip_version = candidate.version;
}

} // namespace Slic3r
