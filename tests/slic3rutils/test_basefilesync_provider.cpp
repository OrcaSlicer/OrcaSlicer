#include <catch2/catch_all.hpp>

#include "slic3r/Utils/BaseFileSyncProvider.hpp"
#include "slic3r/Utils/SyncBackend.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace fs = boost::filesystem;

using namespace Slic3r;

// ============================================================
// In-memory SyncBackend so BaseFileSyncProvider's orchestration
// (flush wiring, ETag/OCC conflict handling) can be exercised
// without a real WebDAV/Git remote.
// ============================================================
namespace {

class FakeBackend : public SyncBackend {
public:
    struct Entry { std::string content; std::string etag; };

    std::map<std::string, Entry> files;   // remote_path -> content/etag
    int                          flush_calls{0};
    bool                         connected{true};
    bool                         flush_should_fail{false};

    bool connect(std::string&) override { connected = true; return true; }
    void disconnect() override { connected = false; }
    bool is_connected() const override { return connected; }
    bool test_connection(std::string&) override { return true; }

    bool ensure_directory(const std::string&, std::string&) override { return true; }

    bool list_files(const std::string& dir, std::vector<RemoteFileInfo>& out, std::string&) override
    {
        std::string prefix = dir;
        if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');
        std::set<std::string> subdirs;
        for (const auto& [path, e] : files) {
            if (path.rfind(prefix, 0) != 0) continue;
            const std::string rest  = path.substr(prefix.size());
            const auto        slash = rest.find('/');
            if (slash == std::string::npos) {
                RemoteFileInfo info;
                info.path = path;
                info.etag = e.etag;
                info.size = e.content.size();
                out.push_back(info);
            } else {
                // Emit each immediate subdirectory once so bundle discovery
                // (which enumerates <prefix>bundles/<id> dirs) has something to
                // iterate -- a real WebDAV/Git listing reports directories too.
                const std::string d = prefix + rest.substr(0, slash);
                if (subdirs.insert(d).second) {
                    RemoteFileInfo info;
                    info.path         = d;
                    info.is_directory = true;
                    out.push_back(info);
                }
            }
        }
        return true;
    }

    bool download_file(const std::string& path, std::string& content, RemoteFileInfo& info,
                       std::string& err, SyncError* code) override
    {
        auto it = files.find(path);
        if (it == files.end()) {
            if (code) *code = SyncError::NotFound;
            err = "404";
            return false;
        }
        content   = it->second.content;
        info.path = path;
        info.etag = it->second.etag;
        info.size = content.size();
        if (code) *code = SyncError::None;
        return true;
    }

    bool upload_file(const std::string& path, const std::string& content,
                     const std::string& expected_etag, std::string& new_etag,
                     std::string& err, SyncError* code) override
    {
        auto it = files.find(path);
        if (!expected_etag.empty() && it != files.end() && it->second.etag != expected_etag) {
            if (code) *code = SyncError::Conflict;
            err = "CONFLICT";
            return false;
        }
        const std::string etag = "etag" + std::to_string(++m_etag_counter);
        files[path] = Entry{content, etag};
        new_etag = etag;
        if (code) *code = SyncError::None;
        return true;
    }

    bool delete_file(const std::string& path, std::string&) override { files.erase(path); return true; }

    SyncBackendType type() const override { return SyncBackendType::WebDAV; }
    std::string     display_name() const override { return "fake"; }
    std::string     fp{"fake"};
    std::string     fingerprint() const override { return fp; }

