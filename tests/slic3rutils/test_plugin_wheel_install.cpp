#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <libslic3r/Utils.hpp>
#include <libslic3r/Zipper.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "catch2/catch_test_macros.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace wheel_install = Slic3r::plugin_wheel_install;

namespace {

// The long-path-capable form of a path, which the production code and a real uv (Rust std) use on
// Windows past MAX_PATH. A test that reaches a deep cache has to reach it the same way; on other
// platforms both helpers are the identity.
fs::path long_path(const fs::path& path)
{
    return extended_path(path);
}

std::string long_path_utf8(const fs::path& path)
{
#ifdef _WIN32
    return boost::nowide::narrow(extended_path(path).wstring());
#else
    return path.string();
#endif
}

void write_wheel(const fs::path& path,
                 const std::string& tag,
                 const std::string& module_payload,
                 const std::string& dist_name = "demo_pkg",
                 const std::string& version = "1.0")
{
    const fs::path io_path = long_path(path);
    if (fs::exists(io_path))
        fs::remove(io_path);

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
    descriptor.plugin_key    = "demo_plugin";
    descriptor.plugin_root   = plugin_root.string();
    descriptor.entry_path    = wheel.string();
    descriptor.entry_package = "demo_pkg";
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
        if (!fs::is_regular_file(long_path(staged))) {
            error = "fake uv: staged wheel missing";
            return false;
        }

        // A real uv writes the target even when it is past MAX_PATH, so this stand-in must too.
        fs::create_directories(long_path(target / "demo_pkg"));
        boost::nowide::ofstream init(long_path_utf8(target / "demo_pkg" / "__init__.py"));
        init << "VALUE = 1\n";
        return true;
    };
}

// Neither a staging directory nor a retired-previous directory may outlive a successful install.
bool has_install_scratch_leftovers(const fs::path& plugin_root)
{
    for (fs::directory_iterator it(long_path(plugin_root)); it != fs::directory_iterator(); ++it) {
        const std::string name = it->path().filename().string();
        if (name.rfind(wheel_install::STAGING_DIR_PREFIX, 0) == 0 ||
            name.rfind(wheel_install::PREVIOUS_DIR_PREFIX, 0) == 0)
            return true;
    }
    return false;
}

// A plugin folder deep enough that its install cache and staging paths exceed the legacy Windows
// MAX_PATH (260), while every component stays far below NAME_MAX so the path is also valid on
// Linux and macOS.
fs::path make_deep_plugin_root(const fs::path& base)
{
    fs::path path = base;
    while (path.string().size() < 300)
        path /= "deep_plugin_directory_component";
    return path / "plugin";
}

// A parent directory that still fits below MAX_PATH (260) while <parent>/plugin and a standard
// wheel file name inside it do not: discovery has to enumerate the entry past that limit.
fs::path make_boundary_plugin_parent(const fs::path& base)
{
    constexpr std::size_t parent_length = 230;
    const std::string     component     = "boundary_plugin_directory";
    fs::path              path          = base;
    while (path.string().size() + 1 + component.size() < parent_length)
        path /= component;
    if (path.string().size() + 1 < parent_length)
        path /= std::string(parent_length - path.string().size() - 1, 'p');
    return path;
}

} // namespace

