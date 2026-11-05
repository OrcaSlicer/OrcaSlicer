#include "PluginFsUtils.hpp"

#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include "PluginAuditManager.hpp"
#include "PythonInterpreter.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

#include <exception>
#include <miniz.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include "slic3r/plugin/PluginDescriptor.hpp"
#include <utility>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ios>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace Slic3r {

const char* const INSTALL_STATE_FILE = ".install_state.json";

std::string get_orca_plugins_dir()
{
    namespace fs = boost::filesystem;
    return (fs::path(data_dir()) / "orca_plugins").string();
}

std::string get_cloud_plugin_dir(const std::string& user_id)
{
    namespace fs = boost::filesystem;
    return (fs::path(get_orca_plugins_dir()) / PLUGIN_SUBSCRIBED_DIR / user_id).string();
}

boost::filesystem::path resolve_plugin_root_from_descriptor(const PluginDescriptor& descriptor)
{
    namespace fs = boost::filesystem;

    if (!descriptor.plugin_root.empty())
        return fs::path(descriptor.plugin_root);
    if (!descriptor.entry_path.empty())
        return fs::path(descriptor.entry_path).parent_path();
    return {};
}

bool is_plugin_root_allowed(const boost::filesystem::path& candidate_root, const std::vector<std::string>& allowed_dirs)
{
    boost::system::error_code ec;
    boost::filesystem::path resolved_root = boost::filesystem::weakly_canonical(candidate_root, ec);
    if (ec) {
        ec.clear();
        resolved_root = boost::filesystem::absolute(candidate_root, ec);
    }

    if (ec || resolved_root.empty())
        return false;

    for (const auto& allowed_dir : allowed_dirs) {
        if (is_inside_allowed_root(boost::filesystem::path(resolved_root.string()),
                                   boost::filesystem::path(allowed_dir)))
            return true;
    }

    return false;
}

bool resolve_allowed_plugin_root(const PluginDescriptor& descriptor,
                                 const std::vector<std::string>& allowed_dirs,
                                 const std::string& out_of_scope_error,
                                 boost::filesystem::path& resolved_root,
                                 std::string& error)
{
    namespace fs = boost::filesystem;

    const fs::path plugin_root = resolve_plugin_root_from_descriptor(descriptor);
    if (plugin_root.empty()) {
        error = _u8L("Plugin folder could not be determined.");
        return false;
    }

    boost::system::error_code ec;
    resolved_root = fs::weakly_canonical(plugin_root, ec);
    if (ec) {
        ec.clear();
        resolved_root = fs::absolute(plugin_root, ec);
    }
    if (ec || resolved_root.empty()) {
        error = GUI::format(_u8L("Failed to resolve plugin folder: %1%"), plugin_root.string());
        return false;
    }

    if (!is_plugin_root_allowed(plugin_root, allowed_dirs)) {
        error = out_of_scope_error;
        return false;
    }

    return true;
}

bool delete_plugin_root(const boost::filesystem::path& resolved_root, const std::string& plugin_id, std::string& error)
{
    namespace fs = boost::filesystem;

    boost::system::error_code ec;
    const auto removed_count = fs::remove_all(resolved_root, ec);
    if (ec) {
        // TRN %1% is a folder path, %2% the system error message.
        error = GUI::format(_u8L("Failed to delete plugin folder %1%: %2%"), resolved_root.string(), ec.message());
        return false;
    }

    if (removed_count == 0) {
        return true;
    }

    BOOST_LOG_TRIVIAL(info) << "Deleted plugin: " << plugin_id << " from " << resolved_root.string();
    return true;
}


// ── Discovery ───────────────────────────────────────────────────────────────────────────────

namespace {

// Derive a discovered descriptor's operational plugin_key: the cloud UUID for cloud entries,
// otherwise the (escaped) stem of name_source (the entry file when one exists, or the plugin
// directory when it does not). plugin_key is always derived, never read back from the sidecar.
void assign_discovered_plugin_key(PluginDescriptor& descriptor, const boost::filesystem::path& name_source)
{
    if (descriptor.is_cloud_plugin())
        descriptor.plugin_key = descriptor.cloud_uuid();
    else
        descriptor.plugin_key = make_local_plugin_key(name_source.stem().string());
}

void scan_plugin_directory(const std::string& dir_path, std::vector<PluginDescriptor>& out)
{
    namespace fs = boost::filesystem;

    if (!fs::exists(dir_path) || !fs::is_directory(dir_path))
        return;

    BOOST_LOG_TRIVIAL(debug) << "Scanning plugin directory: " << dir_path;

    try {
        for (fs::directory_iterator it(dir_path); it != fs::directory_iterator(); ++it) {
            if (!fs::is_directory(it->status()))
                continue;

            const fs::path plugin_dir = it->path();
            if (is_ignored_plugin_directory(plugin_dir))
                continue;

            PluginDescriptor descriptor;
            descriptor.plugin_root = plugin_dir.string();

            std::string    entry_error;
            const fs::path entry_path = find_installed_plugin_entry(plugin_dir, entry_error);

            // No usable entry file: keep the package as an invalid row so the UI can show it and
            // its error, rather than dropping it silently.
            if (entry_path.empty()) {
                descriptor.set_error(entry_error);
                descriptor.install_state_valid = read_install_state(plugin_dir, descriptor);
                assign_discovered_plugin_key(descriptor, plugin_dir);
                out.push_back(std::move(descriptor));
                BOOST_LOG_TRIVIAL(warning) << "Invalid plugin package: " << plugin_dir.string() << " - " << out.back().error;
                continue;
            }

            std::string meta_error;
            const bool  is_wheel = entry_path.extension() == ".whl";
            const bool  parsed   = is_wheel ? read_wheel_plugin_metadata(entry_path, descriptor, meta_error) :
                                              read_python_plugin_metadata(entry_path, descriptor, meta_error);
            if (!parsed) {
                descriptor.set_error(meta_error);
                descriptor.install_state_valid = read_install_state(plugin_dir, descriptor);
                assign_discovered_plugin_key(descriptor, entry_path);
                out.push_back(std::move(descriptor));
                BOOST_LOG_TRIVIAL(warning) << (is_wheel ? "Invalid wheel plugin: " : "Invalid .py plugin: ")
                                           << plugin_dir.string() << " - " << out.back().error;
                continue;
            }

            descriptor.entry_path = entry_path.string();
            descriptor.set_metadata_valid(true);
            descriptor.clear_error();

            // Cloud identity and the package-level auto-load flag. plugin_key is always derived
            // below, never read from the sidecar.
            descriptor.install_state_valid = read_install_state(plugin_dir, descriptor);
            assign_discovered_plugin_key(descriptor, entry_path);

            out.push_back(std::move(descriptor));
            BOOST_LOG_TRIVIAL(info) << "Discovered plugin: " << out.back().name << " (version: " << out.back().version << ")";
        }
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "Error scanning directory " << dir_path << ": " << ex.what();
    }
}


} // namespace

