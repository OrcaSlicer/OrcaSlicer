#include "PluginLoader.hpp"

// plugin_loader fills and tears down a Plugin, so it needs the complete type. The
// declaration lives with the registry that owns it; the dependency is .cpp-only, so the
// service header stays free of the manager.
#include "PluginManager.hpp"

#include "PluginFsUtils.hpp"
#include "PythonFileUtils.hpp"
#include "PythonInterpreter.hpp"
#include "PythonPluginBridge.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process.hpp>
#ifdef _WIN32
#include <boost/process/windows.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Slic3r::plugin_loader {
namespace {

boost::filesystem::path canonical_or_absolute(const boost::filesystem::path& path)
{
    namespace fs = boost::filesystem;

    boost::system::error_code ec;
    fs::path resolved = fs::weakly_canonical(path, ec);
    if (!ec && !resolved.empty())
        return resolved;

    ec.clear();
    resolved = fs::absolute(path, ec);
    if (!ec && !resolved.empty())
        return resolved;

    return path;
}

std::string plugin_package_extension(const boost::filesystem::path& path)
{
    std::string ext = path.extension().string();
    boost::algorithm::to_lower(ext);
    return ext;
}

boost::filesystem::path local_plugin_root()
{
    return boost::filesystem::path(data_dir()) / "orca_plugins";
}

boost::filesystem::path local_plugin_install_dir(const boost::filesystem::path& source_path)
{
    // Folder name follows the plugin filename and is the install/backup conflict unit;
    // plugin_key is the file stem of the same file.
    return local_plugin_root() / filesystem_safe_escape(source_path.filename().string());
}

void assign_local_plugin_key(PluginDescriptor& plugin_descriptor, const boost::filesystem::path& entry_file)
{
    plugin_descriptor.plugin_key = make_local_plugin_key(entry_file.stem().string());
}

bool read_local_plugin_package_metadata(const boost::filesystem::path& source_path,
                                        PluginDescriptor&              plugin_descriptor,
                                        std::string&                   error)
{
    error.clear();

    const std::string ext = plugin_package_extension(source_path);
    if (ext != ".py" && ext != ".whl") {
        error = "Plugin package must be a .py or .whl file, got: " + ext;
        return false;
    }

    bool ok = false;
    if (ext == ".whl")
        ok = read_wheel_plugin_metadata(source_path, plugin_descriptor, error);
    else
        ok = read_python_plugin_metadata(source_path, plugin_descriptor, error);

    if (!ok)
        return false;

    plugin_descriptor.cloud = std::nullopt;
    return true;
}

// on_unload() the first `lifecycle_count` capabilities — the ones that actually got on_load() —
// then drop them all.
//
void teardown_capabilities(std::vector<std::shared_ptr<PluginCapabilityInterface>>& capabilities, std::size_t lifecycle_count)
{
    if (capabilities.empty())
        return;

    if (!PythonInterpreter::instance().is_initialized()) {
        capabilities.clear();
        return;
    }

    PythonGILState gil;
    if (!gil) {
        capabilities.clear();
        return;
    }

    lifecycle_count = std::min(lifecycle_count, capabilities.size());
    for (std::size_t index = 0; index < lifecycle_count; ++index) {
        const auto& capability = capabilities[index];
        if (!capability)
            continue;
        try {
            capability->on_unload();
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Plugin on_unload failed: " << ex.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": Plugin on_unload failed";
        }
    }
    capabilities.clear();
}

// Add every .whl sitting next to the plugin entry file to sys.path, extracting it first.
bool add_wheel_dependencies_to_sys_path(const PluginDescriptor& descriptor,
                                         std::vector<std::string>& plugin_paths,
                                         std::string&              error)
{
    namespace fs = boost::filesystem;

    const fs::path entry_path(descriptor.entry_path);
    const fs::path plugin_dir = entry_path.has_extension() ? entry_path.parent_path() : entry_path;
    if (!fs::exists(plugin_dir) || !fs::is_directory(plugin_dir))
        return true;

    PythonInterpreter& interpreter = PythonInterpreter::instance();
    for (fs::directory_iterator it(plugin_dir); it != fs::directory_iterator(); ++it) {
        if (!fs::is_regular_file(it->status()) || it->path().extension() != ".whl")
            continue;
        // Skip the entry file itself — it is loaded by its own path.
        if (it->path() == entry_path)
            continue;

        const fs::path dep_dir = plugin_dir / "__whl_extracted__" / it->path().stem().string();
        if (!fs::exists(dep_dir)) {
            std::string extract_error;
            if (!extract_zip_to_directory(it->path(), dep_dir, extract_error)) {
                error = "Failed to extract plugin .whl dependency " + it->path().string() + ": " + extract_error;
                return false;
            }
        }

        std::string syspath_error;
        if (!interpreter.add_plugin_sys_path(dep_dir.string(), syspath_error)) {
            error = "Failed to add .whl dependency to sys.path: " + syspath_error;
            return false;
        }
        plugin_paths.push_back(dep_dir.string());
    }

    return true;
}

PyObject* import_plugin_module(const PluginDescriptor& descriptor,
                               std::vector<std::string>& plugin_paths,
                               std::vector<std::string>& plugin_modules,
                               std::string&              error)
{
    PythonInterpreter& interpreter = PythonInterpreter::instance();

    if (descriptor.entry_package.empty())
        return interpreter.load_module_from_file(descriptor.entry_path, error, &plugin_paths, &plugin_modules);

    if (boost::filesystem::path(descriptor.entry_path).extension() == ".whl")
        return interpreter.load_module_from_whl(
            descriptor.entry_path, descriptor.entry_package, error, &plugin_paths, &plugin_modules);

    return interpreter.load_module_from_directory(
        descriptor.entry_path, descriptor.entry_package, error, &plugin_paths, &plugin_modules);
}

std::string plugin_module_name(const PluginDescriptor& descriptor)
{
    if (!descriptor.entry_package.empty())
        return descriptor.entry_package;
    return boost::filesystem::path(descriptor.entry_path).stem().string();
}

} // namespace