    bool flush(std::string& err) override
    {
        if (flush_should_fail) { err = "flush failed"; return false; }
        ++flush_calls;
        return true;
    }

private:
    int m_etag_counter{0};
};

class FakeProvider : public BaseFileSyncProvider {
public:
    explicit FakeProvider(std::unique_ptr<SyncBackend> backend)
        : BaseFileSyncProvider(std::move(backend)) {}
    std::string provider_id() const override { return "fake"; }
};

// Points Slic3r::data_dir() at a fresh temp dir for the duration of a test so
// save_state()/load_state() (which write "<provider_id>_sync_state.json" under
// data_dir) stay isolated, then removes it.
struct ScopedDataDir {
    fs::path dir;
    ScopedDataDir()
    {
        static std::atomic<unsigned> seq{0};
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        dir = fs::temp_directory_path() /
              ("orca-bfs-" + std::to_string(stamp) + "-" + std::to_string(seq++));
        fs::create_directories(dir);
        Slic3r::set_data_dir(dir.string());
    }
    ~ScopedDataDir()
    {
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// Bundle ids currently visible through list_subscribed_bundles.
std::set<std::string> subscribed_ids(BaseFileSyncProvider& provider)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::vector<std::string>                         notfound, unauthorized;
    provider.list_subscribed_bundles(&out, notfound, unauthorized);
    std::set<std::string> ids;
    for (const auto& pr : out) ids.insert(pr.first);
    return ids;
}

} // namespace

// ============================================================
// #2: flush must be wired -- a file backend that only stages
// (Git) never reaches the remote without it.
// ============================================================

TEST_CASE("BaseFileSyncProvider push uploads and flushes", "[ProfileSync][BaseFileSync]") {
    auto         backend = std::make_unique<FakeBackend>();
    FakeBackend* raw     = backend.get();
    FakeProvider provider(std::move(backend));

    auto res = provider.push_preset("print", "MyPreset", R"({"a":"1"})", "", "");
    CHECK(res.http_code == 200);
    CHECK(res.remote_id == "print/MyPreset");
    CHECK(raw->files.count("presets/print/MyPreset.json") == 1);
    CHECK(raw->flush_calls == 1);
}

TEST_CASE("BaseFileSyncProvider delete flushes", "[ProfileSync][BaseFileSync]") {
    auto         backend = std::make_unique<FakeBackend>();
    FakeBackend* raw     = backend.get();
    FakeProvider provider(std::move(backend));

    provider.push_preset("print", "P", "{}", "", "");
    const int before = raw->flush_calls;

    const int rc = provider.delete_preset("print", "print/P");
    CHECK(rc == 0);
    CHECK(raw->files.count("presets/print/P.json") == 0);
    CHECK(raw->flush_calls == before + 1);
}

// ============================================================
// A failed flush (e.g. Git push rejected) must surface as 500 and
// must NOT leave the provider believing the preset was synced --
// otherwise the next cycle would skip re-pushing it. A later push,
// once the remote is reachable, must go through.
// ============================================================

TEST_CASE("BaseFileSyncProvider push surfaces flush failure and stays retryable", "[ProfileSync][BaseFileSync]") {
    auto         backend = std::make_unique<FakeBackend>();
    FakeBackend* raw     = backend.get();
    FakeProvider provider(std::move(backend));

    raw->flush_should_fail = true;
    auto failed = provider.push_preset("print", "P", R"({"v":"1"})", "", "");
    CHECK(failed.http_code == 500);

    raw->flush_should_fail = false;
    auto ok = provider.push_preset("print", "P", R"({"v":"2"})", "", "");
    CHECK(ok.http_code == 200);
    CHECK(raw->files["presets/print/P.json"].content == R"({"v":"2"})");
}

// ============================================================
// #5: on an ETag conflict the cached ETag must be refreshed to
// the current remote value, so a forced re-push (empty expected
// etag -> cached fallback) succeeds instead of 412-looping the
// merge dialog every tick.
// ============================================================

TEST_CASE("BaseFileSyncProvider conflict refreshes etag and re-push succeeds", "[ProfileSync][BaseFileSync]") {
    auto         backend = std::make_unique<FakeBackend>();
    FakeBackend* raw     = backend.get();
    FakeProvider provider(std::move(backend));

    auto first = provider.push_preset("filament", "F", R"({"v":"1"})", "", "");
    REQUIRE(first.http_code == 200);

    const std::string path = "presets/filament/F.json";
    REQUIRE(raw->files.count(path) == 1);

    // Another client overwrites the remote out of band -> new etag.
    raw->files[path] = FakeBackend::Entry{R"({"v":"remote"})", "etag-remote"};

    // Our next push still uses the (now stale) cached etag -> conflict.
    auto conflicted = provider.push_preset("filament", "F", R"({"v":"local"})", "", "");
    CHECK(conflicted.http_code == 409);

    auto conflicts = provider.take_pending_conflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].remote_id == "filament/F");
    CHECK(conflicts[0].remote_json == R"({"v":"remote"})");

    // KeepLocal re-push with an empty expected etag must now succeed because
    // the cached etag was refreshed to the remote one during the conflict.
    auto repush = provider.push_preset("filament", "F", R"({"v":"local"})", "filament/F", "");
    CHECK(repush.http_code == 200);
    CHECK(provider.take_pending_conflicts().empty());
    CHECK(raw->files[path].content == R"({"v":"local"})");
}

// ============================================================
// Sanity: list_presets maps stored files back to (type, remote_id).
// ============================================================

TEST_CASE("BaseFileSyncProvider list_presets returns pushed presets", "[ProfileSync][BaseFileSync]") {
    auto         backend = std::make_unique<FakeBackend>();
    FakeProvider provider(std::move(backend));

    provider.push_preset("print",   "A", "{}", "", "");
    provider.push_preset("printer", "B", "{}", "", "");

    std::map<std::string, std::string> seen; // remote_id -> type
    provider.list_presets([&seen](const std::string& type, const std::string& id,
                                  const std::string&, long long) {
        seen[id] = type;
    });
    CHECK(seen["print/A"] == "print");
    CHECK(seen["printer/B"] == "printer");
}

// ============================================================
// pull_preset returns the stored content and maps a missing file
// to 404.
// ============================================================

