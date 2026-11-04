#include "PluginWheelInstall.hpp"

#include "PluginFsUtils.hpp"
#include "PluginUv.hpp"
#include "PythonInterpreter.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <ios>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::plugin_wheel_install {
namespace {

namespace fs = boost::filesystem;

bool is_tag_char(char ch)
{
    const unsigned char uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) || ch == '.' || ch == '_' || ch == '+';
}

// PEP 427: runs of characters outside [A-Za-z0-9._] become a single '_' in the escaped name.
std::string escape_wheel_name(const std::string& name)
{
    std::string escaped;
    escaped.reserve(name.size());
    bool pending_separator = false;
    for (unsigned char ch : name) {
        if (std::isalnum(ch) || ch == '.' || ch == '_') {
            if (pending_separator && !escaped.empty())
                escaped += '_';
            pending_separator = false;
            escaped += static_cast<char>(ch);
        } else {
            pending_separator = true;
        }
    }
    return escaped;
}

// Versions keep the characters a PEP 440 version may contain; everything else collapses to '_'.
std::string escape_wheel_version(const std::string& version)
{
    std::string escaped;
    escaped.reserve(version.size());
    bool pending_separator = false;
    for (unsigned char ch : version) {
        if (std::isalnum(ch) || ch == '.' || ch == '+' || ch == '!') {
            if (pending_separator && !escaped.empty())
                escaped += '_';
            pending_separator = false;
            escaped += static_cast<char>(ch);
        } else {
            pending_separator = true;
        }
    }
    return escaped;
}

std::vector<std::string> split_tag(const std::string& tag)
{
    std::vector<std::string> parts;
    std::string              part;
    for (char ch : tag) {
        if (ch == '-') {
            parts.push_back(part);
            part.clear();
        } else {
            part += ch;
        }
    }
    parts.push_back(part);
    return parts;
}

bool starts_with_digits_after(const std::string& value, const char* prefix)
{
    const std::string prefix_string(prefix);
    if (value.compare(0, prefix_string.size(), prefix_string) != 0)
        return false;
    return value.size() > prefix_string.size() && std::isdigit(static_cast<unsigned char>(value[prefix_string.size()])) != 0;
}

bool looks_like_python_tag(const std::string& tag)
{
    return starts_with_digits_after(tag, "py") || starts_with_digits_after(tag, "cp") || starts_with_digits_after(tag, "pp") ||
           tag.compare(0, 7, "graalpy") == 0;
}

bool looks_like_abi_tag(const std::string& tag)
{
    return tag == "none" || tag == "abi3" || starts_with_digits_after(tag, "cp") || starts_with_digits_after(tag, "pp") ||
           tag.compare(0, 6, "pypy") == 0;
}

bool looks_like_platform_tag(const std::string& tag)
{
    if (tag == "any")
        return true;

    static constexpr const char* const tokens[] = {"manylinux", "musllinux", "linux",  "win",    "macosx", "freebsd",
                                                   "android",   "ios",       "openbsd", "netbsd", "solaris", "aix"};
    for (const char* token : tokens) {
        if (tag.find(token) != std::string::npos)
            return true;
    }
    return false;
}

// True when the three tag components name a real Python/ABI/platform combination shape, not just
// three dash-separated words. This is what separates a wheel filename from a generated download
// name (cloud_plugin-<random-hex>.whl also splits into five components).
bool looks_like_wheel_tags(const std::string& python_tag, const std::string& abi, const std::string& platform)
{
    return looks_like_python_tag(python_tag) && looks_like_abi_tag(abi) && looks_like_platform_tag(platform);
}

bool entry_package_present(const fs::path& target, const std::string& entry_package)
{
    if (entry_package.empty())
        return false;

    // Dotted import names ("pkg.sub") map onto nested components; the wheel installs
    // top-level distributions, so this is only a sanity check that the root exists.
    fs::path relative;
    std::string component;
    for (char ch : entry_package) {
        if (ch == '.') {
            relative /= component;
            component.clear();
        } else {
            component += ch;
        }
    }
    relative /= component;

    if (fs::is_directory(target / relative))
        return true;
    if (fs::is_regular_file(target / relative.parent_path() / (relative.filename().string() + ".py")))
        return true;

    // A compiled top-level module (<pkg>.cpython-312-x86_64-linux-gnu.so / <pkg>.pyd).
    const fs::path parent = target / relative.parent_path();
    if (fs::is_directory(parent)) {
        const std::string leaf = relative.filename().string() + ".";
        for (fs::directory_iterator it(parent); it != fs::directory_iterator(); ++it) {
            if (!fs::is_regular_file(it->status()))
                continue;
            const std::string filename = it->path().filename().string();
            if (filename.rfind(leaf, 0) != 0)
                continue;
            const std::string ext = it->path().extension().string();
            if (ext == ".so" || ext == ".pyd" || ext == ".dll")
                return true;
        }
    }

    return false;
}

