#include "PluginBrowserProfile.hpp"

#include "PluginDescriptor.hpp"
#include "PluginFsUtils.hpp"
#include "PluginManager.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/GUI/format.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/system/error_code.hpp>

#include <cctype>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <wx/app.h>
#include <wx/string.h>
#include <wx/thread.h>
#include <wx/timer.h>

namespace Slic3r {

namespace {

namespace fs = boost::filesystem;

fs::path profiles_root()
{
    return fs::path(Slic3r::data_dir()) / "web_profiles";
}

// Same sanitizer the base WebView::isolated_profile_path uses, so the old plugin-key-named
// directory can be reported exactly.
std::string sanitize_profile_name(const std::string& profile)
{
    std::string safe;
    safe.reserve(profile.size());
    for (unsigned char ch : profile) {
        const bool keep = std::isalnum(ch) != 0 || ch == '.' || ch == '-' || ch == '_';
        safe += keep ? static_cast<char>(ch) : '_';
    }
    return safe.empty() ? std::string("default") : safe;
}

#if !defined(__APPLE__)
fs::path lease_path(const std::string& profile_id)
{
    return profiles_root() / (".lease-" + profile_id);
}
#endif

// Non-blocking cross-process lock. Deliberately not InstanceLock: a lease is held across UI
// lifetimes and may be released on a different thread.
class ProfileFileLock
{
public:
    ProfileFileLock() = default;
    ~ProfileFileLock() { close(); }
    ProfileFileLock(const ProfileFileLock&)            = delete;
    ProfileFileLock& operator=(const ProfileFileLock&) = delete;

    bool open(const std::string& path)
    {
#if defined(_WIN32)
        const std::wstring wide = boost::nowide::widen(path);
        HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        m_handle = handle;
#elif !defined(__APPLE__)
        m_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (m_fd < 0)
            return false;
        if (::flock(m_fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(m_fd);
            m_fd = -1;
            return false;
        }
#else
        (void) path; // in-memory store; no cross-process disk lease
#endif
        return true;
    }

    void close()
    {
#if defined(_WIN32)
        if (m_handle != nullptr) {
            ::CloseHandle(static_cast<HANDLE>(m_handle));
            m_handle = nullptr;
        }
#elif !defined(__APPLE__)
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
#endif
    }

private:
#if defined(_WIN32)
    void* m_handle{nullptr};
#elif !defined(__APPLE__)
    int m_fd{-1};
#endif
};

struct LeaseState
{
    ProfileFileLock lock;
};

struct LeaseEntry
{
    std::shared_ptr<LeaseState> lease;
    unsigned                    uses{0};
    bool                        releasing{false};
};

std::mutex& leases_mutex()
{
    static auto* mutex = new std::mutex;
    return *mutex;
}

std::unordered_map<std::string, LeaseEntry>& leases()
{
    static auto* map = new std::unordered_map<std::string, LeaseEntry>;
    return *map;
}

std::mutex& identities_mutex()
{
    static auto* mutex = new std::mutex;
    return *mutex;
}

std::unordered_map<std::string, PluginBrowserProfileIdentityRef>& identities()
{
    static auto* map = new std::unordered_map<std::string, PluginBrowserProfileIdentityRef>;
    return *map;
}

void push_warning_notification(const std::string& message)
{
    if (GUI::Plater* plater = GUI::wxGetApp().plater())
        plater->get_notification_manager()->push_notification(
            GUI::NotificationType::CustomNotification,
            GUI::NotificationManager::NotificationLevel::WarningNotificationLevel, message);
}

// Startup/reset warnings can be produced before the plater exists. Retry briefly instead of
// dropping the only notification the user would see.
class WarningRetryTimer : public wxTimer
{
public:
    WarningRetryTimer(std::string message, int attempts) : m_message(std::move(message)), m_attempts(attempts) {}

