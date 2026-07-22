#include "PluginFsUtils.hpp"

#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "PluginAuditManager.hpp"
#include "PythonInterpreter.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <utility>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <unordered_set>
#include <vector>

#ifdef WIN32
#include <boost/locale/encoding_utf.hpp>
#endif

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
        error = "Plugin folder could not be determined.";
        return false;
    }

    boost::system::error_code ec;
    resolved_root = fs::weakly_canonical(plugin_root, ec);
    if (ec) {
        ec.clear();
        resolved_root = fs::absolute(plugin_root, ec);
    }
    if (ec || resolved_root.empty()) {
        error = "Failed to resolve plugin folder: " + plugin_root.string();
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
        error = "Failed to delete plugin folder " + resolved_root.string() + ": " + ec.message();
        return false;
    }

    if (removed_count == 0) {
        error = "Plugin folder was not found: " + resolved_root.string();
        return false;
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
                read_install_state(plugin_dir, descriptor);
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
                read_install_state(plugin_dir, descriptor);
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
            read_install_state(plugin_dir, descriptor);
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

// Split a TOML inline array: ["a", "b"] → {"a", "b"}.
// Handles trailing commas and single-line format.
std::vector<std::string> parse_toml_inline_array(const std::string& val)
{
    std::vector<std::string> result;
    std::string inner = val;
    // Strip outer brackets.
    if (!inner.empty() && inner.front() == '[')
        inner.erase(0, 1);
    if (!inner.empty() && inner.back() == ']')
        inner.pop_back();

    // Simple split by comma, strip quotes and whitespace.
    std::istringstream ss(inner);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // Trim whitespace.
        size_t s = 0, e = item.size();
        while (s < e && (item[s] == ' ' || item[s] == '\t')) ++s;
        while (e > s && (item[e - 1] == ' ' || item[e - 1] == '\t')) --e;
        item = item.substr(s, e - s);
        if (!item.empty())
            result.push_back(unquote_toml_string(item));
    }
    return result;
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
            if (trimmed == "]") {
                section = TomlSection::Root;
                continue;
            }
            std::string val = trimmed;
            if (!val.empty() && val.back() == ',')
                val.pop_back();
            val = unquote_toml_string(val);
            if (!val.empty())
                out_deps.push_back(val);
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
                if (val == "[") {
                    section = TomlSection::InDepsArray;
                } else {
                    // Inline array: dependencies = ["a", "b"]
                    out_deps = parse_toml_inline_array(val);
                }
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
    return name.empty() || name[0] == '.' || name.rfind("__", 0) == 0 || name == PLUGIN_SUBSCRIBED_DIR;
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
    fs::create_directories(destination, ec);
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
            fs::create_directories(output_path, ec);
            if (ec) {
                error = "Failed to create plugin zip directory " + output_path.string() + ": " + ec.message();
                return false;
            }
            continue;
        }

        fs::create_directories(output_path.parent_path(), ec);
        if (ec) {
            error = "Failed to create plugin zip parent directory " + output_path.parent_path().string() + ": " + ec.message();
            return false;
        }

        if (fs::exists(output_path, ec) && fs::is_directory(output_path, ec)) {
            error = "Plugin zip file conflicts with an existing directory: " + output_path.string();
            return false;
        }

        const std::string encoded_output_path = encode_path(output_path.string().c_str());
        mz_bool extracted = mz_zip_reader_extract_to_file(&reader.archive, stat.m_file_index, encoded_output_path.c_str(), 0);
#ifdef WIN32
        if (!extracted) {
            const std::wstring wide_output_path = boost::locale::conv::utf_to_utf<wchar_t>(output_path.generic_string());
            extracted = mz_zip_reader_extract_to_file_w(&reader.archive, stat.m_file_index, wide_output_path.c_str(), 0);
        }
#endif
        if (!extracted) {
            error = "Failed to extract plugin zip entry " + relative_key + ": " +
                    MZ_Archive::get_errorstr(mz_zip_get_last_error(&reader.archive));
            return false;
        }
    }

    return true;
}