// 64-bit FNV-1a over the wheel bytes. The marker only needs to detect a changed file; this is a
// cache hint, not a trust decision.
bool fingerprint_file(const fs::path& path, std::string& fingerprint, std::uint64_t& size, std::string& error)
{
    boost::nowide::ifstream file(path.string(), std::ios::binary);
    if (!file) {
        error = "Failed to read plugin wheel: " + path.string();
        return false;
    }

    std::uint64_t hash = 1469598103934665603ULL;
    std::uint64_t total = 0;
    char          buffer[64 * 1024];
    while (file) {
        file.read(buffer, sizeof(buffer));
        const std::streamsize read = file.gcount();
        for (std::streamsize i = 0; i < read; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ULL;
        }
        total += static_cast<std::uint64_t>(read);
    }
    if (file.bad()) {
        error = "Failed to read plugin wheel: " + path.string();
        return false;
    }

    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    fingerprint = hex;
    size        = total;
    return true;
}

void remove_tree_quietly(const fs::path& path)
{
    boost::system::error_code ec;
    fs::remove_all(path, ec);
    if (ec)
        BOOST_LOG_TRIVIAL(warning) << "Failed to remove plugin wheel installation directory " << path.string() << ": " << ec.message();
}

// Only staging directories are disposable. A `__wheel_prev__-*` directory can be the last usable
// target when a promotion failed and the restore failed; it is removed after a successful
// promotion, never guessed at by a later scan.
bool is_install_scratch_dir(const fs::path& path)
{
    return path.filename().string().rfind(STAGING_DIR_PREFIX, 0) == 0;
}

// Drop staging leftovers from an interrupted install. Discovery ignores them; they are only disk
// noise. Best effort: a failure to remove one is logged, not fatal.
void remove_stale_scratch_dirs(const fs::path& plugin_root)
{
    boost::system::error_code ec;
    if (!fs::is_directory(plugin_root, ec))
        return;

    std::vector<fs::path> stale;
    for (fs::directory_iterator it(plugin_root, ec); !ec && it != fs::directory_iterator(); ++it) {
        if (is_install_scratch_dir(it->path()))
            stale.push_back(it->path());
    }
    for (const fs::path& path : stale)
        remove_tree_quietly(path);
}

bool default_uv_runner(const std::vector<std::string>& args, std::chrono::seconds timeout, std::string& error)
{
    return plugin_uv::run(args, timeout, error);
}

} // namespace

bool is_valid_wheel_filename(const std::string& filename)
{
    constexpr const char* extension = ".whl";
    constexpr std::size_t extension_length = 4;

    if (filename.size() <= extension_length)
        return false;
    if (filename.compare(filename.size() - extension_length, extension_length, extension) != 0)
        return false;

    const std::string           stem = filename.substr(0, filename.size() - extension_length);
    const std::vector<std::string> parts = split_tag(stem);
    if (parts.size() != 5 && parts.size() != 6)
        return false;

    for (const std::string& part : parts) {
        if (part.empty())
            return false;
        if (std::any_of(part.begin(), part.end(), [](char ch) { return !is_tag_char(ch); }))
            return false;
    }

    // A PEP 440 version starts with a digit; the three trailing components are python, ABI and
    // platform tags.
    if (!std::isdigit(static_cast<unsigned char>(parts[1][0])))
        return false;

    const std::size_t tag_offset = parts.size() == 6 ? 3 : 2;
    return looks_like_wheel_tags(parts[tag_offset], parts[tag_offset + 1], parts[tag_offset + 2]);
}