void unload(Plugin& plugin)
{
    // Dropping the capabilities drops their state with them; the user's enable choices survive in
    // the .install_state.json sidecar, which the next load seeds from.
    teardown_capabilities(plugin.capabilities, plugin.capabilities.size());
    plugin.release_module();
}

bool load(const PluginDescriptor&                          descriptor,
          bool                                             skip_deps,
          const std::vector<std::string>&                  capabilities_to_enable,
          const std::function<std::string(const Plugin&)>& registry_precheck,
          Plugin&                                          out,
          std::string&                                     error)
{
    error.clear();
    out = Plugin{};

    BOOST_LOG_TRIVIAL(info) << "[plugin_loader::load] START plugin=" << descriptor.plugin_key
                            << " thread=" << std::this_thread::get_id();

    PythonInterpreter& interpreter = PythonInterpreter::instance();
    if (!interpreter.is_initialized()) {
        error = "Python interpreter not initialized: " + interpreter.last_error();
        return false;
    }

    if (!descriptor.is_metadata_valid()) {
        error = "Plugin manifest is invalid: " + descriptor.plugin_key;
        if (descriptor.has_error())
            error += " - " + descriptor.normalized_error();
        return false;
    }

    // Serialize the entire load sequence — only one thread may run it at a time. This prevents
    // sys.path / sys.modules races when two plugins share the same entry-point filename (e.g.
    // plugin.py), and keeps the caller's check-then-commit of the registry free of an interleaved
    // load (which could otherwise force a wasted on_load/on_unload rollback). install_packages()
    // has a 120s timeout so this cannot block indefinitely.
    static std::mutex           load_serializer;
    std::lock_guard<std::mutex> load_lock(load_serializer);

    Plugin plugin;
    plugin.descriptor  = descriptor;
    plugin.module_name = plugin_module_name(descriptor);

    if (!skip_deps) {
        std::string pkg_install_error;
        if (!install_packages(descriptor.dependencies, pkg_install_error)) {
            error = "Failed to install plugin dependencies: " + pkg_install_error;
            return false;
        }
    }

    PythonPluginBridge& bridge = PythonPluginBridge::instance();
    bridge.begin_plugin_capture(descriptor.entry_path);

    std::string wheel_error;
    if (!add_wheel_dependencies_to_sys_path(descriptor, plugin.plugin_sys_paths, wheel_error)) {
        bridge.cancel_plugin_capture(descriptor.entry_path);
        error = std::move(wheel_error);
        return false;
    }

    std::string load_error;
    PyObject*   module = import_plugin_module(descriptor, plugin.plugin_sys_paths, plugin.plugin_modules, load_error);
    if (module == nullptr) {
        bridge.cancel_plugin_capture(descriptor.entry_path);
        error = "Failed to load plugin module: " + load_error;
        return false;
    }

    // From here on the module reference is owned by `plugin`: every failure path below returns and
    // lets ~Plugin release it.
    plugin.module = module;

    // finalize_plugin_capture runs the module's @orca.plugin package class register_capabilities()
    // (while the active plugin key is set), then instantiates each registered capability and caches
    // its get_name(). Returns one entry per capability.
    std::string bridge_error;
    auto        capabilities_found = bridge.finalize_plugin_capture(descriptor.entry_path, bridge_error);
    if (!bridge_error.empty()) {
        capabilities_found.clear();
        error = "Plugin registration failed: " + bridge_error;
        return false;
    }
    if (capabilities_found.empty()) {
        error = "Plugin module did not register any capabilities";
        return false;
    }

    plugin.descriptor.clear_error();

    // The user's per-capability enable choices live in the sidecar — the only durable record, since
    // a capability has no state (and no existence) while the package is not loaded.
    PluginInstallState install_state;
    const bool         have_install_state = !descriptor.plugin_root.empty() &&
                                    read_install_state(boost::filesystem::path(descriptor.plugin_root), install_state);

    // get_name()/get_type() are read exactly once — here, under the GIL this block already holds —
    // and cached on the capability, which is the only place capability state lives.
    std::unordered_map<PluginCapabilityType, std::unordered_set<std::string>> seen_capabilities;
    std::vector<std::shared_ptr<PluginCapabilityInterface>>                   capabilities;
    capabilities.reserve(capabilities_found.size());
    std::string materialization_error;

    {
        PythonGILState gil;
        if (!gil) {
            materialization_error = "Python interpreter is shutting down";
        } else {
            try {
                for (auto& found : capabilities_found) {
                    if (!found.instance) {
                        materialization_error = "Plugin capability instance is null";
                        break;
                    }

                    std::shared_ptr<PluginCapabilityInterface> instance = std::move(found.instance);
                    const PluginCapabilityType                 type     = instance->get_type();

                    // An empty request restores the sidecar state (or enables capabilities by
                    // default). An explicit request overrides the sidecar for that capability;
                    // all other capabilities retain their persisted state.
                    const bool explicitly_requested =
                        std::find(capabilities_to_enable.begin(), capabilities_to_enable.end(), found.name) !=
                        capabilities_to_enable.end();
                    bool enabled = capabilities_to_enable.empty() || explicitly_requested;
                    if (have_install_state && !explicitly_requested) {
                        for (const auto& [cap_name, cap_enabled] : install_state.capabilities) {
                            if (cap_name == found.name) {
                                enabled = cap_enabled;
                                break;
                            }
                        }
                    }

                    if (!seen_capabilities[type].insert(found.name).second) {
                        materialization_error = "Plugin declares duplicate capability '" + found.name + "' for type " +
                                                plugin_capability_type_to_string(type);
                        break;
                    }

                    instance->set_audit_plugin_key(descriptor.plugin_key);
                    instance->set_resolved_identity(found.name, type);
                    instance->set_enabled(enabled);

                    capabilities.push_back(std::move(instance));
                }
            } catch (const std::exception& ex) {
                materialization_error = std::string("Plugin capability materialization failed: ") + ex.what();
            } catch (...) {
                materialization_error = "Plugin capability materialization failed";
            }
        }

        if (!materialization_error.empty()) {
            capabilities.clear();
            capabilities_found.clear();
        }
    }
    if (!materialization_error.empty()) {
        error = std::move(materialization_error);
        return false;
    }
    capabilities_found.clear();

    plugin.capabilities = std::move(capabilities);

    // Let the caller reject the load against its registry BEFORE on_load() runs, so a duplicate
    // package or a capability-name collision costs no on_load/on_unload cycle.
    if (registry_precheck) {
        std::string precheck_error = registry_precheck(plugin);
        if (!precheck_error.empty()) {
            teardown_capabilities(plugin.capabilities, 0);
            error = std::move(precheck_error);
            return false;
        }
    }

    // lifecycle_count is incremented BEFORE on_load(), so a capability whose on_load() throws
    // still gets its on_unload().
    std::size_t lifecycle_count = 0;
    try {
        PythonGILState gil;
        if (!gil)
            throw std::runtime_error("Python interpreter is shutting down");
        for (const auto& capability : plugin.capabilities) {
            ++lifecycle_count;
            capability->on_load();
        }
    } catch (const std::exception& ex) {
        teardown_capabilities(plugin.capabilities, lifecycle_count);
        error = std::string("Plugin on_load failed: ") + ex.what();
        return false;
    } catch (...) {
        teardown_capabilities(plugin.capabilities, lifecycle_count);
        error = "Plugin on_load failed";
        return false;
    }

    out = std::move(plugin);

    BOOST_LOG_TRIVIAL(info) << "[plugin_loader::load] SUCCESS plugin=" << descriptor.plugin_key
                            << " thread=" << std::this_thread::get_id();
    return true;
}

