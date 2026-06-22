// MarketplaceClient — desktop client for the configured plugin marketplace.
//
// Pure-engine HTTP client (no wxWidgets) that talks to the configured marketplace backend
// over HTTPS. Owns catalog fetch + on-disk snapshot caching and signed
// `.orcaplugin` downloads with sha256 verification. The wx GUI lives only in
// MarketplaceDialog, which drives this class.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {

/// A single plugin version, as returned by the marketplace.
struct MarketplaceVersion {
    std::string version;            // semver string
    std::string kind;               // "native"|"wasm"|"webview"|"hybrid"|"data"
    std::vector<std::string> provides;     // slot-kind names
    std::vector<std::string> permissions;  // permission names
    std::string engine_compat;      // semver VersionReq
    std::string url;                // marketplace /download URL (signed; proxied)
    std::string sha256;             // hex; client re-verifies on download
    std::uint64_t size_bytes = 0;
    std::string published_at;       // RFC3339
};

/// A single plugin entry in the marketplace catalog.
struct MarketplacePlugin {
    std::string id;                 // reverse-DNS
    std::string name;
    std::string description;
    std::string category;           // "device"|"profile_pack"|"gcode"|"ui"|"misc"
    std::vector<std::string> tags;
    std::string author;
    std::optional<std::string> homepage;
    std::optional<std::string> repo;
    std::optional<std::string> license;
    std::vector<std::string> screenshots;
    std::string latest_version;
    std::vector<MarketplaceVersion> versions;
};

/// A snapshot of the marketplace registry.
struct MarketplaceCatalog {
    std::string revision_sha256;
    std::string generated_at;
    std::vector<MarketplacePlugin> plugins;
};

/// Progress callback for downloads. `bytes_so_far` and `bytes_total` are unsigned.
/// `bytes_total = 0` means the server didn't tell us; treat as indeterminate.
using DownloadProgressFn = std::function<void(std::uint64_t bytes_so_far, std::uint64_t bytes_total)>;

/// Thrown by `MarketplaceClient` for any failure mode. Carries a typed `kind`
/// + a human message. Callers should pattern-match on `kind` for UX.
class MarketplaceError : public std::runtime_error {
public:
    enum class Kind {
        Network,
        Http,           // non-2xx from server
        Json,           // parse failure
        Sha256Mismatch,
        Filesystem,
        CacheMiss,
    };
    MarketplaceError(Kind kind, std::string message);
    Kind kind() const noexcept { return kind_; }
private:
    Kind kind_;
};

/// Marketplace HTTP client. Owns:
///   - the marketplace base URL (for /v1/* endpoints)
///   - the catalog cache directory (<data_dir>/marketplace/)
///   - sha256 verification on every download
class MarketplaceClient {
public:
    /// Constructs against the configured marketplace URL. For tests, prefer
    /// `MarketplaceClient::with_base_url(url, cache_dir)`.
    static MarketplaceClient default_instance();

    /// Returns true if the resolved marketplace base URL is non-empty.
    /// Used by the UI to gate showing the marketplace tab / loading the SPA.
    /// Resolution order matches `default_instance()`: AppConfig key
    /// `marketplace_url` → AppConfig `marketplace_base_url` → env var
    /// `ORCA_MARKETPLACE_URL` → compile-time `ORCA_MARKETPLACE_DEFAULT_URL`.
    static bool is_configured();

    /// Returns the resolved marketplace base URL ("" if unconfigured).
    /// Same resolution as `is_configured()` / `default_instance()`.
    static std::string resolved_base_url();

    /// Constructs against a custom base URL + cache dir. cache_dir is created if absent.
    static MarketplaceClient with_base_url(std::string base_url, std::filesystem::path cache_dir);

    /// Fetches /v1/registry/latest, compares against on-disk cache, and either
    /// returns the cached catalog (sha matches) or fetches the immutable
    /// snapshot URL and caches it (sha differs / no cache). Synchronous; uses
    /// `Slic3r::Http::get(...)` under the hood. Throws `MarketplaceError` on
    /// network / JSON / sha mismatch.
    MarketplaceCatalog fetch_catalog();

    /// Downloads the specified `.orcaplugin` to `dest_path`, verifying sha256.
    /// Throws `MarketplaceError` on network failure, sha mismatch, or write failure.
    void download_plugin(const MarketplaceVersion& version,
                         const std::filesystem::path& dest_path,
                         DownloadProgressFn progress = {});

    /// Returns the on-disk cache path for the catalog snapshot with the given sha.
    /// Public so tests can pre-seed the cache.
    std::filesystem::path catalog_cache_path(const std::string& sha256) const;

    /// Removes any cached catalog snapshot whose sha does NOT match the given.
    /// Called after a successful fetch to keep disk usage bounded.
    void prune_catalog_cache(const std::string& keep_sha256);

private:
    MarketplaceClient(std::string base_url, std::filesystem::path cache_dir);
    std::string base_url_;
    std::filesystem::path cache_dir_;
};

} // namespace Slic3r
