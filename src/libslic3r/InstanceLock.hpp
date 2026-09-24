#pragma once

#include <chrono>
#include <mutex>
#include <string>

namespace Slic3r {

// Scoped write lock on a file shared by every running instance of the
// application, such as the app config or the user preset directory: threads
// of this process are serialised through a recursive mutex, other processes
// through an advisory OS file lock on `lock_file_path`. The lock file is
// created on first use and kept; the OS releases the lock when its holder
// exits, so a crashed instance never leaves a stale lock behind.
//
// Best effort: when the lock file cannot be opened or locked, or another
// instance still holds it after `timeout`, the guard keeps only the in-process
// mutex, locked() reports false and the write proceeds, since a hung instance
// must never block another one from saving. For `cooldown` afterwards guards
// leave the file alone. The wait for the in-process mutex is bounded only by
// the longest critical section, so a guard covers a few file operations and
// nothing slower.
//
// Lock order: the preset collection mutex may be held when a guard is taken
// (set_sync_info_and_save() calls save_info() under it), never the reverse;
// that is why the guards sit at the leaf readers and writers and why a guard
// must not be added around save_user_presets(), which takes the collection
// mutex through delete_preset().
class InstanceLock
{
public:
    // Long against a critical section of milliseconds, short against the GUI
    // thread, which is where most guards are taken.
    static constexpr std::chrono::milliseconds default_timeout{1000};
    // Long enough that a holder stuck in a debugger does not cost a stall per
    // save; mutable so tests can shorten it.
    static inline std::chrono::milliseconds cooldown{10000};

    // An empty path makes the guard a no-op.
    explicit InstanceLock(const std::string &lock_file_path, std::chrono::milliseconds timeout = default_timeout);
    ~InstanceLock();

    InstanceLock(const InstanceLock &) = delete;
    InstanceLock &operator=(const InstanceLock &) = delete;

    // True while this process holds the cross-process file lock.
    bool locked() const { return m_locked; }

private:
    struct Slot;
    static Slot &slot_for(const std::string &lock_file_path);
    static bool  open_lock_file(Slot &slot, const std::string &lock_file_path);
    static void  defer(Slot &slot, const std::string &reason);

    Slot *m_slot{nullptr};
    std::unique_lock<std::recursive_mutex> m_slot_guard;
    bool  m_locked{false};
};

} // namespace Slic3r
