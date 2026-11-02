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
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "slic3r/GUI/SourceFileWatcher.hpp"

using namespace Slic3r::GUI;
namespace fs = boost::filesystem;

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

// Owns a watcher plus a recording callback, and drives the debounce timer by hand: delivering its
// event directly runs the same code on_fs_event() would eventually trigger, without needing a
// real event loop or a real wait -- waiting for a file to go quiet is on_fs_event()'s job, so a
// single tick is always enough to see the result of a finished write.
struct Harness
{
    SourceFileWatcher                  watcher;
    std::vector<std::set<std::string>> calls;

    Harness()
    {
        watcher.set_on_changed([this](const std::set<std::string>& files) { calls.push_back(files); });
    }

    void tick()
    {
        wxTimer timer;
        wxTimerEvent evt(timer);
        watcher.ProcessEvent(evt);
    }
};

} // namespace

TEST_CASE("Source path resolution keeps a recorded path that exists", "[SourceFileWatcher]")
{
    TempDir dir;
    const std::string file = dir.write("part.stl", 10);

    CHECK(SourceFileWatcher::resolve_source_file_path(file, "") == file);
    CHECK(SourceFileWatcher::resolve_source_file_path(file, (dir.path / "object.stl").string()) == file);
}

TEST_CASE("Source path resolution finds a bare filename next to the object's input file", "[SourceFileWatcher]")
{
    TempDir dir;
    const std::string file = dir.write("part.stl", 10);

    CHECK(SourceFileWatcher::resolve_source_file_path("part.stl", (dir.path / "object.stl").string()) == file);
}

TEST_CASE("Source path resolution leaves an unresolvable path unchanged", "[SourceFileWatcher]")
{
    TempDir dir;

    CHECK(SourceFileWatcher::resolve_source_file_path("missing.stl", (dir.path / "object.stl").string()) == "missing.stl");
    CHECK(SourceFileWatcher::resolve_source_file_path("missing.stl", "") == "missing.stl");
    CHECK(SourceFileWatcher::resolve_source_file_path("", (dir.path / "object.stl").string()).empty());
}

TEST_CASE("An unchanged tracked file is not reported", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string file = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ file });

    h.tick();

    CHECK(h.calls.empty());
}

TEST_CASE("A changed file is reported once", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string file = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ file });
    dir.write("a.stl", 20);

    h.tick();
    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == std::set<std::string>{ file });

    // The baseline advances unconditionally as part of reporting it -- there is no retry in this
    // design, so the next tick with nothing further changed must not report it again.
    h.tick();
    CHECK(h.calls.size() == 1);
}

TEST_CASE("A same-size rewrite within the same second is reported", "[SourceFileWatcher]")
{
    // A binary STL's size depends only on its triangle count, so a vertex-only edit keeps the
    // size; only a sub-second mtime tells the two exports apart. The short sleep clears the
    // kernel's timestamp granularity (a few ms on Linux) while staying well inside one second.
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string file = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ file });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dir.write("a.stl", 10);

    h.tick();

    CHECK(h.calls.size() == 1);
}

#ifdef _WIN32
TEST_CASE("A file still open for writing is held back until it is closed", "[SourceFileWatcher]")
{
    // Windows only: elsewhere the per-file watch's events are what keep an in-place write from
    // being reported early, and those aren't exercised by ticking the timer directly.
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string file = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ file });

    {
        std::ofstream writer(file, std::ios::binary | std::ios::app);
        writer << std::string(10, 'y') << std::flush;

        h.tick();
        CHECK(h.calls.empty());
    }

    h.tick();
    CHECK(h.calls.size() == 1);
}
#endif

TEST_CASE("A file's baseline advances even if the callback's reload failed", "[SourceFileWatcher]")
{
    // Unlike a design with retries, whether the caller's reload actually succeeded is not
    // something this class tracks at all: the callback returns nothing, and the baseline has
    // already advanced by the time it's called. A permanently broken file simply stops being
    // reported until it changes again -- there's nothing else to distinguish it from a file that
    // reloaded fine.
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string file = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ file });
    dir.write("a.stl", 20);

    h.tick();
    REQUIRE(h.calls.size() == 1);

    h.tick();
    CHECK(h.calls.size() == 1);

    dir.write("a.stl", 30);
    h.tick();
    CHECK(h.calls.size() == 2);
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
    h.tick();
    CHECK(h.calls.empty());

    dir.write("a.stl", 20);
    h.tick();
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

    h.tick();

    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == std::set<std::string>{ b });
}