std::vector<std::string> get_plugin_directories(const std::string& cloud_user_id)
{
    namespace fs = boost::filesystem;

    std::vector<std::string> dirs;

    // Creates the directory when missing — callers (notably the install path) rely on it existing.
    auto add_or_create_dir = [&dirs](const fs::path& path) {
        if (fs::exists(path) && fs::is_directory(path)) {
            dirs.push_back(path.string());
            return;
        }
        try {
            fs::create_directories(path);
            dirs.push_back(path.string());
            BOOST_LOG_TRIVIAL(info) << "Created plugin directory: " << path.string();
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(warning) << "Failed to create plugin directory: " << ex.what();
        }
    };

    // Local plugins: {data_dir}/orca_plugins/
    add_or_create_dir(fs::path(data_dir()) / "orca_plugins");

    // Cloud plugins: {data_dir}/orca_plugins/_subscribed/{user_id}/
    if (!cloud_user_id.empty())
        add_or_create_dir(fs::path(get_cloud_plugin_dir(cloud_user_id)));

    return dirs;
}

std::vector<PluginDescriptor> discover_plugin_packages(const std::vector<std::string>& dirs, std::string& error)
{
    error.clear();

    const auto                    start_time = std::chrono::steady_clock::now();
    std::vector<PluginDescriptor> discovered;

    try {
        BOOST_LOG_TRIVIAL(info) << "Scanning " << dirs.size() << " plugin directories...";

        for (const std::string& dir : dirs)
            scan_plugin_directory(dir, discovered);

        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time);
        BOOST_LOG_TRIVIAL(info) << "Plugin discovery completed in " << duration.count() << "ms. Found " << discovered.size()
                                << " plugin manifests";
    } catch (const std::exception& ex) {
        error = std::string("Plugin discovery failed: ") + ex.what();
        BOOST_LOG_TRIVIAL(error) << error;
    } catch (...) {
        error = "Plugin discovery failed: unknown error";
        BOOST_LOG_TRIVIAL(error) << error;
    }

    return discovered;
}

namespace {

bool is_safe_archive_entry_path(const boost::filesystem::path& path)
{
    if (!is_safe_relative_path(path))
        return false;

    for (const auto& part : path) {
        const std::string token = part.string();
        if (token.empty() || token == "." || token == "..")
            return false;
    }

    return true;
}

std::string decode_zip_entry_extra_path(const std::string& extra, const std::string& fallback)
{
    const char* p = extra.data();
    const char* e = p + extra.length();
    while (p + 4 <= e) {
        const auto len = static_cast<std::uint16_t>(static_cast<unsigned char>(p[2])) |
                         static_cast<std::uint16_t>(static_cast<unsigned char>(p[3]) << 8);
        if (p[0] == '\x75' && p[1] == '\x70' && len >= 5 && p + 4 + len <= e && p[4] == '\x01')
            return std::string(p + 9, p + 4 + len);
        p += 4 + len;
    }

    return decode_path(fallback.c_str());
}

std::string zip_entry_name(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat)
{
    if (stat.m_is_utf8)
        return stat.m_filename;

    std::string extra(1024, 0);
    const size_t n = mz_zip_reader_get_extra(&archive, stat.m_file_index, extra.data(), extra.size());
    return decode_zip_entry_extra_path(extra.substr(0, n), stat.m_filename);
}

std::string normalize_zip_entry_name(std::string entry_name)
{
    std::replace(entry_name.begin(), entry_name.end(), '\\', '/');
    while (!entry_name.empty() && entry_name.back() == '/')
        entry_name.pop_back();
    return entry_name;
}

// Locate the single "<name>.dist-info/" directory in a wheel. Returns false when there is none or
// when the archive holds more than one.
bool find_wheel_dist_info_dir(mz_zip_archive& archive, std::string& dist_info_dir, std::string& error)
{
    dist_info_dir.clear();

    // Scan ALL entries, not just directory entries — some zip writers omit
    // explicit directory entries. normalize_zip_entry_name strips trailing
    // slashes, so we match ".dist-info" as the last path component of a
    // directory entry, and ".dist-info/" embedded in a file path.
    const mz_uint num_entries = mz_zip_reader_get_num_files(&archive);
    mz_zip_archive_file_stat stat;

    for (mz_uint i = 0; i < num_entries; ++i) {
        if (!mz_zip_reader_file_stat(&archive, i, &stat))
            continue;

        std::string entry_name = normalize_zip_entry_name(zip_entry_name(archive, stat));
        if (entry_name.empty())
            continue;

        // Find .dist-info as a path component.
        size_t pos = entry_name.find(".dist-info");
        if (pos == std::string::npos)
            continue;

        std::string candidate;
        if (pos + 10 == entry_name.size()) {
            // Directory entry itself (trailing / stripped by normalize).
            candidate = entry_name + "/";
        } else if (pos + 10 < entry_name.size() && entry_name[pos + 10] == '/') {
            // File inside .dist-info/:  name.dist-info/METADATA
            candidate = entry_name.substr(0, pos + 11); // include trailing /
        } else {
            continue; // .dist-info mid-name, not a path component.
        }

        if (!dist_info_dir.empty() && candidate != dist_info_dir) {
            error = "Wheel contains multiple .dist-info directories: " + dist_info_dir + " and " + candidate;
            return false;
        }
        dist_info_dir = candidate;
    }

    if (dist_info_dir.empty()) {
        error = "Wheel does not contain a .dist-info directory";
        return false;
    }

    return true;
}

// All Tag: values declared by a WHEEL file, in file order.
std::vector<std::string> parse_wheel_tags(const std::string& wheel_content)
{
    std::vector<std::string> tags;

    std::istringstream stream(wheel_content);
    std::string        line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.rfind("Tag:", 0) != 0)
            continue;

        std::string tag = line.substr(4);
        size_t      start = 0;
        size_t      end   = tag.size();
        while (start < end && (tag[start] == ' ' || tag[start] == '\t'))
            ++start;
        while (end > start && (tag[end - 1] == ' ' || tag[end - 1] == '\t'))
            --end;
        tags.push_back(tag.substr(start, end - start));
    }

    return tags;
}

struct ZipReaderGuard
{
    mz_zip_archive archive;
    bool opened = false;

    ZipReaderGuard() { mz_zip_zero_struct(&archive); }

    ~ZipReaderGuard()
    {
        if (opened)
            close_zip_reader(&archive);
    }
};

} // namespace

