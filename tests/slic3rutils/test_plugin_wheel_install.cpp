#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/path.hpp>
#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <libslic3r/Zipper.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginLoader.hpp>
#include <slic3r/plugin/PluginWheelInstall.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "catch2/catch_test_macros.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "plugin_test_utils.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace wheel_install = Slic3r::plugin_wheel_install;

namespace {

void write_wheel(const fs::path& path,
                 const std::string& tag,
                 const std::string& module_payload,
                 const std::string& dist_name = "demo_pkg",
                 const std::string& version = "1.0")
{
    if (fs::exists(path))
        fs::remove(path);

    Zipper zipper(path.string(), Zipper::NO_COMPRESSION);

    const std::string metadata = "Metadata-Version: 2.1\nName: demo-pkg\nVersion: " + version + "\n\n";
    zipper.add_entry(dist_name + "/__init__.py", module_payload.data(), module_payload.size());
    zipper.add_entry(dist_name + "-" + version + ".dist-info/METADATA", metadata.data(), metadata.size());

    const std::string wheel = "Wheel-Version: 1.0\nGenerator: test\nRoot-Is-Purelib: true\nTag: " + tag + "\n";
    zipper.add_entry(dist_name + "-" + version + ".dist-info/WHEEL", wheel.data(), wheel.size());

    const std::string record = dist_name + "/__init__.py,,\n";
    zipper.add_entry(dist_name + "-" + version + ".dist-info/RECORD", record.data(), record.size());

    zipper.finalize();
}

PluginDescriptor make_descriptor(const fs::path& plugin_root, const fs::path& wheel)
{
    PluginDescriptor descriptor;
    descriptor.plugin_key      = "demo_plugin";
    descriptor.plugin_root     = plugin_root.string();
    descriptor.entry_path      = wheel.string();
    descriptor.entry_package   = "demo_pkg";
    descriptor.wheel_install_enabled = true;
    descriptor.set_metadata_valid(true);
    return descriptor;
}

std::string arg_after(const std::vector<std::string>& args, const std::string& flag)
{
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag)
            return args[i + 1];
    }
    return {};
}

// A uv stand-in: creates the directory tree uv would have installed into. It records the command
// line so a test can assert on the staged filename and the safety flags.
wheel_install::UvRunner fake_uv(std::vector<std::string>* captured, int* call_count, std::string failure)
{
    return [captured, call_count, failure](const std::vector<std::string>& args, std::chrono::seconds, std::string& error) {
        if (call_count != nullptr)
            ++*call_count;
        if (captured != nullptr)
            *captured = args;

        if (!failure.empty()) {
            error = failure;
            return false;
        }

        const fs::path target = arg_after(args, "--target");
        if (target.empty()) {
            error = "fake uv: no --target";
            return false;
        }

        // The staged wheel must be a real file with a PEP 427 filename, not the stored name.
        const fs::path staged(args.back());
        if (!fs::is_regular_file(staged)) {
            error = "fake uv: staged wheel missing";
            return false;
        }

        fs::create_directories(target / "demo_pkg");
        std::ofstream init((target / "demo_pkg" / "__init__.py").string());
        init << "VALUE = 1\n";
        return true;
    };
}

