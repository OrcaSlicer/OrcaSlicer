#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <boost/filesystem.hpp>

#include "libslic3r/InstanceLock.hpp"
#include "test_utils.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace Slic3r;
using namespace std::chrono_literals;

// Sets a process-wide knob for one test and restores it however the test ends.
template<typename T> struct ScopedStaticValue
{
    T &ref;
    T  saved;
    ScopedStaticValue(T &ref, T value) : ref(ref), saved(ref) { ref = value; }
    ~ScopedStaticValue() { ref = saved; }
};

TEST_CASE("InstanceLock creates its lock file and holds it for the guard's scope", "[InstanceLock]")
{
    ScopedTemporaryFile lock_file(".lock");
    const std::string   path = lock_file.string();

    {
        InstanceLock lock(path);
        REQUIRE(lock.locked());
        REQUIRE(boost::filesystem::exists(path));
    }
    // Released: a fresh guard gets the lock at once instead of waiting out a timeout.
    const auto   started = std::chrono::steady_clock::now();
    InstanceLock again(path, 5000ms);
    REQUIRE(again.locked());
    // Well inside the timeout it would otherwise have waited out; loose enough for a loaded runner.
    REQUIRE(std::chrono::steady_clock::now() - started < 4000ms);
}

TEST_CASE("InstanceLock nests within one thread", "[InstanceLock]")
{
    ScopedTemporaryFile lock_file(".lock");
    const std::string   path = lock_file.string();

    InstanceLock outer(path);
    {
        InstanceLock inner(path, 100ms);
        REQUIRE(inner.locked());
    }
    // The inner guard leaving does not release the outer one.
    REQUIRE(outer.locked());
}

TEST_CASE("InstanceLock is a no-op for an empty path and survives an unwritable one", "[InstanceLock]")
{
    ScopedTemporaryDir dir;

    InstanceLock none("");
    REQUIRE_FALSE(none.locked());

    // The directory does not exist, so the lock file cannot be created; the
    // guard still constructs and the write it guards can go ahead.
    InstanceLock unwritable((dir.path() / "missing" / "shared.lock").string(), 100ms);
    REQUIRE_FALSE(unwritable.locked());
}

TEST_CASE("InstanceLock retries a lock file it could not open once the cool-down passes", "[InstanceLock]")
{
    ScopedTemporaryDir dir;
    const std::string  path = (dir.path() / "later" / "shared.lock").string();
    ScopedStaticValue  cooldown(InstanceLock::cooldown, 300ms);

    bool before_dir, during_cooldown, after_cooldown;
    const auto started = std::chrono::steady_clock::now();
    {
        InstanceLock lock(path, 100ms);
        before_dir = lock.locked();
    }
    boost::filesystem::create_directories(dir.path() / "later");
    {
        InstanceLock lock(path, 100ms);
        during_cooldown = lock.locked();
    }
    const bool second_guard_inside_cooldown = std::chrono::steady_clock::now() - started < InstanceLock::cooldown;
    std::this_thread::sleep_for(400ms);
    {
        InstanceLock lock(path, 100ms);
        after_cooldown = lock.locked();
    }

    REQUIRE_FALSE(before_dir);
    // A loaded runner may take longer than the cool-down to get here; then the
    // second guard legitimately retried, so only assert when the timing held.
    if (second_guard_inside_cooldown)
        REQUIRE_FALSE(during_cooldown);
    REQUIRE(after_cooldown);
}

TEST_CASE("InstanceLock reopens a lock file that was replaced on disk", "[InstanceLock]")
{
    ScopedTemporaryFile lock_file(".lock");
    const std::string   path = lock_file.string();
    {
        InstanceLock lock(path);
        REQUIRE(lock.locked());
    }

    boost::filesystem::remove(path);
    InstanceLock lock(path);
    REQUIRE(lock.locked());
    // Each outermost guard opens the file afresh, so the deleted path is back.
    REQUIRE(boost::filesystem::exists(path));
}

TEST_CASE("InstanceLock serialises the threads of one process", "[InstanceLock]")
{
    ScopedTemporaryFile lock_file(".lock");
    const std::string   path = lock_file.string();

    std::atomic<bool> holder_ready{false};
    std::atomic<bool> holder_released{false};
    std::thread holder([&] {
        InstanceLock lock(path);
        holder_ready = true;
        std::this_thread::sleep_for(150ms);
        holder_released = true;
    });
    while (! holder_ready)
        std::this_thread::yield();

    bool released_before_acquire = false;
    {
        InstanceLock lock(path);
        released_before_acquire = holder_released;
    }
    holder.join();
    REQUIRE(released_before_acquire);
}

#ifndef _WIN32
// The cross-process side of the lock is a POSIX flock, which a child process
// takes here directly; LockFileEx backs the guard on Windows, but spawning a
// child there is not worth a test.
TEST_CASE("InstanceLock yields to another process and reports it", "[InstanceLock]")
{
    ScopedTemporaryFile lock_file(".lock");
    const std::string   path = lock_file.string();
    ScopedStaticValue   cooldown(InstanceLock::cooldown, 300ms);

    int child_holds[2], child_may_exit[2];
    REQUIRE(::pipe(child_holds) == 0);
    REQUIRE(::pipe(child_may_exit) == 0);

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        int  fd   = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
        char byte = ::flock(fd, LOCK_EX | LOCK_NB) == 0 ? '1' : '0';
        if (::write(child_holds[1], &byte, 1) != 1 || ::read(child_may_exit[0], &byte, 1) != 1)
            ::_exit(1);
        ::_exit(0);
    }

    char byte = '0';
    REQUIRE(::read(child_holds[0], &byte, 1) == 1);
    REQUIRE(byte == '1');

    bool locked_while_child_holds;
    {
        InstanceLock lock(path, 100ms);
        locked_while_child_holds = lock.locked();
    }
    // The timed-out wait starts a cool-down: the next guard does not touch the file.
    const auto started = std::chrono::steady_clock::now();
    bool       locked_during_cooldown;
    {
        InstanceLock lock(path, 5000ms);
        locked_during_cooldown = lock.locked();
    }
    const auto cooldown_wait = std::chrono::steady_clock::now() - started;
    REQUIRE(::write(child_may_exit[1], "x", 1) == 1);
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    for (int fd : {child_holds[0], child_holds[1], child_may_exit[0], child_may_exit[1]})
        ::close(fd);

    REQUIRE_FALSE(locked_while_child_holds);
    REQUIRE_FALSE(locked_during_cooldown);
    REQUIRE(cooldown_wait < 4000ms);
    // Once the cool-down passes, the lock the child released is taken again.
    std::this_thread::sleep_for(400ms);
    InstanceLock lock(path);
    REQUIRE(lock.locked());
}
#endif
