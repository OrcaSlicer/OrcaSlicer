#include "ProfileSyncManager.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string.hpp>

#include <fstream>
#include <sstream>
#include <chrono>
#include <set>

namespace fs = boost::filesystem;
using json = nlohmann::json;

namespace Slic3r {

ProfileSyncManager::ProfileSyncManager() = default;

ProfileSyncManager::~ProfileSyncManager()
{
    stop();
}

std::string ProfileSyncManager::preset_type_dir(Preset::Type preset_type)
{
    switch (preset_type) {
    case Preset::TYPE_PRINT:        return "process";
    case Preset::TYPE_FILAMENT:     return "filament";
    case Preset::TYPE_PRINTER:      return "machine";
    case Preset::TYPE_SLA_PRINT:    return "sla_print";
    case Preset::TYPE_SLA_MATERIAL: return "sla_material";
    default:                        return "unknown";
    }
}

std::string ProfileSyncManager::sync_state_file() const
{
    return (fs::path(Slic3r::data_dir()) / "profile_sync_state.json").string();
}

void ProfileSyncManager::load_sync_state()
{
    std::lock_guard<std::recursive_mutex> lock(m_sync_mutex);
    std::string path = sync_state_file();
    if (!fs::exists(path))
        return;

    try {
        std::ifstream f(path);
        json j;
        f >> j;

        // Invalidate state if sync target changed (different repo/branch/URL)
        std::string saved_fp = j.value("config_fingerprint", "");
        std::string current_fp = m_backend->fingerprint();
        if (!saved_fp.empty() && saved_fp != current_fp) {
            BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: sync target changed, resetting state";
            m_file_states.clear();
            m_last_sync_time = 0;
            return;
        }

        if (j.contains("files") && j["files"].is_object()) {
            for (auto& [key, val] : j["files"].items()) {
                SyncFileState state;
                state.remote_path             = key;
                state.last_etag               = val.value("last_etag", "");
                state.last_synced_time        = val.value("last_synced_time", 0LL);
                state.last_local_modified_time = val.value("last_local_modified_time", 0LL);
                m_file_states[key] = state;
            }
        }
        if (j.contains("last_sync_time"))
            m_last_sync_time = j["last_sync_time"].get<long long>();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to load sync state: " << e.what();
    }
}

void ProfileSyncManager::save_sync_state()
{
    std::lock_guard<std::recursive_mutex> lock(m_sync_mutex);
    try {
        json j;
        j["version"] = 1;
        j["config_fingerprint"] = m_backend ? m_backend->fingerprint() : "";
        j["last_sync_time"] = m_last_sync_time;

        json files = json::object();
        for (const auto& [path, state] : m_file_states) {
            files[path] = {
                {"last_etag",               state.last_etag},
                {"last_synced_time",        state.last_synced_time},
                {"last_local_modified_time", state.last_local_modified_time}
            };
        }
        j["files"] = files;

        std::string path = sync_state_file();
        std::ofstream f(path);
        f << j.dump(2);
        f.close();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to save sync state: " << e.what();
    }
}

void ProfileSyncManager::set_config(const SyncConfig& config)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
}

void ProfileSyncManager::load_config_from_appconfig(const AppConfig& appconfig)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string backend_str = appconfig.get("selfhost_sync_backend");
    if (backend_str == "1") {
        m_config.enabled      = true;
        m_config.backend_type = SyncBackendType::WebDAV;
    } else if (backend_str == "2") {
        m_config.enabled      = true;
        m_config.backend_type = SyncBackendType::Git;
    } else {
        m_config.enabled = false;
    }

    // WebDAV config
    m_config.webdav_config.url      = appconfig.get("selfhost_sync_webdav_url");
    m_config.webdav_config.username = appconfig.get("selfhost_sync_webdav_user");
    m_config.webdav_config.password = appconfig.get("selfhost_sync_webdav_pass");

    // Git config
    m_config.git_config.repo_url = appconfig.get("selfhost_sync_git_url");
    m_config.git_config.branch   = appconfig.get("selfhost_sync_git_branch");
    if (m_config.git_config.branch.empty())
        m_config.git_config.branch = "main";
    m_config.git_config.username = appconfig.get("selfhost_sync_git_user");
    m_config.git_config.token = appconfig.get("selfhost_sync_git_token");
    m_config.git_config.local_clone_path = (fs::path(Slic3r::data_dir()) / "sync_git").string();

    // Use get_bool() to handle both "true"/"1" formats (set_bool saves "true"/"false")
    m_config.read_only = appconfig.get_bool("selfhost_sync_readonly");
    m_config.always_review_remote = appconfig.get_bool("selfhost_sync_always_review");

    // Scope
    m_config.scope.sync_presets   = appconfig.get_bool("selfhost_sync_presets");
    m_config.scope.sync_appconfig = appconfig.get_bool("selfhost_sync_appconfig");
    m_config.scope.sync_projects  = appconfig.get_bool("selfhost_sync_projects");

    // Interval
    std::string interval_str = appconfig.get("selfhost_sync_interval");
    if (interval_str == "1")
        m_config.auto_sync_interval_seconds = 300;   // 5 min
    else if (interval_str == "2")
        m_config.auto_sync_interval_seconds = 900;   // 15 min
    else if (interval_str == "3")
        m_config.auto_sync_interval_seconds = 1800;  // 30 min
    else if (interval_str == "4")
        m_config.auto_sync_interval_seconds = 3600;  // 1 hour
    else
        m_config.auto_sync_interval_seconds = 0;     // manual
}

