// SourceFileWatcher lives in libslic3r_gui; this is the only suite that links it. Same Windows
// include prologue as test_dev_mapping.cpp (wx pulls in <windows.h>; keep WIN32_LEAN_AND_MEAN /
// NOMINMAX ahead of the Catch2 headers).
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include <wx/app.h>
#include <wx/timer.h>

#include <boost/filesystem.hpp>

#include <chrono>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "slic3r/GUI/SourceFileWatcher.hpp"

using namespace Slic3r::GUI;
namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

// Defines the friend hook declared in SourceFileWatcher.hpp; nothing outside this test binary
// calls it. Lets a test replace the watcher's clock with a FakeClock (below) that it advances by
// hand, so the 500ms stability window can be asserted without a real sleep.
void test_set_watcher_clock(SourceFileWatcher& watcher, std::function<std::chrono::steady_clock::time_point()> now)
{
    watcher.m_now = std::move(now);
}

}} // namespace Slic3r::GUI

namespace {

// wxFileSystemWatcher and wxTimer look up the application's traits, so they need an app object to
// exist. A non-GUI one is enough (and needs no display); it is installed only for the test's
// duration so other tests in this executable still see none.
struct WxEnv
{
    wxAppConsole* previous{ wxAppConsole::GetInstance() };
    wxAppConsole* app{ new wxAppConsole };

    WxEnv() { wxAppConsole::SetInstance(app); }
    ~WxEnv()
    {
        wxAppConsole::SetInstance(previous);
        delete app;
    }
};

// A directory that removes itself, holding the files a test tracks.
struct TempDir
{
    fs::path path{ fs::temp_directory_path() / fs::unique_path("orca_watcher_%%%%-%%%%-%%%%") };

    TempDir() { fs::create_directories(path); }
    ~TempDir() { boost::system::error_code ec; fs::remove_all(path, ec); }

    // Writes `size` bytes so the file's (mtime, size) stamp is guaranteed to differ from any write
    // of another size, even within the same wall-clock second.
    std::string write(const std::string& name, size_t size) const
    {
        const fs::path file = path / name;
        std::ofstream(file.string(), std::ios::binary | std::ios::trunc) << std::string(size, 'x');
        return file.string();
    }
    void remove(const std::string& name) const { fs::remove(path / name); }
};

// A steady_clock stand-in a test advances by hand instead of sleeping; installed on a watcher via
// test_set_watcher_clock(). Starts at the real "now" only as an arbitrary monotonic anchor -- its
// value is otherwise never compared against real time.
struct FakeClock
{
    std::chrono::steady_clock::time_point t{ std::chrono::steady_clock::now() };

    std::chrono::steady_clock::time_point operator()() const { return t; }
    void advance(std::chrono::milliseconds delta) { t += delta; }
};

// More than the watcher's 500ms stability window, so advancing a FakeClock by this always settles
// a candidate whose stamp hasn't changed since it was first observed.
constexpr std::chrono::milliseconds past_stability_window{ 600 };

// Owns a watcher plus a recording callback, and drives the debounce timer by hand: the timer's
// event is what the watcher acts on, so delivering it directly runs the same code as a real fire,
// and a FakeClock lets that include waiting out the stability window without an event loop or a
// real sleep.
struct Harness
{
    FakeClock                           clock;
    SourceFileWatcher                   watcher;
    std::vector<std::set<std::string>>  calls;
    bool                                reload_succeeds{ true };

    Harness()
    {
        test_set_watcher_clock(watcher, [this] { return clock.t; });
        watcher.set_on_changed([this](const std::set<std::string>& files) {
            calls.push_back(files);
            return reload_succeeds ? files : std::set<std::string>{};
        });
    }

    // A single raw timer tick, with no clock advance -- for tests asserting the not-yet-settled
    // state itself.
    void tick()
    {
        wxTimer timer;
        wxTimerEvent evt(timer);
        watcher.ProcessEvent(evt);
    }

    // Two ticks with the clock fast-forwarded past the stability window in between: the sequence
    // every existing test wants, "deliver the debounce timer's event and see the settled result,"
    // now that settling needs the same stamp observed on two ticks 500ms apart.
    void fire_timer()
    {
        tick();
        clock.advance(past_stability_window);
        tick();
    }
};

} // namespace

TEST_CASE("Source path resolution keeps a recorded path that exists", "[SourceFileWatcher]")
{
    TempDir dir;
    const std::string file = dir.write("part.stl", 10);

    CHECK(SourceFileWatcher::resolve_source_file_path(file, fs::path()) == file);
    CHECK(SourceFileWatcher::resolve_source_file_path(file, dir.path) == file);
}

TEST_CASE("Source path resolution finds a bare filename next to the project", "[SourceFileWatcher]")
{
    TempDir dir;
    const std::string file = dir.write("part.stl", 10);

    CHECK(SourceFileWatcher::resolve_source_file_path("part.stl", dir.path) == file);
}