TEST_CASE("wheel filenames are validated by their PEP 427 shape", "[PluginWheelInstall]")
{
    CHECK(wheel_install::is_valid_wheel_filename("demo-1.0-py3-none-any.whl"));
    CHECK(wheel_install::is_valid_wheel_filename("demo_pkg-1.0-1build-py3-none-any.whl"));
    CHECK(wheel_install::is_valid_wheel_filename("demo-1.0+local-cp312-cp312-manylinux_2_17_x86_64.whl"));
    CHECK(wheel_install::is_valid_wheel_filename("demo-1!2.0-py3-none-any.whl"));
    CHECK_FALSE(wheel_install::is_valid_wheel_filename("demo-1.0-py3-none-any!.whl"));

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
    CHECK(wheel_install::make_wheel_filename("demo", "1!2.0", "py3-none-any") ==
          "demo-1!2.0-py3-none-any.whl");

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

TEST_CASE("the uv wheel target reuses the legacy entry extraction path", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-target");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);

    // One cache location: the directory the pre-uv loader extracted the entry wheel into.
    const fs::path expected = plugin_root / "__whl_extracted__" / "demo_pkg";
    CHECK(fs::path(wheel_install::install_dir(plugin_root.string(), "demo_pkg")) == expected);
    CHECK(fs::path(wheel_install::install_dir(plugin_root.string(), "pkg.sub")) ==
          plugin_root / "__whl_extracted__" / "pkg.sub");

    // The entry package becomes a path component, so path syntax and traversal are refused.
    CHECK(wheel_install::install_dir(plugin_root.string(), "").empty());
    CHECK(wheel_install::install_dir(plugin_root.string(), "../escape").empty());
    CHECK(wheel_install::install_dir(plugin_root.string(), "pkg/sub").empty());
    CHECK(wheel_install::install_dir(plugin_root.string(), "pkg\\sub").empty());
    CHECK(wheel_install::install_dir(plugin_root.string(), ".hidden").empty());
    CHECK(wheel_install::install_dir(plugin_root.string(), "pkg..sub").empty());
}

TEST_CASE("a markerless legacy extraction is used directly without running uv", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-legacy-accept");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "plugin.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    // A pre-uv extraction, plus a separate dependency extraction that must survive.
    const fs::path legacy_target = plugin_root / wheel_install::EXTRACTED_DIR_NAME / "demo_pkg";
    const fs::path dep_dir       = plugin_root / wheel_install::EXTRACTED_DIR_NAME / "dep_pkg";
    fs::create_directories(legacy_target / "demo_pkg");
    fs::create_directories(dep_dir);
    {
        std::ofstream legacy_file((legacy_target / "written_by_plugin.txt").string());
        legacy_file << "old cache contents\n";
        std::ofstream init_file((legacy_target / "demo_pkg" / "__init__.py").string());
        init_file << "VALUE = 1\n";
        std::ofstream dep_file((dep_dir / "sentinel.txt").string());
        dep_file << "dependency extraction\n";
    }

    int         calls = 0;
    std::string error;
    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, &calls, "should not run")));
    CHECK(calls == 0);

    // The legacy extraction is importable as-is, so it is used without being rewritten.
    CHECK(fs::is_regular_file(legacy_target / "written_by_plugin.txt"));
    CHECK(fs::is_regular_file(legacy_target / "demo_pkg" / "__init__.py"));
    CHECK_FALSE(fs::exists(legacy_target / wheel_install::MARKER_FILE_NAME));

    // Dependency extractions under the same parent are untouched.
    CHECK(fs::is_regular_file(dep_dir / "sentinel.txt"));
    CHECK_FALSE(has_install_scratch_leftovers(plugin_root));
}

TEST_CASE("a markerless entry target without the entry package is replaced", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-legacy-incomplete");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "plugin.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    const fs::path target = plugin_root / wheel_install::EXTRACTED_DIR_NAME / "demo_pkg";
    fs::create_directories(target);
    {
        std::ofstream sentinel((target / "sentinel.txt").string());
        sentinel << "incomplete extraction\n";
    }

    int         calls = 0;
    std::string error;
    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, &calls, "")));
    CHECK(calls == 1);

    CHECK_FALSE(fs::exists(target / "sentinel.txt"));
    CHECK(fs::is_regular_file(target / "demo_pkg" / "__init__.py"));
    CHECK(fs::is_regular_file(target / wheel_install::MARKER_FILE_NAME));
}

TEST_CASE("an invalid marker still triggers reinstallation", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-invalid-marker");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "plugin.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    // A marker makes the target uv-managed even when it is unreadable, so the entry package that
    // is present does not turn it into an accepted legacy extraction.
    const fs::path target = plugin_root / wheel_install::EXTRACTED_DIR_NAME / "demo_pkg";
    fs::create_directories(target / "demo_pkg");
    {
        std::ofstream sentinel((target / "sentinel.txt").string());
        sentinel << "stale uv installation\n";
        std::ofstream marker((target / wheel_install::MARKER_FILE_NAME).string());
        marker << "not json\n";
    }

    int         calls = 0;
    std::string error;
    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, &calls, "")));
    CHECK(calls == 1);

    CHECK_FALSE(fs::exists(target / "sentinel.txt"));
    CHECK(fs::is_regular_file(target / "demo_pkg" / "__init__.py"));
}

