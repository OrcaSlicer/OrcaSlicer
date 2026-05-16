#include "BaseFileSyncProvider.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>

namespace fs = boost::filesystem;

namespace Slic3r {

// Subdirectory inside the backend's remote_prefix() under which per-preset
// JSONs live. Bundles get a sibling "bundles/" tree, see IBundleProvider.
static const char* PRESETS_SUBDIR = "presets";

std::string BaseFileSyncProvider::preset_types_iter[3] = { "print", "filament", "printer" };

BaseFileSyncProvider::BaseFileSyncProvider(std::unique_ptr<SyncBackend> backend)
    : m_backend(std::move(backend))
{}

BaseFileSyncProvider::~BaseFileSyncProvider() = default;

std::string BaseFileSyncProvider::display_name() const
{
    return m_backend ? m_backend->display_name() : std::string("(no backend)");
}

std::string BaseFileSyncProvider::fingerprint() const
{
    return m_backend ? m_backend->fingerprint() : std::string();
}

bool BaseFileSyncProvider::is_configured() const
{
    return static_cast<bool>(m_backend);
}

int BaseFileSyncProvider::connect(std::string& error_out)
{
    if (!m_backend) {
        error_out = "no backend";
        return -1;
    }
    return m_backend->connect(error_out) ? 0 : -1;
}

bool BaseFileSyncProvider::is_connected()
{
    return m_backend && m_backend->is_connected();
}

std::string BaseFileSyncProvider::remote_path_for(const std::string& preset_type,
                                                  const std::string& preset_name) const
{
    std::string prefix = m_backend ? m_backend->remote_prefix() : std::string();
    if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');
    return prefix + PRESETS_SUBDIR + "/" + preset_type + "/" + preset_name + ".json";
}

bool BaseFileSyncProvider::parse_remote_path(const std::string& remote_path,
                                             std::string&       out_type,
                                             std::string&       out_name) const
{
    std::string prefix = m_backend ? m_backend->remote_prefix() : std::string();
    if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');
    std::string anchor = prefix + PRESETS_SUBDIR + "/";

    auto pos = remote_path.find(anchor);
    if (pos == std::string::npos) return false;

    std::string rel = remote_path.substr(pos + anchor.size());
    auto slash = rel.find('/');
    if (slash == std::string::npos) return false;

    out_type = rel.substr(0, slash);
    std::string filename = rel.substr(slash + 1);
    if (filename.size() < 6 || filename.substr(filename.size() - 5) != ".json") return false;
    out_name = filename.substr(0, filename.size() - 5);
    return true;
}

std::string BaseFileSyncProvider::state_file_path() const
{
    fs::path p(Slic3r::data_dir());
    p /= (provider_id() + "_sync_state.json");
    return p.string();
}

static std::string sha256_hex(const std::string& data)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        oss << std::setw(2) << static_cast<int>(digest[i]);
    return oss.str();
}