    void Notify() override
    {
        if (wxTheApp == nullptr || GUI::wxGetApp().is_closing())
            return; // process is going away; leave the timer to the app teardown
        if (GUI::wxGetApp().plater() != nullptr) {
            push_warning_notification(m_message);
            GUI::wxGetApp().CallAfter([this]() { delete this; });
        } else if (--m_attempts > 0) {
            StartOnce(500);
        } else {
            GUI::wxGetApp().CallAfter([this]() { delete this; });
        }
    }

private:
    std::string m_message;
    int         m_attempts;
};

void warn_profile(const std::string& plugin_key, const std::string& message)
{
    static auto* mutex    = new std::mutex;
    static auto* reported = new std::unordered_set<std::string>;
    {
        std::lock_guard<std::mutex> lock(*mutex);
        if (!reported->insert(plugin_key + '\n' + message).second)
            return;
    }
    BOOST_LOG_TRIVIAL(warning) << message;
    if (wxTheApp == nullptr)
        return;
    GUI::wxGetApp().CallAfter([message]() {
        if (wxTheApp == nullptr || GUI::wxGetApp().is_closing())
            return;
        if (GUI::wxGetApp().plater() != nullptr) {
            push_warning_notification(message);
            return;
        }
        auto* timer = new WarningRetryTimer(message, 10);
        timer->StartOnce(500);
    });
}

std::string legacy_name(const std::string& plugin_key)
{
    return plugin_key.empty() ? std::string() : sanitize_profile_name(plugin_key);
}

void delete_profile_on_ui_thread(const std::string& plugin_key, const std::string& profile_id,
                                 const std::string& installation_root, int retries_left);

void schedule_profile_delete(const std::string& plugin_key, const std::string& profile_id,
                             const std::string& installation_root, int retries_left)
{
    if (wxTheApp == nullptr)
        return;
    GUI::wxGetApp().CallAfter([plugin_key, profile_id, installation_root, retries_left]() {
        if (wxTheApp == nullptr || GUI::wxGetApp().is_closing())
            return;
        delete_profile_on_ui_thread(plugin_key, profile_id, installation_root, retries_left);
    });
}

void delete_profile_on_ui_thread(const std::string& plugin_key, const std::string& profile_id,
                                 const std::string& installation_root, int retries_left)
{
    // Never let a hand-edited sidecar turn this into a path traversal outside web_profiles.
    if (!is_uuid(profile_id))
        return;
    if (!wxIsMainThread()) {
        schedule_profile_delete(plugin_key, profile_id, installation_root, retries_left);
        return;
    }

    // Serialize with every sidecar/package writer across processes. A replacement that appears
    // at this root after the package was removed must keep its profile.
    std::unique_lock<PluginInstallStateMutex> state_lock;
    std::string                               lock_error;
    if (!try_lock_plugin_install_state(state_lock, lock_error)) {
        warn_profile(plugin_key,
                     GUI::format(_u8L("The plugin was removed, but its browser profile could not be cleaned up: %1%"),
                                 lock_error));
        return;
    }
    if (!installation_root.empty()) {
        boost::system::error_code exists_ec;
        const fs::path           root(installation_root);
        const bool               root_exists = fs::exists(root, exists_ec);
        if (exists_ec) {
            warn_profile(plugin_key,
                         GUI::format(_u8L("The plugin was removed, but its browser profile could not be checked: %1%"),
                                     exists_ec.message()));
            return;
        }
        if (root_exists) {
            warn_profile(plugin_key,
                         _u8L("The plugin was replaced by another OrcaSlicer instance, so its browser profile "
                              "was left untouched."));
            return;
        }
    }

    // If this process still has a closing view, give the GUI queue a few turns to destroy it.
    {
        std::lock_guard<std::mutex> lock(leases_mutex());
        auto                        it = leases().find(profile_id);
        if (it != leases().end()) {
            if (it->second.uses > 0 || it->second.releasing) {
                if (retries_left > 0)
                    schedule_profile_delete(plugin_key, profile_id, installation_root, retries_left - 1);
                else
                    warn_profile(plugin_key,
                                 _u8L("The plugin was removed, but its browser profile is still in use by a "
                                      "closing plugin window. The profile was left untouched."));
                return;
            }
            leases().erase(it);
        }
    }

#if defined(__APPLE__)
    // The macOS profile store is in-memory and per process; there is no disk profile to delete,
    // but an explicit delete/reset must drop its cookie/session store.
    WebView::ReleaseProfile(wxString::FromUTF8(profile_id));
    (void) profile_id;
    return;
#else
    std::shared_ptr<LeaseState> lease = std::make_shared<LeaseState>();
    boost::system::error_code   ec;
    fs::create_directories(profiles_root(), ec);
    if (ec) {
        warn_profile(plugin_key,
                     GUI::format(_u8L("The plugin was removed, but its browser profile could not be deleted: %1%"),
                                 ec.message()));
        return;
    }
    if (!lease->lock.open(lease_path(profile_id).string())) {
        warn_profile(plugin_key,
                     _u8L("The plugin was removed, but its browser profile is in use by another OrcaSlicer "
                          "instance. The profile was left untouched."));
        return;
    }

    // All views are gone (the lease excludes every process). Release the cached WebView2
    // environment / WebKitWebContext before touching the directory.
    WebView::ReleaseProfile(wxString::FromUTF8(profile_id));

    const fs::path live = profiles_root() / profile_id;
    const bool     live_exists = fs::exists(live, ec);
    if (ec) {
        warn_profile(plugin_key,
                     GUI::format(_u8L("The plugin was removed, but its browser profile could not be checked: %1%"),
                                 ec.message()));
        lease->lock.close();
        return;
    }
    if (!live_exists) {
        lease->lock.close();
        return;
    }

    const fs::path tombstone = profiles_root() /
        ("__deleted-" + profile_id + "-" + fs::unique_path("%%%%-%%%%-%%%%").string());
    ec.clear();
    fs::rename(live, tombstone, ec);
    if (ec) {
        lease->lock.close();
        // WebView2 can keep the environment's files open for a moment after the configuration is
        // dropped; retry on later GUI iterations before giving up.
        if (retries_left > 0) {
            schedule_profile_delete(plugin_key, profile_id, installation_root, retries_left - 1);
            return;
        }
        warn_profile(plugin_key,
                     GUI::format(_u8L("The plugin was removed, but its browser profile could not be deleted: %1%"),
                                 ec.message()));
        return;
    }

    fs::remove_all(tombstone, ec);
    if (ec) {
        warn_profile(plugin_key,
                     GUI::format(_u8L("The plugin was removed, but its browser profile tombstone could not be fully "
                                      "deleted: %1%"),
                                 ec.message()));
    }
    lease->lock.close();
#endif
}

} // namespace

std::string ensure_plugin_browser_profile(const std::string& plugin_key, bool* fresh, std::string* legacy_path,
                                          std::string* installation_revision, const std::string* expected_root,
                                          const std::string* expected_revision)
{
    if (fresh != nullptr)
        *fresh = false;
    if (legacy_path != nullptr)
        legacy_path->clear();
    if (installation_revision != nullptr)
        installation_revision->clear();

    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor(plugin_key, descriptor) || descriptor.plugin_root.empty())
        throw std::runtime_error(
            GUI::format(_u8L("Cannot resolve the browser profile of an unknown plugin: %1%"), plugin_key));