bool is_valid_plugin_id(const std::string& id)
{
    if (id.empty())
        return false;
    if (id == "." || id == ".." || id[0] == '.' || id.rfind("__", 0) == 0)
        return false;

    for (unsigned char ch : id) {
        if (std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.')
            continue;
        return false;
    }

    return true;
}

namespace {

// RAII helper to free heap-allocated memory from miniz.
struct MzHeapFree {
    void* ptr = nullptr;
    ~MzHeapFree() { if (ptr) std::free(ptr); }
};

// Read a text file from within a zip archive into a string.
// Returns true on success, false if the file is not found or cannot be read.
bool read_zip_text_file(mz_zip_archive& archive, const char* filename, std::string& out, std::string& error)
{
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&archive, filename, &size, 0);
    if (!data) {
        error = std::string("Wheel does not contain ") + filename;
        return false;
    }
    MzHeapFree guard{data};
    out.assign(static_cast<const char*>(data), size);
    return true;
}

// TOML section parsing states.
enum class TomlSection { Root, OrcaPlugin, OrcaPluginSettings, InDepsArray };

// Strip a quoted string value: "foo" → foo, 'foo' → foo.
// Returns the unquoted value or the input unchanged if not quoted.
std::string unquote_toml_string(const std::string& val)
{
    if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') || (val.front() == '\'' && val.back() == '\'')))
        return val.substr(1, val.size() - 2);
    return val;
}

// Append the string items of one line of a TOML array to `items`. A quoted item keeps its commas,
// as a version range ("numpy>=1.20,<2") needs; '#' starts a comment and ']' closes the array.
// Returns true when the array closes on this line.
bool read_toml_array_items(const std::string& line, std::vector<std::string>& items)
{
    size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == ']')
            return true;
        if (c == '#')
            return false;
        if (c == ' ' || c == '\t' || c == ',') {
            ++i;
            continue;
        }
        std::string item;
        if (c == '"' || c == '\'') {
            // A basic string ("...") takes backslash escapes; a literal string ('...') takes none.
            for (++i; i < line.size() && line[i] != c; ++i) {
                if (c == '"' && line[i] == '\\' && i + 1 < line.size())
                    ++i;
                item += line[i];
            }
            ++i;
        } else {
            // An unquoted item isn't TOML, but is kept as the requirement it names.
            const size_t end = std::min(line.find_first_of(" \t,]#", i), line.size());
            item = line.substr(i, end - i);
            i = end;
        }
        if (!item.empty())
            items.push_back(item);
    }
    return false;
}

// Parse PEP 723 TOML subset for dependencies, requires-python, and
// [tool.orcaslicer.plugin] identity fields.
//
//   requires-python = ">=3.12"
//   dependencies = ["pkg>=1.0", ]
//
//   [tool.orcaslicer.plugin]
//   id = "my-plugin"
//   name = "My Plugin"
//   description = "Does things."
//   author = "Author"
//   version = "1.0.0"
//
// Returns false only on parse errors; missing block is not an error.
bool parse_pep723_toml(const std::string& toml_content,
                       std::vector<std::string>& out_deps,
                       std::string& out_requires_python,
                       std::string& out_name,
                       std::string& out_description,
                       std::string& out_author,
                       std::string& out_version,
                       std::string& error)
{
    out_deps.clear();
    out_requires_python.clear();
    out_name.clear();
    out_description.clear();
    out_author.clear();
    out_version.clear();

    TomlSection section = TomlSection::Root;

    std::istringstream stream(toml_content);
    std::string line;

    while (std::getline(stream, line)) {
        // Trim leading/trailing whitespace.
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
            ++start;
        size_t end = line.size();
        while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t'))
            --end;
        std::string trimmed = line.substr(start, end - start);

        if (trimmed.empty() || trimmed[0] == '#')
            continue;

        // TOML section header.
        if (trimmed[0] == '[') {
            if (trimmed == "[tool.orcaslicer.plugin]") {
                section = TomlSection::OrcaPlugin;
            } else if (trimmed == "[tool.orcaslicer.plugin.settings]") {
                // Legacy table, superseded by PluginConfig. Recognized so its keys are ignored
                // rather than falling through to Root, where a stray dependencies/requires-python
                // key inside it would be parsed as package metadata.
                section = TomlSection::OrcaPluginSettings;
            } else {
                section = TomlSection::Root; // Unknown section — skip.
            }
            continue;
        }

        if (section == TomlSection::InDepsArray) {
            if (read_toml_array_items(trimmed, out_deps))
                section = TomlSection::Root;
            continue;
        }

        // Look for key = value.
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = trimmed.substr(0, eq);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();

        std::string val = trimmed.substr(eq + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
            val.erase(0, 1);
        // Trim trailing.
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
            val.pop_back();

        if (section == TomlSection::Root) {
            if (key == "requires-python") {
                out_requires_python = unquote_toml_string(val);
            } else if (key == "dependencies") {
                // The array may close on this line or run on over the following ones.
                out_deps.clear();
                const bool array = !val.empty() && val.front() == '[';
                if (!read_toml_array_items(array ? val.substr(1) : val, out_deps) && array)
                    section = TomlSection::InDepsArray;
            }
        } else if (section == TomlSection::OrcaPlugin) {
            if (key == "name")         out_name = unquote_toml_string(val);
            else if (key == "description")  out_description = unquote_toml_string(val);
            else if (key == "author")       out_author = unquote_toml_string(val);
            else if (key == "version")      out_version = unquote_toml_string(val);
        }
    }

    // Check for unclosed arrays.
    if (section == TomlSection::InDepsArray) {
        error = "PEP 723 metadata: unclosed dependencies array";
        return false;
    }
    return true;
}

// Normalize a distribution name to a Python import package name.
// Converts hyphens to underscores and lowercases.
std::string normalize_package_name(const std::string& name)
{
    std::string result;
    result.reserve(name.size());
    for (unsigned char ch : name) {
        if (ch == '-' || ch == '.')
            result += '_';
        else
            result += static_cast<char>(std::tolower(ch));
    }
    return result;
}

// Parse METADATA (RFC 822 style) into a flat multimap.
// https://packaging.python.org/en/latest/specifications/core-metadata/
void parse_metadata_rfc822(const std::string& content,
                           std::string& out_name,
                           std::string& out_version,
                           std::string& out_summary,
                           std::string& out_author,
                           std::string& out_requires_python,
                           std::string& out_import_name,
                           std::vector<std::string>& out_requires_dist,
                           std::string& error)
{
    std::istringstream stream(content);
    std::string line;
    std::string current_header;
    std::string current_value;

    auto flush = [&]() {
        if (current_header.empty())
            return;
        std::string lower = current_header;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (lower == "name")
            out_name = current_value;
        else if (lower == "version")
            out_version = current_value;
        else if (lower == "summary")
            out_summary = current_value;
        else if (lower == "author")
            out_author = current_value;
        else if (lower == "requires-python")
            out_requires_python = current_value;
        else if (lower == "import-name")
            out_import_name = current_value;
        else if (lower == "requires-dist")
            out_requires_dist.push_back(current_value);

        current_header.clear();
        current_value.clear();
    };

    while (std::getline(stream, line)) {
        // Blank line after headers marks the start of the body.
        if (line.empty() && current_header.empty())
            continue;
        if (line.empty()) {
            flush();
            // Remaining content is the body (description); stop parsing headers.
            break;
        }

        // Continuation line.
        if (line[0] == ' ' || line[0] == '\t') {
            if (!current_value.empty())
                current_value += '\n';
            size_t pos = line.find_first_not_of(" \t");
            current_value += (pos != std::string::npos) ? line.substr(pos) : "";
            continue;
        }

        flush();

        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;

        current_header = line.substr(0, colon);
        size_t val_start = colon + 1;
        while (val_start < line.size() && (line[val_start] == ' ' || line[val_start] == '\t'))
            ++val_start;
        current_value = line.substr(val_start);
    }

    flush();
}

} // namespace

