#include <boost/filesystem/path.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/file_status.hpp>
#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include "libslic3r/Utils.hpp"

#include "test_utils.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <thread>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <unistd.h>     // getuid
#endif

using namespace Slic3r;

TEST_CASE("per_user_temp_dir composes a per-user temp root", "[utils]") {
    const std::string base = "/tmp";

    SECTION("an empty id returns base unchanged") {
        REQUIRE(per_user_temp_dir(base, "") == base);
    }
    SECTION("a non-empty id is appended at the top level") {
        REQUIRE(per_user_temp_dir(base, "1000") == base + "/orcaslicer_1000");
    }
    SECTION("distinct ids produce distinct roots") {
        REQUIRE(per_user_temp_dir(base, "1000") != per_user_temp_dir(base, "1001"));
    }
}

TEST_CASE("per_user_temp_id follows the platform contract", "[utils]") {
    const std::string id = per_user_temp_id();

    SECTION("stable across calls") {
        REQUIRE(per_user_temp_id() == id);
    }
#ifdef _WIN32
    SECTION("empty on Windows (its temp dir is already per-user)") {
        REQUIRE(id.empty());
    }
#else
    SECTION("the current uid on Linux/macOS") {
        REQUIRE_FALSE(id.empty());
        REQUIRE(id == std::to_string(static_cast<unsigned long>(::getuid())));
    }
#endif
}

// The end-to-end contract callers depend on: the temp root is left alone on
// Windows and isolated per user on Linux/macOS.
TEST_CASE("per-user temp root is unchanged on Windows, isolated elsewhere", "[utils]") {
    const std::string base = "/tmp";
    const std::string root = per_user_temp_dir(base, per_user_temp_id());
#ifdef _WIN32
    REQUIRE(root == base);
#else
    REQUIRE(root != base);
    REQUIRE_THAT(root, Catch::Matchers::StartsWith(base + "/orcaslicer_"));
#endif
}

TEST_CASE("write_file_atomically replaces the target and leaves no temporary file", "[utils]") {
    ScopedTemporaryDir dir;
    const boost::filesystem::path target = dir.path() / "preset.json";

    REQUIRE_FALSE(write_file_atomically(target.string(), "first"));
    REQUIRE_FALSE(write_file_atomically(target.string(), "second"));

    std::string content;
    load_string_file(target, content);
    REQUIRE(content == "second");
    size_t entries = 0;
    for (auto &entry : boost::filesystem::directory_iterator(dir.path())) {
        (void) entry;
        ++entries;
    }
    REQUIRE(entries == 1);
}

TEST_CASE("write_file_atomically reports a missing directory and writes nothing", "[utils]") {
    ScopedTemporaryDir dir;
    const boost::filesystem::path target = dir.path() / "missing" / "preset.json";

    const std::error_code ec = write_file_atomically(target.string(), "x");
    REQUIRE(ec == std::errc::no_such_file_or_directory);
    REQUIRE_FALSE(boost::filesystem::exists(target));
}

#ifndef _WIN32
// The read-only bit on a directory stops file creation only on POSIX.
TEST_CASE("write_file_atomically writes in place when no temporary can be created beside an existing target", "[utils]") {
    if (::geteuid() == 0)
        SKIP("a read-only directory does not stop root");
    ScopedTemporaryDir dir;
    const boost::filesystem::path target = dir.path() / "preset.json";
    REQUIRE_FALSE(write_file_atomically(target.string(), "first"));
    boost::filesystem::permissions(dir.path(), boost::filesystem::owner_read | boost::filesystem::owner_exe);

    const std::error_code replaced = write_file_atomically(target.string(), "second");
    const std::error_code created  = write_file_atomically((dir.path() / "new.json").string(), "x");
    // Restored before any assertion, so a failure never leaves an unremovable directory behind.
    boost::filesystem::permissions(dir.path(), boost::filesystem::owner_all);

    REQUIRE_FALSE(replaced);
    REQUIRE(created == std::errc::permission_denied);
    std::string content;
    load_string_file(target, content);
    REQUIRE(content == "second");
}
#endif

