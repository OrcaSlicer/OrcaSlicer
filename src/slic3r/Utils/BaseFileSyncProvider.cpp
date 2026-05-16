#include "BaseFileSyncProvider.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/PresetBundle.hpp"

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
// IBundleProvider -- default stubs. WebDAV/Git subclasses override after the
// on-disk bundle layout is settled in a follow-up commit.
// ----------------------------------------------------------------------------

int BaseFileSyncProvider::list_subscribed_bundles(
    std::vector<std::pair<std::string, std::string>>* /*out_id_version*/,
    std::vector<std::string>&                         /*out_notfound*/,
    std::vector<std::string>&                         /*out_unauthorized*/)
{
    return -1; // not yet implemented
}

int BaseFileSyncProvider::fetch_bundle(
    const std::string& /*bundle_id*/,
    const std::string& /*version*/,
    std::map<std::string, std::map<std::string, std::string>>* /*out_presets*/,
    BundleMetadata*    /*out_metadata*/)
{
    return -1;
}

int BaseFileSyncProvider::publish_local_bundle(
    const BundleMetadata&                                            /*metadata*/,
    const std::map<std::string, std::map<std::string, std::string>>& /*presets*/,
    std::string&                                                     /*out_published_version*/)
{
    return -1;
}

int BaseFileSyncProvider::unsubscribe_bundle(const std::string& /*bundle_id*/)
{
    return -1;
}

} // namespace Slic3r