TEST_CASE("BaseFileSyncProvider pull_preset returns content and maps not-found", "[ProfileSync][BaseFileSync]") {
    auto         backend = std::make_unique<FakeBackend>();
    FakeProvider provider(std::move(backend));

    provider.push_preset("print", "P", R"({"v":"1"})", "", "");

    std::string out;
    auto hit = provider.pull_preset("print", "print/P", out);
    CHECK(hit.http_code == 200);
    CHECK(out == R"({"v":"1"})");
    CHECK_FALSE(hit.etag.empty());

    std::string missing;
    auto miss = provider.pull_preset("print", "print/Nope", missing);
    CHECK(miss.http_code == 404);
}

// ============================================================
// IBundleProvider: publish -> fetch must preserve the metadata
// fields and the per-type preset values (the value_map <-> JSON
// serialization that also carries WebDAV/Git preset content).
// ============================================================

TEST_CASE("BaseFileSyncProvider publishes and fetches a bundle round-trip", "[ProfileSync][BaseFileSync]") {
    auto         backend = std::make_unique<FakeBackend>();
    FakeProvider provider(std::move(backend));

    BundleMetadata meta;
    meta.id            = "B1";
    meta.name          = "My Bundle";
    meta.version       = "1.0";
    meta.description   = "desc";
    meta.author        = "me";
    meta.imported_time = 111;
    meta.updated_time  = 222;

    const std::map<std::string, std::map<std::string, std::string>> presets = {
        {"print/PA",    {{"layer_height", "0.2"}}},
        {"filament/FA", {{"temperature", "210"}}},
    };
    std::string published_version;
    REQUIRE(provider.publish_local_bundle(meta, presets, published_version) == 0);
    CHECK(published_version == "1.0");

    BundleMetadata got;
    std::map<std::string, std::map<std::string, std::string>> got_presets;
    REQUIRE(provider.fetch_bundle("B1", "", &got_presets, &got) == 0);

    CHECK(got.id            == "B1");
    CHECK(got.name          == "My Bundle");
    CHECK(got.version       == "1.0");
    CHECK(got.author        == "me");
    CHECK(got.description   == "desc");
    CHECK(got.imported_time == 111);
    CHECK(got.updated_time  == 222);

    REQUIRE(got_presets.count("PA") == 1);
    REQUIRE(got_presets.count("FA") == 1);
    CHECK(got_presets["PA"]["layer_height"] == "0.2");
    CHECK(got_presets["FA"]["temperature"]  == "210");
}

// ============================================================
// unsubscribe hides a bundle from the local listing (file
// backends have no server-side subscription list).
// ============================================================

TEST_CASE("BaseFileSyncProvider list_subscribed_bundles hides unsubscribed ids", "[ProfileSync][BaseFileSync]") {
    ScopedDataDir data_dir;

    auto         backend = std::make_unique<FakeBackend>();
    FakeProvider provider(std::move(backend));

    auto publish = [&](const std::string& id) {
        BundleMetadata m; m.id = id; m.name = id; m.version = "1";
        std::map<std::string, std::map<std::string, std::string>> p = {{"print/X", {{"a", "1"}}}};
        std::string v;
        provider.publish_local_bundle(m, p, v);
    };
    publish("B1");
    publish("B2");

    CHECK(subscribed_ids(provider) == std::set<std::string>{"B1", "B2"});
    provider.unsubscribe_bundle("B1");
    CHECK(subscribed_ids(provider) == std::set<std::string>{"B2"});
}

// ============================================================
// State persistence: a hidden bundle survives save_state/load_state
// when the fingerprint matches, and is dropped when it changes
// (the backend now points at a different remote).
// ============================================================

TEST_CASE("BaseFileSyncProvider persists hidden bundles and honours the fingerprint", "[ProfileSync][BaseFileSync]") {
    ScopedDataDir data_dir;

    // Shared remote contents: two published bundles.
    std::map<std::string, FakeBackend::Entry> remote;
    for (const std::string& id : {"B1", "B2"}) {
        remote["bundles/" + id + "/bundle_metadata.json"] =
            FakeBackend::Entry{ "{\"id\":\"" + id + "\",\"name\":\"" + id + "\",\"version\":\"1\"}", "e" };
    }

    // Provider 1 unsubscribes B1 and persists the hidden set.
    {
        auto backend    = std::make_unique<FakeBackend>();
        backend->files  = remote;
        FakeProvider provider(std::move(backend));
        provider.unsubscribe_bundle("B1"); // updates m_hidden_bundles + save_state()
    }

    auto visible_with_fingerprint = [&](const std::string& fingerprint) {
        auto backend   = std::make_unique<FakeBackend>();
        backend->files = remote;
        backend->fp    = fingerprint;
        FakeProvider provider(std::move(backend));
        provider.load_state();
        return subscribed_ids(provider);
    };

    // Matching fingerprint -> the hidden B1 is restored from disk.
    CHECK(visible_with_fingerprint("fake") == std::set<std::string>{"B2"});
    // Different fingerprint -> cached state dropped, B1 visible again.
    CHECK(visible_with_fingerprint("other") == std::set<std::string>{"B1", "B2"});
}