void ProfileSyncManager::save_config_to_appconfig(AppConfig& appconfig) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_config.enabled)
        appconfig.set("selfhost_sync_backend", "0");
    else if (m_config.backend_type == SyncBackendType::WebDAV)
        appconfig.set("selfhost_sync_backend", "1");
    else
        appconfig.set("selfhost_sync_backend", "2");

    appconfig.set("selfhost_sync_webdav_url",  m_config.webdav_config.url);
    appconfig.set("selfhost_sync_webdav_user", m_config.webdav_config.username);
    appconfig.set("selfhost_sync_webdav_pass", m_config.webdav_config.password);

    appconfig.set("selfhost_sync_git_url",    m_config.git_config.repo_url);
    appconfig.set("selfhost_sync_git_branch", m_config.git_config.branch);
    appconfig.set("selfhost_sync_git_user",   m_config.git_config.username);
    appconfig.set("selfhost_sync_git_token",  m_config.git_config.token);

    appconfig.set("selfhost_sync_readonly",       m_config.read_only              ? "1" : "0");
    appconfig.set("selfhost_sync_always_review", m_config.always_review_remote   ? "1" : "0");
    appconfig.set("selfhost_sync_presets",        m_config.scope.sync_presets     ? "1" : "0");
    appconfig.set("selfhost_sync_appconfig", m_config.scope.sync_appconfig ? "1" : "0");
    appconfig.set("selfhost_sync_projects",  m_config.scope.sync_projects  ? "1" : "0");

    int interval_idx = 0;
    if (m_config.auto_sync_interval_seconds == 300)  interval_idx = 1;
    else if (m_config.auto_sync_interval_seconds == 900)  interval_idx = 2;
    else if (m_config.auto_sync_interval_seconds == 1800) interval_idx = 3;
    else if (m_config.auto_sync_interval_seconds == 3600) interval_idx = 4;
    appconfig.set("selfhost_sync_interval", std::to_string(interval_idx));
}

std::unique_ptr<SyncBackend> ProfileSyncManager::create_backend()
{
    if (m_config.backend_type == SyncBackendType::WebDAV) {
        return std::make_unique<WebDAVSync>(m_config.webdav_config);
    } else {
        return std::make_unique<GitSync>(m_config.git_config);
    }
}

bool ProfileSyncManager::start(std::string& error_out)
{
    if (m_running.load())
        return true;

    if (!m_config.enabled) {
        error_out = "Sync is disabled";
        return false;
    }

    // Validate required fields before creating backend
    if (m_config.backend_type == SyncBackendType::WebDAV) {
        if (m_config.webdav_config.url.empty()) {
            error_out = "WebDAV URL is not configured";
            return false;
        }
    } else {
        if (m_config.git_config.repo_url.empty()) {
            error_out = "Git repository URL is not configured";
            return false;
        }
    }

    {
        std::lock_guard<std::recursive_mutex> lock(m_sync_mutex);
        m_backend = create_backend();
        load_sync_state();

        if (!m_backend->connect(error_out))
            return false;
    }

    if (m_config.auto_sync_interval_seconds > 0) {
        m_running.store(true);
        m_sync_thread = std::make_unique<std::thread>(&ProfileSyncManager::sync_thread_fn, this);
    }

    return true;
}

void ProfileSyncManager::stop()
{
    m_running.store(false);
    if (m_sync_thread && m_sync_thread->joinable()) {
        m_sync_thread->join();
    }
    m_sync_thread.reset();

    {
        std::lock_guard<std::recursive_mutex> lock(m_sync_mutex);
        if (m_backend) {
            m_backend->disconnect();
            m_backend.reset();
        }
        save_sync_state();
    }
}

bool ProfileSyncManager::sync_now(std::string& error_out)
{
    if (!m_config.enabled) {
        error_out = "Sync is disabled";
        return false;
    }

    // If background thread is running, just signal it
    if (m_running.load()) {
        m_sync_requested.store(true);
        return true;
    }

    // Otherwise, do inline sync
    {
        std::lock_guard<std::recursive_mutex> lock(m_sync_mutex);
        if (!m_backend) {
            m_backend = create_backend();
            load_sync_state();
            if (!m_backend->connect(error_out))
                return false;
        }
    }

    m_syncing.store(true);

    // Pull latest remote changes before sync (git fetch+merge / WebDAV no-op)
    {
        std::lock_guard<std::recursive_mutex> lock(m_sync_mutex);
        if (m_backend && !m_backend->refresh(error_out)) {
            BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: refresh failed: " << error_out;
        }
    }

    bool ok = m_config.read_only ? true : ensure_remote_dirs(error_out);
    // Note: actual preset sync requires a PresetBundle reference,
    // which will be provided by the GUI_App integration layer.
    // This method is primarily for manual trigger signaling.

    m_syncing.store(false);
    return ok;
}

void ProfileSyncManager::set_conflict_callback(ConflictCallbackFn fn)
{
    m_conflict_fn = std::move(fn);
}