TEST_CASE("Source path resolution leaves an unresolvable path unchanged", "[SourceFileWatcher]")
{
    TempDir dir;

    CHECK(SourceFileWatcher::resolve_source_file_path("missing.stl", dir.path) == "missing.stl");
    CHECK(SourceFileWatcher::resolve_source_file_path("missing.stl", fs::path()) == "missing.stl");
    CHECK(SourceFileWatcher::resolve_source_file_path("", dir.path).empty());
}

TEST_CASE("An unchanged tracked file is not reported", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string file = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ file });

    h.fire_timer();

    CHECK(h.calls.empty());
}

TEST_CASE("A changed file is reported until its reload succeeds, then not again", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string file = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ file });
    dir.write("a.stl", 20);

    h.fire_timer();
    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == std::set<std::string>{ file });

    h.fire_timer();
    CHECK(h.calls.size() == 1);
}

TEST_CASE("A changed file is not reported until its stamp has held for the stability window", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string file = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ file });
    dir.write("a.stl", 20);

    h.tick();
    CHECK(h.calls.empty()); // first observation: stamp recorded, not yet settled

    h.clock.advance(past_stability_window);
    h.tick();
    CHECK(h.calls.size() == 1); // same stamp held long enough: settled
}

TEST_CASE("A file whose stamp keeps changing is never reported", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string file = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ file });

    // Simulates a slow export still growing: each write lands well inside the stability window of
    // the one before it, so the "since" it's timed against keeps resetting and it never settles --
    // this is the behavior a size-blind, purely event-based debounce couldn't guarantee.
    for (int size = 20; size <= 60; size += 10) {
        dir.write("a.stl", size);
        h.tick();
        h.clock.advance(std::chrono::milliseconds(200));
    }

    CHECK(h.calls.empty());
}

TEST_CASE("A settled file is reported while another keeps being polled", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string a = dir.write("a.stl", 10);
    const std::string b = dir.write("b.stl", 10);
    h.watcher.set_watched_files({ a, b });
    dir.write("a.stl", 20); // will settle
    dir.write("b.stl", 20); // will keep changing

    h.tick(); // both observed for the first time
    h.clock.advance(past_stability_window);
    dir.write("b.stl", 30); // b moves again just before the settling tick
    h.tick();

    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == std::set<std::string>{ a }); // only the settled one is reported
}

TEST_CASE("A failed reload is not retried at the same stamp but is at the next", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    h.reload_succeeds = false;
    const std::string file = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ file });
    dir.write("a.stl", 20);

    h.fire_timer();
    REQUIRE(h.calls.size() == 1);

    // Still the stamp that failed: a permanently unloadable file must not retry on every event.
    h.fire_timer();
    CHECK(h.calls.size() == 1);

    // The file moved on (still being written, say): it is a new candidate again.
    dir.write("a.stl", 30);
    h.fire_timer();
    CHECK(h.calls.size() == 2);

    // And once a reload finally succeeds the change is consumed.
    h.reload_succeeds = true;
    dir.write("a.stl", 40);
    h.fire_timer();
    REQUIRE(h.calls.size() == 3);
    h.fire_timer();
    CHECK(h.calls.size() == 3);
}

TEST_CASE("A file that vanishes is not reported until it returns changed", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string file = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ file });

    // Caught between a rename-into-place's removal of the old name and the arrival of the new one.
    dir.remove("a.stl");
    h.fire_timer();
    CHECK(h.calls.empty());

    dir.write("a.stl", 20);
    h.fire_timer();
    CHECK(h.calls.size() == 1);
}

TEST_CASE("Only the files that changed are reported", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string a = dir.write("a.stl", 10);
    const std::string b = dir.write("b.stl", 10);
    const std::string c = dir.write("c.stl", 10);
    h.watcher.set_watched_files({ a, b, c });
    dir.write("b.stl", 20);

    h.fire_timer();

    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == std::set<std::string>{ b });
}

TEST_CASE("A failed batch fails every file in it and retries them together", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    h.reload_succeeds = false;
    const std::string a = dir.write("a.stl", 10);
    const std::string b = dir.write("b.stl", 10);
    h.watcher.set_watched_files({ a, b });
    dir.write("a.stl", 20);
    dir.write("b.stl", 20);

    h.fire_timer();
    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == std::set<std::string>{ a, b });

    // Only one of them moves on; the other is still parked at its failed stamp.
    dir.write("a.stl", 30);
    h.fire_timer();
    REQUIRE(h.calls.size() == 2);
    CHECK(h.calls[1] == std::set<std::string>{ a });
}