TEST_CASE("write_file_atomically keeps bytes intact in binary mode", "[utils]") {
    ScopedTemporaryDir dir;
    const boost::filesystem::path target = dir.path() / "blob.bin";
    const std::string bytes("a\r\nb\0c", 6);

    REQUIRE_FALSE(write_file_atomically(target.string(), bytes, /*binary=*/true));
    REQUIRE(boost::filesystem::file_size(target) == bytes.size());
}

#ifndef _WIN32
TEST_CASE("write_file_atomically writes through a symlink and keeps the target's permissions", "[utils]") {
    ScopedTemporaryDir dir;
    const boost::filesystem::path real = dir.path() / "real.json";
    const boost::filesystem::path link = dir.path() / "link.json";
    REQUIRE_FALSE(write_file_atomically(real.string(), "first"));
    boost::filesystem::permissions(real, boost::filesystem::owner_read | boost::filesystem::owner_write);
    boost::filesystem::create_symlink(real, link);

    REQUIRE_FALSE(write_file_atomically(link.string(), "second"));

    REQUIRE(boost::filesystem::is_symlink(boost::filesystem::symlink_status(link)));
    std::string content;
    load_string_file(real, content);
    REQUIRE(content == "second");

    REQUIRE_FALSE(write_file_atomically(real.string(), "third"));
    const auto perms = boost::filesystem::status(real).permissions() & boost::filesystem::all_all;
    REQUIRE(perms == (boost::filesystem::owner_read | boost::filesystem::owner_write));
}
#endif

TEST_CASE("write_file_atomically survives two threads writing one target", "[utils]") {
    ScopedTemporaryDir dir;
    const boost::filesystem::path target = dir.path() / "shared.json";
    const std::string a(20000, 'a'), b(20000, 'b');

    std::thread other([&] {
        for (int i = 0; i < 50; ++i)
            write_file_atomically(target.string(), a);
    });
    for (int i = 0; i < 50; ++i)
        write_file_atomically(target.string(), b);
    other.join();

    std::string content;
    load_string_file(target, content);
    const bool whole = content == a || content == b;
    REQUIRE(whole);
    // No temporary may be left; a scanner on Windows may briefly hold the old
    // file under another name, so only the temporaries are counted.
    size_t temporaries = 0;
    for (auto &entry : boost::filesystem::directory_iterator(dir.path()))
        if (entry.path().extension() == ".tmp")
            ++temporaries;
    REQUIRE(temporaries == 0);
}

TEST_CASE("copy_file reports the OS error when the destination cannot be written", "[utils]") {
    ScopedTemporaryFile source(".txt");
    {
        std::ofstream ofs(source.string(), std::ios::binary);
        ofs << "orca";
    }
    REQUIRE(boost::filesystem::exists(source.path()));

    // A directory that was never created, so the copy fails on every platform.
    const boost::filesystem::path destination = source.path().parent_path() / "orca-missing-dir" / "copy.txt";
    REQUIRE_FALSE(boost::filesystem::exists(destination.parent_path()));

    std::string error_message;
    REQUIRE(copy_file(source.string(), destination.string(), error_message) == FAIL_COPY_FILE);
    REQUIRE_FALSE(error_message.empty());

#ifdef _WIN32
    // The Windows branch formats GetLastError() itself. Writing that as
    // "Error: " + errCode adds an integer to a string literal, which indexes into the
    // literal instead of appending and runs off its end for any code above 7.
    const std::string prefix = "Error: ";
    REQUIRE(error_message.rfind(prefix, 0) == 0);

    const std::string code = error_message.substr(prefix.size());
    REQUIRE_FALSE(code.empty());
    REQUIRE(std::all_of(code.begin(), code.end(), [](unsigned char c) { return std::isdigit(c) != 0; }));
#endif // _WIN32
}