TEST_CASE("the shared extraction parent alone is not a legacy entry extraction", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-parent-only");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "plugin.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    // Dependency extractions under __whl_extracted__/ do not make the entry look installed.
    const fs::path dep_dir = plugin_root / wheel_install::EXTRACTED_DIR_NAME / "dep_pkg";
    fs::create_directories(dep_dir);
    {
        std::ofstream dep_file((dep_dir / "__init__.py").string());
        dep_file << "DEPENDENCY = 1\n";
    }

    int         calls = 0;
    std::string error;
    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, &calls, "")));
    CHECK(calls == 1);

    CHECK(fs::is_directory(plugin_root / wheel_install::EXTRACTED_DIR_NAME / "demo_pkg"));
    CHECK(fs::is_regular_file(dep_dir / "__init__.py"));
}

TEST_CASE("a dependency extraction at the entry target is not replaced", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-collision");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "plugin.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");
    // A dependency wheel whose stem is the entry package maps to the same extraction directory.
    write_wheel(plugin_root / "demo_pkg.whl", "py3-none-any", "DEPENDENCY = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    const fs::path target = plugin_root / wheel_install::EXTRACTED_DIR_NAME / "demo_pkg";
    fs::create_directories(target);
    {
        std::ofstream sentinel((target / "sentinel.txt").string());
        sentinel << "dependency extraction\n";
    }

    int         calls = 0;
    std::string error;
    CHECK_FALSE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, &calls, "should not run")));
    CHECK(calls == 0);
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("collides"));

    // The dependency extraction is left exactly as it was.
    CHECK(fs::is_regular_file(target / "sentinel.txt"));
    CHECK_FALSE(fs::exists(target / "demo_pkg"));
}

TEST_CASE("the entry wheel itself is not treated as a dependency collision", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-entry-stem");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    // A stored wheel renamed to exactly the entry package: its stem matches the target directory,
    // but it is the entry and must not be mistaken for a dependency extraction.
    const fs::path wheel = plugin_root / "demo_pkg.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    int         calls = 0;
    std::string error;
    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, &calls, "")));
    CHECK(calls == 1);
    CHECK(fs::is_directory(plugin_root / wheel_install::EXTRACTED_DIR_NAME / "demo_pkg"));
}

TEST_CASE("an uppercase dependency wheel extension is treated as a wheel", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-uppercase-dependency");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "plugin.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");
    // The extension is matched case-insensitively everywhere a wheel is recognized, so this
    // dependency maps to the same extraction directory as the entry package and must be refused.
    write_wheel(plugin_root / "demo_pkg.WHL", "py3-none-any", "DEPENDENCY = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    int         calls = 0;
    std::string error;
    CHECK_FALSE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, &calls, "should not run")));
    CHECK(calls == 0);
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("collides"));
    CHECK_FALSE(fs::exists(plugin_root / wheel_install::EXTRACTED_DIR_NAME / "demo_pkg"));
}

TEST_CASE("non-ASCII entry package names are accepted", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-unicode-package");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);

    // "pkg_ü" in UTF-8. A Unicode Python identifier must remain a safe path component.
    const std::string unicode_package = "pkg_\xC3\xBC";
    CHECK(wheel_install::is_valid_entry_package(unicode_package));
    CHECK(fs::path(wheel_install::install_dir(plugin_root.string(), unicode_package)) ==
          plugin_root / wheel_install::EXTRACTED_DIR_NAME / unicode_package);
}

TEST_CASE("an unsafe entry package is rejected before touching the disk", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-unsafe-package");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "plugin.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    for (const std::string& entry_package :
         {"../escape", "pkg/sub", "pkg\\sub", "pkg..sub", ".hidden", "pkg*sub", "pkg:sub", "pkg\x01sub"}) {
        PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);
        descriptor.entry_package    = entry_package;

        int         calls = 0;
        std::string error;
        CHECK_FALSE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, &calls, "should not run")));
        CHECK(calls == 0);
        CHECK_FALSE(fs::exists(plugin_root / wheel_install::EXTRACTED_DIR_NAME));
    }
}

