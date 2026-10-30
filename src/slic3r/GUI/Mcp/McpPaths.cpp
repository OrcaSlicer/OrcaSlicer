#include "McpPaths.hpp"
#include "McpToken.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

namespace fs = boost::filesystem;

fs::path path_from_utf8(const std::string& utf8)
{
#ifdef _WIN32
    return fs::path(boost::nowide::widen(utf8));
#else
    return fs::path(utf8);
#endif
}

std::string path_to_utf8(const fs::path& path)
{
#ifdef _WIN32
    return boost::nowide::narrow(path.wstring());
#else
    return path.string();
#endif
}

std::string path_compare_key(const fs::path& path)
{
#if defined(_WIN32)
    std::wstring wide = path.lexically_normal().wstring();
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    // Upper case by the file system's rules (LCMapStringEx's default, unless asked for linguistic casing),
    // as NTFS compares names: one character to one, so "ß" stays "ß" as it does on NTFS. The size is
    // asked for first all the same.
    if (!wide.empty()) {
        const int size = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr, 0);
        if (size > 0) {
            std::wstring upper(std::size_t(size), L'\0');
            if (::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data(), int(wide.size()), upper.data(), size, nullptr, nullptr,
                                0) == size)
                wide = std::move(upper);
        }
    }
    return boost::nowide::narrow(wide);
#elif defined(__APPLE__)
    const std::string utf8 = path.lexically_normal().string();
    CFStringRef       text = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8.data()), CFIndex(utf8.size()),
                                                     kCFStringEncodingUTF8, false);
    if (text == nullptr)
        return utf8;
    CFMutableStringRef folded = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, text);
    CFRelease(text);
    if (folded == nullptr)
        return utf8;
    CFStringNormalize(folded, kCFStringNormalizationFormD);
    CFStringFold(folded, kCFCompareCaseInsensitive, nullptr);
    const CFIndex     length = CFStringGetLength(folded);
    CFIndex           bytes  = 0;
    CFStringGetBytes(folded, CFRangeMake(0, length), kCFStringEncodingUTF8, 0, false, nullptr, 0, &bytes);
    std::string out(std::size_t(bytes), '\0');
    CFStringGetBytes(folded, CFRangeMake(0, length), kCFStringEncodingUTF8, 0, false, reinterpret_cast<UInt8*>(out.data()), bytes, nullptr);
    CFRelease(folded);
    return out;
#else
    return path.lexically_normal().string();
#endif
}

namespace {

std::optional<fs::path> absolute(const std::string& path, std::string& err)
{
    if (path.empty()) {
        err = "path is empty";
        return std::nullopt;
    }
    fs::path p = path_from_utf8(path);
    if (!p.is_absolute()) {
        err = "path must be absolute: " + path;
        return std::nullopt;
    }
    return p.lexically_normal();
}

} // namespace

std::optional<fs::path> resolve_input_path(const std::string& path, std::string& err)
{
    auto p = absolute(path, err);
    if (!p)
        return std::nullopt;
    boost::system::error_code ec;
    if (!fs::exists(*p, ec)) {
        err = "file not found: " + path;
        return std::nullopt;
    }
    return p;
}

std::optional<fs::path> resolve_output_path(const std::string& path, bool overwrite, std::string& err)
{
    auto p = absolute(path, err);
    if (!p)
        return std::nullopt;
    boost::system::error_code ec;
    if (p->filename().empty() || fs::is_directory(*p, ec)) {
        err = "path names a directory, not a file: " + path;
        return std::nullopt;
    }
    if (!fs::is_directory(p->parent_path(), ec)) {
        err = "directory does not exist: " + path_to_utf8(p->parent_path());
        return std::nullopt;
    }
    if (fs::exists(*p, ec) && !overwrite) {
        err = "file exists (pass overwrite: true to replace it): " + path;
        return std::nullopt;
    }
    return p;
}

std::optional<fs::path> resolve_output_dir(const std::string& path, std::string& err)
{
    auto p = absolute(path, err);
    if (!p)
        return std::nullopt;
    boost::system::error_code ec;
    if (!fs::is_directory(*p, ec)) {
        err = "directory does not exist: " + path;
        return std::nullopt;
    }
    return p;
}

std::optional<PathParam> parse_path_param(const std::string& param, std::string& err)
{
    PathParam out;
    const auto first = param.find_first_not_of(" \t");
    if (first != std::string::npos && param[first] == '{') {
        const nlohmann::json j = nlohmann::json::parse(param, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("path") || !j["path"].is_string()) {
            err = "the parameter must be a path or {\"path\": \"...\", \"overwrite\": true}";
            return std::nullopt;
        }
        out.path      = j["path"].get<std::string>();
        out.overwrite = j.value("overwrite", false);
    } else {
        out.path = param;
    }
    if (out.path.empty()) {
        err = "path is empty";
        return std::nullopt;
    }
    return out;
}

fs::path output_dir()
{
    const fs::path dir = mcp_dir(path_from_utf8(data_dir())) / "out";
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path timestamped_path(const fs::path& dir, const std::string& kind, const std::string& extension, const std::vector<std::string>& companions,
                          std::time_t when)
{
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&when));
    const std::string         stem = "OrcaSlicer-" + kind + "-" + buf;
    boost::system::error_code ec;
    for (int n = 1;; ++n) {
        const std::string name  = n == 1 ? stem : stem + "-" + std::to_string(n);
        const auto        taken = [&](const std::string& ext) { return fs::exists(dir / path_from_utf8(name + ext), ec); };
        if (!taken(extension) && std::none_of(companions.begin(), companions.end(), taken))
            return dir / path_from_utf8(name + extension);
    }
}

}}} // namespace Slic3r::GUI::Mcp
