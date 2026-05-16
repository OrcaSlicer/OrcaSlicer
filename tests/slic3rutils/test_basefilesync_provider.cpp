#include <catch2/catch_all.hpp>

#include "slic3r/Utils/BaseFileSyncProvider.hpp"
#include "slic3r/Utils/SyncBackend.hpp"

#include <map>
#include <memory>
#include <string>

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
        for (const auto& [path, e] : files) {
            if (path.rfind(prefix, 0) != 0) continue;
            // direct children only
            if (path.substr(prefix.size()).find('/') != std::string::npos) continue;
            RemoteFileInfo info;
            info.path = path;
            info.etag = e.etag;
            info.size = e.content.size();
            out.push_back(info);
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
    std::string     fingerprint() const override { return "fake"; }

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