void BaseFileSyncProvider::load_state()
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_state.clear();
    m_state_fingerprint.clear();

    std::string path = state_file_path();
    if (!fs::exists(path)) return;

    try {
        std::ifstream in(path);
        nlohmann::json j;
        in >> j;

        std::string stored_fp = j.value("fingerprint", "");
        std::string current_fp = fingerprint();
        if (stored_fp != current_fp) {
            // Backend pointed at a different remote -- start fresh.
            BOOST_LOG_TRIVIAL(info) << "[" << provider_id()
                << "] state fingerprint mismatch, dropping cached state";
            return;
        }
        m_state_fingerprint = stored_fp;

        if (auto it = j.find("files"); it != j.end() && it->is_object()) {
            for (auto& [rid, entry] : it->items()) {
                CachedEntry ce;
                ce.etag         = entry.value("etag", "");
                ce.updated_time = entry.value("updated_time", 0LL);
                ce.base_hash    = entry.value("base_hash", "");
                m_state[rid] = std::move(ce);
            }
        }
        if (auto it = j.find("hidden_bundles"); it != j.end() && it->is_array()) {
            m_hidden_bundles = it->get<std::vector<std::string>>();
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[" << provider_id()
            << "] failed to parse state file: " << e.what();
    }
}

void BaseFileSyncProvider::save_state()
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    nlohmann::json j;
    j["fingerprint"] = fingerprint();
    nlohmann::json files = nlohmann::json::object();
    for (auto& [rid, ce] : m_state) {
        files[rid] = {
            {"etag",         ce.etag},
            {"updated_time", ce.updated_time},
            {"base_hash",    ce.base_hash},
        };
    }
    j["files"] = std::move(files);
    j["hidden_bundles"] = m_hidden_bundles;

    try {
        std::ofstream out(state_file_path());
        out << j.dump(2);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[" << provider_id()
            << "] failed to write state file: " << e.what();
    }
}

PresetSyncResult BaseFileSyncProvider::push_preset(const std::string& preset_type,
                                                   const std::string& preset_name,
                                                   const std::string& json_content,
                                                   const std::string& expected_etag)
{
    PresetSyncResult result;
    if (!m_backend) {
        result.http_code = 500;
        result.error_message = "no backend";
        return result;
    }

    std::string remote_path = remote_path_for(preset_type, preset_name);
    std::string remote_dir  = remote_path.substr(0, remote_path.find_last_of('/'));
    std::string err;
    if (!m_backend->ensure_directory(remote_dir, err)) {
        result.http_code = 500;
        result.error_message = "ensure_directory: " + err;
        return result;
    }

    SyncError sync_err = SyncError::None;
    std::string new_etag;
    bool ok = m_backend->upload_file(remote_path, json_content, expected_etag,
                                     new_etag, err, &sync_err);

    if (ok) {
        result.http_code   = 200;
        result.remote_id   = preset_type + "/" + preset_name;
        result.etag        = new_etag;
        result.updated_time = 0; // file backends fill this from list_files

        std::lock_guard<std::mutex> lock(m_state_mutex);
        auto& ce = m_state[result.remote_id];
        ce.etag      = new_etag;
        ce.base_hash = sha256_hex(json_content);
        return result;
    }

    if (sync_err == SyncError::Conflict) {
        // ETag mismatch -- somebody else pushed since our last sync.
        // Fetch the current remote version so the GUI can run a merge.
        std::string remote_content, dl_err;
        RemoteFileInfo info;
        if (m_backend->download_file(remote_path, remote_content, info, dl_err)) {
            PresetSyncConflict conflict;
            conflict.preset_type = preset_type;
            conflict.preset_name = preset_name;
            conflict.local_json  = json_content;
            conflict.remote_json = remote_content;
            conflict.remote_id   = preset_type + "/" + preset_name;
            {
                std::lock_guard<std::mutex> lk(m_state_mutex);
                if (auto it = m_state.find(conflict.remote_id); it != m_state.end())
                    conflict.base_json = ""; // we don't keep the base body, only hash
            }
            std::lock_guard<std::mutex> lk(m_conflicts_mutex);
            m_pending_conflicts.push_back(std::move(conflict));
        }
        result.http_code     = 409;
        result.error_message = "conflict: " + err;
        return result;
    }

    result.http_code     = (sync_err == SyncError::AuthFailed) ? 401 : 500;
    result.error_message = err;
    return result;
}

PresetSyncResult BaseFileSyncProvider::pull_preset(const std::string& preset_type,
                                                   const std::string& remote_id,
                                                   std::string&       out_json)
{
    PresetSyncResult result;
    if (!m_backend) {
        result.http_code = 500;
        result.error_message = "no backend";
        return result;
    }

    // remote_id format is "<type>/<name>"; preset_type is informational here.
    (void) preset_type;
    auto slash = remote_id.find('/');
    if (slash == std::string::npos) {
        result.http_code = 400;
        result.error_message = "bad remote_id";
        return result;
    }
    std::string preset_name = remote_id.substr(slash + 1);
    std::string type_in_id  = remote_id.substr(0, slash);
    std::string remote_path = remote_path_for(type_in_id, preset_name);

    SyncError sync_err = SyncError::None;
    std::string err;
    RemoteFileInfo info;
    bool ok = m_backend->download_file(remote_path, out_json, info, err, &sync_err);
    if (!ok) {
        result.http_code     = (sync_err == SyncError::NotFound) ? 404
                             : (sync_err == SyncError::AuthFailed) ? 401 : 500;
        result.error_message = err;
        return result;
    }

    result.http_code    = 200;
    result.remote_id    = remote_id;
    result.etag         = info.etag;
    result.updated_time = info.modified_time;

    std::lock_guard<std::mutex> lock(m_state_mutex);
    auto& ce = m_state[remote_id];
    ce.etag         = info.etag;
    ce.updated_time = info.modified_time;
    ce.base_hash    = sha256_hex(out_json);
    return result;
}

int BaseFileSyncProvider::delete_preset(const std::string& preset_type,
                                        const std::string& remote_id)
{
    if (!m_backend) return -1;

    auto slash = remote_id.find('/');
    std::string type_in_id  = (slash == std::string::npos) ? preset_type : remote_id.substr(0, slash);
    std::string preset_name = (slash == std::string::npos) ? remote_id : remote_id.substr(slash + 1);
    std::string remote_path = remote_path_for(type_in_id, preset_name);

    std::string err;
    if (!m_backend->delete_file(remote_path, err)) {
        BOOST_LOG_TRIVIAL(warning) << "[" << provider_id()
            << "] delete_preset(" << remote_id << ") failed: " << err;
        return -1;
    }
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_state.erase(remote_id);
    }
    return 0;
}