bool is_ignored_plugin_directory(const boost::filesystem::path& path)
{
    const std::string name = path.filename().string();
    return name.empty() || name[0] == '.' || name.rfind("__", 0) == 0 || name == PLUGIN_SUBSCRIBED_DIR || name == PLUGIN_DATA_DIR;
}

bool is_safe_relative_path(const boost::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_directory() || path.has_root_name())
        return false;

    for (const auto& part : path) {
        const std::string token = part.string();
        if (token == "..")
            return false;
    }

    return true;
}

bool extract_zip_to_directory(const boost::filesystem::path& zip_path, const boost::filesystem::path& destination, std::string& error)
{
    namespace fs = boost::filesystem;

    boost::system::error_code ec;
    fs::create_directories(extended_path(destination), ec);
    if (ec) {
        error = "Failed to create plugin staging directory: " + ec.message();
        return false;
    }

    ZipReaderGuard reader;
    if (!open_zip_reader(&reader.archive, zip_path.string())) {
        error = "Failed to open plugin zip: " + MZ_Archive::get_errorstr(mz_zip_get_last_error(&reader.archive));
        return false;
    }
    reader.opened = true;

    std::unordered_set<std::string> extracted_entries;
    const mz_uint num_entries = mz_zip_reader_get_num_files(&reader.archive);
    mz_zip_archive_file_stat stat;
    for (mz_uint i = 0; i < num_entries; ++i) {
        if (!mz_zip_reader_file_stat(&reader.archive, i, &stat)) {
            error = "Failed to read plugin zip entry metadata: " + MZ_Archive::get_errorstr(mz_zip_get_last_error(&reader.archive));
            return false;
        }

        std::string entry_name = normalize_zip_entry_name(zip_entry_name(reader.archive, stat));
        if (entry_name.empty())
            continue;
        if (entry_name.find(':') != std::string::npos) {
            error = "Plugin zip entry contains an invalid path: " + entry_name;
            return false;
        }

        const fs::path relative_path(entry_name);
        if (!is_safe_archive_entry_path(relative_path)) {
            error = "Plugin zip entry escapes the plugin package: " + entry_name;
            return false;
        }

        const std::string relative_key = relative_path.generic_string();
        if (!extracted_entries.insert(relative_key).second) {
            error = "Plugin zip contains duplicate entry: " + relative_key;
            return false;
        }

        const fs::path output_path = destination / relative_path;
        if (stat.m_is_directory || mz_zip_reader_is_file_a_directory(&reader.archive, stat.m_file_index)) {
            fs::create_directories(extended_path(output_path), ec);
            if (ec) {
                error = "Failed to create plugin zip directory " + output_path.string() + ": " + ec.message();
                return false;
            }
            continue;
        }

        fs::create_directories(extended_path(output_path.parent_path()), ec);
        if (ec) {
            error = "Failed to create plugin zip parent directory " + output_path.parent_path().string() + ": " + ec.message();
            return false;
        }

        if (fs::exists(extended_path(output_path), ec) && fs::is_directory(extended_path(output_path), ec)) {
            error = "Plugin zip file conflicts with an existing directory: " + output_path.string();
            return false;
        }

        // Shared with the rest of the archive code: long-path capable and binary-safe.
        if (!extract_zip_entry_to_file(&reader.archive, stat.m_file_index, output_path)) {
            error = "Failed to extract plugin zip entry " + relative_key + ": " +
                    MZ_Archive::get_errorstr(mz_zip_get_last_error(&reader.archive));
            return false;
        }
    }

    return true;
}

bool read_install_state(const boost::filesystem::path& plugin_dir, PluginDescriptor& entry)
{
    PluginInstallState state;
    if (!read_install_state(plugin_dir, state))
        return false;

    // The cloud identity and the persisted installed version are read back. plugin_key
    // is always derived by the catalog scan (filename for local, the cloud uuid for
    // cloud), so it is not read from the sidecar. installed_version is the source of
    // truth for a cloud plugin's installed version: it records the version fetched from
    // the cloud at install time, independent of the (possibly stale) manifest/PEP723
    // header that scan_directory parses into entry.version.
    //
    // A local plugin has no cloud copy to diverge from: the entry file's own header is the only
    // source of truth for its version. Trusting the sidecar there would pin the version shown
    // in the UI to whatever it was at first install, even after the file is edited.
    const bool is_local_install = state.installed_from == "local" && state.cloud_uuid.empty();
    if (is_local_install && !entry.version.empty())
        entry.installed_version = entry.version;
    else if (!state.installed_version.empty())
        entry.installed_version = state.installed_version;
    if (!state.cloud_uuid.empty())
        entry.cloud = CloudPluginState{state.cloud_uuid, true, false, false};

    // Package-level auto-load flag only. The per-capability enable flags stay in the sidecar: a
    // capability has no existence — and so no state — until it is materialized, at which point the
    // loader seeds the flag onto the capability itself.
    entry.enabled = state.enabled;
    return true;
}

