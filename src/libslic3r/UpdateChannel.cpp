#include "UpdateChannel.hpp"

#include "Semver.hpp"

#include <cstdint>
#include <cstdio>
#include <ctime>
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
        return is_arm ? contains(name, "aarch64") : (is_x64 ? !contains(name, "aarch64") : false);
    case InstallFlavour::Msix:
    case InstallFlavour::Flatpak:
    case InstallFlavour::Other:
        return false;
    }
    return false;
}

// Stable assets name the release, nightly assets are the rolling build.
bool asset_matches_channel(const std::string& name, UpdateChannel channel, const std::string& version)
{
    if (channel == UpdateChannel::Nightly)
        return contains(name, "_nightly");
    if (version.empty())
        return false;
    return contains(name, std::string("V") + version) || contains(name, std::string("v") + version);
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

bool is_newer_stable(const std::string& candidate_version, const std::string& running_version)
{
    auto candidate = Semver::parse(candidate_version);
    auto running   = Semver::parse(running_version);
    if (!candidate || !running)
        return false;
    return *candidate > *running;
}

} // namespace

std::string update_channel_key(UpdateChannel channel)
{
    return channel == UpdateChannel::Nightly ? "nightly" : "stable";
}

UpdateChannel update_channel_from_key(const std::string& key)
{
    return key == "nightly" ? UpdateChannel::Nightly : UpdateChannel::Stable;
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
    case InstallFlavour::WindowsPortable:
    case InstallFlavour::MacOSBundle:
    case InstallFlavour::LinuxAppImage:
        return true;
    case InstallFlavour::Msix:
    case InstallFlavour::Flatpak:
    case InstallFlavour::Other:
        return false;
    }
    return false;
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
        Semver best;
        bool   have_best = false;
        for (const auto& entry : releases) {
            if (!entry.is_object() || bool_field(entry, "prerelease"))
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

    const bool channel_switch = !installed_channel.empty() && installed_channel != update_channel_key(channel);

    // Whether the release itself is worth offering, whether or not an asset fits this flavour.
    bool release_is_newer = false;
    if (channel == UpdateChannel::Stable) {
        release_is_newer = is_newer_stable(candidate.version, running_version);
    } else if (!installed_artifact.empty()) {
        release_is_newer = true;
    } else if (!build_timestamp.empty() && candidate.version > build_timestamp) {
        release_is_newer = true;
    }

    if (candidate.asset.name.empty()) {
        // Store, Flatpak and other flavours have no asset; offer the release so that the
        // caller can open the Store or release page instead of hiding the update forever.
        // A flavour that installs in-app must not be offered without an asset: there is
        // nothing to download, so the caller would hand the updater an empty URL.
        candidate.reason    = "no_asset_for_platform";
        candidate.available = !in_app_install_supported(flavour) && (release_is_newer || channel_switch);
        return candidate;
    }
    if (candidate.asset.digest.empty()) {
        candidate.reason = "no_digest";
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

    if (channel == UpdateChannel::Stable) {
        if (!release_is_newer) {
            candidate.reason = "up_to_date";
            return candidate;
        }
    } else if (installed_artifact.empty()) {
        if (build_timestamp.empty()) {
            candidate.reason = "source_build";
            return candidate;
        }
        if (candidate.version <= build_timestamp) {
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