int BaseFileSyncProvider::list_presets(const PresetListCallback& cb)
{
    if (!m_backend || !cb) return -1;

    std::string err;
    if (!m_backend->refresh(err)) {
        BOOST_LOG_TRIVIAL(warning) << "[" << provider_id()
            << "] refresh before list_presets failed: " << err;
        // continue anyway -- WebDAV refresh is a no-op
    }

    std::string prefix = m_backend->remote_prefix();
    if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');

    for (const auto& type : preset_types_iter) {
        std::string dir = prefix + PRESETS_SUBDIR + "/" + type;
        std::vector<RemoteFileInfo> entries;
        std::string list_err;
        if (!m_backend->list_files(dir, entries, list_err)) {
            BOOST_LOG_TRIVIAL(debug) << "[" << provider_id() << "] list "
                << dir << " failed (probably empty): " << list_err;
            continue;
        }
        for (const auto& f : entries) {
            if (f.is_directory) continue;
            std::string t, name;
            if (!parse_remote_path(f.path, t, name)) continue;
            std::string remote_id = t + "/" + name;
            cb(t, remote_id, f.etag, f.modified_time);
        }
    }
    return 0;
}

std::vector<PresetSyncConflict> BaseFileSyncProvider::take_pending_conflicts()
{
    std::lock_guard<std::mutex> lock(m_conflicts_mutex);
    std::vector<PresetSyncConflict> out;
    out.swap(m_pending_conflicts);
    return out;
}

int BaseFileSyncProvider::apply_conflict_resolution(
    const PresetSyncConflict&           conflict,
    const PresetSyncConflictResolution& resolution)
{
    switch (resolution.choice) {
    case PresetSyncConflictChoice::KeepRemote: {
        // Caller is expected to write remote_json to disk; we just update
        // the cached etag so the next list_presets does not re-trigger.
        std::lock_guard<std::mutex> lock(m_state_mutex);
        auto& ce = m_state[conflict.remote_id];
        ce.base_hash = sha256_hex(conflict.remote_json);
        return 0;
    }
    case PresetSyncConflictChoice::KeepLocal: {
        // Push local content forcing past the server version.
        // We do not yet have the latest remote etag here, so the orchestrator
        // is responsible for re-issuing push_preset with a fresh etag.
        return 0;
    }
    case PresetSyncConflictChoice::Merge: {
        // Same as KeepLocal but with merged content -- orchestrator pushes.
        return 0;
    }
    case PresetSyncConflictChoice::Skip:
    default:
        return 0;
    }
}

// ----------------------------------------------------------------------------
// IBundleProvider -- on-disk layout below the backend prefix:
//   <prefix>bundles/<bundle_id>/bundle_metadata.json
//   <prefix>bundles/<bundle_id>/print/*.json
//   <prefix>bundles/<bundle_id>/filament/*.json
//   <prefix>bundles/<bundle_id>/printer/*.json
// Unlike Orca Cloud, file backends do not have a per-user subscription list
// server-side -- every bundle in <prefix>bundles/ is considered available
// to every client configured against this remote. unsubscribe is local-only:
// the bundle_id is added to m_hidden_bundles and filtered out of listing.
// ----------------------------------------------------------------------------

static const char* BUNDLES_SUBDIR = "bundles";