std::string ProfileSyncManager::last_sync_status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_status;
}

long long ProfileSyncManager::last_sync_time() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_sync_time;
}

std::string ProfileSyncManager::remote_prefix() const
{
    if (m_backend)
        return m_backend->remote_prefix();
    return "";
}

bool ProfileSyncManager::ensure_remote_dirs(std::string& error_out)
{
    std::lock_guard<std::recursive_mutex> lock(m_sync_mutex);
    if (!m_backend) return false;

    std::string pfx = remote_prefix();

    // For WebDAV, create the root namespace directory first
    if (!pfx.empty()) {
        std::string root = pfx;
        if (root.back() == '/') root.pop_back();
        if (!m_backend->ensure_directory(root, error_out))
            return false;
    }

    const std::vector<std::string> dirs = {
        pfx + "presets",
        pfx + "presets/machine",
        pfx + "presets/filament",
        pfx + "presets/process",
        pfx + "config",
        pfx + "projects"
    };

    for (const auto& dir : dirs) {
        if (!m_backend->ensure_directory(dir, error_out))
            return false;
    }
    return true;
}

std::vector<SyncConflict> ProfileSyncManager::take_pending_conflicts()
{
    std::lock_guard<std::mutex> lock(m_pending_mutex);
    std::vector<SyncConflict> result;
    result.swap(m_pending_conflicts);
    return result;
}

void ProfileSyncManager::clear_pending_conflicts()
{
    std::lock_guard<std::mutex> lock(m_pending_mutex);
    m_pending_conflicts.clear();
}

SyncConflictResult ProfileSyncManager::resolve_conflict(const std::string& remote_path,
                                                        const std::string& local_filepath,
                                                        const std::string& local_content,
                                                        long long local_time,
                                                        const std::string& remote_content,
                                                        long long remote_time,
                                                        const std::string& remote_etag,
                                                        Preset::Type preset_type)
{
    // During auto-sync, don't block with a modal dialog — queue the conflict
    if (!m_manual_sync.load()) {
        {
            std::lock_guard<std::mutex> lock(m_pending_mutex);
            SyncConflict deferred;
            deferred.path           = remote_path;
            deferred.local_filepath = local_filepath;
            deferred.local_content  = local_content;
            deferred.local_time     = local_time;
            deferred.remote_content = remote_content;
            deferred.remote_time    = remote_time;
            deferred.remote_etag    = remote_etag;
            deferred.preset_type    = preset_type;
            m_pending_conflicts.push_back(std::move(deferred));
        }
        BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: deferred conflict for " << remote_path << " (auto-sync)";
        SyncConflictResult result;
        result.resolution = ConflictResolution::Skip;
        return result;
    }

    if (m_conflict_fn) {
        SyncConflict conflict;
        conflict.path           = remote_path;
        conflict.local_filepath = local_filepath;
        conflict.local_content  = local_content;
        conflict.local_time     = local_time;
        conflict.remote_content = remote_content;
        conflict.remote_time    = remote_time;
        conflict.remote_etag    = remote_etag;
        conflict.preset_type    = preset_type;
        try {
            return m_conflict_fn(conflict);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: conflict callback threw: " << e.what();
        }
    }
    // Default: newer wins
    SyncConflictResult result;
    result.resolution = (local_time >= remote_time) ? ConflictResolution::KeepLocal : ConflictResolution::KeepRemote;
    return result;
}

