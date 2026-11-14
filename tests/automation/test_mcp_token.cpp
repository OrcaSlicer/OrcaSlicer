#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Mcp/McpToken.hpp"
#include "test_utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <string>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

using namespace Slic3r::GUI::Mcp;
namespace fs = boost::filesystem;

namespace {

// Above every platform's pid range, so no process has it.
constexpr int kNoSuchPid = 0x7FFFFFF0;

int this_pid()
{
#ifdef _WIN32
    return int(GetCurrentProcessId());
#else
    return int(getpid());
#endif
}

// A running process other than this one: the one that started the test run.
int other_running_pid()
{
#ifdef _WIN32
    const DWORD     self     = GetCurrentProcessId();
    HANDLE          snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        if (entry.th32ProcessID == self) {
            parent = entry.th32ParentProcessID;
            break;
        }
    CloseHandle(snapshot);
    return int(parent);
#else
    return int(getppid());
#endif
}

void write_server(const fs::path& datadir, int port, int pid)
{
    std::string err;
    REQUIRE(write_discovery(datadir, {{"port", port}, {"pid", pid}}, err));
}

} // namespace

TEST_CASE("the discovery file is removed only by the process that wrote it", "[Mcp]")
{
    ScopedTemporaryDir datadir("orca-mcp");

    SECTION("another window's file stays")
    {
        write_server(datadir.path(), 13619, other_running_pid());
        remove_discovery(datadir.path());
        CHECK(fs::exists(discovery_path(datadir.path())));
    }
    SECTION("this window's file goes")
    {
        write_server(datadir.path(), 13619, this_pid());
        remove_discovery(datadir.path());
        CHECK_FALSE(fs::exists(discovery_path(datadir.path())));
    }
    SECTION("a file that names no owner goes")
    {
        std::string err;
        mcp_dir(datadir.path());
        REQUIRE(write_private_file(discovery_path(datadir.path()), "not json", err));
        remove_discovery(datadir.path());
        CHECK_FALSE(fs::exists(discovery_path(datadir.path())));
    }
}

TEST_CASE("a port is held by another window only when the discovery file names it and a running process", "[Mcp]")
{
    ScopedTemporaryDir datadir("orca-mcp");
    const int          port = 13619;

    CHECK_FALSE(discovery_held_by_other(datadir.path(), port));

    write_server(datadir.path(), port, other_running_pid());
    CHECK(discovery_held_by_other(datadir.path(), port));
    CHECK_FALSE(discovery_held_by_other(datadir.path(), port + 1));

    write_server(datadir.path(), port, this_pid());
    CHECK_FALSE(discovery_held_by_other(datadir.path(), port));

    // A window that crashed leaves its file behind.
    write_server(datadir.path(), port, kNoSuchPid);
    CHECK_FALSE(discovery_held_by_other(datadir.path(), port));
}
