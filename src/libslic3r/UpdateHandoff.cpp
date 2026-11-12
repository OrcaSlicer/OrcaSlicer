#include "UpdateHandoff.hpp"

#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/directory.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <ios>
#include <iterator>
#include <string>
#include <vector>

#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

#ifdef _WIN32
#include <boost/nowide/cstdlib.hpp>
#endif

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include "Utils.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Slic3r {

namespace {

namespace fs = boost::filesystem;
using nlohmann::json;

std::string text_field(const json& object, const char* key)
{
    auto it = object.find(key);
    if (it == object.end() || !it->is_string())
        return std::string();
    return it->get<std::string>();
}

int64_t integer_field(const json& object, const char* key)
{
    auto it = object.find(key);
    if (it == object.end() || !it->is_number())
        return 0;
    return it->get<int64_t>();
}

std::string& cache_dir_override()
{
    static std::string dir;
    return dir;
}

// The OS cache directory, resolved without wx: the application resolves the
// cache with wx and passes it in the handoff, so this is only reached by tests and
// by a helper started without one. Nothing here may fall back to the data
// directory, which the updater is not supposed to write to.
std::string os_cache_root()
{
#ifdef _WIN32
    // nowide: a profile path such as C:\Users\José\AppData is not representable in the
    // ANSI bytes std::getenv returns, and every path the updater handles is UTF-8.
    if (const char* local = boost::nowide::getenv("LOCALAPPDATA"); local != nullptr && local[0] != '\0')
        return (fs::path(local) / "OrcaSlicer" / "updater").string();
#elif defined __APPLE__
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
        return (fs::path(home) / "Library" / "Caches" / "OrcaSlicer" / "updater").string();
#else
    if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache != nullptr && cache[0] != '\0')
        return (fs::path(cache) / "OrcaSlicer" / "updater").string();
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
        return (fs::path(home) / ".cache" / "OrcaSlicer" / "updater").string();
#endif
    boost::system::error_code ec;
    const fs::path          temp = fs::temp_directory_path(ec);
    if (!ec)
        return (temp / "OrcaSlicer" / "updater").string();
    return "OrcaSlicer-updater";
}

std::string cache_root()
{
    const std::string& override_dir = cache_dir_override();
    if (!override_dir.empty())
        return override_dir;
    return os_cache_root();
}

void ensure_parent_dir(const std::string& path)
{
    const fs::path parent = fs::path(path).parent_path();
    if (parent.empty())
        return;
    boost::system::error_code ec;
    fs::create_directories(parent, ec);
}

bool read_file(const std::string& path, std::string& out)
{
    boost::nowide::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

bool write_file_atomic(const std::string& path, const std::string& payload)
{
    ensure_parent_dir(path);
    return !write_file_atomically(path, payload, /*binary=*/true);
}

void remove_file(const std::string& path)
{
    boost::system::error_code ec;
    fs::remove(path, ec);
}

bool read_pid(const std::string& path, int64_t& pid)
{
    std::string text;
    if (!read_file(path, text))
        return false;
    char*      end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || value <= 0)
        return false;
    pid = static_cast<int64_t>(value);
    return true;
}

bool process_is_alive(int64_t pid)
{
    if (pid <= 0)
        return false;
#ifdef _WIN32
    HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr)
        return false;
    DWORD exit_code = 0;
    const bool alive = ::GetExitCodeProcess(handle, &exit_code) != 0 && exit_code == STILL_ACTIVE;
    ::CloseHandle(handle);
    return alive;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
        return true;
    return errno == EPERM;
#endif
}

int64_t current_process_id()
{
#ifdef _WIN32
    return static_cast<int64_t>(::_getpid());
#else
    return static_cast<int64_t>(::getpid());
#endif
}

bool create_lock_file(const std::string& path, int64_t owner_pid)
{
    const std::string text = std::to_string(owner_pid);
#ifdef _WIN32
    const int fd = ::_wopen(boost::nowide::widen(path).c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
#endif
    if (fd < 0)
        return false;
#ifdef _WIN32
    const int written = ::_write(fd, text.data(), static_cast<unsigned int>(text.size()));
    ::_close(fd);
#else
    const ssize_t written = ::write(fd, text.data(), text.size());
    ::close(fd);
#endif
    return written == static_cast<int>(text.size());
}

bool read_install_record_file(InstallRecord& out)
{
    std::string text;
    if (!read_file(updater_install_record_path(), text))
        return false;
    return install_record_from_json(text, out);
}

} // namespace