SyncFileResult ProfileSyncManager::sync_single_file(const std::string& remote_path,
                                                     const std::string& local_filepath,
                                                     const std::string& local_content,
                                                     long long local_modified_time,
                                                     Preset::Type preset_type)
{
    std::lock_guard<std::recursive_mutex> lock(m_sync_mutex);
    if (!m_backend)
        return {false, false, "", "No backend"};

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Git without token can only read (no push credentials)
    bool effective_read_only = m_config.read_only;
    if (m_config.backend_type == SyncBackendType::Git && m_config.git_config.token.empty())
        effective_read_only = true;

    BOOST_LOG_TRIVIAL(debug) << "ProfileSyncManager::sync_single_file: " << remote_path
                             << " read_only=" << effective_read_only
                             << " manual=" << m_manual_sync.load()
                             << " always_review=" << m_config.always_review_remote;

    // Read-only mode: only download, never upload
    if (effective_read_only) {
        RemoteFileInfo remote_info;
        std::string    remote_content;
        std::string    dl_error;
        SyncError      err_code = SyncError::None;

        if (!m_backend->download_file(remote_path, remote_content, remote_info, dl_error, &err_code)) {
            // File doesn't exist remotely or permission error — skip silently
            return {};
        }

        // Check if remote changed since last sync
        auto it = m_file_states.find(remote_path);
        if (it != m_file_states.end() && remote_info.etag == it->second.last_etag)
            return {}; // No changes

        // Content identical — no dialog or file write needed
        if (remote_content == local_content) {
            SyncFileState& state = m_file_states[remote_path];
            state.remote_path              = remote_path;
            state.last_etag                = remote_info.etag;
            state.last_synced_time         = now;
            state.last_local_modified_time = local_modified_time;
            return {};
        }

        // If always_review_remote and this is a preset, show merge dialog
        if (m_config.always_review_remote && preset_type != Preset::Type::TYPE_INVALID) {
            auto cr = resolve_conflict(remote_path, local_filepath, local_content, local_modified_time,
                                       remote_content, remote_info.modified_time,
                                       remote_info.etag, preset_type);
            switch (cr.resolution) {
            case ConflictResolution::Skip:
                // Don't update state — re-detect next cycle
                return {};
            case ConflictResolution::KeepLocal:
                return {}; // Read-only: keeping local means do nothing
            case ConflictResolution::Merge: {
                SyncFileState& state = m_file_states[remote_path];
                state.remote_path              = remote_path;
                state.last_etag                = remote_info.etag;
                state.last_synced_time         = now;
                state.last_local_modified_time = local_modified_time;
                return {true, true, cr.merged_content, ""};
            }
            case ConflictResolution::KeepRemote:
                break; // Fall through to accept remote
            }
        }

        {
            SyncFileState& state = m_file_states[remote_path];
            state.remote_path              = remote_path;
            state.last_etag                = remote_info.etag;
            state.last_synced_time         = now;
            state.last_local_modified_time = local_modified_time;
        }
        return {true, true, remote_content, ""};
    }

    // Check if remote file exists
    RemoteFileInfo remote_info;
    std::string    remote_content;
    std::string    dl_error;
    SyncError      err_code = SyncError::None;
    bool           remote_exists = false;

    if (m_backend->download_file(remote_path, remote_content, remote_info, dl_error, &err_code)) {
        remote_exists = true;
    } else if (err_code == SyncError::NotFound) {
        remote_exists = false;
    } else {
        BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: download_file failed: " << dl_error;
        return {false, false, "", dl_error};
    }

    BOOST_LOG_TRIVIAL(debug) << "ProfileSyncManager: " << remote_path
                             << " remote_exists=" << remote_exists
                             << " remote_size=" << remote_content.size()
                             << " local_size=" << local_content.size()
                             << " content_equal=" << (remote_content == local_content);

    auto it = m_file_states.find(remote_path);
    SyncFileState& state = m_file_states[remote_path];

    if (!remote_exists) {
        BOOST_LOG_TRIVIAL(debug) << "ProfileSyncManager: uploading new file " << remote_path;
        // Upload new file
        std::string new_etag;
        std::string ul_error;
        if (!m_backend->upload_file(remote_path, local_content, "", new_etag, ul_error))
            return {false, false, "", ul_error};

        state.remote_path              = remote_path;
        state.last_etag                = new_etag;
        state.last_synced_time         = now;
        state.last_local_modified_time = local_modified_time;
        return {};
    }

    // Both exist — check for changes
    bool has_state = (it != m_file_states.end());
    bool local_changed  = !has_state || (local_modified_time > state.last_local_modified_time);
    bool remote_changed = !has_state || (remote_info.etag != state.last_etag);

    BOOST_LOG_TRIVIAL(debug) << "ProfileSyncManager: " << remote_path
                             << " has_state=" << has_state
                             << " local_changed=" << local_changed
                             << " remote_changed=" << remote_changed;

    if (!local_changed && !remote_changed)
        return {};

    if (local_changed && !remote_changed) {
        // Local is newer, upload
        std::string new_etag;
        std::string ul_error;
        SyncError   ul_err_code = SyncError::None;
        if (!m_backend->upload_file(remote_path, local_content, state.last_etag, new_etag, ul_error, &ul_err_code)) {
            if (ul_err_code == SyncError::Conflict) {
                // Race condition: remote was also updated. Treat as conflict.
                remote_changed = true;
                // Fall through to conflict resolution
            } else {
                return {false, false, "", ul_error};
            }
        } else {
            state.last_etag                = new_etag;
            state.last_synced_time         = now;
            state.last_local_modified_time = local_modified_time;
            return {};
        }
    }

    if (!local_changed && remote_changed) {
        // Remote is newer — content already downloaded above

        // If always_review_remote and this is a preset, let user review
        if (m_config.always_review_remote && preset_type != Preset::Type::TYPE_INVALID && remote_content != local_content) {
            auto cr = resolve_conflict(remote_path, local_filepath, local_content, local_modified_time,
                                       remote_content, remote_info.modified_time,
                                       remote_info.etag, preset_type);
            switch (cr.resolution) {
            case ConflictResolution::Skip:
                // Don't update state — so the change is re-detected next cycle
                return {};
            case ConflictResolution::KeepLocal: {
                std::string new_etag;
                std::string ul_error;
                if (!m_backend->upload_file(remote_path, local_content, "", new_etag, ul_error))
                    return {false, false, "", ul_error};
                state.last_etag                = new_etag;
                state.last_synced_time         = now;
                state.last_local_modified_time = local_modified_time;
                return {};
            }
            case ConflictResolution::Merge: {
                std::string new_etag;
                std::string ul_error;
                if (!m_backend->upload_file(remote_path, cr.merged_content, "", new_etag, ul_error))
                    return {false, false, "", ul_error};
                state.last_etag        = new_etag;
                state.last_synced_time = now;
                return {true, true, cr.merged_content, ""};
            }
            case ConflictResolution::KeepRemote:
                break; // Fall through to accept remote
            }
        }

        state.last_etag                = remote_info.etag;
        state.last_synced_time         = now;
        state.last_local_modified_time = local_modified_time;
        return {true, true, remote_content, ""};
    }

    // Both changed — conflict
    auto cr = resolve_conflict(
        remote_path, local_filepath, local_content, local_modified_time,
        remote_content, remote_info.modified_time, remote_info.etag, preset_type);

    switch (cr.resolution) {
    case ConflictResolution::KeepLocal: {
        std::string new_etag;
        std::string ul_error;
        if (!m_backend->upload_file(remote_path, local_content, "", new_etag, ul_error))
            return {false, false, "", ul_error};
        state.last_etag                = new_etag;
        state.last_synced_time         = now;
        state.last_local_modified_time = local_modified_time;
        return {};
    }
    case ConflictResolution::KeepRemote: {
        state.last_etag                = remote_info.etag;
        state.last_synced_time         = now;
        state.last_local_modified_time = local_modified_time;
        return {true, true, remote_content, ""};
    }
    case ConflictResolution::Merge: {
        std::string new_etag;
        std::string ul_error;
        if (!m_backend->upload_file(remote_path, cr.merged_content, "", new_etag, ul_error))
            return {false, false, "", ul_error};
        state.last_etag                = new_etag;
        state.last_synced_time         = now;
        state.last_local_modified_time = local_modified_time;
        return {true, true, cr.merged_content, ""};
    }
    case ConflictResolution::Skip:
        return {};
    }

    return {};
}