bool has_staging_leftovers(const fs::path& plugin_root)
{
    for (fs::directory_iterator it(plugin_root); it != fs::directory_iterator(); ++it) {
        const std::string name = it->path().filename().string();
        if (name.rfind(wheel_install::STAGING_DIR_PREFIX, 0) == 0)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("wheel filenames are validated by their PEP 427 shape", "[PluginWheelInstall]")
{
    CHECK(wheel_install::is_valid_wheel_filename("demo-1.0-py3-none-any.whl"));
    CHECK(wheel_install::is_valid_wheel_filename("demo_pkg-1.0-1build-py3-none-any.whl"));
    CHECK(wheel_install::is_valid_wheel_filename("demo-1.0+local-cp312-cp312-manylinux_2_17_x86_64.whl"));

    // Too few or too many components, a non-.whl extension, an empty component, or a path separator
    // is not a wheel filename uv would accept.
    CHECK_FALSE(wheel_install::is_valid_wheel_filename("demo.whl"));
    CHECK_FALSE(wheel_install::is_valid_wheel_filename("demo-1.0-py3-none-any.zip"));
    CHECK_FALSE(wheel_install::is_valid_wheel_filename("demo-1.0-py3-none.whl"));
    CHECK_FALSE(wheel_install::is_valid_wheel_filename("demo-1.0--py3-none-any.whl"));
    CHECK_FALSE(wheel_install::is_valid_wheel_filename("demo/1.0-py3-none-any.whl"));

    // A cloud download name splits into five components but its components are not tags, so it
    // must be restaged from metadata before uv sees it.
    CHECK_FALSE(wheel_install::is_valid_wheel_filename("cloud_plugin-1a2b-3c4d-5e6f-7a8b9c0d1e2f.whl"));
    CHECK_FALSE(wheel_install::is_valid_wheel_filename("demo-notaversion-py3-none-any.whl"));
}

TEST_CASE("a missing wheel filename is rebuilt from metadata", "[PluginWheelInstall]")
{
    CHECK(wheel_install::make_wheel_filename("My Plugin", "1.0.0", "py3-none-any") ==
          "My_Plugin-1.0.0-py3-none-any.whl");

    // The tag is a python-abi-platform triple, so its two '-' separators are part of the tag and
    // must survive into the filename, which then has to pass the wheel-filename check.
    const std::string pure = wheel_install::make_wheel_filename("demo", "1.0", "py3-none-any");
    REQUIRE_FALSE(pure.empty());
    CHECK(pure == "demo-1.0-py3-none-any.whl");
    CHECK(wheel_install::is_valid_wheel_filename(pure));

    const std::string native =
        wheel_install::make_wheel_filename("demo", "1.0", "cp312-cp312-manylinux_2_17_x86_64");
    REQUIRE_FALSE(native.empty());
    CHECK(native == "demo-1.0-cp312-cp312-manylinux_2_17_x86_64.whl");
    CHECK(wheel_install::is_valid_wheel_filename(native));

    // Characters uv would reject are escaped; a component that escapes to nothing is refused.
    CHECK(wheel_install::make_wheel_filename("demo", "1.0 rc1", "py3-none-any") == "demo-1.0_rc1-py3-none-any.whl");
    CHECK(wheel_install::make_wheel_filename("", "1.0", "py3-none-any").empty());
    CHECK(wheel_install::make_wheel_filename("demo", "1.0", "").empty());

    // Not a triple, or not a plausible triple: no valid wheel filename can be built.
    CHECK(wheel_install::make_wheel_filename("demo", "1.0", "py3-none").empty());
    CHECK(wheel_install::make_wheel_filename("demo", "1.0", "py3/any").empty());
    CHECK(wheel_install::make_wheel_filename("demo", "1.0", "abc-def-ghi").empty());
}

TEST_CASE("a staged wheel filename uses the tags the wheel declares", "[PluginWheelInstall]")
{
    CHECK(wheel_install::wheel_filename_tag({}).empty());
    CHECK(wheel_install::wheel_filename_tag({"not-a-tag"}).empty());

    // No tag is invented: a wheel that only declares py2 keeps that declared tag (uv reports the
    // incompatibility), while a wheel declaring both uses the interpreter-compatible one.
    CHECK(wheel_install::wheel_filename_tag({"py2-none-any"}) == "py2-none-any");
    CHECK(wheel_install::wheel_filename_tag({"py2-none-any", "py3-none-any"}) == "py3-none-any");
    CHECK(wheel_install::wheel_filename_tag({"cp311-cp311-linux_x86_64", "cp312-cp312-linux_x86_64"}) ==
          "cp312-cp312-linux_x86_64");

    // Tags that differ only in platform are combined into a standard compressed tag set.
    CHECK(wheel_install::wheel_filename_tag({"cp312-cp312-manylinux_2_17_x86_64",
                                             "cp312-cp312-musllinux_1_2_x86_64"}) ==
          "cp312-cp312-manylinux_2_17_x86_64.musllinux_1_2_x86_64");

    // A different interpreter/ABI tag is not merged: the first declared tag is kept.
    CHECK(wheel_install::wheel_filename_tag({"cp312-cp312-linux_x86_64", "cp311-cp311-linux_x86_64"}) ==
          "cp312-cp312-linux_x86_64");
}

TEST_CASE("the uv opt-in is sidecar state and defaults to off", "[PluginWheelInstall][PluginInstall]")
{
    ScopedTemporaryDir dir("wheel-opt-in");
    const fs::path      plugin_dir = dir.path();

    PluginDescriptor descriptor;
    descriptor.name    = "Demo";
    descriptor.version = "1.0";

    REQUIRE(write_install_state(plugin_dir, descriptor));

    PluginInstallState state;
    REQUIRE(read_install_state(plugin_dir, state));
    CHECK_FALSE(state.wheel_install);

    PluginDescriptor scanned;
    REQUIRE(read_install_state(plugin_dir, scanned));
    CHECK_FALSE(scanned.wheel_install_enabled);

    descriptor.wheel_install_enabled = true;
    REQUIRE(write_install_state(plugin_dir, descriptor));

    REQUIRE(read_install_state(plugin_dir, scanned));
    CHECK(scanned.wheel_install_enabled);

    // The load-time writer rewrites the sidecar around a live plugin; it must not drop the opt-in.
    REQUIRE(write_install_state(plugin_dir, descriptor, /*enabled=*/true, {}));
    PluginInstallState after_load;
    REQUIRE(read_install_state(plugin_dir, after_load));
    CHECK(after_load.wheel_install);
}

TEST_CASE("an update keeps a plugin's uv opt-in", "[PluginWheelInstall][PluginInstall]")
{
    ScopedDataDir data_dir_guard("wheel-update-opt-in");

    const std::string contents = "# /// script\n"
                                 "# requires-python = \">=3.12\"\n"
                                 "#\n"
                                 "# [tool.orcaslicer.plugin]\n"
                                 "# name = \"Keep Opt-in\"\n"
                                 "# type = \"script\"\n"
                                 "# ///\n"
                                 "print('ok')\n";

    const fs::path source = data_dir_guard.dir / "src" / "keepopt.py";
    fs::create_directories(source.parent_path());
    {
        std::ofstream out(source.string());
        out << contents;
    }

    PluginDescriptor first;
    std::string      error;
    REQUIRE(plugin_loader::install_plugin(source, /*cloud_user_id=*/"", first, error));

    const fs::path plugin_root = data_dir_guard.plugins_dir() / "keepopt.py";
    PluginInstallState state;
    REQUIRE(read_install_state(plugin_root, state));
    state.wheel_install = true;
    REQUIRE(write_install_state(plugin_root, state));

    // A fresh descriptor (as produced by inspection for an update) knows nothing about the sidecar.
    PluginDescriptor second;
    REQUIRE(plugin_loader::install_plugin(source, /*cloud_user_id=*/"", second, error));

    PluginInstallState after_update;
    REQUIRE(read_install_state(plugin_root, after_update));
    CHECK(after_update.wheel_install);
}

TEST_CASE("a wheel with an invalid stored name is staged under a valid name", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-stage");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "plugin.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    std::vector<std::string> captured;
    int                      calls = 0;
    std::string              error;

    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(&captured, &calls, "")));
    CHECK(error.empty());
    CHECK(calls == 1);

    // The stored wheel keeps its name; only the staged copy uv reads carries a valid one.
    CHECK(fs::path(descriptor.entry_path).filename().string() == "plugin.whl");
    CHECK(fs::path(captured.back()).filename().string() == "demo_pkg-1.0-py3-none-any.whl");
    // uv installs into a staging target; the promoted target is the one loading uses.
    const fs::path uv_target(arg_after(captured, "--target"));
    CHECK(uv_target.filename().string() == "target");
    CHECK(uv_target.parent_path().filename().string().rfind(wheel_install::STAGING_DIR_PREFIX, 0) == 0);
    CHECK(uv_target != fs::path(wheel_install::install_dir(plugin_root.string())));
    CHECK(std::find(captured.begin(), captured.end(), "--no-deps") != captured.end());
    CHECK(std::find(captured.begin(), captured.end(), "--no-index") != captured.end());
    CHECK(std::find(captured.begin(), captured.end(), "--no-python-downloads") != captured.end());

    CHECK(fs::is_directory(wheel_install::install_dir(plugin_root.string())));
    CHECK(fs::is_regular_file(fs::path(wheel_install::install_dir(plugin_root.string())) / wheel_install::MARKER_FILE_NAME));
    CHECK_FALSE(has_staging_leftovers(plugin_root));
}