bool read_install_state(const boost::filesystem::path& plugin_dir, PluginInstallState& out)
{
    namespace fs                = boost::filesystem;
    const fs::path sidecar_path = plugin_dir / ".install_state.json";

    if (!fs::exists(sidecar_path) || !fs::is_regular_file(sidecar_path))
        return false;

    boost::nowide::ifstream f(sidecar_path.string());
    if (!f)
        return false;

    try {
        nlohmann::json state = nlohmann::json::parse(f, nullptr, false, true);
        if (state.is_discarded() || !state.is_object())
            return false;

        PluginInstallState parsed;
        if (state.contains("installed_from") && state["installed_from"].is_string())
            parsed.installed_from = state["installed_from"].get<std::string>();
        if (state.contains("installed_version") && state["installed_version"].is_string())
            parsed.installed_version = state["installed_version"].get<std::string>();
        if (state.contains("plugin_name") && state["plugin_name"].is_string())
            parsed.plugin_name = state["plugin_name"].get<std::string>();
        if (state.contains("cloud_uuid") && state["cloud_uuid"].is_string())
            parsed.cloud_uuid = state["cloud_uuid"].get<std::string>();

        if (state.contains("permissions") && state["permissions"].is_object()) {
            const auto& permissions = state["permissions"];
            auto read_string_list = [&permissions](const char* key, std::vector<std::string>& out) {
                if (!permissions.contains(key) || !permissions[key].is_array())
                    return;
                for (const auto& entry : permissions[key])
                    if (entry.is_string())
                        out.push_back(entry.get<std::string>());
            };
            read_string_list("fs_read", parsed.permissions.fs_read);
            read_string_list("fs_readwrite", parsed.permissions.fs_readwrite);
            read_string_list("network_http", parsed.permissions.network_http);
            read_string_list("network_socket", parsed.permissions.network_socket);
            read_string_list("process", parsed.permissions.process);
            read_string_list("threading", parsed.permissions.threading);
            read_string_list("events", parsed.permissions.events);
        }

        if (state.contains("enabled") && state["enabled"].is_boolean())
            parsed.enabled = state["enabled"].get<bool>();

        // capabilities is a JSON array of single-key objects {<cap_name>: <bool>}.
        if (state.contains("capabilities") && state["capabilities"].is_array()) {
            for (const auto& item : state["capabilities"]) {
                if (!item.is_object())
                    continue;
                for (auto it = item.begin(); it != item.end(); ++it) {
                    if (it.value().is_boolean())
                        parsed.capabilities.emplace_back(it.key(), it.value().get<bool>());
                }
            }
        }

        out = std::move(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

namespace {

// The single descriptor -> sidecar metadata mapping. Prefer installed_version (fetched at install
// time and preserved across rewrites) over a possibly stale manifest version.
void apply_install_metadata(PluginInstallState& state, const PluginDescriptor& entry)
{
    state.installed_from    = entry.is_cloud_plugin() ? "cloud" : "local";
    state.installed_version = !entry.installed_version.empty() ? entry.installed_version : entry.version;
    state.plugin_name       = entry.name;
    state.cloud_uuid        = entry.cloud_uuid();
}

} // namespace

void seed_install_metadata(PluginInstallState& state, const PluginDescriptor& entry)
{
    if (!state.plugin_name.empty())
        return;

    apply_install_metadata(state, entry);
    state.enabled = true;
}

bool write_install_state(const boost::filesystem::path& plugin_dir, const PluginInstallState& state)
{
    namespace fs                = boost::filesystem;
    const fs::path sidecar_path = plugin_dir / ".install_state.json";

    nlohmann::json json;
    json["installed_from"]    = state.installed_from;
    json["installed_version"] = state.installed_version;
    json["plugin_name"]       = state.plugin_name;
    json["enabled"]           = state.enabled;
    if (!state.cloud_uuid.empty())
        json["cloud_uuid"] = state.cloud_uuid;

    json["permissions"] = {
        {"fs_read", state.permissions.fs_read},
        {"fs_readwrite", state.permissions.fs_readwrite},
        {"network_http", state.permissions.network_http},
        {"network_socket", state.permissions.network_socket},
        {"process", state.permissions.process},
        {"threading", state.permissions.threading},
        {"events", state.permissions.events},
    };

    nlohmann::json capabilities = nlohmann::json::array();
    for (const auto& [name, enabled] : state.capabilities)
        capabilities.push_back(nlohmann::json{{name, enabled}});
    json["capabilities"] = std::move(capabilities);

    boost::nowide::ofstream f(sidecar_path.string());
    if (!f)
        return false;

    f << json.dump(2);
    return static_cast<bool>(f);
}

bool write_install_state(const boost::filesystem::path& plugin_dir, const PluginDescriptor& entry, bool enabled,
                         const std::vector<std::pair<std::string, bool>>& capabilities)
{
    PluginInstallState state;
    // Loading a plugin updates its lifecycle/capability state, but must retain permissions granted
    // during register_capabilities() or by a previous runtime audit prompt.
    read_install_state(plugin_dir, state);
    apply_install_metadata(state, entry);
    state.enabled      = enabled;
    state.capabilities = capabilities;
    return write_install_state(plugin_dir, state);
}

bool write_install_state(const boost::filesystem::path& plugin_dir, const PluginDescriptor& entry)
{
    // Install-time writer: the package is not loaded, so its capabilities are not known yet and the
    // sidecar is (re)initialized to "auto-load, nothing disabled". This intentionally resets
    // permissions on a fresh install/reinstall. PluginManager writes the real per-capability flags
    // while preserving permissions once the package is loaded, via the overload above.
    PluginInstallState state;
    seed_install_metadata(state, entry);
    return write_install_state(plugin_dir, state);
}

bool read_python_plugin_metadata(const boost::filesystem::path& py_path, PluginDescriptor& descriptor, std::string& error)
{
    namespace fs = boost::filesystem;

    if (!fs::exists(py_path) || !fs::is_regular_file(py_path)) {
        error = "Python plugin file does not exist: " + py_path.string();
        return false;
    }

    boost::nowide::ifstream f(py_path.string());
    if (!f) {
        error = "Failed to open Python plugin file: " + py_path.string();
        return false;
    }

    // Scan for PEP 723 inline script metadata block.
    // The block is delimited by:
    //   # /// script
    //   # <toml content>
    //   # ///
    std::string pep723_content;
    bool in_block = false;
    std::string line;

    while (std::getline(f, line)) {
        // Strip trailing carriage return (Windows line endings).
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (!in_block) {
            // Look for opening delimiter.
            if (line == "# /// script")
                in_block = true;
            continue;
        }

        if (line == "# ///") {
            in_block = false;
            continue;
        }

        // Extract TOML content from the comment line.
        // Lines must start with "# " or "#\t" per PEP 723.
        if (line.size() >= 2 && line[0] == '#' && (line[1] == ' ' || line[1] == '\t'))
            pep723_content += line.substr(2) + "\n";
        else if (line == "#")
            pep723_content += "\n";
        // If line doesn't start with "# ", it's still part of the block content
        // but we skip it as it doesn't follow the spec.
    }

    if (!pep723_content.empty()) {
        std::string pep723_error;
        std::string requires_python;
        std::string pep_name, pep_desc, pep_author, pep_version;
        if (!parse_pep723_toml(pep723_content,
                               descriptor.dependencies,
                               requires_python,
                               pep_name,
                               pep_desc,
                               pep_author,
                               pep_version,
                               pep723_error)) {
            error = "Failed to parse PEP 723 metadata: " + pep723_error;
            return false;
        }
        // requires-python is stored but not validated against the bundled Python here.
        (void) requires_python;

        // Populate identity fields from the PEP 723 [tool.orcaslicer.plugin] section.
        // Cloud metadata overrides these when available; they serve as the local
        // source of truth for side-loaded .py plugins and as fallback values.
        if (!pep_name.empty())            descriptor.name       = sanitize_plugin_name(pep_name);
        if (!pep_desc.empty())            descriptor.description = pep_desc;
        if (!pep_author.empty())          descriptor.author     = pep_author;
        if (!pep_version.empty())         descriptor.version    = pep_version;
    }

    // Validate that required identity fields are present (either from PEP 723 or
    // from cloud metadata already set on the manifest by the caller).
    // Validation is deferred to the install/discovery layer so cloud metadata
    // can fill in gaps.
    return true;
}

bool read_wheel_plugin_metadata(const boost::filesystem::path& whl_path, PluginDescriptor& descriptor, std::string& error)
{
    namespace fs = boost::filesystem;

    if (!fs::exists(whl_path) || !fs::is_regular_file(whl_path)) {
        error = "Wheel plugin file does not exist: " + whl_path.string();
        return false;
    }

    ZipReaderGuard reader;
    if (!open_zip_reader(&reader.archive, whl_path.string())) {
        error = "Failed to open wheel as zip: " + MZ_Archive::get_errorstr(mz_zip_get_last_error(&reader.archive));
        return false;
    }
    reader.opened = true;

    // Find the single .dist-info directory.
    std::string dist_info_dir;
    if (!find_wheel_dist_info_dir(reader.archive, dist_info_dir, error))
        return false;

    // Read METADATA.
    const std::string metadata_path = dist_info_dir + "METADATA";
    std::string meta_content;
    if (!read_zip_text_file(reader.archive, metadata_path.c_str(), meta_content, error))
        return false;

    std::string meta_name, meta_version, meta_summary, meta_author, meta_requires_python, meta_import_name;
    std::vector<std::string> requires_dist;
    std::string meta_error;
    parse_metadata_rfc822(meta_content, meta_name, meta_version, meta_summary, meta_author,
                          meta_requires_python, meta_import_name, requires_dist, meta_error);

    if (meta_name.empty()) {
        error = "Wheel METADATA missing required Name field";
        return false;
    }
    if (meta_version.empty()) {
        error = "Wheel METADATA missing required Version field";
        return false;
    }

    // Read WHEEL (verify existence and Wheel-Version).
    const std::string wheel_path = dist_info_dir + "WHEEL";
    std::string wheel_content;
    if (!read_zip_text_file(reader.archive, wheel_path.c_str(), wheel_content, error))
        return false;
    // Verify there's at least a Wheel-Version header line.
    if (wheel_content.find("Wheel-Version:") == std::string::npos) {
        error = "Wheel WHEEL file missing Wheel-Version header";
        return false;
    }

    // Parse and validate wheel platform tags.
    {
        const std::vector<std::string> wheel_tags = parse_wheel_tags(wheel_content);
        if (!wheel_tags.empty()) {
            bool compatible = false;
            const std::string abi_tag = PythonInterpreter::python_abi_tag();
            for (const auto& tag : wheel_tags) {
                // Pure Python wheel: py3-none-any or cp312-none-any
                if (tag.find("-none-any") != std::string::npos) {
                    compatible = true;
                    break;
                }
                // Platform-specific: check ABI tag matches.
                if (tag.find(abi_tag) == 0) {
                    // Accept if the platform tag matches the current OS.
#ifdef _WIN32
                    if (tag.find("-win") != std::string::npos)
                        compatible = true;
#elif __APPLE__
                    if (tag.find("-macosx") != std::string::npos)
                        compatible = true;
#else
                    if (tag.find("-linux") != std::string::npos || tag.find("-manylinux") != std::string::npos)
                        compatible = true;
#endif
                }
            }
            if (!compatible) {
                error = "Wheel is incompatible with this platform. Tags: ";
                for (size_t i = 0; i < wheel_tags.size(); ++i) {
                    if (i > 0) error += ", ";
                    error += wheel_tags[i];
                }
                error += "; expected ABI: " + abi_tag;
                return false;
            }
        }
    }

    // Read RECORD (verify existence).
    const std::string record_path = dist_info_dir + "RECORD";
    std::string record_content;
    if (!read_zip_text_file(reader.archive, record_path.c_str(), record_content, error))
        return false;
    if (record_content.empty()) {
        error = "Wheel RECORD file is empty";
        return false;
    }

    // Parse top_level.txt if present.
    std::string top_level;
    const std::string top_level_path = dist_info_dir + "top_level.txt";
    std::string top_level_content;
    if (read_zip_text_file(reader.archive, top_level_path.c_str(), top_level_content, error)) {
        // top_level.txt contains one package name per line.
        std::istringstream tl_stream(top_level_content);
        std::string tl_line;
        std::vector<std::string> top_levels;
        while (std::getline(tl_stream, tl_line)) {
            while (!tl_line.empty() && (tl_line.back() == '\r' || tl_line.back() == '\n'))
                tl_line.pop_back();
            if (!tl_line.empty())
                top_levels.push_back(tl_line);
        }
        if (top_levels.size() == 1)
            top_level = top_levels[0];
        else if (top_levels.size() > 1) {
            // Ambiguous: multiple top-level packages. Fall through to Name-based fallback.
        }
        // Zero entries: leave top_level empty.
    }
    // If top_level.txt is not found, that's OK — it's optional per the wheel spec.

    // Determine the entry package in priority order.
    // 1. Cloud/catalog metadata — handled by caller, not here.
    // 2. Core Metadata Import-Name.
    // 3. top_level.txt if unambiguous.
    // 4. Normalized Name as fallback.
    if (!meta_import_name.empty()) {
        descriptor.entry_package = meta_import_name;
    } else if (!top_level.empty()) {
        descriptor.entry_package = top_level;
    } else {
        descriptor.entry_package = normalize_package_name(meta_name);
    }

    descriptor.dependencies = std::move(requires_dist);

    // Populate local identity fallbacks from wheel metadata.
    // Cloud metadata will override these when available.
    descriptor.name       = sanitize_plugin_name(meta_name);
    descriptor.version    = meta_version;
    descriptor.description = meta_summary;
    descriptor.author     = meta_author;

    return true;
}

bool read_wheel_install_metadata(const boost::filesystem::path& whl_path, WheelInstallMetadata& out, std::string& error)
{
    namespace fs = boost::filesystem;

    out = {};
    error.clear();

    if (!fs::exists(whl_path) || !fs::is_regular_file(whl_path)) {
        error = "Wheel plugin file does not exist: " + whl_path.string();
        return false;
    }

    ZipReaderGuard reader;
    if (!open_zip_reader(&reader.archive, whl_path.string())) {
        error = "Failed to open wheel as zip: " + MZ_Archive::get_errorstr(mz_zip_get_last_error(&reader.archive));
        return false;
    }
    reader.opened = true;

    std::string dist_info_dir;
    if (!find_wheel_dist_info_dir(reader.archive, dist_info_dir, error))
        return false;

    std::string meta_content;
    if (!read_zip_text_file(reader.archive, (dist_info_dir + "METADATA").c_str(), meta_content, error))
        return false;

    std::string              meta_name, meta_version, meta_summary, meta_author, meta_requires_python, meta_import_name;
    std::vector<std::string> requires_dist;
    std::string              meta_error;
    parse_metadata_rfc822(meta_content, meta_name, meta_version, meta_summary, meta_author, meta_requires_python, meta_import_name,
                          requires_dist, meta_error);

    if (meta_name.empty()) {
        error = "Wheel METADATA missing required Name field";
        return false;
    }
    if (meta_version.empty()) {
        error = "Wheel METADATA missing required Version field";
        return false;
    }

    std::string wheel_content;
    if (!read_zip_text_file(reader.archive, (dist_info_dir + "WHEEL").c_str(), wheel_content, error))
        return false;

    out.name    = std::move(meta_name);
    out.version = std::move(meta_version);
    out.tags    = parse_wheel_tags(wheel_content);
    return true;
}

boost::filesystem::path find_installed_plugin_entry(const boost::filesystem::path& plugin_dir, std::string& error)
{
    namespace fs = boost::filesystem;

    if (!fs::exists(plugin_dir) || !fs::is_directory(plugin_dir)) {
        error = GUI::format(_u8L("Plugin directory does not exist: %1%"), plugin_dir.string());
        return {};
    }

    fs::path py_entry;
    fs::path whl_entry;

    for (fs::directory_iterator it(plugin_dir); it != fs::directory_iterator(); ++it) {
        if (is_ignored_plugin_directory(it->path()))
            continue;
        if (!fs::is_regular_file(it->status()))
            continue;

        const fs::path ext = it->path().extension();
        if (ext == ".py") {
            if (!py_entry.empty()) {
                // TRN %1% and %2% are file names.
                error = GUI::format(_u8L("Plugin directory contains multiple .py files: %1% and %2%"),
                                    py_entry.filename().string(), it->path().filename().string());
                return {};
            }
            py_entry = it->path();
        } else if (ext == ".whl") {
            if (!whl_entry.empty()) {
                // TRN %1% and %2% are file names.
                error = GUI::format(_u8L("Plugin directory contains multiple .whl files: %1% and %2%"),
                                    whl_entry.filename().string(), it->path().filename().string());
                return {};
            }
            whl_entry = it->path();
        }
    }

    if (!py_entry.empty() && !whl_entry.empty()) {
        error = _u8L("Plugin directory contains both .py and .whl entry files");
        return {};
    }

    if (!py_entry.empty())
        return py_entry;

    if (!whl_entry.empty())
        return whl_entry;

    error = _u8L("Plugin directory does not contain a .py or .whl entry file");
    return {};
}

std::vector<std::string> collect_resource_texture_files(const std::string& dir,
                                                        const std::string& plugin_root,
                                                        std::string& error)
{
    namespace fs = boost::filesystem;

    // Picker sanity bound, not a security control: the popup draws every entry in its grid.
    constexpr size_t kMaxResourceTextureFiles = 64;

    std::vector<std::string> files;
    error.clear();

    if (dir.empty()) {
        error = _u8L("Resource texture directory is not set");
        return files;
    }
    if (plugin_root.empty()) {
        error = _u8L("Plugin folder could not be determined.");
        return files;
    }

    boost::system::error_code ec;
    const fs::path resolved_dir = fs::weakly_canonical(fs::path(dir), ec);
    if (ec) {
        error = _u8L("Resource texture directory could not be resolved");
        return files;
    }

    if (!fs::is_directory(resolved_dir, ec) || ec) {
        error = _u8L("Resource texture path is not a directory");
        return files;
    }

    const fs::path resolved_root = fs::weakly_canonical(fs::path(plugin_root), ec);
    if (ec) {
        error = _u8L("Plugin folder could not be resolved");
        return files;
    }

    if (!is_inside_allowed_root(resolved_dir, resolved_root)) {
        error = _u8L("Resource texture directory is outside the plugin folder");
        return files;
    }

    for (fs::directory_iterator it(resolved_dir, ec), end; it != end && !ec; it.increment(ec)) {
        const fs::path& path = it->path();

        if (!fs::is_regular_file(path, ec) || ec)
            continue;

        std::string ext = path.extension().string();
        boost::algorithm::to_lower(ext);
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".bmp")
            continue;

        // Re-check per file: a symlink inside the pack must not resolve outside it.
        if (!is_inside_allowed_root(path, resolved_root))
            continue;

        files.push_back(path.string());
        if (files.size() >= kMaxResourceTextureFiles)
            break;
    }

    if (ec) {
        error = _u8L("Resource texture directory could not be read");
        return {};
    }

    return files;
}

} // namespace Slic3r

// ── uv wheel installation ─────────────────────────────────────────────

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

bool names_equal_case_insensitive(const std::string& lhs, const std::string& rhs)
{
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](unsigned char a, unsigned char b) {
               return std::tolower(a) == std::tolower(b);
           });
}

