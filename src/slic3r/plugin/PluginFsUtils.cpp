#include "PluginFsUtils.hpp"

#include "PythonFileUtils.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <chrono>
#include <filesystem>
#include <utility>

#include "PluginAuditManager.hpp"

namespace Slic3r {

const char* const INSTALL_STATE_FILE = ".install_state.json";

std::string get_cloud_plugin_dir(const std::string& user_id)
{
    namespace fs = boost::filesystem;
    return (fs::path(data_dir()) / "orca_plugins" / PLUGIN_SUBSCRIBED_DIR / user_id).string();
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

bool is_plugin_root_allowed(const boost::filesystem::path& candidate_root,
                            const std::vector<std::string>& allowed_dirs)
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
        if (is_inside_allowed_root(std::filesystem::path(resolved_root.string()),
                                   std::filesystem::path(allowed_dir)))
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

bool delete_plugin_root(const boost::filesystem::path& resolved_root,
                        const std::string& plugin_id,
                        std::string& error)
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


} // namespace Slic3r