TEST_CASE("a wheel with a valid stored name is passed to uv unchanged", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-valid-name");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "demo_pkg-1.0-py3-none-any.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    std::vector<std::string> captured;
    std::string              error;
    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(&captured, nullptr, "")));

    // The stored wheel is neither renamed on disk nor renamed in the staged copy.
    CHECK(fs::path(descriptor.entry_path).filename().string() == "demo_pkg-1.0-py3-none-any.whl");
    CHECK(fs::path(captured.back()).filename().string() == "demo_pkg-1.0-py3-none-any.whl");
}

TEST_CASE("an unchanged wheel reuses its installation without running uv again", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-reuse");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "plugin.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    int         calls = 0;
    std::string error;
    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, &calls, "")));
    REQUIRE(calls == 1);

    // No uv invocation, no network, no disk rewrite on the next load.
    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, &calls, "should not run")));
    CHECK(calls == 1);
}

TEST_CASE("changed wheel content invalidates the uv installation", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-changed");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "plugin.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    int         calls = 0;
    std::string error;
    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, &calls, "")));
    REQUIRE(calls == 1);

    write_wheel(wheel, "py3-none-any", "VALUE = 2\n");

    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, &calls, "")));
    CHECK(calls == 2);
}

TEST_CASE("a failed uv install leaves no importable target behind", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-fail");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "plugin.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    std::string error;
    CHECK_FALSE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, nullptr, "simulated uv failure")));
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("simulated uv failure"));

    CHECK_FALSE(fs::exists(wheel_install::install_dir(plugin_root.string())));
    CHECK_FALSE(has_staging_leftovers(plugin_root));
}