TEST_CASE("A resolved input path still names the same file after the working directory changes", "[utils]") {
    ScopedTemporaryFile model(".3mf");
    { std::ofstream out(model.string()); out << "3mf"; }
    const std::string name = model.path().filename().string();

    // Resolve the bare name from the directory holding the file, then move away from it. The guard
    // restores the directory the test started in, wherever this leaves it.
    ScopedWorkingDirectory cwd(model.path().parent_path());
    const std::string resolved = resolve_cli_input_path(name);
    boost::filesystem::current_path(boost::filesystem::path(TEST_DATA_DIR));

    REQUIRE(boost::filesystem::exists(resolved));
    REQUIRE(boost::filesystem::equivalent(resolved, model.path()));
    // Control: the bare name finds nothing from here, so resolving it this late would have failed.
    REQUIRE_FALSE(boost::filesystem::exists(name));
}

TEST_CASE("resolve_cli_input_path completes a relative path against the working directory", "[utils]") {
    ScopedWorkingDirectory cwd(boost::filesystem::temp_directory_path());
    // Read back rather than reusing temp_directory_path(): changing to it resolves any symlink.
    const boost::filesystem::path here = boost::filesystem::current_path();

    SECTION("a bare name") {
        REQUIRE(resolve_cli_input_path("model.3mf") == (here / "model.3mf").make_preferred().string());
    }
    SECTION("a ./ prefix is dropped") {
        REQUIRE(resolve_cli_input_path("./model.3mf") == (here / "model.3mf").make_preferred().string());
    }
    SECTION("a ../ traversal is collapsed") {
        REQUIRE(resolve_cli_input_path("../model.3mf") == (here.parent_path() / "model.3mf").make_preferred().string());
    }
}

TEST_CASE("resolve_cli_input_path leaves inputs that must not be completed unchanged", "[utils]") {
    SECTION("an absolute path") {
        const boost::filesystem::path absolute = (boost::filesystem::temp_directory_path() / "model.3mf").make_preferred();
        REQUIRE(resolve_cli_input_path(absolute.string()) == absolute.string());
    }
#ifdef _WIN32
    // Every absolute form Windows accepts opens today, so each must come back byte for byte:
    // normalizing them would rewrite the forward slashes and rebuild the \\?\ and UNC prefixes.
    SECTION("an absolute Windows path of any form") {
        for (const std::string absolute : {R"(C:\models\model.3mf)",
                                           R"(C:/models/model.3mf)",
                                           R"(\\server\share\model.3mf)",
                                           R"(\\?\C:\models\model.3mf)"})
            REQUIRE(resolve_cli_input_path(absolute) == absolute);
    }
#endif
    // These are downloaded rather than opened, and completing one would produce a path, not a URL.
    SECTION("a custom open protocol URL") {
        for (const std::string url : {"orcaslicer://open/?file=https://example.com/model.3mf",
                                      "prusaslicer://open/?file=https://example.com/model.3mf",
                                      "bambustudio://open/?file=https://example.com/model.3mf",
                                      "cura://open/?file=https://example.com/model.3mf"})
            REQUIRE(resolve_cli_input_path(url) == url);
    }
    SECTION("an empty argument") {
        REQUIRE(resolve_cli_input_path("").empty());
    }
}

TEST_CASE("sanitize_file_basename keeps only a plain file name from an untrusted name", "[Utils]") {
    const std::string unicode = "\xe6\xa8\xa1\xe5\x9e\x8b \xc3\xa9t\xc3\xa9.3mf"; // UTF-8 CJK and accented Latin
    const auto [input, expected] = GENERATE_COPY(table<std::string, std::string>({
        {"normal.3mf", "normal.3mf"},
        {"../../x.3mf", "x.3mf"},
        {"..\\..\\x.3mf", "x.3mf"},
        {"C:\\x.3mf", "x.3mf"},
        {"C:x.3mf", "C_x.3mf"},
        {"/etc/x", "x"},
        {"a/b\\c.gcode", "c.gcode"},
        {"x:stream", "x_stream"}, // no NTFS alternate data stream
        {"x.", "x."},
        {".3mf", ".3mf"},
        {unicode, unicode},
    }));
    CAPTURE(input);
    CHECK(sanitize_file_basename(input) == expected);
}

