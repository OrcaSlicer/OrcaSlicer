// MarketplaceClient implementation. See header for surface contract.
//
// Catalog fetch is two-hop:
//   1. GET <base>/v1/registry/latest        — small {revision_sha256, url} JSON
//   2. GET <url> (Cloudflare-cached, immutable) — full registry snapshot
//
// On-disk cache lives at <data_dir>/marketplace/registry-<sha>.json so that
// the immutable snapshots can be re-used across launches without re-validating
// against the origin. The fetched body's sha256 is recomputed and compared
// against the revision_sha256 from step 1 to defend against a poisoned CDN.
//
// Plugin downloads go through the `/v1/plugins/{id}/versions/{ver}/download`
// proxy endpoint on the marketplace. The server validates an HMAC sig + exp on the
// request; the URL we hold in MarketplaceVersion already carries those query
// params (registry-gen embeds them at publish time). The client just follows
// redirects, streams the body, and re-checks the published sha256.

#include "MarketplaceClient.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "Http.hpp"

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <ios>
#include <sstream>
#include <system_error>
#include <utility>

namespace Slic3r {

namespace {

constexpr std::size_t kCatalogSizeLimit = 10ull * 1024 * 1024;
constexpr std::size_t kPluginSizeLimit  = 50ull * 1024 * 1024;
constexpr long        kRequestTimeoutSeconds = 60;

// Hex-encodes the raw SHA-256 digest of `data` in lowercase. Used both to
// fingerprint the catalog body and to verify downloaded plugin archives.
std::string sha256_hex(const std::string& data)
{
    std::array<unsigned char, SHA256_DIGEST_LENGTH> hash{};
    SHA256(reinterpret_cast<const unsigned char*>(data.data()),
           data.size(),
           hash.data());
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char b : hash) {
        oss << std::setw(2) << static_cast<unsigned>(b);
    }
    return oss.str();
}

// Constant-time-ish lowercase compare on hex digests. The two sides come from
// trusted sources (server + local sha256()), so timing-safety is defensive,
// not load-bearing — but matching the convention used elsewhere in OrcaSlicer.
bool hex_equal_ci(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
        const unsigned char cb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
        diff |= (ca ^ cb);
    }
    return diff == 0;
}