    const fs::path root = (expected_root != nullptr && !expected_root->empty()) ? fs::path(*expected_root)
                                                                               : fs::path(descriptor.plugin_root);
    std::string    profile_id;

    {
        std::lock_guard<PluginInstallStateMutex> lock(install_state_mutex());

        PluginInstallState state;
        const InstallStateReadStatus status = read_install_state_status(root, state);
        if (status == InstallStateReadStatus::Unreadable)
            throw std::runtime_error(GUI::format(_u8L("Cannot read the plugin install state at %1%."),
                                                 (root / INSTALL_STATE_FILE).string()));

        // Compare the selected descriptor with the sidecar before binding or writing anything.
        // A stale load must not adopt a replacement's identity/revision.
        if (expected_revision != nullptr && !expected_revision->empty()) {
            if (status != InstallStateReadStatus::Readable || state.installation_revision != *expected_revision)
                throw std::runtime_error(
                    _u8L("The installed plugin changed while it was loading. Refresh the plugin list and try again."));
        } else if (status == InstallStateReadStatus::Readable && !state.installation_revision.empty()) {
            // The descriptor has no selected revision, but the sidecar has one: this process's
            // view of the installation is stale (or another instance replaced it).
            throw std::runtime_error(
                _u8L("The installed plugin revision is stale. Refresh the plugin list and try again."));
        }

        if (status == InstallStateReadStatus::Readable && !state.browser_profile_id.empty()) {
            if (!is_uuid(state.browser_profile_id))
                throw std::runtime_error(GUI::format(
                    _u8L("The plugin install state at %1% has an invalid browser profile id. Reinstall the plugin "
                         "or repair the file; the id was left untouched."),
                    (root / INSTALL_STATE_FILE).string()));
            profile_id = state.browser_profile_id;
            // A first load of a pre-identity/pre-revision install must not leave an empty revision:
            // delete and audit compare that token against the package on disk.
            if (state.installation_revision.empty()) {
                state.installation_revision = make_plugin_installation_revision();
                if (!write_install_state(root, state))
                    throw std::runtime_error(GUI::format(_u8L("Cannot write the plugin install state at %1%."),
                                                         (root / INSTALL_STATE_FILE).string()));
                PluginManager::instance().set_plugin_installation_revision(plugin_key, root.string(),
                                                                           state.installation_revision);
            }
        } else {
            if (status == InstallStateReadStatus::Absent)
                seed_install_metadata(state, descriptor);
            state.browser_profile_id    = make_plugin_browser_profile_id();
            state.installation_revision = make_plugin_installation_revision();
            if (!write_install_state(root, state))
                throw std::runtime_error(GUI::format(_u8L("Cannot write the plugin install state at %1%."),
                                                     (root / INSTALL_STATE_FILE).string()));
            PluginManager::instance().set_plugin_installation_revision(plugin_key, root.string(),
                                                                       state.installation_revision);
            profile_id = state.browser_profile_id;
        }
        if (installation_revision != nullptr)
            *installation_revision = state.installation_revision;
    }

