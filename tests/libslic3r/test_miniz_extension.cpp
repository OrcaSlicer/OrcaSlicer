#include <catch2/catch_all.hpp>

#include "libslic3r/miniz_extension.hpp"

#include "test_utils.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

void write_zip(const fs::path &zip_file, const std::vector<std::pair<std::string, std::string>> &entries)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_writer(&zip, zip_file.string()));
    for (const auto &[name, content] : entries)
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    REQUIRE(close_zip_writer(&zip));
}

// miniz refuses to write a name starting with '/', so write a placeholder of the same length and patch it in place.
void rename_entry(const fs::path &zip_file, const std::string &from, const std::string &to)
{
    REQUIRE(from.size() == to.size());
    std::string bytes;
    {
        std::ifstream in(zip_file.string(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    size_t count = 0;
    for (size_t pos = bytes.find(from); pos != std::string::npos; pos = bytes.find(from, pos + to.size()), ++count)
        bytes.replace(pos, from.size(), to);
    // Once in the local header and once in the central directory.
    REQUIRE(count == 2);
    std::ofstream out(zip_file.string(), std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::vector<std::string> list_dir(const fs::path &dir)
{
    std::vector<std::string> names;
    for (const fs::directory_entry &entry : fs::directory_iterator(dir))
        names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

std::string read_file(const fs::path &file)
{
    std::ifstream in(file.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("Confined extraction writes a well-formed archive under the target directory", "[MinizExtension]")
{
    ScopedTemporaryDir tmp;
    const fs::path     zip_file = tmp.path() / "bundle.zip";
    const fs::path     target   = tmp.path() / "cache";
    fs::create_directories(target);
    write_zip(zip_file, {{"vendor/", ""}, {"vendor/machine/", ""}, {"vendor.json", "{\"a\":1}"}, {"vendor/machine/printer.json", "{\"b\":2}"}});

    REQUIRE(extract_archive_confined(zip_file.string(), target.string()));
    CHECK(fs::is_directory(target / "vendor"));
    CHECK(read_file(target / "vendor.json") == "{\"a\":1}");
    CHECK(read_file(target / "vendor" / "machine" / "printer.json") == "{\"b\":2}");
}

TEST_CASE("Confined extraction rejects an archive with an entry outside the target directory", "[MinizExtension]")
{
    ScopedTemporaryDir tmp;
    const fs::path     zip_file = tmp.path() / "bundle.zip";
    const fs::path     target   = tmp.path() / "cache";
    fs::create_directories(target);

    const std::string escaping_entry = GENERATE(std::string("../escape.txt"), std::string("..\\escape.txt"),
                                                std::string("sub/../../escape.txt"), std::string("C:/escape.txt"),
                                                std::string("C:escape.txt"), std::string("\\escape.txt"));
    // The normal entry comes first so a per-entry check would already have written it.
    write_zip(zip_file, {{"normal.json", "{}"}, {escaping_entry, "escaped"}});

    CAPTURE(escaping_entry);
    CHECK_FALSE(extract_archive_confined(zip_file.string(), target.string()));
    CHECK_FALSE(fs::exists(tmp.path() / "escape.txt"));
    CHECK(fs::is_empty(target));
}

TEST_CASE("Confined extraction rejects an archive with an absolute entry name", "[MinizExtension]")
{
    ScopedTemporaryDir tmp;
    const fs::path     zip_file = tmp.path() / "bundle.zip";
    const fs::path     target   = tmp.path() / "cache";
    fs::create_directories(target);

    const std::string absolute    = (tmp.path() / "escape.txt").generic_string();
    const std::string placeholder = "#" + absolute.substr(1);
    write_zip(zip_file, {{"normal.json", "{}"}, {placeholder, "escaped"}});
    rename_entry(zip_file, placeholder, absolute);

    CHECK_FALSE(extract_archive_confined(zip_file.string(), target.string()));
    CHECK_FALSE(fs::exists(tmp.path() / "escape.txt"));
    CHECK(fs::is_empty(target));
}

TEST_CASE("Confined extraction rejects a directory entry outside the target directory", "[MinizExtension]")
{
    ScopedTemporaryDir tmp;
    const fs::path     zip_file = tmp.path() / "bundle.zip";
    const fs::path     target   = tmp.path() / "cache";
    fs::create_directories(target);
    write_zip(zip_file, {{"vendor/", ""}, {"../outside/", ""}});

    CHECK_FALSE(extract_archive_confined(zip_file.string(), target.string()));
    CHECK_FALSE(fs::exists(tmp.path() / "outside"));
    CHECK(fs::is_empty(target));
}

TEST_CASE("Confined extraction validates zero-size entries like any other", "[MinizExtension]")
{
    ScopedTemporaryDir tmp;
    const fs::path     zip_file = tmp.path() / "bundle.zip";
    const fs::path     target   = tmp.path() / "cache";
    fs::create_directories(target);

    SECTION("an empty file inside the target does not fail the archive") {
        write_zip(zip_file, {{"empty.json", ""}, {"vendor.json", "{}"}});
        CHECK(extract_archive_confined(zip_file.string(), target.string()));
        CHECK(read_file(target / "vendor.json") == "{}");
    }
    SECTION("an empty file outside the target rejects the archive") {
        write_zip(zip_file, {{"vendor.json", "{}"}, {"../escape.txt", ""}});
        CHECK_FALSE(extract_archive_confined(zip_file.string(), target.string()));
        CHECK_FALSE(fs::exists(tmp.path() / "escape.txt"));
        CHECK(fs::is_empty(target));
    }
}

TEST_CASE("Confined extraction writes nothing outside the target for Windows-specific name forms", "[MinizExtension]")
{
    ScopedTemporaryDir tmp;
    const fs::path     zip_file = tmp.path() / "bundle.zip";
    const fs::path     target   = tmp.path() / "cache";
    fs::create_directories(target);

    // Windows strips trailing dots and spaces and maps device names; whether these extract depends on the
    // platform, but none of them may land beside the target.
    const std::string name = GENERATE(std::string("name."), std::string("name "), std::string("..."), std::string(".. "),
                                      std::string(".. /escape.txt"), std::string(".../escape.txt"), std::string("CON"),
                                      std::string("sub/NUL.txt"), std::string("C:escape.txt"));
    write_zip(zip_file, {{name, "payload"}});

    CAPTURE(name);
    extract_archive_confined(zip_file.string(), target.string());
    CHECK(list_dir(tmp.path()) == std::vector<std::string>{"bundle.zip", "cache"});
}

#ifndef _WIN32
TEST_CASE("Confined extraction replaces a symlink at the destination instead of writing through it", "[MinizExtension]")
{
    ScopedTemporaryDir tmp;
    const fs::path     zip_file = tmp.path() / "bundle.zip";
    const fs::path     target   = tmp.path() / "cache";
    const fs::path     outside  = tmp.path() / "outside";
    fs::create_directories(target);
    fs::create_directories(outside);
    write_zip(zip_file, {{"vendor.json", "{\"a\":1}"}});

    SECTION("a dangling symlink") {
        fs::create_symlink(outside / "vendor.json", target / "vendor.json");
        CHECK(extract_archive_confined(zip_file.string(), target.string()));
        CHECK_FALSE(fs::exists(outside / "vendor.json"));
        CHECK_FALSE(fs::is_symlink(fs::symlink_status(target / "vendor.json")));
        CHECK(read_file(target / "vendor.json") == "{\"a\":1}");
    }
    SECTION("a symlink to an existing file") {
        { std::ofstream((outside / "vendor.json").string()) << "original"; }
        fs::create_symlink(outside / "vendor.json", target / "vendor.json");
        extract_archive_confined(zip_file.string(), target.string());
        CHECK(read_file(outside / "vendor.json") == "original");
    }
}
#endif