// Path-component safety for a metadata-derived package name. Bytes >= 0x80 are left alone so
// non-ASCII (Unicode) import names are not rejected. ASCII path separators, control characters and
// characters Windows forbids in filenames are refused. This is deliberately not a Python
// identifier check: the import step reports a name that cannot be imported.
bool is_safe_package_path_char(unsigned char ch)
{
    if (ch >= 0x80)
        return true;
    if (ch < 0x20 || ch == 0x7F)
        return false;

    switch (ch) {
    case '/':
    case '\\':
    case ':':
    case '<':
    case '>':
    case '"':
    case '|':
    case '?':
    case '*':
        return false;
    default:
        return true;
    }
}

// A dependency wheel <dep>.whl is extracted to __whl_extracted__/<dep>/. If that stem equals the
// entry package, the entry target and the dependency extraction are the same directory. Compare
// case-insensitively so a case-insensitive filesystem cannot hide the collision.
bool dependency_extraction_collides(const fs::path& entry_path, const std::string& entry_package)
{
    if (!is_valid_entry_package(entry_package))
        return false;

    // Mirror add_wheel_dependencies_to_sys_path(): it scans the entry's parent directory (or the
    // entry path itself when it has no extension) and recognizes only lowercase ".whl" files.
    // Keep the same rules here so this guard protects exactly the directories that loader uses.
    const fs::path           plugin_dir = entry_path.has_extension() ? entry_path.parent_path() : entry_path;
    boost::system::error_code ec;
    if (!fs::is_directory(plugin_dir, ec))
        return false;

    for (fs::directory_iterator it(plugin_dir, ec); !ec && it != fs::directory_iterator(); ++it) {
        if (!fs::is_regular_file(it->status()))
            continue;

        const fs::path candidate = it->path();
        if (candidate == entry_path)
            continue;
        if (candidate.extension() != ".whl")
            continue;

        if (names_equal_case_insensitive(candidate.stem().string(), entry_package))
            return true;
    }
    return false;
}