std::string updater_cache_dir()
{
    return cache_root();
}

void set_updater_cache_dir(const std::string& dir)
{
    cache_dir_override() = dir;
}

void clear_updater_cache_dir()
{
    cache_dir_override().clear();
}

std::string updater_download_dir()
{
    return (fs::path(cache_root()) / "downloads").string();
}

std::string updater_helper_dir()
{
    return (fs::path(cache_root()) / "helper").string();
}

std::string updater_handoff_path()
{
    return (fs::path(cache_root()) / "handoff.json").string();
}

std::string updater_install_record_path()
{
    return (fs::path(cache_root()) / "installed.json").string();
}

std::string updater_pending_path()
{
    return (fs::path(cache_root()) / "pending.json").string();
}

std::string updater_lock_path()
{
    return (fs::path(cache_root()) / "install.lock").string();
}

std::string updater_log_path()
{
    return (fs::path(cache_root()) / "updater.log").string();
}

DownloadPrep prepare_download_dir(const std::string& dir,
                                  const std::string& asset_name,
                                  const std::string& digest,
                                  int64_t            expected_size)
{
    DownloadPrep      prep;
    const fs::path    directory(dir);
    const std::string name   = fs::path(asset_name).filename().string();
    const std::string keep   = name.empty() ? std::string("update.part") : name;
    const fs::path    part   = directory / (keep + ".part");
    const fs::path    marker = directory / (keep + ".part.digest");

    boost::system::error_code ec;
    if (!fs::is_directory(directory, ec))
        fs::create_directories(directory, ec);

    // A .part is resumed only for the build it was started for, and only while it is
    // smaller than that build: anything else is a leftover.
    std::string recorded;
    if (!digest.empty() && fs::is_regular_file(part, ec) && read_file(marker.string(), recorded)) {
        while (!recorded.empty() && (recorded.back() == '\n' || recorded.back() == '\r' || recorded.back() == ' '))
            recorded.pop_back();
        prep.resumable = recorded == digest;
        if (prep.resumable && expected_size > 0) {
            const auto size = fs::file_size(part, ec);
            if (!ec) {
                prep.complete  = static_cast<int64_t>(size) == expected_size;
                prep.resumable = static_cast<int64_t>(size) < expected_size;
            }
        }
    }
    if (!prep.resumable && !prep.complete) {
        for (const fs::path& stale : {part, marker})
            if (fs::is_regular_file(stale, ec) && fs::remove(stale, ec))
                prep.dropped.push_back(stale.string());
    }

    // Whatever else sits here belongs to another release.
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path entry = it->path();
        if (!fs::is_regular_file(entry, ec))
            continue;
        if (entry == part || entry == marker)
            continue;
        if (fs::remove(entry, ec))
            prep.dropped.push_back(entry.string());
    }

    // Recorded before the download, so a later run knows which build the .part belongs to.
    // Not while an old .part survives: the download would resume on top of it.
    if (!prep.complete && !fs::exists(part, ec))
        (void) write_file_atomic(marker.string(), digest);
    return prep;
}

bool is_release_download_url(const std::string& url)
{
    static const std::string prefix = "https://github.com/OrcaSlicer/OrcaSlicer/releases/download/";
    return url.compare(0, prefix.size(), prefix) == 0;
}

std::string handoff_to_json(const UpdateHandoff& handoff)
{
    json j;
    j["channel"]       = handoff.channel;
    j["version"]       = handoff.version;
    j["version_label"] = handoff.version_label;
    j["asset_name"]    = handoff.asset_name;
    j["url"]           = handoff.url;
    j["digest"]        = handoff.digest;
    j["size"]          = handoff.size;
    j["flavour"]       = handoff.flavour;
    j["install_root"]  = handoff.install_root;
    j["exe_path"]      = handoff.exe_path;
    j["cache_dir"]     = handoff.cache_dir;
    j["parent_pid"]    = handoff.parent_pid;
    j["log_path"]      = handoff.log_path;
    j["lock_path"]     = handoff.lock_path;
    return j.dump();
}