    // The old WebView profile was named by plugin_key. Its data is ambiguous: it may belong to a
    // different installation of the same plugin. Never reuse, move or delete it. Check even when
    // the installer already wrote the fresh id (a cloud update of a pre-identity install), so the
    // user still learns why the old login did not carry over. warn_profile deduplicates.
    const std::string legacy = legacy_name(plugin_key);
    if (!legacy.empty() && legacy != profile_id) {
        const fs::path old = profiles_root() / legacy;
        boost::system::error_code ec;
        const bool exists = fs::exists(old, ec);
        if (ec) {
            warn_profile(plugin_key,
                         GUI::format(_u8L("A previous browser profile at %1% could not be checked: %2%"),
                                     old.string(), ec.message()));
        } else if (exists) {
            if (fresh != nullptr)
                *fresh = true;
            if (legacy_path != nullptr)
                *legacy_path = old.string();
            warn_profile(plugin_key,
                         GUI::format(_u8L("A previous browser profile exists at %1%. A new browser profile was "
                                          "created; the old data was left untouched."),
                                     old.string()));
        }
    }
    return profile_id;
}

static void release_profile_use(const std::string& profile_id)
{
    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(leases_mutex());
        auto                        it = leases().find(profile_id);
        if (it == leases().end())
            return;
        if (it->second.uses > 0)
            --it->second.uses;
        if (it->second.uses == 0 && !it->second.releasing) {
            it->second.releasing = true;
            schedule             = true;
        }
    }
    if (!schedule || wxTheApp == nullptr)
        return;

    // Release the cached WebView environment/context only after the last view's destructor has
    // finished, then close the OS lease. Holding the lease until then is what prevents another
    // process from deleting a live backend.
    GUI::wxGetApp().CallAfter([profile_id]() {
        std::shared_ptr<LeaseState> lease;
        {
            std::lock_guard<std::mutex> lock(leases_mutex());
            auto                        it = leases().find(profile_id);
            if (it == leases().end() || it->second.uses > 0 || !it->second.releasing)
                return;
            lease = std::move(it->second.lease);
            leases().erase(it);
        }
        if (lease) {
#if !defined(__APPLE__)
            // Disk-backed profiles release the cached backend environment on close. On macOS the
            // store itself is the in-memory session, so it must survive closing all views and is
            // released only by an explicit delete/reset.
            WebView::ReleaseProfile(wxString::FromUTF8(profile_id));
#endif
            lease->lock.close();
            // The lease file is deliberately never unlinked: another process may have opened the
            // same inode and not locked it yet. Removing it would let two processes lock different
            // inodes for the same profile.
        }
    });
}

BrowserProfileLease acquire_plugin_browser_profile_lease(const PluginBrowserProfileIdentityRef& identity)
{
    if (!identity || !is_uuid(identity->profile_id))
        return nullptr;
    const std::string profile_id = identity->profile_id;
    if (!plugin_browser_profile_is_current(identity))
        return nullptr;

    {
        std::lock_guard<std::mutex> lock(leases_mutex());
        auto&                       entry = leases()[profile_id];
        if (!entry.lease) {
#if defined(__APPLE__)
            entry.lease = std::make_shared<LeaseState>(); // in-memory store; no disk lease
#else
            boost::system::error_code ec;
            fs::create_directories(profiles_root(), ec);
            entry.lease = std::make_shared<LeaseState>();
            if (!entry.lease->lock.open(lease_path(profile_id).string())) {
                leases().erase(profile_id);
                return nullptr;
            }
#endif
        }
        ++entry.uses;
        entry.releasing = false;
    }

    // Revalidate after taking the lease: the load instance may have been replaced in between,
    // or the sidecar on disk may now describe a different installation/revision.
    if (!plugin_browser_profile_is_current(identity) || !plugin_browser_profile_installation_is_current(identity)) {
        release_profile_use(profile_id);
        return nullptr;
    }
    return BrowserProfileLease(reinterpret_cast<void*>(1), [profile_id](void*) { release_profile_use(profile_id); });
}