TEST_CASE("A batch that partially succeeds commits only the succeeded file's stamp", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    SourceFileWatcher watcher;
    FakeClock clock;
    test_set_watcher_clock(watcher, [&clock] { return clock.t; });
    const std::string a = dir.write("a.stl", 10);
    const std::string b = dir.write("b.stl", 10);
    watcher.set_watched_files({ a, b });
    dir.write("a.stl", 20);
    dir.write("b.stl", 20);

    std::vector<std::set<std::string>> calls;
    // Only `a` reloads; `b` is left over (a missing source, a declined paint-loss prompt, or a
    // load exception further down the same batch), the way a real caller reports a mixed outcome.
    watcher.set_on_changed([&](const std::set<std::string>& files) {
        calls.push_back(files);
        return std::set<std::string>{ a };
    });

    wxTimer timer;
    wxTimerEvent evt(timer);
    watcher.ProcessEvent(evt);
    clock.advance(past_stability_window);
    watcher.ProcessEvent(evt);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0] == std::set<std::string>{ a, b });

    // `a`'s stamp advanced (committed), so it is not reported again on its own.
    watcher.ProcessEvent(evt);
    clock.advance(past_stability_window);
    watcher.ProcessEvent(evt);
    CHECK(calls.size() == 1);

    // `b` is still at the stamp that didn't succeed, so it is not retried until it changes again --
    // once it does, only `b` is reported, not `a` too.
    dir.write("b.stl", 30);
    watcher.ProcessEvent(evt);
    clock.advance(past_stability_window);
    watcher.ProcessEvent(evt);
    REQUIRE(calls.size() == 2);
    CHECK(calls[1] == std::set<std::string>{ b });
}

TEST_CASE("Re-arming the watch keeps a change that has not been reloaded yet", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string a = dir.write("a.stl", 10);
    const std::string b = dir.write("b.stl", 10);
    h.watcher.set_watched_files({ a });
    dir.write("a.stl", 20);

    // An unrelated object-list change re-derives the set of watched files before the debounce
    // fires; the pending change to `a` must survive it.
    h.watcher.set_watched_files({ a, b });
    h.fire_timer();

    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == std::set<std::string>{ a });
}

TEST_CASE("Forgetting the watched files re-arms without dropping a pending change", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string a = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ a });
    dir.write("a.stl", 20);

    // A rename-into-place leaves the file-level watch bound to the old inode, so the caller
    // forgets and re-arms with the same path set.
    h.watcher.forget_watched_files();
    h.watcher.set_watched_files({ a });
    h.fire_timer();

    CHECK(h.calls.size() == 1);
}

TEST_CASE("A file that is no longer tracked is dropped and re-baselined if tracked again", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string a = dir.write("a.stl", 10);
    const std::string b = dir.write("b.stl", 10);
    h.watcher.set_watched_files({ a, b });

    h.watcher.set_watched_files({ a });
    dir.write("b.stl", 20);
    h.fire_timer();
    CHECK(h.calls.empty());

    // Tracked again: its baseline is taken from the file as it is now, not from before.
    h.watcher.set_watched_files({ a, b });
    h.fire_timer();
    CHECK(h.calls.empty());
}

TEST_CASE("Clearing the watcher drops the baseline and every pending change", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string a = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ a });
    dir.write("a.stl", 20);

    h.watcher.clear();
    h.fire_timer();
    CHECK(h.calls.empty());

    h.watcher.set_watched_files({ a });
    h.fire_timer();
    CHECK(h.calls.empty());
}

TEST_CASE("A change is not reported while a reload is already running", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    SourceFileWatcher watcher;
    FakeClock clock;
    test_set_watcher_clock(watcher, [&clock] { return clock.t; });
    const std::string file = dir.write("a.stl", 10);
    watcher.set_watched_files({ file });
    dir.write("a.stl", 20);

    int  calls  = 0;
    bool nested = false;
    watcher.set_on_changed([&](const std::set<std::string>& files) {
        ++calls;
        // Stands in for a modal dialog or wxBusyInfo pumping the event loop mid-reload and letting
        // the debounce timer fire again: the callback must not be re-entered. This nested tick
        // hits the m_reload_in_progress guard immediately, before any stability check, so it
        // doesn't need the clock advanced first.
        if (!nested) {
            nested = true;
            wxTimer timer;
            wxTimerEvent evt(timer);
            watcher.ProcessEvent(evt);
        }
        return files;
    });

    wxTimer timer;
    wxTimerEvent evt(timer);
    watcher.ProcessEvent(evt);
    clock.advance(past_stability_window);
    watcher.ProcessEvent(evt);

    CHECK(calls == 1);
}

TEST_CASE("Without a callback a change stays pending", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    SourceFileWatcher watcher;
    FakeClock clock;
    test_set_watcher_clock(watcher, [&clock] { return clock.t; });
    const std::string file = dir.write("a.stl", 10);
    watcher.set_watched_files({ file });
    dir.write("a.stl", 20);

    wxTimer timer;
    wxTimerEvent evt(timer);
    // No callback yet: changed_source_files() sees the change, but on_timer() returns without
    // starting any stability tracking for it (see the early "!m_on_changed" return) -- so once a
    // callback is set below, it also needs a full stability window, not an immediate report.
    watcher.ProcessEvent(evt);

    int calls = 0;
    watcher.set_on_changed([&](const std::set<std::string>& files) { ++calls; return files; });
    watcher.ProcessEvent(evt);
    clock.advance(past_stability_window);
    watcher.ProcessEvent(evt);

    CHECK(calls == 1);
}
