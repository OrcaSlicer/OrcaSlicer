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

#include <fstream>
#include <functional>
#include <set>
#include <string>
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

// Owns a watcher plus a recording callback, and drives the debounce timer by hand: the timer's
// event is what the watcher acts on, so delivering it directly runs the same code as a real fire
// without needing an event loop or waiting out the debounce.
struct Harness
{
    SourceFileWatcher                   watcher;
    std::vector<std::set<std::string>>  calls;
    bool                                reload_succeeds{ true };

    Harness()
    {
        watcher.set_on_changed([this](const std::set<std::string>& files) {
            calls.push_back(files);
            return reload_succeeds;
        });
    }

    void fire_timer()
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
    const std::string file = dir.write("a.stl", 10);
    watcher.set_watched_files({ file });
    dir.write("a.stl", 20);

    int  calls  = 0;
    bool nested = false;
    watcher.set_on_changed([&](const std::set<std::string>&) {
        ++calls;
        // Stands in for a modal dialog or wxBusyInfo pumping the event loop mid-reload and letting
        // the debounce timer fire again: the callback must not be re-entered.
        if (!nested) {
            nested = true;
            wxTimer timer;
            wxTimerEvent evt(timer);
            watcher.ProcessEvent(evt);
        }
        return true;
    });

    wxTimer timer;
    wxTimerEvent evt(timer);
    watcher.ProcessEvent(evt);

    CHECK(calls == 1);
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
    watcher.ProcessEvent(evt);

    int calls = 0;
    watcher.set_on_changed([&](const std::set<std::string>&) { ++calls; return true; });
    watcher.ProcessEvent(evt);

    CHECK(calls == 1);
}
