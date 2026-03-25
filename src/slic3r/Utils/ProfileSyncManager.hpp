#pragma once

#include "SyncBackend.hpp"
#include "WebDAVSync.hpp"
#include "GitSync.hpp"
#include "libslic3r/AppConfig.hpp"

#include <atomic>
#include <mutex>
#include <memory>
#include <map>
#include <functional>
#include <thread>
#include <boost/filesystem/path.hpp>

namespace Slic3r {

class PresetBundle;

struct SyncScope {
    bool sync_presets{true};
    bool sync_appconfig{false};
    bool sync_projects{false};
};

struct SyncConfig {
    bool            enabled{false};
    bool            read_only{false};
    bool            always_review_remote{false}; // show merge dialog for all remote changes
    SyncBackendType backend_type{SyncBackendType::WebDAV};
    WebDAVConfig    webdav_config;
    GitSyncConfig   git_config;
    SyncScope       scope;
    int             auto_sync_interval_seconds{300}; // 0 = manual only
};

struct SyncFileState {
    std::string remote_path;
    std::string last_etag;
    long long   last_synced_time{0};
    long long   last_local_modified_time{0};
};

struct SyncFileResult {
    bool        success{true};
    bool        remote_newer{false};
    std::string remote_content;  // populated when remote_newer == true
    std::string error;
};

using ConflictCallbackFn = std::function<SyncConflictResult(SyncConflict&)>;
using SyncExecuteFn     = std::function<void(bool manual)>;

class ProfileSyncManager {
public:
    ProfileSyncManager();
    ~ProfileSyncManager();

    // Configuration
    void              set_config(const SyncConfig& config);
    const SyncConfig& get_config() const { return m_config; }
    void              load_config_from_appconfig(const AppConfig& appconfig);
    void              save_config_to_appconfig(AppConfig& appconfig) const;

    // Lifecycle
    bool start(std::string& error_out);
    void stop();
    bool is_running() const { return m_running.load(); }

    // Manual sync trigger
    bool sync_now(std::string& error_out);

    // Conflict resolution callback (must be set before start)
    void set_conflict_callback(ConflictCallbackFn fn);

    // Sync execute callback — called by background thread to perform actual sync
    void set_sync_execute_callback(SyncExecuteFn fn) { m_sync_execute_fn = std::move(fn); }

    // Manual/auto sync mode — controls whether conflicts show modal dialogs
    void set_manual_sync(bool manual) { m_manual_sync.store(manual); }
    bool is_manual_sync() const       { return m_manual_sync.load(); }

    // Pending conflicts deferred during auto-sync
    std::vector<SyncConflict> take_pending_conflicts();
    void                      clear_pending_conflicts();

    // Status
    bool        is_syncing() const { return m_syncing.load(); }
    std::string last_sync_status() const;
    long long   last_sync_time() const;
    std::string backend_info_message() const {
        std::lock_guard<std::recursive_mutex> lock(m_sync_mutex);
        return m_backend ? m_backend->info_message() : std::string();
    }

    // Access to the sync backend (e.g. for Git commit_and_push after conflict resolution)
    SyncBackend* get_backend() const { return m_backend.get(); }

    // Sync operations (can be called directly or from background thread)
    bool sync_presets(PresetBundle& bundle, std::string& error_out);
    bool sync_appconfig(AppConfig& appconfig, std::string& error_out);
    bool sync_projects(const std::vector<std::string>& project_paths, std::string& error_out);

    // Apply a previously deferred conflict resolution (called from GUI thread)
    bool apply_conflict_resolution(const SyncConflict& conflict,
                                   const SyncConflictResult& result,
                                   std::string& error_out);

    // Tracks whether sync wrote any local files (for conditional reload)
    bool had_local_changes() const        { return m_had_local_changes.load(); }
    void reset_local_changes_flag()       { m_had_local_changes.store(false); }

private:
    SyncConfig                            m_config;
    std::unique_ptr<SyncBackend>          m_backend;
    std::map<std::string, SyncFileState>  m_file_states;
    ConflictCallbackFn                    m_conflict_fn;
    SyncExecuteFn                         m_sync_execute_fn;

    std::unique_ptr<std::thread>          m_sync_thread;
    std::atomic_bool                      m_running{false};
    std::atomic_bool                      m_syncing{false};
    std::atomic_bool                      m_sync_requested{false};
    std::atomic_bool                      m_manual_sync{false};
    std::atomic_bool                      m_had_local_changes{false};

    std::vector<SyncConflict>             m_pending_conflicts;
    std::mutex                            m_pending_mutex;

    std::string   m_last_status;
    long long     m_last_sync_time{0};
    mutable std::mutex m_mutex;

    // Protects m_backend and m_file_states from concurrent access
    // across sync thread, manual sync, and GUI queries.
    // Recursive because sync_presets() → sync_single_file() → m_backend.
    mutable std::recursive_mutex m_sync_mutex;

    // State persistence
    void load_sync_state();
    void save_sync_state();
    std::string sync_state_file() const;

    // Internal
    std::unique_ptr<SyncBackend> create_backend();
    void                         sync_thread_fn();
    bool                         ensure_remote_dirs(std::string& error_out);

    SyncFileResult sync_single_file(const std::string& remote_path,
                                    const std::string& local_filepath,
                                    const std::string& local_content,
                                    long long local_modified_time,
                                    Preset::Type preset_type);

    SyncConflictResult resolve_conflict(const std::string& remote_path,
                                    const std::string& local_filepath,
                                    const std::string& local_content,
                                    long long local_time,
                                    const std::string& remote_content,
                                    long long remote_time,
                                    const std::string& remote_etag,
                                    Preset::Type preset_type);

    // Deletion sync handlers
    // Returns true if the caller should erase this entry from m_file_states
    bool handle_remote_deletion(const std::string& remote_path,
                                const std::string& preset_name,
                                const boost::filesystem::path& local_filepath,
                                Preset::Type preset_type);

    // Returns true if the caller should erase this entry from m_file_states
    bool handle_local_deletion(const std::string& remote_path,
                               const std::string& preset_name,
                               const RemoteFileInfo& remote_info,
                               Preset::Type preset_type);

    // Preset type to directory name mapping
    static std::string preset_type_dir(Preset::Type preset_type);

    // Remote path prefix: empty for Git (files at repo root), "orcaslicer-sync/" for WebDAV
    std::string remote_prefix() const;
};

} // namespace Slic3r