// Strips trailing slashes so we can build URLs by concatenation without
// emitting `//v1/...`.
std::string normalize_base_url(std::string url)
{
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

std::filesystem::path default_cache_dir()
{
    return std::filesystem::path(Slic3r::data_dir()) / "marketplace";
}

// Synchronous GET that captures status + body + libcurl error. Returns the
// body on 2xx; throws MarketplaceError otherwise.
std::string http_get_sync(const std::string& url,
                          std::size_t        size_limit,
                          const Http::ProgressFn& progress = {})
{
    std::string body;
    std::string err;
    unsigned    status = 0;
    bool        completed = false;

    auto req = Http::get(url);
    if (size_limit > 0) req.size_limit(size_limit);
    req.timeout_max(kRequestTimeoutSeconds);
    if (progress) req.on_progress(progress);
    req.on_complete([&](std::string b, unsigned s) {
        body      = std::move(b);
        status    = s;
        completed = true;
    });
    req.on_error([&](std::string b, std::string e, unsigned s) {
        body   = std::move(b);
        err    = std::move(e);
        status = s;
    });
    req.perform_sync();

    if (!completed) {
        if (status != 0) {
            throw MarketplaceError(MarketplaceError::Kind::Http,
                "HTTP " + std::to_string(status) + " from " + url +
                (err.empty() ? std::string{} : (": " + err)));
        }
        throw MarketplaceError(MarketplaceError::Kind::Network,
            "request to " + url + " failed: " + (err.empty() ? "unknown error" : err));
    }
    if (status < 200 || status >= 300) {
        throw MarketplaceError(MarketplaceError::Kind::Http,
            "HTTP " + std::to_string(status) + " from " + url);
    }
    return body;
}

template <typename T>
T json_get_or(const nlohmann::json& j, const char* key, T fallback)
{
    if (!j.contains(key)) return fallback;
    try { return j.at(key).get<T>(); }
    catch (...) { return fallback; }
}

std::optional<std::string> json_get_optional_string(const nlohmann::json& j, const char* key)
{
    if (!j.contains(key) || j.at(key).is_null()) return std::nullopt;
    try { return j.at(key).get<std::string>(); }
    catch (...) { return std::nullopt; }
}

std::vector<std::string> json_get_string_array(const nlohmann::json& j, const char* key)
{
    std::vector<std::string> out;
    if (!j.contains(key) || !j.at(key).is_array()) return out;
    for (const auto& v : j.at(key)) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

MarketplaceVersion parse_version(const nlohmann::json& j)
{
    MarketplaceVersion v;
    v.version        = json_get_or<std::string>(j, "version", "");
    v.kind           = json_get_or<std::string>(j, "kind", "");
    v.provides       = json_get_string_array(j, "provides");
    v.permissions    = json_get_string_array(j, "permissions");
    v.engine_compat  = json_get_or<std::string>(j, "engine_compat", "");
    v.url            = json_get_or<std::string>(j, "url", "");
    v.sha256         = json_get_or<std::string>(j, "sha256", "");
    v.size_bytes     = json_get_or<std::uint64_t>(j, "size_bytes", 0);
    v.published_at   = json_get_or<std::string>(j, "published_at", "");
    return v;
}

MarketplacePlugin parse_plugin(const nlohmann::json& j)
{
    MarketplacePlugin p;
    p.id             = json_get_or<std::string>(j, "id", "");
    p.name           = json_get_or<std::string>(j, "name", "");
    p.description    = json_get_or<std::string>(j, "description", "");
    p.category       = json_get_or<std::string>(j, "category", "");
    p.tags           = json_get_string_array(j, "tags");
    p.author         = json_get_or<std::string>(j, "author", "");
    p.homepage       = json_get_optional_string(j, "homepage");
    p.repo           = json_get_optional_string(j, "repo");
    p.license        = json_get_optional_string(j, "license");
    p.screenshots    = json_get_string_array(j, "screenshots");
    p.latest_version = json_get_or<std::string>(j, "latest_version", "");
    if (j.contains("versions") && j.at("versions").is_array()) {
        for (const auto& vj : j.at("versions")) {
            p.versions.push_back(parse_version(vj));
        }
    }
    return p;
}

MarketplaceCatalog parse_catalog(const std::string& body, const std::string& expected_sha)
{
    nlohmann::json j;
    try { j = nlohmann::json::parse(body); }
    catch (const std::exception& e) {
        throw MarketplaceError(MarketplaceError::Kind::Json,
            std::string("catalog JSON parse failed: ") + e.what());
    }

    MarketplaceCatalog cat;
    cat.revision_sha256 = json_get_or<std::string>(j, "revision_sha256", "");
    cat.generated_at    = json_get_or<std::string>(j, "generated_at", "");
    if (cat.revision_sha256.empty() && !expected_sha.empty()) {
        cat.revision_sha256 = expected_sha;
    }
    if (j.contains("plugins") && j.at("plugins").is_array()) {
        for (const auto& pj : j.at("plugins")) {
            cat.plugins.push_back(parse_plugin(pj));
        }
    }
    return cat;
}

void write_file_atomic(const std::filesystem::path& dest, const std::string& body)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        throw MarketplaceError(MarketplaceError::Kind::Filesystem,
            "create_directories failed for " + dest.parent_path().string() +
            ": " + ec.message());
    }

    auto tmp = dest;
    tmp += ".part";
    {
        std::ofstream of(tmp, std::ios::binary | std::ios::trunc);
        if (!of) {
            throw MarketplaceError(MarketplaceError::Kind::Filesystem,
                "cannot open " + tmp.string() + " for write");
        }
        of.write(body.data(), static_cast<std::streamsize>(body.size()));
        of.flush();
        if (!of) {
            throw MarketplaceError(MarketplaceError::Kind::Filesystem,
                "write failed for " + tmp.string());
        }
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        // Fallback for cross-device: copy then remove.
        fs::copy_file(tmp, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            throw MarketplaceError(MarketplaceError::Kind::Filesystem,
                "rename/copy to " + dest.string() + " failed: " + ec.message());
        }
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
    }
}

std::optional<std::string> read_file_to_string(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream oss;
    oss << f.rdbuf();
    if (!f && !f.eof()) return std::nullopt;
    return oss.str();
}

} // namespace

// ---- MarketplaceError --------------------------------------------------------

MarketplaceError::MarketplaceError(Kind kind, std::string message)
    : std::runtime_error(std::move(message)), kind_(kind) {}

// ---- MarketplaceClient -------------------------------------------------------

MarketplaceClient::MarketplaceClient(std::string base_url, std::filesystem::path cache_dir)
    : base_url_(normalize_base_url(std::move(base_url)))
    , cache_dir_(std::move(cache_dir))
{
    std::error_code ec;
    std::filesystem::create_directories(cache_dir_, ec);
    // If we can't create it now, we'll surface the error on the first write.
}

std::string MarketplaceClient::resolved_base_url()
{
    // Resolution path matches `AppConfig::marketplace_base_url()`. We re-implement
    // here so the GUI-less callers (tests, CLI) don't need an AppConfig.
    if (auto* app = GUI::wxGetApp().app_config) {
        return app->marketplace_base_url();
    }
    return "";
}

bool MarketplaceClient::is_configured()
{
    return !resolved_base_url().empty();
}