TEST_CASE("Every file in a reported batch has its baseline advanced", "[SourceFileWatcher]")
{
    // There's no partial-success bookkeeping in this design: a batch of files that changed
    // together is reported together, and every one of their baselines advances together, whether
    // or not the caller's reload actually managed all of them.
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string a = dir.write("a.stl", 10);
    const std::string b = dir.write("b.stl", 10);
    h.watcher.set_watched_files({ a, b });
    dir.write("a.stl", 20);
    dir.write("b.stl", 20);

    h.tick();
    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == std::set<std::string>{ a, b });

    h.tick();
    CHECK(h.calls.size() == 1);

    // Only one of them moves on; it alone is reported next.
    dir.write("a.stl", 30);
    h.tick();
    REQUIRE(h.calls.size() == 2);
    CHECK(h.calls[1] == std::set<std::string>{ a });
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
    h.tick();

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
    h.tick();

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
    h.tick();
    CHECK(h.calls.empty());

    // Tracked again: its baseline is taken from the file as it is now, not from before.
    h.watcher.set_watched_files({ a, b });
    h.tick();
    CHECK(h.calls.empty());
}

TEST_CASE("Clearing the watcher drops the baseline", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string a = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ a });
    dir.write("a.stl", 20);

    h.watcher.clear();
    h.tick();
    CHECK(h.calls.empty());

    h.watcher.set_watched_files({ a });
    h.tick();
    CHECK(h.calls.empty());
}

TEST_CASE("A change is not reported while a reload is already running", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    SourceFileWatcher watcher;
    const std::string file = dir.write("a.stl", 10);
    watcher.set_watched_files({ file });
    dir.write("a.stl", 20);

    int  calls  = 0;
    bool nested = false;
    watcher.set_on_changed([&](const std::set<std::string>&) {
        ++calls;
        // Stands in for a modal dialog or wxBusyInfo pumping the event loop mid-reload and letting
        // the debounce timer fire again: the callback must not be re-entered. The file changes
        // again first, so a nested tick that got past the guard would find something to report.
        if (!nested) {
            nested = true;
            dir.write("a.stl", 30);
            wxTimer timer;
            wxTimerEvent evt(timer);
            watcher.ProcessEvent(evt);
        }
    });

    wxTimer timer;
    wxTimerEvent evt(timer);
    watcher.ProcessEvent(evt);

    CHECK(calls == 1);
}

TEST_CASE("A change is held back while the UI is blocked and reported afterwards", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    Harness h;
    const std::string a = dir.write("a.stl", 10);
    h.watcher.set_watched_files({ a });
    dir.write("a.stl", 20);

    bool blocked = true;
    h.watcher.set_is_ui_blocked([&blocked]() { return blocked; });

    h.tick();
    CHECK(h.calls.empty());

    blocked = false;
    h.tick();
    REQUIRE(h.calls.size() == 1);
    CHECK(h.calls[0] == std::set<std::string>{ a });
}

TEST_CASE("Without a callback a change stays pending", "[SourceFileWatcher]")
{
    WxEnv wx;
    TempDir dir;
    SourceFileWatcher watcher;
    const std::string file = dir.write("a.stl", 10);
    watcher.set_watched_files({ file });
    dir.write("a.stl", 20);

    wxTimer timer;
    wxTimerEvent evt(timer);
    // No callback yet: changed_source_files() sees the change, but on_timer() returns without
    // committing it (see the early "!m_on_changed" return) -- so once a callback is set below, the
    // change is still there to report.
    watcher.ProcessEvent(evt);

    int calls = 0;
    watcher.set_on_changed([&](const std::set<std::string>&) { ++calls; });
    watcher.ProcessEvent(evt);

    CHECK(calls == 1);
}
