#ifndef slic3r_Format_ResourcePathUtils_hpp_
#define slic3r_Format_ResourcePathUtils_hpp_

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace resource_path {

inline std::string ascii_lower_copy(const std::string& value)
{
    std::string lowered;
    lowered.reserve(value.size());
    for (unsigned char ch : value)
        lowered.push_back(static_cast<char>(std::tolower(ch)));
    return lowered;
}

inline boost::filesystem::path portable_path_copy(const boost::filesystem::path& value)
{
    std::string portable = value.string();
    std::replace(portable.begin(), portable.end(), '\\', '/');
    return boost::filesystem::path(portable);
}

inline int hex_digit_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

// Byte-level percent decoding. Per RFC 3986 the %XX byte stream is expected to be
// UTF-8 when produced from URIs / Assimp aiString; this function performs no
// transcoding, so callers must treat both input and output as raw UTF-8 bytes.
inline std::string percent_decode_copy(const std::string& value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = hex_digit_value(value[i + 1]);
            const int lo = hex_digit_value(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i]);
    }
    return decoded;
}

inline std::string strip_file_uri_prefix_copy(const std::string& value)
{
    const std::string lower = ascii_lower_copy(value);
    if (lower.rfind("file://", 0) != 0)
        return value;

    std::string path = value.substr(7);
    if (ascii_lower_copy(path).rfind("localhost/", 0) == 0)
        path.erase(0, std::string("localhost").size());
    else if (!path.empty() && path.front() != '/')
        path = "//" + path;

    // file:///C:/... should become C:/..., while file:///tmp/... keeps /tmp/...
    if (path.size() >= 3 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':')
        path.erase(path.begin());
    return path;
}

inline bool file_uri_has_remote_authority(const std::string& value)
{
    const std::string lower = ascii_lower_copy(value);
    if (lower.rfind("file://", 0) != 0)
        return false;

    const std::string path = value.substr(7);
    if (path.empty() || path.front() == '/')
        return false;

    const std::size_t slash = path.find('/');
    const std::string authority = path.substr(0, slash);
    return ascii_lower_copy(authority) != "localhost";
}

inline bool looks_like_windows_absolute_path(const boost::filesystem::path& path)
{
    const std::string portable = portable_path_copy(path).string();
    return portable.size() >= 3
        && std::isalpha(static_cast<unsigned char>(portable[0]))
        && portable[1] == ':'
        && portable[2] == '/';
}

inline boost::filesystem::path filename_from_portable_path(const boost::filesystem::path& value)
{
    const boost::filesystem::path portable = portable_path_copy(value);
    return portable.filename();
}

inline boost::filesystem::path find_child_case_insensitive(
    const boost::filesystem::path& directory,
    const boost::filesystem::path& requested_name,
    const char* context)
{
    if (!boost::filesystem::exists(directory) || !boost::filesystem::is_directory(directory))
        return {};

    const std::string requested_lower = ascii_lower_copy(requested_name.filename().string());
    std::vector<boost::filesystem::path> matches;

    boost::system::error_code ec;
    for (boost::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (ascii_lower_copy(it->path().filename().string()) == requested_lower)
            matches.push_back(it->path());
    }

    if (matches.size() == 1)
        return matches.front();

    if (matches.size() > 1) {
        BOOST_LOG_TRIVIAL(warning) << context << ": ambiguous case-insensitive resource match for "
                                   << requested_name << " in " << directory;
    }

    return {};
}

inline boost::filesystem::path resolve_existing_path_case_insensitive(
    const boost::filesystem::path& requested_path,
    const char* context = "resource_path")
{
    const boost::filesystem::path normalized_path = portable_path_copy(requested_path);

    if (normalized_path.empty())
        return {};

    if (boost::filesystem::exists(normalized_path))
        return normalized_path;

    boost::filesystem::path current;
    bool initialized = false;

    for (const boost::filesystem::path& part : normalized_path) {
        if (part == normalized_path.root_name() || part == normalized_path.root_directory()) {
            current /= part;
            initialized = true;
            continue;
        }

        if (!initialized) {
            current = boost::filesystem::current_path();
            initialized = true;
        }

        boost::filesystem::path exact = current / part;
        if (boost::filesystem::exists(exact)) {
            current = exact;
            continue;
        }

        boost::filesystem::path matched = find_child_case_insensitive(current, part, context);
        if (matched.empty())
            return {};

        BOOST_LOG_TRIVIAL(info) << context << ": resolved resource path case-insensitively from "
                                << exact << " to " << matched;
        current = matched;
    }

    return boost::filesystem::exists(current) ? current : boost::filesystem::path();
}

inline boost::filesystem::path resolve_existing_relative_path_case_insensitive(
    const boost::filesystem::path& base_dir,
    const boost::filesystem::path& resource_path,
    const char* context = "resource_path")
{
    const boost::filesystem::path requested = resource_path.is_absolute() ? resource_path : base_dir / resource_path;
    return resolve_existing_path_case_insensitive(requested, context);
}

// Resolve a resource path that originated outside our own code (e.g. a glTF/FBX
// material texture reference or a file:// URI inside a 3MF descriptor).
//
// `raw_path` is expected to be UTF-8 regardless of host platform: file URIs are
// UTF-8 by spec, and Assimp aiString uses UTF-8 internally. Cross-platform
// correctness on Windows additionally relies on the process having called
// boost::nowide::nowide_filesystem() during startup (see src/BambuStudio.cpp),
// which imbues boost::filesystem::path with a UTF-8 codecvt so that
// `path(std::string)` constructs from UTF-8 byte sequences. Callers that bypass
// the main entry point (standalone CLI tools, unit tests) must reproduce that
// setup themselves before invoking this helper.
inline boost::filesystem::path resolve_external_resource_path(
    const boost::filesystem::path& base_dir,
    const std::string& raw_path,
    const char* context = "resource_path",
    bool allow_basename_fallback = true)
{
    if (raw_path.empty())
        return {};

    const bool remote_file_uri = file_uri_has_remote_authority(raw_path);
    const std::string decoded_path = percent_decode_copy(strip_file_uri_prefix_copy(raw_path));
    const boost::filesystem::path requested = portable_path_copy(boost::filesystem::path(decoded_path));

    boost::filesystem::path resolved = (requested.is_absolute() || looks_like_windows_absolute_path(requested)) ?
        resolve_existing_path_case_insensitive(requested, context) :
        resolve_existing_relative_path_case_insensitive(base_dir, requested, context);
    if (!resolved.empty())
        return resolved;

    if (!allow_basename_fallback || remote_file_uri)
        return {};

    const boost::filesystem::path basename = filename_from_portable_path(requested);
    if (basename.empty())
        return {};

    resolved = resolve_existing_relative_path_case_insensitive(base_dir, basename, context);
    if (!resolved.empty()) {
        BOOST_LOG_TRIVIAL(info) << context << ": resolved resource by basename from "
                                << requested << " to " << resolved;
    }
    return resolved;
}

} // namespace resource_path
} // namespace Slic3r

#endif /* slic3r_Format_ResourcePathUtils_hpp_ */
