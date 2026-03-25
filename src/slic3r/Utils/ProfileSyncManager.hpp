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
    std::string backend_info_message() const { return m_backend ? m_backend->info_message() : std::string(); }

    // Sync operations (can be called directly or from background thread)
    bool sync_presets(PresetBundle& bundle, std::string& error_out);
    bool sync_appconfig(AppConfig& appconfig, std::string& error_out);
    bool sync_projects(const std::vector<std::string>& project_paths, std::string& error_out);

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

    std::vector<SyncConflict>             m_pending_conflicts;
    std::mutex                            m_pending_mutex;

    std::string   m_last_status;
    long long     m_last_sync_time{0};
    mutable std::mutex m_mutex;

    // State persistence
    void load_sync_state();
    void save_sync_state();
    std::string sync_state_file() const;

    // Internal
    std::unique_ptr<SyncBackend> create_backend();
    void                         sync_thread_fn();
    bool                         ensure_remote_dirs(std::string& error_out);

    SyncFileResult sync_single_file(const std::string& remote_path,
                                    const std::string& local_content,
                                    long long local_modified_time,
                                    int preset_type);

    SyncConflictResult resolve_conflict(const std::string& remote_path,
                                    const std::string& local_content,
                                    long long local_time,
                                    const std::string& remote_content,
                                    long long remote_time,
                                    const std::string& remote_etag,
                                    int preset_type);

    // Preset type to directory name mapping
    static std::string preset_type_dir(int preset_type);
};

} // namespace Slic3r