bool ProfileSyncManager::sync_presets(PresetBundle& bundle, std::string& error_out)
{
    std::lock_guard<std::recursive_mutex> lock(m_sync_mutex);
    if (!m_backend || !m_config.scope.sync_presets)
        return true;

    BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: syncing presets...";

    struct PresetCollectionRef {
        PresetCollection& collection;
        Preset::Type      type;
        std::string       dir;
    };

    std::vector<PresetCollectionRef> collections = {
        {bundle.prints,    Preset::TYPE_PRINT,    "process"},
        {bundle.filaments, Preset::TYPE_FILAMENT,  "filament"},
        {bundle.printers,  Preset::TYPE_PRINTER,   "machine"},
    };

    for (auto& ref : collections) {
        // Get list of remote files for this type
        std::string remote_dir = remote_prefix() + "presets/" + ref.dir;
        std::vector<RemoteFileInfo> remote_files;
        if (!m_backend->list_files(remote_dir, remote_files, error_out)) {
            BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to list " << remote_dir << ": " << error_out;
            continue; // Try other types
        }

        // Build a set of remote file names (without .json extension)
        std::map<std::string, RemoteFileInfo> remote_map;
        for (const auto& rf : remote_files) {
            if (rf.is_directory) continue;
            // Extract filename from path
            fs::path p(rf.path);
            std::string name = p.stem().string();
            if (p.extension() == ".json")
                remote_map[name] = rf;
        }

        // Scan local user preset directory for all .json files
        // (get_user_presets() filters out presets without base_id, which
        //  excludes virtually all local presets inheriting from system ones)
        fs::path user_dir = fs::path(Slic3r::data_dir()) / "user" / "default" / ref.dir;
        if (!fs::exists(user_dir))
            continue;

        std::map<std::string, fs::path> local_files;
        for (const auto& entry : fs::directory_iterator(user_dir)) {
            if (!fs::is_regular_file(entry) || entry.path().extension() != ".json")
                continue;
            local_files[entry.path().stem().string()] = entry.path();
        }

        BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: found " << local_files.size()
                                << " local presets in " << ref.dir;

        // Phase 2 — Detect deletions via m_file_states
        {
            std::string prefix = remote_dir + "/";
            std::vector<std::string> states_to_erase;
            for (const auto& [state_path, state] : m_file_states) {
                if (state_path.rfind(prefix, 0) != 0)
                    continue; // not this collection type

                // Extract preset name from remote_path
                fs::path p(state_path);
                std::string name = p.stem().string();

                bool in_local  = local_files.count(name) > 0;
                bool in_remote = remote_map.count(name) > 0;

                if (!in_local && !in_remote) {
                    // Both sides deleted — clean up state
                    BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: preset deleted on both sides: " << name;
                    states_to_erase.push_back(state_path);
                } else if (in_local && !in_remote) {
                    // Remote deleted
                    if (handle_remote_deletion(state_path, name, local_files[name], ref.type))
                        states_to_erase.push_back(state_path);
                    local_files.erase(name);  // don't re-upload in Phase 3
                } else if (!in_local && in_remote) {
                    // Local deleted
                    if (handle_local_deletion(state_path, name, remote_map[name], ref.type))
                        states_to_erase.push_back(state_path);
                    if (!m_config.read_only)
                        remote_map.erase(name);  // don't re-download in Phase 3
                    // In read-only mode, leave in remote_map so Phase 3 re-downloads
                }
            }
            for (const auto& key : states_to_erase)
                m_file_states.erase(key);
        }

        for (const auto& [name, filepath] : local_files) {
            std::string remote_path = remote_dir + "/" + name + ".json";

            std::string local_content;
            try {
                std::ifstream f(filepath.string(), std::ios::binary);
                std::ostringstream ss;
                ss << f.rdbuf();
                local_content = ss.str();
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(warning) << "ProfileSyncManager: failed to read " << filepath << ": " << e.what();
                continue;
            }

            if (local_content.empty())
                continue;

            long long local_mtime = static_cast<long long>(fs::last_write_time(filepath));

            auto sfr = sync_single_file(remote_path, filepath.string(), local_content, local_mtime, ref.type);

            if (!sfr.success) {
                BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to sync preset " << name << ": " << sfr.error;
            } else if (sfr.remote_newer) {
                try {
                    std::ofstream f(filepath.string(), std::ios::binary | std::ios::trunc);
                    f << sfr.remote_content;
                    f.close();
                    m_had_local_changes.store(true);
                    BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: updated local preset from remote: " << name;
                } catch (const std::exception& e) {
                    BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to write preset: " << e.what();
                }
            }

            remote_map.erase(name);
        }

        // Download remote presets that don't exist locally
        for (const auto& [name, remote_info] : remote_map) {
            std::string remote_path = remote_dir + "/" + name + ".json";
            std::string content;
            RemoteFileInfo info;
            std::string dl_error;

            if (!m_backend->download_file(remote_path, content, info, dl_error)) {
                BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to download " << remote_path << ": " << dl_error;
                continue;
            }

            // Determine local file path
            std::string user_dir = (fs::path(Slic3r::data_dir()) / "user" / "default" / ref.dir).string();
            fs::create_directories(user_dir);
            std::string local_file = (fs::path(user_dir) / (name + ".json")).string();

            try {
                std::ofstream f(local_file, std::ios::binary | std::ios::trunc);
                f << content;
                f.close();
                m_had_local_changes.store(true);
                BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: downloaded new preset from remote: " << name;
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to save downloaded preset: " << e.what();
            }

            // Update sync state
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            SyncFileState& state         = m_file_states[remote_path];
            state.remote_path            = remote_path;
            state.last_etag              = info.etag;
            state.last_synced_time       = now;
            state.last_local_modified_time = 0;
        }
    }

    // Handle Git: batch commit after syncing all presets
    if (m_config.backend_type == SyncBackendType::Git) {
        auto* git_backend = dynamic_cast<GitSync*>(m_backend.get());
        if (git_backend) {
            std::string commit_error;
            if (!git_backend->commit_and_push("OrcaSlicer profile sync", commit_error)) {
                BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: git commit/push failed: " << commit_error;
            }
        }
    }

    save_sync_state();
    return true;
}