TEST_CASE("a declared wheel tag is used unchanged in the staged filename", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-declared-tag");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "plugin.whl";
    // A Python-2-only tag: the backend never rewrites it into a claim about this interpreter. uv
    // performs the compatibility check on the staged filename.
    write_wheel(wheel, "py2-none-any", "VALUE = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    std::vector<std::string> captured;
    int                      calls = 0;
    std::string              error;
    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(&captured, &calls, "")));
    CHECK(calls == 1);
    CHECK(fs::path(captured.back()).filename().string() == "demo_pkg-1.0-py2-none-any.whl");
}

TEST_CASE("rewriting install state preserves its contents and leaves no temporary sidecar",
          "[PluginWheelInstall][PluginInstall]")
{
    ScopedTemporaryDir dir("sidecar-replace");

    PluginInstallState state;
    state.installed_from     = "local";
    state.installed_version  = "1.0";
    state.plugin_name        = "Demo";
    state.enabled            = false;
    state.wheel_install      = true;
    state.permissions.fs_read = {"/path/to/read"};
    state.capabilities        = {{"demo_cap", false}};

    REQUIRE(write_install_state(dir.path(), state));

    PluginInstallState loaded;
    REQUIRE(read_install_state(dir.path(), loaded));
    CHECK(loaded.enabled == false);
    CHECK(loaded.wheel_install);
    CHECK(loaded.permissions.fs_read == state.permissions.fs_read);
    REQUIRE(loaded.capabilities.size() == 1);

    // The second write replaces the file through a temporary, not by truncating it in place.
    state.enabled = true;
    REQUIRE(write_install_state(dir.path(), state));

    REQUIRE(read_install_state(dir.path(), loaded));
    CHECK(loaded.enabled);
    CHECK(loaded.wheel_install);
    CHECK(loaded.permissions.fs_read == state.permissions.fs_read);

    int sidecars = 0;
    for (fs::directory_iterator it(dir.path()); it != fs::directory_iterator(); ++it) {
        const std::string name = it->path().filename().string();
        CHECK(name.find(".tmp-") == std::string::npos);
        CHECK(name.find(".prev-") == std::string::npos);
        if (name == INSTALL_STATE_FILE)
            ++sidecars;
    }
    CHECK(sidecars == 1);
}

TEST_CASE("an unreadable install state is not treated as absent or overwritten", "[PluginWheelInstall][PluginInstall]")
{
    ScopedTemporaryDir dir("sidecar-corrupt");
    const fs::path      sidecar = dir.path() / INSTALL_STATE_FILE;
    {
        std::ofstream out(sidecar.string());
        out << "{ this is not json";
    }

    PluginInstallState state;
    CHECK(read_install_state_status(dir.path(), state) == PluginInstallStateStatus::Invalid);
    CHECK_FALSE(write_install_state(dir.path(), state));

    // The corrupt file is left exactly as it was; only a later explicit repair may replace it.
    std::ifstream in(sidecar.string());
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(contents == "{ this is not json");
}

TEST_CASE("the on-disk opt-out wins over a stale descriptor during install", "[PluginWheelInstall][PluginInstall]")
{
    ScopedDataDir data_dir_guard("opt-out-sidecar");
    const fs::path plugin_root = data_dir_guard.plugins_dir() / "opt.whl";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "opt.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    PluginInstallState state;
    state.installed_from    = "local";
    state.installed_version = "0.9";
    state.wheel_install     = false;
    REQUIRE(write_install_state(plugin_root, state));

    // A stale catalog/manager copy claims the plugin is opted in; the sidecar says it is not.
    PluginDescriptor descriptor;
    descriptor.plugin_key            = "opt";
    descriptor.plugin_root           = plugin_root.string();
    descriptor.wheel_install_enabled = true;

    std::string error;
    REQUIRE(plugin_loader::install_plugin(wheel, /*cloud_user_id=*/"", descriptor, error));
    CHECK_FALSE(descriptor.wheel_install_enabled);

    PluginInstallState installed;
    REQUIRE(read_install_state(plugin_root, installed));
    CHECK_FALSE(installed.wheel_install);
}