static std::string make_bundles_prefix(const SyncBackend* backend)
{
    std::string prefix = backend ? backend->remote_prefix() : std::string();
    if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');
    return prefix + BUNDLES_SUBDIR + "/";
}

static nlohmann::json values_map_to_json(const std::map<std::string, std::string>& vm)
{
    nlohmann::json j = nlohmann::json::object();
    for (auto& [k, v] : vm) j[k] = v;
    return j;
}

static std::map<std::string, std::string> json_to_values_map(const nlohmann::json& j)
{
    std::map<std::string, std::string> out;
    if (!j.is_object()) return out;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.value().is_string()) out[it.key()] = it.value().get<std::string>();
        else                        out[it.key()] = it.value().dump();
    }
    return out;
}

static nlohmann::json bundle_metadata_to_json(const BundleMetadata& m)
{
    nlohmann::json j;
    j["id"]            = m.id;
    j["name"]          = m.name;
    j["version"]       = m.version;
    j["description"]   = m.description;
    j["author"]        = m.author;
    j["imported_time"] = m.imported_time;
    j["updated_time"]  = m.updated_time;
    auto strip = [](const std::vector<std::string>& xs) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& x : xs)
            arr.push_back(boost::filesystem::path(x).filename().string());
        return arr;
    };
    j["print_presets"]    = strip(m.print_presets);
    j["filament_presets"] = strip(m.filament_presets);
    j["printer_presets"]  = strip(m.printer_presets);
    return j;
}

static bool bundle_metadata_from_json(const std::string& content, BundleMetadata& out)
{
    try {
        auto j = nlohmann::json::parse(content);
        if (j.contains("id"))             out.id           = j["id"].get<std::string>();
        if (j.contains("name"))           out.name         = j["name"].get<std::string>();
        if (j.contains("version"))        out.version      = j["version"].get<std::string>();
        if (j.contains("description"))    out.description  = j["description"].get<std::string>();
        if (j.contains("author"))         out.author       = j["author"].get<std::string>();
        if (j.contains("imported_time"))  out.imported_time = j["imported_time"].get<long long>();
        if (j.contains("updated_time"))   out.updated_time  = j["updated_time"].get<long long>();
        if (j.contains("print_presets"))    out.print_presets    = j["print_presets"].get<std::vector<std::string>>();
        if (j.contains("filament_presets")) out.filament_presets = j["filament_presets"].get<std::vector<std::string>>();
        if (j.contains("printer_presets"))  out.printer_presets  = j["printer_presets"].get<std::vector<std::string>>();
        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "bundle_metadata_from_json failed: " << e.what();
        return false;
    }
}

int BaseFileSyncProvider::list_subscribed_bundles(
    std::vector<std::pair<std::string, std::string>>* out_id_version,
    std::vector<std::string>&                         out_notfound,
    std::vector<std::string>&                         out_unauthorized)
{
    if (!m_backend || !out_id_version) return -1;
    out_id_version->clear();

    std::vector<std::string> hidden;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        hidden = m_hidden_bundles;
    }

    std::vector<RemoteFileInfo> entries;
    std::string err;
    if (!m_backend->list_files(make_bundles_prefix(m_backend.get()), entries, err)) {
        BOOST_LOG_TRIVIAL(debug) << "[" << provider_id()
            << "] list bundles failed (probably empty): " << err;
        return 0; // empty is not an error
    }

    for (const auto& e : entries) {
        if (!e.is_directory) continue;
        // Last path segment is the bundle id.
        auto pos = e.path.find_last_of('/');
        std::string id = (pos == std::string::npos) ? e.path : e.path.substr(pos + 1);
        if (id.empty()) continue;
        if (std::find(hidden.begin(), hidden.end(), id) != hidden.end()) continue;

        std::string meta_path = make_bundles_prefix(m_backend.get()) + id + "/bundle_metadata.json";
        std::string body;
        RemoteFileInfo info;
        std::string dl_err;
        SyncError sync_err = SyncError::None;
        if (!m_backend->download_file(meta_path, body, info, dl_err, &sync_err)) {
            if (sync_err == SyncError::NotFound)        out_notfound.push_back(id);
            else if (sync_err == SyncError::AuthFailed) out_unauthorized.push_back(id);
            continue;
        }
        BundleMetadata meta;
        if (!bundle_metadata_from_json(body, meta)) continue;
        out_id_version->emplace_back(id, meta.version);
    }
    return 0;
}