MarketplaceClient MarketplaceClient::default_instance()
{
    // Empty base URL is allowed — every method short-circuits with a typed
    // error if it's blank, and `is_configured()` is the canonical gate.
    return MarketplaceClient(resolved_base_url(), default_cache_dir());
}

MarketplaceClient MarketplaceClient::with_base_url(std::string base_url,
                                                   std::filesystem::path cache_dir)
{
    return MarketplaceClient(std::move(base_url), std::move(cache_dir));
}

std::filesystem::path MarketplaceClient::catalog_cache_path(const std::string& sha256) const
{
    return cache_dir_ / (std::string("registry-") + sha256 + ".json");
}

MarketplaceCatalog MarketplaceClient::fetch_catalog()
{
    if (base_url_.empty()) {
        throw MarketplaceError(MarketplaceError::Kind::Http,
            "marketplace not configured (set Preferences → Online → Marketplace URL)");
    }

    // Step 1: pointer fetch. The /latest endpoint is small and edge-cached
    // for ~60s; treat any non-2xx as fatal.
    const std::string latest_url = base_url_ + "/v1/registry/latest";
    const std::string latest_body = http_get_sync(latest_url, 64 * 1024);

    nlohmann::json latest_json;
    try { latest_json = nlohmann::json::parse(latest_body); }
    catch (const std::exception& e) {
        throw MarketplaceError(MarketplaceError::Kind::Json,
            std::string("/v1/registry/latest parse failed: ") + e.what());
    }
    const std::string revision_sha = json_get_or<std::string>(latest_json, "revision_sha256", "");
    const std::string snapshot_url = json_get_or<std::string>(latest_json, "url", "");
    if (revision_sha.empty() || snapshot_url.empty()) {
        throw MarketplaceError(MarketplaceError::Kind::Json,
            "/v1/registry/latest missing revision_sha256 or url");
    }

    // Step 2: cache lookup. The snapshot is immutable, so if we already
    // hold a body whose recomputed sha matches, skip the network entirely.
    const auto cache_path = catalog_cache_path(revision_sha);
    if (auto cached = read_file_to_string(cache_path)) {
        if (hex_equal_ci(sha256_hex(*cached), revision_sha)) {
            return parse_catalog(*cached, revision_sha);
        }
        // Stale/poisoned cache file — drop it so we re-download cleanly.
        std::error_code rm_ec;
        std::filesystem::remove(cache_path, rm_ec);
    }

    // Step 3: snapshot fetch from the (Cloudflare-cached) immutable URL.
    const std::string snapshot_body = http_get_sync(snapshot_url, kCatalogSizeLimit);
    const std::string actual_sha = sha256_hex(snapshot_body);
    if (!hex_equal_ci(actual_sha, revision_sha)) {
        throw MarketplaceError(MarketplaceError::Kind::Sha256Mismatch,
            "catalog sha256 mismatch: expected " + revision_sha +
            ", got " + actual_sha);
    }

    write_file_atomic(cache_path, snapshot_body);
    prune_catalog_cache(revision_sha);
    return parse_catalog(snapshot_body, revision_sha);
}

void MarketplaceClient::download_plugin(const MarketplaceVersion& version,
                                        const std::filesystem::path& dest_path,
                                        DownloadProgressFn progress)
{
    if (version.url.empty()) {
        throw MarketplaceError(MarketplaceError::Kind::Http,
            "plugin version has no download URL");
    }
    if (version.sha256.empty()) {
        throw MarketplaceError(MarketplaceError::Kind::Sha256Mismatch,
            "plugin version has no published sha256");
    }

    Http::ProgressFn http_progress;
    if (progress) {
        http_progress = [progress = std::move(progress)](Http::Progress p, bool& /*cancel*/) {
            progress(static_cast<std::uint64_t>(p.dlnow),
                     static_cast<std::uint64_t>(p.dltotal));
        };
    }

    const std::string body = http_get_sync(version.url, kPluginSizeLimit, http_progress);

    const std::string actual_sha = sha256_hex(body);
    if (!hex_equal_ci(actual_sha, version.sha256)) {
        throw MarketplaceError(MarketplaceError::Kind::Sha256Mismatch,
            "plugin sha256 mismatch: expected " + version.sha256 +
            ", got " + actual_sha);
    }

    write_file_atomic(dest_path, body);
}

void MarketplaceClient::prune_catalog_cache(const std::string& keep_sha256)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(cache_dir_, ec)) return;

    const std::string keep_name = std::string("registry-") + keep_sha256 + ".json";
    for (auto it = fs::directory_iterator(cache_dir_, ec);
         !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const auto& entry = *it;
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        if (name.rfind("registry-", 0) != 0) continue;
        if (name == keep_name) continue;
        std::error_code rm_ec;
        fs::remove(entry.path(), rm_ec);
    }
}

} // namespace Slic3r