TEST_CASE("a failed uv install leaves the previous extraction in place", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-legacy-fail");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "plugin.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    const fs::path target = plugin_root / wheel_install::EXTRACTED_DIR_NAME / "demo_pkg";
    fs::create_directories(target);
    {
        std::ofstream sentinel((target / "sentinel.txt").string());
        sentinel << "previous contents\n";
    }

    std::string error;
    CHECK_FALSE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, nullptr, "simulated uv failure")));
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("simulated uv failure"));

    // uv failed before promotion, so the previous directory is still the only cache copy.
    CHECK(fs::is_regular_file(target / "sentinel.txt"));
    CHECK_FALSE(fs::exists(target / "demo_pkg"));
    CHECK_FALSE(has_install_scratch_leftovers(plugin_root));
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
    const std::string installed_dir = wheel_install::install_dir(plugin_root.string(), "demo_pkg");
    CHECK(uv_target != fs::path(installed_dir));
    CHECK(std::find(captured.begin(), captured.end(), "--no-deps") != captured.end());
    CHECK(std::find(captured.begin(), captured.end(), "--no-index") != captured.end());
    CHECK(std::find(captured.begin(), captured.end(), "--no-python-downloads") != captured.end());

    CHECK(fs::is_directory(installed_dir));
    CHECK(fs::path(installed_dir) == plugin_root / "__whl_extracted__" / "demo_pkg");
    CHECK(fs::is_regular_file(fs::path(installed_dir) / wheel_install::MARKER_FILE_NAME));
    CHECK_FALSE(has_install_scratch_leftovers(plugin_root));
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

TEST_CASE("an uppercase stored wheel extension is staged under a lowercase .whl name", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-uppercase");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "demo_pkg-1.0-py3-none-any.WHL";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    std::vector<std::string> captured;
    std::string              error;
    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(&captured, nullptr, "")));

    // The stored file keeps its name; uv only sees the lowercase extension it accepts.
    CHECK(fs::path(descriptor.entry_path).filename().string() == "demo_pkg-1.0-py3-none-any.WHL");
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

    CHECK_FALSE(fs::exists(wheel_install::install_dir(plugin_root.string(), "demo_pkg")));
    CHECK_FALSE(has_install_scratch_leftovers(plugin_root));
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

TEST_CASE("a wheel installs into a plugin path longer than MAX_PATH", "[PluginWheelInstall][Regression]")
{
    ScopedTemporaryDir dir("wheel-install-long-path");
    const fs::path      plugin_root = make_deep_plugin_root(dir.path());
    // Past the legacy Windows MAX_PATH (260): the staging, cache, marker and promotion paths are
    // all deeper still, so none of them is reachable without the extended-length form there. The
    // same tree is valid on Linux and macOS.
    REQUIRE(plugin_root.string().size() > 260);
    fs::create_directories(long_path(plugin_root));

    const fs::path wheel = plugin_root / "plugin.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    // An incomplete previous extraction: the install has to retire and replace it, so the
    // promotion renames and the cleanup of the retired directory run on deep paths too.
    const fs::path target = long_path(fs::path(wheel_install::install_dir(plugin_root.string(), "demo_pkg")));
    fs::create_directories(target);
    {
        boost::nowide::ofstream sentinel(long_path_utf8(target / "sentinel.txt"));
        sentinel << "previous contents\n";
    }

    std::vector<std::string> captured;
    int                      calls = 0;
    std::string              error;
    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(&captured, &calls, "")));
    CHECK(error.empty());
    CHECK(calls == 1);
    // The wheel's stored name is invalid, so it is staged under a PEP 427 name read from the
    // metadata of the deep wheel.
    CHECK(fs::path(captured.back()).filename().string() == "demo_pkg-1.0-py3-none-any.whl");

    // The new extraction, its marker and the entry package are all readable at the deep cache
    // path, and the retired previous extraction is gone.
    CHECK(fs::is_directory(target / "demo_pkg"));
    CHECK(fs::is_regular_file(target / "demo_pkg" / "__init__.py"));
    CHECK(fs::is_regular_file(target / wheel_install::MARKER_FILE_NAME));
    CHECK_FALSE(fs::exists(target / "sentinel.txt"));
    CHECK_FALSE(has_install_scratch_leftovers(plugin_root));

    // The marker and the entry wheel fingerprint are read back from the deep paths, so no uv run
    // and no rewrite happens on the next load.
    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, &calls, "should not run")));
    CHECK(calls == 1);

    // The scoped temp guard removes the base directory best-effort, but a path past MAX_PATH needs
    // the extended form to be removed at all.
    boost::system::error_code cleanup_ec;
    fs::remove_all(long_path(plugin_root), cleanup_ec);
}