int BaseFileSyncProvider::fetch_bundle(
    const std::string& bundle_id,
    const std::string& /*version*/, // file backends store only the current version
    std::map<std::string, std::map<std::string, std::string>>* out_presets,
    BundleMetadata*    out_metadata)
{
    if (!m_backend || bundle_id.empty()) return -1;

    std::string base = make_bundles_prefix(m_backend.get()) + bundle_id + "/";

    if (out_metadata) {
        std::string body;
        RemoteFileInfo info;
        std::string err;
        if (!m_backend->download_file(base + "bundle_metadata.json", body, info, err)) {
            BOOST_LOG_TRIVIAL(warning) << "[" << provider_id()
                << "] fetch_bundle: metadata missing for " << bundle_id << ": " << err;
            return -1;
        }
        if (!bundle_metadata_from_json(body, *out_metadata)) return -1;
        out_metadata->id = bundle_id;
    }

    if (!out_presets) return 0;

    static const char* types[] = { "print", "filament", "printer" };
    for (const char* type : types) {
        std::vector<RemoteFileInfo> entries;
        std::string err;
        if (!m_backend->list_files(base + type, entries, err)) continue;
        for (const auto& e : entries) {
            if (e.is_directory) continue;
            std::string filename;
            auto pos = e.path.find_last_of('/');
            filename = (pos == std::string::npos) ? e.path : e.path.substr(pos + 1);
            if (filename.size() < 6 || filename.substr(filename.size() - 5) != ".json") continue;
            std::string preset_name = filename.substr(0, filename.size() - 5);

            std::string body;
            RemoteFileInfo info;
            std::string dl_err;
            if (!m_backend->download_file(e.path, body, info, dl_err)) continue;

            try {
                auto j = nlohmann::json::parse(body);
                (*out_presets)[preset_name] = json_to_values_map(j);
            } catch (...) {}
        }
    }
    return 0;
}

int BaseFileSyncProvider::publish_local_bundle(
    const BundleMetadata&                                            metadata,
    const std::map<std::string, std::map<std::string, std::string>>& presets,
    std::string&                                                     out_published_version)
{
    if (!m_backend || metadata.id.empty()) return -1;

    std::string base = make_bundles_prefix(m_backend.get()) + metadata.id + "/";
    std::string err;
    if (!m_backend->ensure_directory(base.substr(0, base.size() - 1), err)) return -1;

    // Write metadata.
    {
        std::string body = bundle_metadata_to_json(metadata).dump(2);
        std::string new_etag;
        // No OCC on bundle publish -- author owns it.
        if (!m_backend->upload_file(base + "bundle_metadata.json", body, "", new_etag, err)) {
            BOOST_LOG_TRIVIAL(error) << "[" << provider_id()
                << "] publish_local_bundle metadata upload failed: " << err;
            return -1;
        }
    }

    // Write presets grouped by type. The orchestrator decides which preset
    // goes into which type subdir by setting its key inside `presets` as
    // "<type>/<name>" -- we honour that prefix when present, otherwise fall
    // back to "print/" so the bundle never has loose presets at the root.
    for (auto& [key, values] : presets) {
        std::string type = "print";
        std::string name = key;
        auto slash = key.find('/');
        if (slash != std::string::npos) {
            type = key.substr(0, slash);
            name = key.substr(slash + 1);
        }
        std::string type_dir = base + type;
        std::string dir_err;
        if (!m_backend->ensure_directory(type_dir, dir_err)) continue;

        std::string body = values_map_to_json(values).dump(2);
        std::string new_etag;
        std::string up_err;
        m_backend->upload_file(type_dir + "/" + name + ".json", body, "", new_etag, up_err);
    }

    out_published_version = metadata.version;
    return 0;
}

int BaseFileSyncProvider::unsubscribe_bundle(const std::string& bundle_id)
{
    if (bundle_id.empty()) return -1;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (std::find(m_hidden_bundles.begin(), m_hidden_bundles.end(), bundle_id) == m_hidden_bundles.end())
            m_hidden_bundles.push_back(bundle_id);
    }
    save_state();
    return 0;
}

} // namespace Slic3r
