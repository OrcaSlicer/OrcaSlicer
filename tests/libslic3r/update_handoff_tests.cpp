#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/filesystem.hpp>
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <locale>
#include <string>
#include <vector>

#include "catch2/catch_test_macros.hpp"
#include "libslic3r/UpdateHandoff.hpp"
#include "libslic3r/Utils.hpp"
#include "test_utils.hpp"

using namespace Slic3r;

namespace {

namespace fs = boost::filesystem;

// Sets the data directory for one test case and puts the previous one back.
class ScopedDataDir
{
public:
    explicit ScopedDataDir(const fs::path& dir) : m_previous(data_dir()) { set_data_dir(dir.string()); }
    ~ScopedDataDir() { set_data_dir(m_previous); }

private:
    std::string m_previous;
};

// Converts paths as UTF-8 for the test's lifetime, as the app does from startup
// (boost::nowide::nowide_filesystem() in OrcaSlicer.cpp). Without it Boost.Filesystem reads a
// narrow path as the ANSI code page on Windows, while the file writes go through nowide.
class Utf8PathConversion
{
public:
    Utf8PathConversion() : m_previous(boost::nowide::nowide_filesystem()) {}
    ~Utf8PathConversion() { fs::path::imbue(m_previous); }

private:
    std::locale m_previous;
};

const char* kAsset  = "OrcaSlicer_Linux_AppImage_Ubuntu2404_V2.5.0.AppImage";
const char* kDigest = "sha256:1111";
constexpr int64_t kSize = 1000;

std::string read_text(const fs::path& path)
{
    std::ifstream in(path.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_text(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    out << text;
}

fs::path part_path(const fs::path& dir)
{
    return dir / (std::string(kAsset) + ".part");
}

fs::path marker_path(const fs::path& dir)
{
    return dir / (std::string(kAsset) + ".part.digest");
}

// A download directory holding a partial download of the artifact, recorded for `digest`.
void write_partial_download(const fs::path& dir, const std::string& digest, std::size_t bytes)
{
    write_text(part_path(dir), std::string(bytes, 'x'));
    if (!digest.empty())
        write_text(marker_path(dir), digest);
}

bool contains(const std::vector<std::string>& paths, const fs::path& path)
{
    for (const std::string& entry : paths)
        if (fs::path(entry) == path)
            return true;
    return false;
}

} // namespace

TEST_CASE("The updater cache belongs to the OS cache directory", "[UpdateHandoff]")
{
    ScopedTemporaryDir data;
    ScopedDataDir      data_dir_guard(data.path());
    clear_updater_cache_dir();

    const fs::path cache = fs::path(updater_cache_dir());
    CHECK(cache.filename() == "updater");
    CHECK(cache.parent_path().filename() == "OrcaSlicer");

    // The whole point of the move: nothing is written into the data directory.
    const std::string cache_text = cache.generic_string();
    const std::string data_text  = fs::path(data.path()).generic_string();
    CHECK(cache_text.compare(0, data_text.size(), data_text) != 0);
}

TEST_CASE("An explicit cache directory wins until it is cleared", "[UpdateHandoff]")
{
    clear_updater_cache_dir();
    const std::string default_dir = updater_cache_dir();
    const fs::path    custom      = fs::path("orca-updater-test-cache");

    set_updater_cache_dir(custom.string());
    CHECK(updater_cache_dir() == custom.string());
    CHECK(fs::path(updater_download_dir()) == custom / "downloads");

    clear_updater_cache_dir();
    CHECK(updater_cache_dir() == default_dir);
}

TEST_CASE("A partial download of the same build is kept for resuming", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    write_partial_download(dir.path(), kDigest, 400);

    const DownloadPrep prep = prepare_download_dir(dir.string(), kAsset, kDigest, kSize);
    CHECK(prep.resumable);
    CHECK_FALSE(prep.complete);
    CHECK(prep.dropped.empty());
    CHECK(fs::exists(part_path(dir.path())));
}

TEST_CASE("A partial download of another build is dropped", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    write_partial_download(dir.path(), "sha256:2222", 400);

    const DownloadPrep prep = prepare_download_dir(dir.string(), kAsset, kDigest, kSize);
    CHECK_FALSE(prep.resumable);
    CHECK_FALSE(prep.complete);
    CHECK(contains(prep.dropped, part_path(dir.path())));
    CHECK_FALSE(fs::exists(part_path(dir.path())));
    // The marker is rewritten for the new build before it downloads, so what must be
    // gone is the other build's digest, not the file.
    CHECK(read_text(marker_path(dir.path())) == kDigest);
}

TEST_CASE("A partial download without a recorded build is dropped", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    write_text(part_path(dir.path()), std::string(400, 'x'));

    const DownloadPrep prep = prepare_download_dir(dir.string(), kAsset, kDigest, kSize);
    CHECK_FALSE(prep.resumable);
    CHECK(contains(prep.dropped, part_path(dir.path())));
    CHECK_FALSE(fs::exists(part_path(dir.path())));
}

TEST_CASE("A partial download bigger than the build is dropped", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    write_partial_download(dir.path(), kDigest, kSize + 1);

    const DownloadPrep prep = prepare_download_dir(dir.string(), kAsset, kDigest, kSize);
    CHECK_FALSE(prep.resumable);
    CHECK_FALSE(prep.complete);
    CHECK(contains(prep.dropped, part_path(dir.path())));
    CHECK_FALSE(fs::exists(part_path(dir.path())));
}

TEST_CASE("A download the size of the build is verified instead of fetched again", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    write_partial_download(dir.path(), kDigest, kSize);

    const DownloadPrep prep = prepare_download_dir(dir.string(), kAsset, kDigest, kSize);
    CHECK(prep.complete);
    CHECK_FALSE(prep.resumable);
    CHECK(fs::exists(part_path(dir.path())));
}

TEST_CASE("Other artifacts in the download directory are dropped", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    write_partial_download(dir.path(), kDigest, 400);
    const fs::path other = dir.path() / "OrcaSlicer_Linux_AppImage_Ubuntu2404_V2.4.0.AppImage.part";
    write_text(other, std::string(50, 'y'));

    const DownloadPrep prep = prepare_download_dir(dir.string(), kAsset, kDigest, kSize);
    CHECK(contains(prep.dropped, other));
    CHECK_FALSE(fs::exists(other));
    CHECK(fs::exists(part_path(dir.path())));
}

TEST_CASE("The build a partial download belongs to is recorded next to it", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;

    const DownloadPrep prep = prepare_download_dir(dir.string(), kAsset, kDigest, kSize);
    CHECK(prep.dropped.empty());
    CHECK(read_text(marker_path(dir.path())) == kDigest);
}

TEST_CASE("A leftover partial download that cannot be removed is not recorded for resuming", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    // A directory where the .part should be: it is not a regular file, so it cannot be dropped.
    fs::create_directories(part_path(dir.path()));

    prepare_download_dir(dir.string(), kAsset, kDigest, kSize);
    CHECK_FALSE(fs::exists(marker_path(dir.path())));
}

TEST_CASE("Only https release downloads from the OrcaSlicer repository are accepted", "[UpdateHandoff]")
{
    CHECK(is_release_download_url("https://github.com/OrcaSlicer/OrcaSlicer/releases/download/v2.5.0/app.AppImage"));
    CHECK_FALSE(is_release_download_url(""));
    CHECK_FALSE(is_release_download_url("http://github.com/OrcaSlicer/OrcaSlicer/releases/download/v2.5.0/app.AppImage"));
    CHECK_FALSE(is_release_download_url("https://github.com/someone/OrcaSlicer/releases/download/v2.5.0/app.AppImage"));
    CHECK_FALSE(is_release_download_url("https://example.com/https://github.com/OrcaSlicer/OrcaSlicer/releases/download/v2.5.0/app"));
}

TEST_CASE("The handoff round-trips through a cache directory with non-ASCII characters", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    Utf8PathConversion utf8; // before the path below is built
    const fs::path     cache = dir.path() / "caf\xc3\xa9";
    set_updater_cache_dir(cache.string());

    UpdateHandoff written;
    written.version = "2.5.0";
    written.url     = "https://github.com/OrcaSlicer/OrcaSlicer/releases/download/v2.5.0/app.AppImage";
    CHECK(write_handoff(written));

    UpdateHandoff read;
    CHECK(read_handoff(read));
    CHECK(read.version == "2.5.0");
    CHECK(read.url == written.url);

    clear_updater_cache_dir();
}