TEST_CASE("an uppercase wheel extension is discovered as a wheel plugin", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-uppercase-discovery");
    const fs::path      plugin_parent = dir.path() / "plugins";
    const fs::path      plugin_root   = plugin_parent / "demo_plugin";
    fs::create_directories(plugin_root);
    // A package stored with an upper-case extension is still a wheel: discovery has to read its
    // metadata instead of reporting a .py entry that does not exist.
    const fs::path wheel = plugin_root / "demo_pkg-1.0-py3-none-any.WHL";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    std::string error;
    const std::vector<PluginDescriptor> discovered = discover_plugin_packages({plugin_parent.string()}, error);
    REQUIRE(error.empty());
    REQUIRE(discovered.size() == 1);
    CHECK(discovered.front().is_metadata_valid());
    CHECK(discovered.front().entry_package == "demo_pkg");
    CHECK(discovered.front().entry_path == wheel.string());
}

TEST_CASE("a wheel is discovered under a plugin folder just below MAX_PATH", "[PluginWheelInstall][Regression]")
{
    ScopedTemporaryDir dir("wheel-install-discovery-boundary");
    const fs::path      plugin_parent = make_boundary_plugin_parent(dir.path());
    const fs::path      plugin_root   = plugin_parent / "plugin";
    const fs::path      wheel         = plugin_root / "demo_pkg-1.0-py3-none-any.whl";
    // The plugin folder fits below MAX_PATH; the entry wheel inside it does not, so discovery has to
    // probe the children in extended form or the package looks like it has no entry file at all.
    REQUIRE(plugin_root.string().size() < 260);
    REQUIRE(wheel.string().size() > 260);
    fs::create_directories(long_path(plugin_root));
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");

    std::string error;
    const std::vector<PluginDescriptor> discovered = discover_plugin_packages({plugin_parent.string()}, error);
    REQUIRE(error.empty());
    REQUIRE(discovered.size() == 1);
    CHECK(discovered.front().is_metadata_valid());
    CHECK(discovered.front().entry_package == "demo_pkg");
    // The discovered paths keep their normal spelling; only the probes use the extended form.
    CHECK(discovered.front().plugin_root == plugin_root.string());
    CHECK(discovered.front().entry_path == wheel.string());

    // The install stages one level deeper still, and the next load reuses it from the marker.
    int         calls = 0;
    std::string install_error;
    REQUIRE(wheel_install::ensure_installed(discovered.front(), install_error, fake_uv(nullptr, &calls, "")));
    CHECK(calls == 1);

    const fs::path target = long_path(fs::path(wheel_install::install_dir(plugin_root.string(), "demo_pkg")));
    CHECK(fs::is_regular_file(target / wheel_install::MARKER_FILE_NAME));
    REQUIRE(wheel_install::ensure_installed(discovered.front(), install_error, fake_uv(nullptr, &calls, "should not run")));
    CHECK(calls == 1);

    boost::system::error_code cleanup_ec;
    fs::remove_all(long_path(plugin_root), cleanup_ec);
}

TEST_CASE("the install-state marker round-trips at a path longer than MAX_PATH", "[PluginWheelInstall][Regression]")
{
    ScopedTemporaryDir dir("plugin-state-long-path");
    const fs::path      plugin_root = make_deep_plugin_root(dir.path());
    REQUIRE(plugin_root.string().size() > 260);
    fs::create_directories(long_path(plugin_root));

    PluginDescriptor descriptor = make_descriptor(plugin_root, plugin_root / "plugin.whl");
    descriptor.name             = "Demo Plugin";

    // The sidecar is written through a temporary in the same deep directory and moved into place
    // with the platform replace primitive, then read back through the same extended paths.
    PluginInstallState state;
    {
        // The sidecar API expects the install-state lock across its read-modify-write callers.
        std::lock_guard<std::mutex> state_lock(install_state_mutex());
        REQUIRE(write_install_state(plugin_root, descriptor));
        REQUIRE(read_install_state(plugin_root, state));
    }
    CHECK(fs::is_regular_file(long_path(plugin_root / INSTALL_STATE_FILE)));
    CHECK(state.plugin_name == "Demo Plugin");
    CHECK(state.installed_from == "local");

    boost::system::error_code cleanup_ec;
    fs::remove_all(long_path(plugin_root), cleanup_ec);
}