bool handoff_from_json(const std::string& text, UpdateHandoff& out)
{
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception&) {
        return false;
    }
    if (!j.is_object())
        return false;

    out.channel       = text_field(j, "channel");
    out.version       = text_field(j, "version");
    out.version_label = text_field(j, "version_label");
    out.asset_name    = text_field(j, "asset_name");
    out.url           = text_field(j, "url");
    out.digest        = text_field(j, "digest");
    out.size          = integer_field(j, "size");
    out.flavour       = text_field(j, "flavour");
    out.install_root  = text_field(j, "install_root");
    out.exe_path      = text_field(j, "exe_path");
    out.cache_dir     = text_field(j, "cache_dir");
    out.parent_pid    = integer_field(j, "parent_pid");
    out.log_path      = text_field(j, "log_path");
    out.lock_path     = text_field(j, "lock_path");
    return true;
}

bool write_handoff(const UpdateHandoff& handoff)
{
    return write_file_atomic(updater_handoff_path(), handoff_to_json(handoff));
}

bool read_handoff(UpdateHandoff& out)
{
    std::string text;
    if (!read_file(updater_handoff_path(), text))
        return false;
    return handoff_from_json(text, out);
}

void remove_handoff()
{
    remove_file(updater_handoff_path());
}

std::string install_record_to_json(const InstallRecord& record)
{
    json j;
    j["digest"]  = record.digest;
    j["channel"] = record.channel;
    j["version"] = record.version;
    return j.dump();
}

bool install_record_from_json(const std::string& text, InstallRecord& out)
{
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception&) {
        return false;
    }
    if (!j.is_object())
        return false;

    out.digest  = text_field(j, "digest");
    out.channel = text_field(j, "channel");
    out.version = text_field(j, "version");
    return true;
}

bool write_install_record(const InstallRecord& record)
{
    return write_file_atomic(updater_install_record_path(), install_record_to_json(record));
}

bool take_install_record(InstallRecord& out)
{
    InstallRecord record;
    if (!read_install_record_file(record))
        return false;
    out = record;
    remove_file(updater_install_record_path());
    return true;
}

std::string pending_install_to_json(const PendingInstall& pending)
{
    json j;
    j["backup"]  = pending.backup;
    j["target"]  = pending.target;
    j["digest"]  = pending.digest;
    j["channel"] = pending.channel;
    j["version"]    = pending.version;
    j["data_aside"] = pending.data_aside;
    return j.dump();
}

bool pending_install_from_json(const std::string& text, PendingInstall& out)
{
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception&) {
        return false;
    }
    if (!j.is_object())
        return false;

    out.backup  = text_field(j, "backup");
    out.target  = text_field(j, "target");
    out.digest  = text_field(j, "digest");
    out.channel = text_field(j, "channel");
    out.version    = text_field(j, "version");
    out.data_aside = text_field(j, "data_aside");
    return true;
}

bool write_pending_install(const PendingInstall& pending)
{
    return write_file_atomic(updater_pending_path(), pending_install_to_json(pending));
}

bool read_pending_install(PendingInstall& out)
{
    std::string text;
    if (!read_file(updater_pending_path(), text))
        return false;
    return pending_install_from_json(text, out);
}

void remove_pending_install()
{
    remove_file(updater_pending_path());
}

bool acquire_install_lock(const std::string& path, int64_t owner_pid)
{
    ensure_parent_dir(path);
    if (create_lock_file(path, owner_pid))
        return true;

    int64_t existing_pid = 0;
    if (read_pid(path, existing_pid) && process_is_alive(existing_pid))
        return false;

    remove_file(path);
    return create_lock_file(path, owner_pid);
}

void release_install_lock(const std::string& path)
{
    int64_t held_pid = 0;
    if (!read_pid(path, held_pid))
        return;
    if (held_pid != current_process_id())
        return;
    remove_file(path);
}

} // namespace Slic3r