void read_install_state(const boost::filesystem::path& plugin_dir, PluginDescriptor& entry)
{
    PluginInstallState state;
    if (!read_install_state(plugin_dir, state))
        return;

    // The cloud identity and the persisted installed version are read back. plugin_key
    // is always derived by the catalog scan (filename for local, the cloud uuid for
    // cloud), so it is not read from the sidecar. installed_version is the source of
    // truth for a cloud plugin's installed version: it records the version fetched from
    // the cloud at install time, independent of the (possibly stale) manifest/PEP723
    // header that scan_directory parses into entry.version.
    if (!state.installed_version.empty())
        entry.installed_version = state.installed_version;
    if (!state.cloud_uuid.empty())
        entry.cloud = CloudPluginState{state.cloud_uuid, true, false, false};

    // Package-level auto-load flag only. The per-capability enable flags stay in the sidecar: a
    // capability has no existence — and so no state — until it is materialized, at which point the
    // loader seeds the flag onto the capability itself.
    entry.enabled = state.enabled;
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
    state.installed_from    = entry.is_cloud_plugin() ? "cloud" : "local";
    // Prefer the descriptor's recorded installed_version (the version fetched from the cloud
    // at install time, preserved across sidecar re-writes) so a stale manifest/PEP723 header
    // never overwrites the source-of-truth version. Fall back to the manifest version for
    // first-time/local installs where installed_version is not yet populated.
    state.installed_version = !entry.installed_version.empty() ? entry.installed_version : entry.version;
    state.plugin_name       = entry.name;
    state.cloud_uuid        = entry.cloud_uuid();
    state.enabled           = enabled;
    state.capabilities      = capabilities;
    return write_install_state(plugin_dir, state);
}

bool write_install_state(const boost::filesystem::path& plugin_dir, const PluginDescriptor& entry)
{
    // Install-time writer: the package is not loaded, so its capabilities are not known yet and the
    // sidecar is (re)initialized to "auto-load, nothing disabled". PluginManager writes the real
    // per-capability flags once the package is loaded, via the (dir, entry, enabled, capabilities)
    // overload.
    return write_install_state(plugin_dir, entry, true, {});
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
    // Scan ALL entries, not just directory entries — some zip writers omit
    // explicit directory entries.  normalize_zip_entry_name strips trailing
    // slashes, so we match ".dist-info" as the last path component of a
    // directory entry, and ".dist-info/" embedded in a file path.
    const mz_uint num_entries = mz_zip_reader_get_num_files(&reader.archive);
    std::string dist_info_dir;
    mz_zip_archive_file_stat stat;

    for (mz_uint i = 0; i < num_entries; ++i) {
        if (!mz_zip_reader_file_stat(&reader.archive, i, &stat))
            continue;

        std::string entry_name = normalize_zip_entry_name(zip_entry_name(reader.archive, stat));
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
        std::vector<std::string> wheel_tags;
        std::istringstream wstream(wheel_content);
        std::string wline;
        while (std::getline(wstream, wline)) {
            while (!wline.empty() && (wline.back() == '\r' || wline.back() == '\n'))
                wline.pop_back();
            if (wline.rfind("Tag:", 0) == 0) {
                std::string tag = wline.substr(4);
                size_t s = 0, e = tag.size();
                while (s < e && (tag[s] == ' ' || tag[s] == '\t')) ++s;
                while (e > s && (tag[e - 1] == ' ' || tag[e - 1] == '\t')) --e;
                wheel_tags.push_back(tag.substr(s, e - s));
            }
        }

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

boost::filesystem::path find_installed_plugin_entry(const boost::filesystem::path& plugin_dir, std::string& error)
{
    namespace fs = boost::filesystem;

    if (!fs::exists(plugin_dir) || !fs::is_directory(plugin_dir)) {
        error = "Plugin directory does not exist: " + plugin_dir.string();
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
                error = "Plugin directory contains multiple .py files: " + py_entry.filename().string() +
                        " and " + it->path().filename().string();
                return {};
            }
            py_entry = it->path();
        } else if (ext == ".whl") {
            if (!whl_entry.empty()) {
                error = "Plugin directory contains multiple .whl files: " + whl_entry.filename().string() +
                        " and " + it->path().filename().string();
                return {};
            }
            whl_entry = it->path();
        }
    }

    if (!py_entry.empty() && !whl_entry.empty()) {
        error = "Plugin directory contains both .py and .whl entry files";
        return {};
    }

    if (!py_entry.empty())
        return py_entry;

    if (!whl_entry.empty())
        return whl_entry;

    error = "Plugin directory does not contain a .py or .whl entry file";
    return {};
}

} // namespace Slic3r