// ---------------------------------------------------------------------------
// Deletion sync handlers
// ---------------------------------------------------------------------------

bool ProfileSyncManager::handle_remote_deletion(const std::string& remote_path,
                                                 const std::string& preset_name,
                                                 const fs::path& local_filepath,
                                                 Preset::Type preset_type)
{
    BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: remote deleted preset: " << preset_name;

    auto state_it = m_file_states.find(remote_path);
    if (state_it == m_file_states.end())
        return false;

    // Read local file to check if it was modified since last sync
    long long local_mtime = static_cast<long long>(fs::last_write_time(local_filepath));
    bool local_changed = local_mtime > state_it->second.last_local_modified_time;

    if (m_config.read_only || !local_changed) {
        // Accept remote deletion — remove local file
        boost::system::error_code ec;
        fs::remove(local_filepath, ec);
        if (ec)
            BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to remove " << local_filepath << ": " << ec.message();
        else {
            m_had_local_changes.store(true);
            BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: deleted local preset (remote deleted): " << preset_name;
        }
        return true; // caller should erase from m_file_states
    }

    // Local was modified after last sync, but remote deleted — conflict
    std::string local_content;
    try {
        std::ifstream f(local_filepath.string(), std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        local_content = ss.str();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to read " << local_filepath;
        return false;
    }

    auto result = resolve_conflict(remote_path, local_filepath.string(), local_content, local_mtime,
                                   "", 0, "", preset_type);

    if (result.resolution == ConflictResolution::KeepLocal) {
        // Re-upload local file to remote
        std::string new_etag, error;
        if (m_backend->upload_file(remote_path, local_content, "", new_etag, error)) {
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            auto& state = m_file_states[remote_path];
            state.remote_path = remote_path;
            state.last_etag = new_etag;
            state.last_synced_time = now;
            state.last_local_modified_time = local_mtime;
            BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: re-uploaded locally modified preset: " << preset_name;
        }
        return false;
    } else if (result.resolution == ConflictResolution::KeepRemote) {
        // Accept deletion
        boost::system::error_code ec;
        fs::remove(local_filepath, ec);
        BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: accepted remote deletion: " << preset_name;
        return true; // caller should erase from m_file_states
    }
    // Skip → do nothing
    return false;
}

bool ProfileSyncManager::handle_local_deletion(const std::string& remote_path,
                                                const std::string& preset_name,
                                                const RemoteFileInfo& remote_info,
                                                Preset::Type preset_type)
{
    BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: local deleted preset: " << preset_name;

    auto state_it = m_file_states.find(remote_path);
    if (state_it == m_file_states.end())
        return false;

    if (m_config.read_only) {
        // Read-only: remote is truth, will be re-downloaded by Phase 3
        // Just clean up stale state so it gets a fresh entry on download
        return true; // caller should erase from m_file_states
    }

    bool remote_changed = remote_info.etag != state_it->second.last_etag;

    if (!remote_changed) {
        // Remote unchanged — propagate local deletion to remote
        std::string error;
        if (m_backend->delete_file(remote_path, error)) {
            BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: deleted remote preset (local deleted): " << preset_name;
        } else {
            BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to delete remote " << remote_path << ": " << error;
        }
        return true; // caller should erase from m_file_states
    }

    // Remote was modified, but local deleted — conflict
    std::string remote_content;
    RemoteFileInfo info;
    std::string dl_error;
    if (!m_backend->download_file(remote_path, remote_content, info, dl_error)) {
        BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to download " << remote_path << " for conflict: " << dl_error;
        return false;
    }

    // local_filepath not applicable (file was deleted locally), pass empty
    auto result = resolve_conflict(remote_path, "", "", 0,
                                   remote_content, remote_info.modified_time,
                                   remote_info.etag, preset_type);

    if (result.resolution == ConflictResolution::KeepLocal) {
        // User wants to delete — propagate to remote
        std::string error;
        m_backend->delete_file(remote_path, error);
        BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: deleted remote (user chose local/delete): " << preset_name;
        return true; // caller should erase from m_file_states
    } else if (result.resolution == ConflictResolution::KeepRemote) {
        // Restore locally from remote
        std::string user_dir = (fs::path(Slic3r::data_dir()) / "user" / "default"
                                / preset_type_dir(preset_type)).string();
        fs::create_directories(user_dir);
        std::string local_file = (fs::path(user_dir) / (preset_name + ".json")).string();
        try {
            std::ofstream f(local_file, std::ios::binary | std::ios::trunc);
            f << remote_content;
            f.close();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to restore " << preset_name << ": " << e.what();
            return false;
        }
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto& state = m_file_states[remote_path];
        state.remote_path = remote_path;
        state.last_etag = info.etag;
        state.last_synced_time = now;
        state.last_local_modified_time = 0;
        m_had_local_changes.store(true);
        BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: restored from remote (user chose remote/keep): " << preset_name;
        return false;
    }
    // Skip → do nothing
    return false;
}

bool ProfileSyncManager::sync_appconfig(AppConfig& appconfig, std::string& error_out)
{
    std::lock_guard<std::recursive_mutex> lock(m_sync_mutex);
    if (!m_backend || !m_config.scope.sync_appconfig)
        return true;

    BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: syncing app config...";

    // Serialize relevant AppConfig sections to JSON
    json j;
    // We sync a subset of settings that make sense across devices
    const std::vector<std::string> sync_keys = {
        "default_page", "dark_color_mode", "language",
        "use_free_camera", "reverse_mouse_wheel_zoom",
        "show_splash_screen", "show_model_mesh", "show_model_shadow",
        "auto_arrange", "auto_orient",
    };

    for (const auto& key : sync_keys) {
        std::string val = appconfig.get(key);
        if (!val.empty())
            j[key] = val;
    }

    std::string local_content = j.dump(2);
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string remote_path = remote_prefix() + "config/appconfig.json";
    // AppConfig is not a file-on-disk preset; local_filepath left empty
    auto sfr = sync_single_file(remote_path, "", local_content, now, Preset::Type::TYPE_INVALID);

    if (!sfr.success) {
        BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to sync app config: " << sfr.error;
    } else if (sfr.remote_newer) {
        // Apply remote config — only allow keys from the sync whitelist
        try {
            json remote_j = json::parse(sfr.remote_content);
            std::set<std::string> allowed_keys(sync_keys.begin(), sync_keys.end());
            for (auto& [key, val] : remote_j.items()) {
                if (val.is_string() && allowed_keys.count(key))
                    appconfig.set(key, val.get<std::string>());
            }
            m_had_local_changes.store(true);
            BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: applied remote app config";
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to parse remote config: " << e.what();
        }
    }

    if (m_config.backend_type == SyncBackendType::Git) {
        auto* git_backend = dynamic_cast<GitSync*>(m_backend.get());
        if (git_backend) {
            std::string commit_error;
            if (!git_backend->commit_and_push("OrcaSlicer config sync", commit_error)) {
                BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: git commit/push failed: " << commit_error;
            }
        }
    }

    save_sync_state();
    return true;
}

bool ProfileSyncManager::sync_projects(const std::vector<std::string>& project_paths, std::string& error_out)
{
    std::lock_guard<std::recursive_mutex> lock(m_sync_mutex);
    if (!m_backend || !m_config.scope.sync_projects)
        return true;

    BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: syncing projects...";

    for (const auto& project_path : project_paths) {
        if (!fs::exists(project_path))
            continue;

        fs::path p(project_path);
        std::string filename     = p.filename().string();
        std::string remote_path  = remote_prefix() + "projects/" + filename;

        // Read file content
        std::string content;
        try {
            std::ifstream f(project_path, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            content = ss.str();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "ProfileSyncManager: failed to read project file " << project_path << ": " << e.what();
            continue;
        }

        auto local_time = static_cast<long long>(fs::last_write_time(p));

        auto sfr = sync_single_file(remote_path, project_path, content, local_time, Preset::Type::TYPE_INVALID);

        if (!sfr.success) {
            BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to sync project " << filename << ": " << sfr.error;
        } else if (sfr.remote_newer) {
            try {
                std::ofstream f(project_path, std::ios::binary | std::ios::trunc);
                f << sfr.remote_content;
                f.close();
                BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: updated local project from remote: " << filename;
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: failed to write project: " << e.what();
            }
        }
    }

    if (m_config.backend_type == SyncBackendType::Git) {
        auto* git_backend = dynamic_cast<GitSync*>(m_backend.get());
        if (git_backend) {
            std::string commit_error;
            if (!git_backend->commit_and_push("OrcaSlicer project sync", commit_error)) {
                BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: git commit/push failed: " << commit_error;
            }
        }
    }

    save_sync_state();
    return true;
}

bool ProfileSyncManager::apply_conflict_resolution(const SyncConflict& conflict,
                                                    const SyncConflictResult& result,
                                                    std::string& error_out)
{
    std::lock_guard<std::recursive_mutex> lock(m_sync_mutex);
    if (!m_backend) {
        error_out = "No backend connected";
        return false;
    }

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    bool effective_read_only = m_config.read_only;
    if (m_config.backend_type == SyncBackendType::Git && m_config.git_config.token.empty())
        effective_read_only = true;

    // Use the local file path stored at conflict creation time
    if (conflict.local_filepath.empty()) {
        error_out = "No local file path in conflict";
        return false;
    }
    fs::path local_filepath(conflict.local_filepath);

    switch (result.resolution) {
    case ConflictResolution::KeepLocal: {
        if (!effective_read_only && !conflict.local_content.empty()) {
            std::string new_etag;
            if (!m_backend->upload_file(conflict.path, conflict.local_content, "", new_etag, error_out))
                return false;
            auto& state = m_file_states[conflict.path];
            state.remote_path = conflict.path;
            state.last_etag = new_etag;
            state.last_synced_time = now;
            state.last_local_modified_time = conflict.local_time;
        }
        save_sync_state();
        return true;
    }
    case ConflictResolution::KeepRemote: {
        fs::create_directories(local_filepath.parent_path());
        std::ofstream f(local_filepath.string(), std::ios::binary | std::ios::trunc);
        if (!f) { error_out = "Failed to open " + local_filepath.string(); return false; }
        f << conflict.remote_content;
        f.close();

        auto& state = m_file_states[conflict.path];
        state.remote_path = conflict.path;
        state.last_etag = conflict.remote_etag;
        state.last_synced_time = now;
        state.last_local_modified_time = static_cast<long long>(fs::last_write_time(local_filepath));
        save_sync_state();
        return true;
    }
    case ConflictResolution::Merge: {
        fs::create_directories(local_filepath.parent_path());
        std::ofstream f(local_filepath.string(), std::ios::binary | std::ios::trunc);
        if (!f) { error_out = "Failed to open " + local_filepath.string(); return false; }
        f << result.merged_content;
        f.close();

        if (!effective_read_only) {
            std::string new_etag;
            if (!m_backend->upload_file(conflict.path, result.merged_content, "", new_etag, error_out))
                return false;
            auto& state = m_file_states[conflict.path];
            state.last_etag = new_etag;
        } else {
            auto& state = m_file_states[conflict.path];
            state.last_etag = conflict.remote_etag;
        }
        auto& state = m_file_states[conflict.path];
        state.remote_path = conflict.path;
        state.last_synced_time = now;
        state.last_local_modified_time = static_cast<long long>(fs::last_write_time(local_filepath));
        save_sync_state();
        return true;
    }
    case ConflictResolution::Skip:
        return true;
    }
    return true;
}

void ProfileSyncManager::sync_thread_fn()
{
    BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: background sync thread started";

    while (m_running.load()) {
        // Copy config fields under lock to avoid racing with main thread
        int wait_seconds;
        bool read_only;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            wait_seconds = m_config.auto_sync_interval_seconds;
            read_only = m_config.read_only;
        }

        // Wait for interval or manual trigger
        for (int i = 0; i < wait_seconds * 10 && m_running.load(); ++i) {
            if (m_sync_requested.load())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!m_running.load())
            break;

        m_sync_requested.store(false);
        m_syncing.store(true);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_last_status = "Syncing...";
        }

        std::string error;

        // Pull latest remote changes before background sync cycle
        {
            std::lock_guard<std::recursive_mutex> lock(m_sync_mutex);
            if (m_backend && !m_backend->refresh(error)) {
                BOOST_LOG_TRIVIAL(error) << "ProfileSyncManager: refresh failed: " << error;
            }
        }

        bool ok = read_only ? true : ensure_remote_dirs(error);

        // Execute actual sync via callback (set by GUI_App, which has PresetBundle/AppConfig)
        if (ok && m_sync_execute_fn) {
            m_manual_sync.store(false);
            m_sync_execute_fn(false);
        }

        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_last_sync_time = now;
            m_last_status    = ok ? "OK" : ("Error: " + error);
        }

        m_syncing.store(false);
    }

    BOOST_LOG_TRIVIAL(info) << "ProfileSyncManager: background sync thread stopped";
}

} // namespace Slic3r