std::string make_wheel_filename(const std::string& name, const std::string& version, const std::string& tag)
{
    const std::string escaped_name    = escape_wheel_name(name);
    const std::string escaped_version = escape_wheel_version(version);
    if (escaped_name.empty() || escaped_version.empty() || tag.empty())
        return {};

    // `tag` is the triple "python-abi-platform" from the WHEEL file, so it legitimately contains
    // the two '-' separators that join the filename components. Validate it as a triple here;
    // is_valid_wheel_filename() below applies the per-component character and tag-plausibility
    // rules to the assembled name.
    const std::vector<std::string> tag_parts = split_tag(tag);
    if (tag_parts.size() != 3)
        return {};

    const std::string filename = escaped_name + "-" + escaped_version + "-" + tag + ".whl";
    return is_valid_wheel_filename(filename) ? filename : std::string{};
}

namespace {

// Parse a CPython-style tag ("cp312", "cp38") into major/minor. A tag with only a major digit
// ("cp3") reports minor == -1, meaning "any minor of that major".
bool parse_cpython_tag(const std::string& tag, int& major, int& minor)
{
    if (tag.size() < 3 || tag.compare(0, 2, "cp") != 0)
        return false;

    std::size_t end = 2;
    while (end < tag.size() && std::isdigit(static_cast<unsigned char>(tag[end])) != 0)
        ++end;
    if (end == 2)
        return false;

    major = tag[2] - '0';
    minor = -1;
    if (end > 3) {
        int value = 0;
        for (std::size_t i = 3; i < end; ++i) {
            if (value > 100000)
                return false;
            value = value * 10 + (tag[i] - '0');
        }
        minor = value;
    }
    return true;
}

// True when a declared python tag (py3, py3.12, py2.py3, cp312, ...) can run on the bundled
// interpreter. PyPy tags (pp*) cannot.
bool python_tag_compatible(const std::string& python_tag, int major, int minor)
{
    std::size_t start = 0;
    while (start <= python_tag.size()) {
        const std::size_t dot       = python_tag.find('.', start);
        const std::string component = python_tag.substr(start, dot == std::string::npos ? std::string::npos : dot - start);

        if (component.compare(0, 2, "py") == 0 && component.size() > 2) {
            bool only_digits = true;
            for (std::size_t i = 2; i < component.size(); ++i) {
                if (std::isdigit(static_cast<unsigned char>(component[i])) == 0) {
                    only_digits = false;
                    break;
                }
            }
            if (only_digits) {
                const int component_major = component[2] - '0';
                int       component_minor = 0;
                for (std::size_t i = 3; i < component.size(); ++i)
                    component_minor = component_minor * 10 + (component[i] - '0');
                if (component_major == major && (component.size() == 3 || component_minor <= minor))
                    return true;
            }
        } else if (component.compare(0, 2, "cp") == 0) {
            int tag_major = 0, tag_minor = -1;
            if (parse_cpython_tag(component, tag_major, tag_minor) && tag_major == major &&
                (tag_minor < 0 || tag_minor <= minor))
                return true;
        }

        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return false;
}

// True when the declared ABI can be loaded by the bundled CPython. Pure wheels ("none") follow the
// python tag; "abi3" is the stable ABI up to the running minor; a concrete cpXY ABI must match.
// This is interpreter compatibility only: platform/architecture remains uv's decision.
bool abi_compatible(const std::string& python_tag,
                    const std::string& abi,
                    int                major,
                    int                minor,
                    const std::string& current_abi)
{
    if (abi == "none")
        return python_tag_compatible(python_tag, major, minor);

    if (abi == "abi3") {
        int tag_major = 0, tag_minor = -1;
        return parse_cpython_tag(python_tag, tag_major, tag_minor) && tag_major == major && tag_minor <= minor;
    }

    if (abi.compare(0, 2, "cp") == 0)
        return abi == current_abi && python_tag_compatible(python_tag, major, minor);

    return false;
}

} // namespace

std::string wheel_filename_tag(const std::vector<std::string>& tags)
{
    struct TagParts
    {
        std::string python;
        std::string abi;
        std::string platform;
    };

    std::vector<TagParts> valid;
    for (const std::string& tag : tags) {
        const std::vector<std::string> parts = split_tag(tag);
        if (parts.size() != 3 || parts[0].empty() || parts[1].empty() || parts[2].empty())
            continue;
        if (!looks_like_wheel_tags(parts[0], parts[1], parts[2]))
            continue;
        valid.push_back({parts[0], parts[1], parts[2]});
    }

    if (valid.empty())
        return {};

    // Prefer a tag the bundled interpreter can actually run. This is only a selection among the
    // tags the wheel declares; it never changes one. If no declared tag is interpreter-compatible,
    // keep the declared tags so uv can report the incompatibility clearly.
    std::vector<TagParts> compatible;
    {
        const std::string current_abi = PythonInterpreter::python_abi_tag();
        int               major = 0, minor = 0;
        if (parse_cpython_tag(current_abi, major, minor)) {
            for (const TagParts& tag : valid) {
                if (abi_compatible(tag.python, tag.abi, major, minor, current_abi))
                    compatible.push_back(tag);
            }
        }
    }
    const std::vector<TagParts>& candidates = compatible.empty() ? valid : compatible;

    // A declared universal tag covers every platform; prefer it over a platform-specific one.
    for (const TagParts& tag : candidates) {
        if (tag.platform == "any")
            return tag.python + "-" + tag.abi + "-any";
    }

    const TagParts& first = candidates.front();

    // Only combine tags that differ in nothing but a simple platform. Anything else (a different
    // interpreter or ABI tag, or an already-compressed platform) is left exactly as the wheel
    // declared it; uv decides whether the resulting filename is usable.
    for (const TagParts& tag : candidates) {
        if (tag.python != first.python || tag.abi != first.abi)
            return first.python + "-" + first.abi + "-" + first.platform;
        if (tag.platform.find('.') != std::string::npos)
            return first.python + "-" + first.abi + "-" + first.platform;
    }

    std::vector<std::string> platforms;
    for (const TagParts& tag : candidates) {
        if (std::find(platforms.begin(), platforms.end(), tag.platform) == platforms.end())
            platforms.push_back(tag.platform);
    }

    // A single tag, or a set that cannot be expressed as platform alternatives: keep the first tag.
    if (platforms.size() <= 1)
        return first.python + "-" + first.abi + "-" + first.platform;

    // PEP 425 compressed tag set: one python/abi with alternative platforms.
    std::string platform = platforms.front();
    for (std::size_t i = 1; i < platforms.size(); ++i)
        platform += "." + platforms[i];
    return first.python + "-" + first.abi + "-" + platform;
}

std::string install_dir(const std::string& plugin_root)
{
    return (fs::path(plugin_root) / INSTALL_DIR_NAME).string();
}

bool is_installed(const PluginDescriptor& descriptor, std::string* reason)
{
    if (reason != nullptr)
        reason->clear();

    auto fail = [reason](std::string message) {
        if (reason != nullptr)
            *reason = std::move(message);
        return false;
    };

    if (descriptor.plugin_root.empty())
        return fail("plugin has no installation directory");
    if (descriptor.entry_path.empty())
        return fail("plugin has no entry file");
    if (descriptor.entry_package.empty())
        return fail("plugin has no entry package");

    const fs::path target = fs::path(descriptor.plugin_root) / INSTALL_DIR_NAME;
    if (!fs::is_directory(target))
        return fail("no installed wheel target");

    const fs::path marker_path = target / MARKER_FILE_NAME;
    if (!fs::is_regular_file(marker_path))
        return fail("missing wheel installation marker");

    nlohmann::json marker;
    {
        boost::nowide::ifstream file(marker_path.string());
        if (!file)
            return fail("unreadable wheel installation marker");
        marker = nlohmann::json::parse(file, nullptr, false, true);
    }
    if (marker.is_discarded() || !marker.is_object())
        return fail("malformed wheel installation marker");

    const auto int_field    = [&marker](const char* key) -> long long {
        return marker.contains(key) && marker[key].is_number_integer() ? marker[key].get<long long>() : -1;
    };
    const auto size_field   = [&marker](const char* key, std::uint64_t& out) -> bool {
        if (!marker.contains(key) || !marker[key].is_number_integer())
            return false;
        if (marker[key].is_number_unsigned()) {
            out = marker[key].get<std::uint64_t>();
            return true;
        }
        const auto value = marker[key].get<long long>();
        if (value < 0)
            return false;
        out = static_cast<std::uint64_t>(value);
        return true;
    };
    const auto string_field = [&marker](const char* key) -> std::string {
        return marker.contains(key) && marker[key].is_string() ? marker[key].get<std::string>() : std::string{};
    };

    if (int_field("format") != INSTALL_FORMAT)
        return fail("installation format changed");
    if (string_field("abi") != PythonInterpreter::python_abi_tag())
        return fail("interpreter ABI changed");
    if (string_field("entry_package") != descriptor.entry_package)
        return fail("entry package changed");

    std::string    fingerprint;
    std::uint64_t  size = 0;
    std::string    fingerprint_error;
    if (!fingerprint_file(descriptor.entry_path, fingerprint, size, fingerprint_error))
        return fail(fingerprint_error);

    std::uint64_t recorded_size = 0;
    if (!size_field("size", recorded_size))
        return fail("malformed wheel installation marker");
    if (string_field("fingerprint") != fingerprint || recorded_size != size)
        return fail("wheel content changed");

    if (!entry_package_present(target, descriptor.entry_package))
        return fail("installed target is incomplete");

    return true;
}

bool ensure_installed(const PluginDescriptor& descriptor, std::string& error, const UvRunner& runner)
{
    error.clear();

    if (descriptor.plugin_root.empty()) {
        error = "Plugin has no installation directory.";
        return false;
    }
    if (descriptor.entry_path.empty()) {
        error = "Plugin has no entry file.";
        return false;
    }
    if (fs::path(descriptor.entry_path).extension() != ".whl") {
        error = "uv wheel installation is only available for .whl plugins.";
        return false;
    }
    if (!descriptor.is_metadata_valid()) {
        error = "Plugin metadata is invalid; its wheel cannot be installed.";
        return false;
    }
    if (descriptor.entry_package.empty()) {
        error = "Wheel does not identify an importable entry package.";
        return false;
    }

    std::string reuse_reason;
    if (is_installed(descriptor, &reuse_reason)) {
        BOOST_LOG_TRIVIAL(info) << "[plugin_wheel_install] reusing installation for " << descriptor.plugin_key << " ("
                                << install_dir(descriptor.plugin_root) << ")";
        return true;
    }

    // Wheel installs mutate a per-plugin directory; serialize them so two plugin loads cannot
    // interleave staging/promotion (load already serializes, this also covers direct callers).
    static std::mutex install_mutex;
    std::lock_guard<std::mutex> install_lock(install_mutex);

    // Another caller may have completed the same install while this one waited.
    if (is_installed(descriptor, &reuse_reason)) {
        BOOST_LOG_TRIVIAL(info) << "[plugin_wheel_install] reusing installation for " << descriptor.plugin_key << " ("
                                << install_dir(descriptor.plugin_root) << ")";
        return true;
    }

    const fs::path plugin_root = fs::path(descriptor.plugin_root);
    const fs::path entry       = fs::path(descriptor.entry_path);
    const fs::path target      = plugin_root / INSTALL_DIR_NAME;

    std::string staged_name = entry.filename().string();
    if (!is_valid_wheel_filename(staged_name)) {
        // Cloud downloads and side-loaded wheels can carry names uv rejects. Stage a copy under a
        // name derived from the wheel's own metadata; the stored wheel is left untouched.
        WheelInstallMetadata metadata;
        std::string           metadata_error;
        if (!read_wheel_install_metadata(entry, metadata, metadata_error)) {
            error = "Failed to read wheel metadata for installation: " + metadata_error;
            return false;
        }

        const std::string tag = wheel_filename_tag(metadata.tags);
        if (tag.empty()) {
            error = "Wheel " + entry.filename().string() +
                    " has no usable filename tag and declares no well-formed python-abi-platform tag in its WHEEL file.";
            return false;
        }
        staged_name = make_wheel_filename(metadata.name, metadata.version, tag);
        if (staged_name.empty()) {
            error = "Cannot build a valid wheel filename from the metadata of " + entry.filename().string() + ".";
            return false;
        }
    }

    remove_stale_scratch_dirs(plugin_root);

    boost::system::error_code ec;
    const std::string        unique        = fs::unique_path("%%%%-%%%%-%%%%").string();
    const fs::path           staging_root  = plugin_root / (std::string(STAGING_DIR_PREFIX) + unique);
    const fs::path           staging_input = staging_root / "input";
    const fs::path           staging_target = staging_root / "target";

    auto fail = [&error, &staging_root](const std::string& message) {
        remove_tree_quietly(staging_root);
        error = message;
        return false;
    };

    fs::create_directories(staging_input, ec);
    if (ec)
        return fail("Failed to create wheel staging directory " + staging_input.string() + ": " + ec.message());

    const fs::path staged_wheel = staging_input / staged_name;
    fs::copy_file(entry, staged_wheel, ec);
    if (ec)
        return fail("Failed to stage plugin wheel " + entry.string() + ": " + ec.message());

    const std::string python_executable = PythonInterpreter::bundled_python_executable();
    if (python_executable.empty())
        return fail("Bundled Python executable not found. Plugin wheel installation is unavailable.");

    const std::vector<std::string> args = {"--no-config",
                                           "pip",
                                           "install",
                                           "--python",
                                           python_executable,
                                           "--no-python-downloads",
                                           "--no-deps",
                                           "--no-index",
                                           "--no-cache",
                                           "--link-mode",
                                           "copy",
                                           "--target",
                                           staging_target.string(),
                                           staged_wheel.string()};

    BOOST_LOG_TRIVIAL(info) << "[plugin_wheel_install] installing " << staged_name << " for " << descriptor.plugin_key
                            << " into " << staging_target.string();

    const UvRunner uv_runner = runner ? runner : UvRunner(default_uv_runner);
    std::string    uv_error;
    if (!uv_runner(args, INSTALL_TIMEOUT, uv_error)) {
        if (uv_error.empty())
            uv_error = "uv pip install failed";
        return fail("Failed to install plugin wheel: " + uv_error);
    }

    if (!fs::is_directory(staging_target, ec))
        return fail("uv reported success but did not create the plugin target.");
    if (!entry_package_present(staging_target, descriptor.entry_package))
        return fail("uv reported success but did not install the entry package '" + descriptor.entry_package + "'.");

    std::string   fingerprint;
    std::uint64_t wheel_size = 0;
    std::string   fingerprint_error;
    if (!fingerprint_file(entry, fingerprint, wheel_size, fingerprint_error))
        return fail(fingerprint_error);

    {
        nlohmann::json marker;
        marker["format"]        = INSTALL_FORMAT;
        marker["wheel"]         = entry.filename().string();
        marker["size"]          = wheel_size;
        marker["fingerprint"]   = fingerprint;
        marker["abi"]           = PythonInterpreter::python_abi_tag();
        marker["entry_package"] = descriptor.entry_package;

        boost::nowide::ofstream file((staging_target / MARKER_FILE_NAME).string());
        if (!file)
            return fail("Failed to write the wheel installation marker.");
        file << marker.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        if (!file)
            return fail("Failed to write the wheel installation marker.");
    }

    // Promote only a complete target. The previous target is retired under a name discovery
    // ignores and is restored if promotion fails; it is never deleted before the new one is live.
    const fs::path previous = plugin_root / (std::string(PREVIOUS_DIR_PREFIX) + unique);
    const bool     had_target = fs::is_directory(target, ec);
    if (had_target) {
        fs::rename(target, previous, ec);
        if (ec) {
            std::string message = "Failed to move the previous wheel installation aside: " + ec.message();
            remove_tree_quietly(staging_root);
            error = std::move(message);
            return false;
        }
    }

    fs::rename(staging_target, target, ec);
    if (ec) {
        std::string message = "Failed to activate the new wheel installation: " + ec.message();
        if (had_target) {
            boost::system::error_code restore_ec;
            fs::rename(previous, target, restore_ec);
            if (restore_ec) {
                // Keep the retired target: it may still be the only usable copy.
                message += " The previous installation could not be restored and was kept at " + previous.string() + ": " +
                           restore_ec.message();
            }
        }
        remove_tree_quietly(staging_root);
        error = std::move(message);
        return false;
    }

    // staging_target moved into place; this removes the input copy and the empty staging wrapper.
    remove_tree_quietly(staging_root);

    if (had_target)
        remove_tree_quietly(previous);

    BOOST_LOG_TRIVIAL(info) << "[plugin_wheel_install] installed " << entry.filename().string() << " for "
                            << descriptor.plugin_key << " into " << target.string();
    return true;
}

} // namespace Slic3r::plugin_wheel_install
