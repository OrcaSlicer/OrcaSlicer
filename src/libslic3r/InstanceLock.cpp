#include "InstanceLock.hpp"

#include <map>
#include <memory>
#include <system_error>
#include <thread>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#ifdef _WIN32
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/nowide/convert.hpp>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Slic3r {

#ifdef _WIN32
// LockFileEx, held by this handle alone.
using NativeFileLock = boost::interprocess::file_lock;
#else
// flock(2) rather than an fcntl lock: it belongs to this open file description,
// so any other code in the process that opens and closes the lock file, as a
// backup or an export walking the data dir might, cannot drop it. An fcntl
// lock would go with the first such close.
class NativeFileLock
{
public:
    explicit NativeFileLock(const char *path) : m_fd(::open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644))
    {
        if (m_fd < 0)
            throw std::system_error(errno, std::generic_category(), path);
    }
    ~NativeFileLock() { ::close(m_fd); }
    bool try_lock()
    {
        if (::flock(m_fd, LOCK_EX | LOCK_NB) == 0)
            return true;
        // A signal (a child exiting, for one) interrupts the call like any other; the caller polls again.
        if (errno == EWOULDBLOCK || errno == EINTR)
            return false;
        throw std::system_error(errno, std::generic_category(), "flock");
    }
    void unlock() { ::flock(m_fd, LOCK_UN); }
private:
    int m_fd;
};
#endif

// One slot per lock file, shared by every guard in the process: one lock
// object per path behind a mutex is what makes the guard re-entrant and safe
// to use from the preset sync thread and the GUI thread at once. The lock
// file is opened by the outermost guard and closed when it goes, so the file
// is never held open between guards: whatever is at the path is what gets
// locked, and a data dir can be removed once nothing is saving into it. The
// file is kept rather than deleted on release because the lock state lives in
// the kernel on the open file, and deleting it would let a third instance
// lock a fresh file while the second still holds the old one.
struct InstanceLock::Slot
{
    std::recursive_mutex mutex;
    // Non-null exactly while this process holds the file lock.
    std::unique_ptr<NativeFileLock> file_lock;
    int  depth{0};
    // Until this point, after a guard could not open, lock or wait out the
    // file, guards do not touch it.
    std::chrono::steady_clock::time_point cooldown_until{};
};

InstanceLock::Slot &InstanceLock::slot_for(const std::string &lock_file_path)
{
    // Never freed: a save during static destruction still needs its slot.
    static auto *registry_mutex = new std::mutex();
    static auto *registry       = new std::map<std::string, std::unique_ptr<Slot>>();

    std::lock_guard<std::mutex> guard(*registry_mutex);
    std::unique_ptr<Slot> &slot = (*registry)[lock_file_path];
    if (! slot)
        slot = std::make_unique<Slot>();
    return *slot;
}

// Starts the cool-down. Called with the slot mutex held.
void InstanceLock::defer(Slot &slot, const std::string &reason)
{
    slot.cooldown_until = std::chrono::steady_clock::now() + cooldown;
    BOOST_LOG_TRIVIAL(warning) << reason << "; proceeding without the lock for the next " << cooldown.count() << " ms";
}

// Creates the lock file if needed and opens it, or starts the cool-down.
// Called with the slot mutex held.
bool InstanceLock::open_lock_file(Slot &slot, const std::string &lock_file_path)
{
    try {
#ifdef _WIN32
        // The lock opens an existing file; created once, on the first miss.
        const std::wstring wide_path = boost::nowide::widen(lock_file_path);
        try {
            slot.file_lock = std::make_unique<NativeFileLock>(wide_path.c_str());
        } catch (const std::exception &) {
            boost::nowide::ofstream(lock_file_path, std::ios::app).close();
            slot.file_lock = std::make_unique<NativeFileLock>(wide_path.c_str());
        }
#else
        slot.file_lock = std::make_unique<NativeFileLock>(lock_file_path.c_str());
#endif
        return true;
    } catch (const std::exception &e) {
        defer(slot, "Cannot open lock file " + lock_file_path + ": " + e.what() + " (check its owner and permissions)");
        return false;
    }
}

InstanceLock::InstanceLock(const std::string &lock_file_path, std::chrono::milliseconds timeout)
{
    if (lock_file_path.empty())
        return;
    m_slot       = &slot_for(lock_file_path);
    m_slot_guard = std::unique_lock<std::recursive_mutex>(m_slot->mutex);
    const auto now = std::chrono::steady_clock::now();
    if (m_slot->depth == 0 && now >= m_slot->cooldown_until && open_lock_file(*m_slot, lock_file_path)) {
        const auto deadline = now + timeout;
        bool       taken    = false;
        for (;;) {
            try {
                if ((taken = m_slot->file_lock->try_lock()))
                    break;
            } catch (const std::exception &e) {
                defer(*m_slot, "Cannot lock " + lock_file_path + ": " + e.what());
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                defer(*m_slot, "Another instance has held " + lock_file_path + " for over " + std::to_string(timeout.count()) + " ms");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (! taken)
            m_slot->file_lock.reset();
    }
    // Counted last, so a throw above leaves the slot exactly as it was found.
    ++ m_slot->depth;
    m_locked = m_slot->file_lock != nullptr;
}

InstanceLock::~InstanceLock()
{
    if (m_slot == nullptr)
        return;
    if (-- m_slot->depth == 0 && m_slot->file_lock) {
        try {
            m_slot->file_lock->unlock();
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "Cannot unlock instance lock: " << e.what();
        }
        m_slot->file_lock.reset();
    }
}

} // namespace Slic3r