PluginBrowserProfileIdentityRef begin_plugin_browser_profile(const std::string& plugin_key,
                                                             const std::string& expected_plugin_root,
                                                             const std::string& expected_revision)
{
    std::string revision;
    const std::string profile_id = ensure_plugin_browser_profile(plugin_key, nullptr, nullptr, &revision,
                                                                 &expected_plugin_root, &expected_revision);
    auto identity = std::make_shared<PluginBrowserProfileIdentity>(
        PluginBrowserProfileIdentity{plugin_key, profile_id, expected_plugin_root, revision});
    {
        std::lock_guard<std::mutex> lock(identities_mutex());
        identities()[plugin_key] = identity;
    }
    return identity;
}

void end_plugin_browser_profile(const std::string& plugin_key)
{
    std::lock_guard<std::mutex> lock(identities_mutex());
    identities().erase(plugin_key);
}

PluginBrowserProfileIdentityRef current_plugin_browser_profile_identity(const std::string& plugin_key)
{
    std::lock_guard<std::mutex> lock(identities_mutex());
    auto                        it = identities().find(plugin_key);
    return it == identities().end() ? nullptr : it->second;
}

bool plugin_browser_profile_is_current(const PluginBrowserProfileIdentityRef& identity)
{
    if (!identity)
        return false;
    std::lock_guard<std::mutex> lock(identities_mutex());
    auto                        it = identities().find(identity->plugin_key);
    return it != identities().end() && it->second == identity;
}

bool plugin_browser_profile_installation_is_current(const PluginBrowserProfileIdentityRef& identity)
{
    if (!identity || identity->plugin_root.empty() || identity->installation_revision.empty())
        return false;
    std::unique_lock<PluginInstallStateMutex> state_lock;
    std::string                               lock_error;
    if (!try_lock_plugin_install_state(state_lock, lock_error))
        return false;
    PluginInstallState state;
    if (read_install_state_status(boost::filesystem::path(identity->plugin_root), state) !=
        InstallStateReadStatus::Readable)
        return false;
    return state.browser_profile_id == identity->profile_id &&
           state.installation_revision == identity->installation_revision;
}

void warn_plugin_browser_profile_in_use(const std::string& plugin_key)
{
    warn_profile(plugin_key,
                 _u8L("The plugin's browser profile is in use by another OrcaSlicer instance. Its plugin UI "
                      "was not opened."));
}

void warn_plugin_browser_profile_reset(const std::string& plugin_key)
{
    warn_profile(plugin_key,
                 _u8L("The plugin was reinstalled, so its browser profile and sign-in session were reset."));
}

void warn_plugin_browser_profile_cleanup_unknown(const std::string& plugin_key)
{
    warn_profile(plugin_key,
                 _u8L("The plugin was removed, but its browser profile could not be identified and may have been "
                      "left on disk."));
}

void warn_legacy_plugin_browser_profile(const std::string& plugin_key, const std::string& current_profile_id)
{
    const std::string legacy = legacy_name(plugin_key);
    if (legacy.empty() || legacy == current_profile_id)
        return;
    const fs::path old = profiles_root() / legacy;
    boost::system::error_code ec;
    const bool exists = fs::exists(old, ec);
    if (ec) {
        warn_profile(plugin_key,
                     GUI::format(_u8L("A previous browser profile at %1% could not be checked: %2%"),
                                 old.string(), ec.message()));
        return;
    }
    if (exists)
        warn_profile(plugin_key,
                     GUI::format(_u8L("A previous browser profile exists at %1%. A new browser profile was "
                                      "created; the old data was left untouched."),
                                 old.string()));
}

void delete_plugin_browser_profile(const std::string& plugin_key, const std::string& profile_id,
                                   const std::string& installation_root)
{
    if (profile_id.empty()) {
#if defined(__APPLE__)
        // The pre-identity macOS store was keyed by the plugin key; drop it on delete/reset too.
        const std::string legacy = legacy_name(plugin_key);
        if (wxTheApp != nullptr && !legacy.empty())
            WebView::ReleaseProfile(wxString::FromUTF8(legacy));
#endif
        return;
    }
    if (!is_uuid(profile_id)) {
        BOOST_LOG_TRIVIAL(warning) << "Refusing to delete an invalid browser profile id for " << plugin_key;
        return;
    }
    if (wxTheApp == nullptr)
        return;
    if (wxIsMainThread())
        delete_profile_on_ui_thread(plugin_key, profile_id, installation_root, 20);
    else
        schedule_profile_delete(plugin_key, profile_id, installation_root, 20);
}

} // namespace Slic3r