TEST_CASE("sanitize_file_basename rejects names that do not name a file", "[Utils]") {
    const std::string input = GENERATE(as<std::string>{}, "", ".", "..", "../..", "dir/", "..\\", " ", ". .", "...");
    CAPTURE(input);
    CHECK(sanitize_file_basename(input).empty());
}

namespace {
void touch(const boost::filesystem::path &path) { std::ofstream(path.string()) << "existing"; }
std::string file_contents(const boost::filesystem::path &path)
{
    std::ifstream file(path.string());
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("find_unused_filename keeps a name nothing uses", "[Utils]") {
    ScopedTemporaryDir dir;
    std::string name;
    REQUIRE(find_unused_filename(dir.path(), "model.3mf", {}, name));
    CHECK(name == "model.3mf");
}

TEST_CASE("find_unused_filename versions a name an existing file uses", "[Utils]") {
    ScopedTemporaryDir dir;
    touch(dir.path() / "model.3mf");
    std::string name;
    REQUIRE(find_unused_filename(dir.path(), "model.3mf", {}, name));
    CHECK(name == "model(1).3mf");
}

TEST_CASE("find_unused_filename versions a name that maps onto an existing file once sanitized", "[Utils]") {
    ScopedTemporaryDir dir;
    touch(dir.path() / "my_model.3mf");
    const std::string input = GENERATE(as<std::string>{}, "my?model.3mf", "my:model.3mf", "my*model.3mf");
    CAPTURE(input);
    std::string name;
    REQUIRE(find_unused_filename(dir.path(), input, {}, name));
    CHECK(name == "my_model(1).3mf");
    CHECK(file_contents(dir.path() / "my_model.3mf") == "existing");
}

TEST_CASE("find_unused_filename treats the marker of another download as used", "[Utils]") {
    ScopedTemporaryDir dir;
    touch(download_marker_path(dir.path(), "model.3mf"));
    std::string name;
    REQUIRE(find_unused_filename(dir.path(), "model.3mf", {}, name));
    CHECK(name == "model(1).3mf");
}

TEST_CASE("find_unused_filename ignores the marker of the download asking", "[Utils]") {
    ScopedTemporaryDir dir;
    const boost::filesystem::path own_marker = download_marker_path(dir.path(), "model.3mf");
    touch(own_marker);
    std::string name;
    REQUIRE(find_unused_filename(dir.path(), "model.3mf", own_marker, name));
    CHECK(name == "model.3mf");
}

TEST_CASE("find_unused_filename gives up after 999 versions", "[Utils]") {
    ScopedTemporaryDir dir;
    touch(dir.path() / "model.3mf");
    for (int version = 1; version < 999; ++version)
        touch(dir.path() / ("model(" + std::to_string(version) + ").3mf"));
    std::string name;
    REQUIRE(find_unused_filename(dir.path(), "model.3mf", {}, name));
    CHECK(name == "model(999).3mf");

    touch(dir.path() / name);
    REQUIRE_FALSE(find_unused_filename(dir.path(), "model.3mf", {}, name));
    CHECK(name == "model(999).3mf");
}

TEST_CASE("is_path_within_root accepts a root given with a trailing separator", "[utils]") {
    ScopedTemporaryDir tmp;
    const std::string root = tmp.path().string();
    const std::string with_separator = GENERATE_COPY(root + "/", root + std::string(1, static_cast<char>(boost::filesystem::path::preferred_separator)));

    CAPTURE(with_separator);
    CHECK(is_path_within_root("vendor.json", with_separator));
    CHECK(is_path_within_root("vendor/machine/printer.json", with_separator));
    CHECK_FALSE(is_path_within_root("../vendor.json", with_separator));
}

TEST_CASE("is_path_within_root treats Windows-specific name forms the same on every platform", "[utils]") {
    ScopedTemporaryDir tmp;

    SECTION("names ending in dots or spaces stay inside the root") {
        const std::string name = GENERATE(std::string("name."), std::string("name "), std::string("dir./file.json"), std::string("dir /file.json"));
        CAPTURE(name);
        CHECK(is_path_within_root(name, tmp.path()));
    }
    SECTION("drive-relative names are rejected") {
        const std::string name = GENERATE(std::string("C:x"), std::string("c:x/y.json"), std::string("C:"));
        CAPTURE(name);
        CHECK_FALSE(is_path_within_root(name, tmp.path()));
    }
}

TEST_CASE("is_path_within_root rejects a name with an embedded NUL", "[utils]") {
    ScopedTemporaryDir tmp;
    // The filesystem calls stop at the NUL, so they would act on a different path than the one checked.
    const std::string name = GENERATE(std::string("..\0", 3), std::string("..\0x/file.json", 14), std::string("sub/..\0x", 8),
                                      std::string("file.json\0", 10), std::string("\0file.json", 10));
    CAPTURE(name.size());
    CHECK_FALSE(is_path_within_root(name, tmp.path()));
}

TEST_CASE("is_symlink_target_within_root accepts relative targets that stay inside the root", "[utils]") {
    ScopedTemporaryDir tmp;
    const auto [link, target] = GENERATE(std::make_pair(std::string("Versions/Current"), std::string("A")),
                                         std::make_pair(std::string("Foo.framework/Foo"), std::string("Versions/Current/Foo")),
                                         std::make_pair(std::string("libfoo.so"), std::string("libfoo.so.1")),
                                         std::make_pair(std::string("a/b/link"), std::string("c/d")));
    CAPTURE(link, target);
    CHECK(is_symlink_target_within_root(link, target, tmp.path()));
}

TEST_CASE("is_symlink_target_within_root rejects absolute targets and targets that climb out", "[utils]") {
    ScopedTemporaryDir tmp;
    const std::string outside = (tmp.path().parent_path() / "outside").generic_string();
    const auto [link, target] = GENERATE_COPY(std::make_pair(std::string("sub/link"), outside),
                                              std::make_pair(std::string("sub/link"), std::string("/etc/passwd")),
                                              std::make_pair(std::string("sub/link"), std::string("\\outside")),
                                              std::make_pair(std::string("sub/link"), std::string("C:/outside")),
                                              std::make_pair(std::string("sub/link"), std::string("C:outside")),
                                              std::make_pair(std::string("sub/link"), std::string("")),
                                              std::make_pair(std::string("link"), std::string("..")),
                                              std::make_pair(std::string("link"), std::string("../outside")),
                                              std::make_pair(std::string("sub/link"), std::string("../../outside")),
                                              std::make_pair(std::string("sub/link"), std::string("x/../../../outside")),
                                              std::make_pair(std::string("sub/link"), std::string("..\\..\\outside")),
                                              // symlink() stops at the NUL, so this target would be created as "..".
                                              std::make_pair(std::string("link"), std::string("..\0", 3)));
    CAPTURE(link, target);
    CHECK_FALSE(is_symlink_target_within_root(link, target, tmp.path()));
}

#ifndef _WIN32
TEST_CASE("is_symlink_target_within_root rejects a target that passes through a symlink leading out", "[utils]") {
    ScopedTemporaryDir tmp;
    const boost::filesystem::path root    = tmp.path() / "root";
    const boost::filesystem::path outside = tmp.path() / "outside";
    boost::filesystem::create_directories(root);
    boost::filesystem::create_directories(outside);
    boost::filesystem::create_symlink(outside, root / "out");

    CHECK_FALSE(is_symlink_target_within_root("link", "out/lib.so", root));
    CHECK(is_symlink_target_within_root("link", "in/lib.so", root));
}
#endif

TEST_CASE("is_absolute_path_within_root accepts only entries inside the root", "[utils]") {
    namespace fs = boost::filesystem;
    ScopedTemporaryDir outer;
    const fs::path root = outer.path() / "Auxiliaries";
    fs::create_directories(root / "Others");
    const fs::path inside = root / "Others" / "note.txt";
    const fs::path outside = outer.path() / "secret.txt";
    std::ofstream(inside.string()) << "inside";
    std::ofstream(outside.string()) << "outside";

    SECTION("a file inside the root") {
        REQUIRE(is_absolute_path_within_root(inside, root));
    }
    SECTION("a path inside the root whose file does not exist yet") {
        REQUIRE(is_absolute_path_within_root(root / "Others" / "missing.txt", root));
    }
    SECTION("the root itself") {
        REQUIRE_FALSE(is_absolute_path_within_root(root, root));
    }
    SECTION("a parent-directory escape spelled under the root") {
        REQUIRE_FALSE(is_absolute_path_within_root(root / "Others" / ".." / ".." / "secret.txt", root));
    }
    SECTION("an absolute path elsewhere") {
        REQUIRE_FALSE(is_absolute_path_within_root(outside, root));
    }
    SECTION("a sibling directory sharing the root's name as a prefix") {
        const fs::path sibling = outer.path() / "Auxiliaries2" / "note.txt";
        REQUIRE_FALSE(is_absolute_path_within_root(sibling, root));
    }
    SECTION("a relative path") {
        REQUIRE_FALSE(is_absolute_path_within_root(fs::path("Others") / "note.txt", root));
    }
    SECTION("an empty path") {
        REQUIRE_FALSE(is_absolute_path_within_root(fs::path(), root));
    }
#ifndef _WIN32
    // Creating symlinks on Windows needs elevated rights or developer mode.
    SECTION("a symlink inside the root that points outside") {
        const fs::path link = root / "Others" / "link.txt";
        fs::create_symlink(outside, link);
        REQUIRE_FALSE(is_absolute_path_within_root(link, root));
    }
#endif
}

TEST_CASE("is_safe_to_open_file_name accepts plain documents, images and models", "[utils]") {
    const std::string safe = GENERATE(as<std::string>{},
        "Manual.pdf", "BOM.xlsx", "BOM.csv", "guide.docx", "notes.txt", "README.md", "photo.JPG", "render.png",
        "assembly.step", "part.stl", "project.3mf", "drawing.dxf", "build.mp4", "setup.exe.pdf", ".pdf");
    INFO(safe);
    CHECK(is_safe_to_open_file_name(safe));
}

TEST_CASE("is_safe_to_open_file_name rejects programs and anything it does not know", "[utils]") {
    const std::string unsafe = GENERATE(as<std::string>{},
        "setup.exe", "SETUP.EXE", "Manual.pdf.exe", "run.bat", "shortcut.lnk", "site.url", "script.ps1", "help.chm",
        "tool.jar", "script.py", "Install.command", "install.sh", "launcher.desktop", "Printer.AppImage",
        // Documents that can carry macros or scripts.
        "BOM.xls", "BOM.xlsm", "guide.doc", "guide.docm", "sheet.ods", "page.html", "logo.svg", "bundle.zip",
        // No extension, an unknown one, or a name the desktop would read differently.
        "readme", "pdf", "data.xyz", "", "...", "Manual.pdf.", "Manual.pdf ", "setup.exe:note.txt", "dir.pdf/readme");
    INFO(unsafe);
    CHECK_FALSE(is_safe_to_open_file_name(unsafe));
}