bool install_packages(const std::vector<std::string>& pkgs, std::string& error)
{
    if (pkgs.empty())
        return true;

    const std::string uv_path = PythonInterpreter::bundled_uv_path();
    if (uv_path.empty()) {
        error = "Bundled uv executable not found. Python package installation is unavailable.";
        return false;
    }

    const std::string python_executable = PythonInterpreter::bundled_python_executable();
    if (python_executable.empty()) {
        error = "Bundled Python executable not found. Python package installation is unavailable.";
        return false;
    }

    const std::string target_dir = PythonInterpreter::shared_packages_dir();

    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    fs::create_directories(target_dir, ec);
    if (ec) {
        error = "Failed to create package target directory: " + ec.message();
        return false;
    }

    std::vector<std::string> args = {"pip", "install", "--python", python_executable, "--no-python-downloads", "--target", target_dir};
    args.insert(args.end(), pkgs.begin(), pkgs.end());

    BOOST_LOG_TRIVIAL(info) << "Installing Python packages via uv for " << python_executable << ": "
                            << boost::algorithm::join(pkgs, ", ");

    try {
        namespace process = boost::process;

        process::ipstream std_err;
        process::child    child(uv_path, process::args(args),
#ifdef _WIN32
                                // uv.exe (and the python.exe it spawns) are console-subsystem programs.
                                // OrcaSlicer is a GUI app with no console of its own, so without this flag
                                // Windows allocates a fresh console window for the child that flashes on
                                // screen during startup plugin loading. Matches ProcessRunner/MediaPlayCtrl.
                                process::windows::create_no_window,
#endif
                                process::std_err > std_err);

        std::string err_output;
        std::thread stderr_reader([&std_err, &err_output]() {
            std::string line;
            while (std::getline(std_err, line)) {
                err_output += line + '\n';
            }
        });

        // Wait up to 120 seconds for package install to complete.
        //
        // NOTE: we poll child.running() instead of calling child.wait_for().
        // On macOS (which lacks sigtimedwait) boost.process v1 implements the
        // timed waits with a sigwait()-based fallback that deadlocks inside a
        // multi-threaded process: SIGCHLD is delivered to an arbitrary thread
        // and consumed by an async handler, so the worker thread's sigwait()
        // never returns and the timeout never fires — the plugin load hangs
        // forever (the "Loading" status never clears). child.running() uses
        // waitpid(WNOHANG), which behaves correctly on every platform.
        // (boost.process v2, in Boost >= 1.86, implements timed waits via Asio
        // and is unaffected — revisit this when the bundled Boost is upgraded.)
        constexpr auto kInstallTimeout = std::chrono::seconds(120);
        const auto     deadline        = std::chrono::steady_clock::now() + kInstallTimeout;

        std::error_code run_ec;
        while (child.running(run_ec)) {
            if (run_ec) {
                stderr_reader.join();
                error = "Failed to query uv process status: " + run_ec.message();
                BOOST_LOG_TRIVIAL(error) << error;
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                std::error_code term_ec;
                child.terminate(term_ec);
                child.wait(term_ec);
                stderr_reader.join();
                error = "uv pip install timed out after 120s";
                BOOST_LOG_TRIVIAL(error) << error;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        stderr_reader.join();

        const int exit_code = child.exit_code();
        if (exit_code != 0) {
            if (!err_output.empty())
                error = "uv pip install failed (exit code " + std::to_string(exit_code) + "): " + err_output;
            else
                error = "uv pip install failed with exit code " + std::to_string(exit_code);
            BOOST_LOG_TRIVIAL(error) << error;
            return false;
        }

        BOOST_LOG_TRIVIAL(info) << "Python packages installed successfully";
        return true;
    } catch (const std::exception& ex) {
        error = std::string("Failed to run uv: ") + ex.what();
        BOOST_LOG_TRIVIAL(error) << error;
        return false;
    }
}

bool inspect_local_plugin_package(const boost::filesystem::path& filepath,
                                  PluginDescriptor&              plugin_descriptor,
                                  bool&                          existing_installation,
                                  std::string&                   error)
{
    namespace fs = boost::filesystem;

    error.clear();
    plugin_descriptor     = PluginDescriptor{};
    existing_installation = false;

    auto fail = [&error](const std::string& message) {
        error = "Plugin package inspection failed: " + message;
        return false;
    };

    boost::system::error_code ec;
    const fs::path            source_path = canonical_or_absolute(filepath);

    if (!fs::exists(source_path, ec) || !fs::is_regular_file(source_path, ec))
        return fail("Plugin package is not a file: " + filepath.string());

    std::string metadata_error;
    if (!read_local_plugin_package_metadata(source_path, plugin_descriptor, metadata_error))
        return fail(metadata_error);

    const fs::path final_dir = local_plugin_install_dir(source_path);
    assign_local_plugin_key(plugin_descriptor, source_path);
    plugin_descriptor.plugin_root = final_dir.string();

    ec.clear();
    const fs::file_status final_status = fs::status(final_dir, ec);
    if (ec && final_status.type() != fs::file_not_found)
        return fail("Failed to check existing plugin " + final_dir.string() + ": " + ec.message());

    existing_installation = final_status.type() != fs::file_not_found && final_status.type() != fs::status_error;

    return true;
}

bool install_plugin(const boost::filesystem::path& filepath, const std::string& cloud_user_id, std::string& error)
{
    PluginDescriptor descriptor{};
    return install_plugin(filepath, cloud_user_id, descriptor, error);
}

bool install_plugin(const boost::filesystem::path& filepath,
                    const std::string&             cloud_user_id,
                    PluginDescriptor&              plugin_descriptor,
                    std::string&                   error)
{
    namespace fs = boost::filesystem;

    const std::string cloud_uuid       = plugin_descriptor.cloud_uuid();
    const bool        is_cloud_install = !cloud_uuid.empty();

    fs::path backup_dir;
    bool     backup_created = false;

    auto fail = [&error](const std::string& message) {
        error = "Plugin installation failed: " + message;
        return false;
    };

    boost::system::error_code ec;
    const fs::path            source_path = canonical_or_absolute(filepath);

    if (!fs::exists(source_path, ec) || !fs::is_regular_file(source_path, ec))
        return fail("Plugin package is not a file: " + filepath.string());

    const std::string ext = plugin_package_extension(source_path);
    if (ext != ".py" && ext != ".whl")
        return fail("Plugin package must be a .py or .whl file, got: " + ext);
    // The cloud UUID is concatenated into the install path (final_dir = plugin_root / cloud_uuid).
    // It comes verbatim from the server's catalog JSON, so reject anything that could escape the
    // per-user plugin root (traversal, leading dots, separators) before touching the filesystem.
    if (is_cloud_install && !is_valid_plugin_id(cloud_uuid))
        return fail("Cloud plugin UUID is not a valid identifier: " + cloud_uuid);

    const fs::path plugin_root = is_cloud_install && !cloud_user_id.empty() ? fs::path(get_cloud_plugin_dir(cloud_user_id)) :
                                                                             local_plugin_root();
    fs::create_directories(plugin_root, ec);
    if (ec)
        return fail("Failed to create plugin directory " + plugin_root.string() + ": " + ec.message());

    std::string meta_error;
    if (is_cloud_install) {
        PluginDescriptor package_metadata;
        if (!read_local_plugin_package_metadata(source_path, package_metadata, meta_error))
            return fail(meta_error);
        apply_plugin_metadata_fallbacks(plugin_descriptor, package_metadata);
        plugin_descriptor.dependencies = std::move(package_metadata.dependencies);

        if (plugin_descriptor.plugin_key.empty())
            plugin_descriptor.plugin_key = cloud_uuid;
        if (!plugin_descriptor.cloud.has_value())
            plugin_descriptor.cloud = CloudPluginState{cloud_uuid, true, false, false, false};
        else
            plugin_descriptor.cloud->installed = true;
    } else {
        if (!read_local_plugin_package_metadata(source_path, plugin_descriptor, meta_error))
            return fail(meta_error);
        // A side-loaded .py with no (or incomplete) PEP 723 block parses as success but has no
        // usable identity: an empty name collides in local_plugin_install_dir() (every nameless
        // plugin maps to orca_plugins/"path"), so it installs but never loads. Wheels are exempt:
        // read_wheel_plugin_metadata() already requires a Name and may legitimately leave the type
        // Unknown.
        if (ext == ".py" && plugin_descriptor.name.empty())
            return fail("Side-loaded .py plugin is missing required PEP 723 metadata: 'name' is required");
    }

    const fs::path final_dir = is_cloud_install ? plugin_root / cloud_uuid : local_plugin_install_dir(source_path);
    const fs::path dest_file = final_dir / source_path.filename();

    // Local key is the plugin file stem; cloud key is the cloud UUID.
    if (is_cloud_install)
        plugin_descriptor.plugin_key = cloud_uuid;
    else
        assign_local_plugin_key(plugin_descriptor, source_path);

    // Backup existing installation if present.
    if (fs::exists(final_dir, ec)) {
        backup_dir = plugin_root / (final_dir.filename().string() + ".backup-" + fs::unique_path("%%%%-%%%%-%%%%").string());
        fs::rename(final_dir, backup_dir, ec);
        if (ec)
            return fail("Failed to backup existing plugin " + final_dir.string() + ": " + ec.message());
        backup_created = true;
    }

    // Create the plugin directory and copy the file.
    fs::create_directories(final_dir, ec);
    if (ec) {
        if (backup_created) {
            boost::system::error_code restore_ec;
            fs::rename(backup_dir, final_dir, restore_ec);
        }
        return fail("Failed to create plugin directory " + final_dir.string() + ": " + ec.message());
    }

    fs::copy_file(source_path, dest_file, ec);
    if (ec) {
        if (backup_created) {
            boost::system::error_code restore_ec;
            fs::remove_all(final_dir, restore_ec);
            fs::rename(backup_dir, final_dir, restore_ec);
        }
        return fail("Failed to copy plugin file to " + dest_file.string() + ": " + ec.message());
    }

    // Update entry_path to the installed location.
    plugin_descriptor.plugin_root = final_dir.string();
    plugin_descriptor.entry_path  = dest_file.string();
    plugin_descriptor.set_metadata_valid(true);
    plugin_descriptor.clear_error();

    // Write install state sidecar before removing the backup.
    {
        PluginDescriptor installed_entry = plugin_descriptor;
        if (is_cloud_install)
            installed_entry.cloud = CloudPluginState{cloud_uuid, true, false, false, plugin_descriptor.cloud->is_mine};
        if (!write_install_state(final_dir, installed_entry)) {
            // Roll back: remove new files and restore backup.
            boost::system::error_code rollback_ec;
            fs::remove_all(final_dir, rollback_ec);
            if (backup_created)
                fs::rename(backup_dir, final_dir, rollback_ec);
            return fail("Failed to write plugin install state: " + (final_dir / INSTALL_STATE_FILE).string());
        }
    }

    if (backup_created) {
        boost::system::error_code remove_ec;
        fs::remove_all(backup_dir, remove_ec);
        if (remove_ec)
            BOOST_LOG_TRIVIAL(warning) << "Failed to remove plugin backup " << backup_dir.string() << ": " << remove_ec.message();
    }

    BOOST_LOG_TRIVIAL(info) << "Installed plugin " << plugin_descriptor.name << " to " << final_dir.string();

    if (is_cloud_install) {
        boost::system::error_code remove_ec;
        fs::remove(source_path, remove_ec);
    }
    return true;
}

} // namespace Slic3r::plugin_loader