bool entry_package_present(const fs::path& target, const std::string& entry_package)
{
    if (!is_valid_entry_package(entry_package))
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

// True when <plugin root>/__whl_extracted__/<entry package> is a markerless pre-uv extraction that
// already contains the entry package. Such a target is importable through the ordinary directory
// import, so it is used directly instead of being reinstalled. A marker, even an invalid one, makes
// the directory a uv-managed target whose reuse/reinstall decision belongs to is_installed().
// Only the entry package's own directory is inspected; the shared __whl_extracted__/ parent also
// holds dependency extractions.
bool has_legacy_entry_extraction(const PluginDescriptor& descriptor)
{
    if (!is_valid_entry_package(descriptor.entry_package))
        return false;

    const std::string target_path = install_dir(descriptor.plugin_root, descriptor.entry_package);
    if (target_path.empty())
        return false;

    const fs::path target(target_path);
    boost::system::error_code ec;
    if (!fs::is_directory(target, ec))
        return false;

    ec.clear();
    if (fs::exists(target / MARKER_FILE_NAME, ec) || ec)
        return false;

    return entry_package_present(target, descriptor.entry_package);
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
    return PythonInterpreter::run_uv(args, timeout, error);
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

bool is_valid_entry_package(const std::string& entry_package)
{
    if (entry_package.empty())
        return false;

    bool component_has_char = false;
    for (unsigned char ch : entry_package) {
        if (ch == '.') {
            // Dotted names need non-empty components; this also rejects "." and "..".
            if (!component_has_char)
                return false;
            component_has_char = false;
        } else {
            if (!is_safe_package_path_char(ch))
                return false;
            component_has_char = true;
        }
    }
    return component_has_char;
}

std::string install_dir(const std::string& plugin_root, const std::string& entry_package)
{
    if (!is_valid_entry_package(entry_package))
        return {};
    return (fs::path(plugin_root) / EXTRACTED_DIR_NAME / entry_package).string();
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
    if (!is_valid_entry_package(descriptor.entry_package))
        return fail("entry package is not a safe package path");

    const fs::path target = fs::path(install_dir(descriptor.plugin_root, descriptor.entry_package));
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
    std::string extension = fs::path(descriptor.entry_path).extension().string();
    boost::algorithm::to_lower(extension);
    if (extension != ".whl") {
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
    if (!is_valid_entry_package(descriptor.entry_package)) {
        error = "Wheel entry package '" + descriptor.entry_package + "' is not a safe package path.";
        return false;
    }

    const fs::path plugin_root = fs::path(descriptor.plugin_root);
    const fs::path entry       = fs::path(descriptor.entry_path);

    // __whl_extracted__/ is shared with dependency extractions. Never take over a directory that
    // belongs to a dependency wheel: the two would fight over the same directory and import name.
    // Checked before reuse so a marker in a coincidentally named dependency directory cannot make
    // the entry look installed.
    if (dependency_extraction_collides(entry, descriptor.entry_package)) {
        error = "Wheel entry package '" + descriptor.entry_package +
                "' collides with the extraction directory of a dependency wheel next to the entry; "
                "rename the dependency wheel so its extraction uses a different directory.";
        return false;
    }

    std::string reuse_reason;
    if (is_installed(descriptor, &reuse_reason)) {
        BOOST_LOG_TRIVIAL(info) << "[plugin_wheel_install] reusing installation for " << descriptor.plugin_key << " ("
                                << install_dir(descriptor.plugin_root, descriptor.entry_package) << ")";
        return true;
    }

    // Wheel installs mutate a per-plugin directory; serialize them so two plugin loads cannot
    // interleave staging/promotion (load already serializes, this also covers direct callers).
    static std::mutex install_mutex;
    std::lock_guard<std::mutex> install_lock(install_mutex);

    // Another caller may have completed the same install while this one waited.
    if (is_installed(descriptor, &reuse_reason)) {
        BOOST_LOG_TRIVIAL(info) << "[plugin_wheel_install] reusing installation for " << descriptor.plugin_key << " ("
                                << install_dir(descriptor.plugin_root, descriptor.entry_package) << ")";
        return true;
    }

    // A markerless entry-specific directory that already contains the entry package is the pre-uv
    // loader's extraction. It is imported through the same directory path, so accept it without
    // running uv. Checked under the same lock as installation: a concurrent load cannot observe a
    // target that another caller is replacing. A marker-present target is uv-managed and keeps the
    // reinstall path above, so a stale or invalid uv installation is still rebuilt.
    if (has_legacy_entry_extraction(descriptor)) {
        BOOST_LOG_TRIVIAL(info) << "[plugin_wheel_install] reusing legacy extraction for " << descriptor.plugin_key << " ("
                                << install_dir(descriptor.plugin_root, descriptor.entry_package) << ")";
        return true;
    }

    const fs::path target = fs::path(install_dir(plugin_root.string(), descriptor.entry_package));

    std::string staged_name = entry.filename().string();
    if (!is_valid_wheel_filename(staged_name)) {
        // uv accepts only the lowercase ".whl" extension. A stored wheel with an uppercase
        // extension keeps its name on disk; only the staged input copy is normalized.
        const std::string normalized_name = entry.stem().string() + ".whl";
        if (is_valid_wheel_filename(normalized_name)) {
            staged_name = normalized_name;
        } else {
            // Cloud downloads and side-loaded wheels can carry names uv rejects. Stage a copy
            // under a name derived from the wheel's own metadata; the stored wheel is left
            // untouched.
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
    // The target's parent (__whl_extracted__) also holds dependency extractions, so creating it is
    // safe; only the entry package's own subdirectory is replaced.
    ec.clear();
    fs::create_directories(target.parent_path(), ec);
    if (ec)
        return fail("Failed to create the plugin wheel extraction directory " + target.parent_path().string() +
                    ": " + ec.message());

    const fs::path previous = plugin_root / (std::string(PREVIOUS_DIR_PREFIX) + unique);
    ec.clear();
    const bool had_target = fs::exists(target, ec);
    if (ec)
        return fail("Failed to inspect the existing wheel installation target: " + ec.message());
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
