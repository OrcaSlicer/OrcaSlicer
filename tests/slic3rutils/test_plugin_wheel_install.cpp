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

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

#include "catch2/catch_test_macros.hpp"
#include "catch2/matchers/catch_matchers.hpp"
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
    CHECK_FALSE(has_staging_leftovers(plugin_root));
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

TEST_CASE("an uppercase dependency wheel extension is not treated as a collision", "[PluginWheelInstall]")
{
    ScopedTemporaryDir dir("wheel-install-uppercase-dependency");
    const fs::path      plugin_root = dir.path() / "plugin";
    fs::create_directories(plugin_root);
    const fs::path wheel = plugin_root / "plugin.whl";
    write_wheel(wheel, "py3-none-any", "VALUE = 1\n");
    // The dependency loader extracts only lowercase ".whl" files, so this one is ignored and the
    // entry may use the same directory stem.
    write_wheel(plugin_root / "demo_pkg.WHL", "py3-none-any", "DEPENDENCY = 1\n");

    const PluginDescriptor descriptor = make_descriptor(plugin_root, wheel);

    int         calls = 0;
    std::string error;
    REQUIRE(wheel_install::ensure_installed(descriptor, error, fake_uv(nullptr, &calls, "")));
    CHECK(calls == 1);
    CHECK(fs::is_directory(plugin_root / wheel_install::EXTRACTED_DIR_NAME / "demo_pkg"));
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
    CHECK_FALSE(has_staging_leftovers(plugin_root));
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
