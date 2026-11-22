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
#include "catch2/generators/catch_generators.hpp"
#include "libslic3r/AppConfig.hpp"
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

fs::path artifact_path(const fs::path& dir)
{
    return dir / kAsset;
}

fs::path verified_path(const fs::path& dir)
{
    return dir / (std::string(kAsset) + ".part.verified");
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
    // No install root named: the shareable flat directory, as before this split.
    CHECK(updater_install_root().empty());
    CHECK(fs::path(updater_download_dir()) == custom / "downloads");

    clear_updater_cache_dir();
    CHECK(updater_cache_dir() == default_dir);
}

TEST_CASE("Each install downloads into its own directory", "[UpdateHandoff]")
{
    clear_updater_cache_dir();
    const fs::path shared = fs::path(updater_cache_dir()) / "downloads";

    CHECK(fs::path(updater_download_dir()) == shared);

    set_updater_install_root("/opt/OrcaSlicer");
    const fs::path parent = fs::path(updater_download_dir());
    CHECK(updater_install_root() == "/opt/OrcaSlicer");
    CHECK(parent.parent_path() == shared);
    CHECK(parent.filename() == install_identity_hex("/opt/OrcaSlicer"));

    // The download lock lives inside the install's own download directory, not beside the
    // install lock: two copies must be able to prefetch at the same time.
    CHECK(fs::path(updater_download_lock_path()) == parent / "download.lock");
    CHECK(fs::path(updater_download_lock_path()) != fs::path(updater_lock_path()));

    // The job handed to the helper is keyed too: two copies share one cache, and one must
    // not overwrite the job the other one handed over.
    const std::string keyed_handoff = updater_handoff_path();
    CHECK(fs::path(keyed_handoff).parent_path() == fs::path(updater_cache_dir()));
    CHECK(fs::path(keyed_handoff) != fs::path(updater_cache_dir()) / "handoff.json");

    set_updater_install_root("/opt/OrcaSlicer/client");
    CHECK(fs::path(updater_download_dir()) != parent);

    // Clearing the cache also clears the install root, or the next one to use it inherits this install.
    clear_updater_cache_dir();
    CHECK(updater_install_root().empty());
    CHECK(fs::path(updater_download_dir()) == fs::path(updater_cache_dir()) / "downloads");
}

TEST_CASE("Each install extracts its helper into its own directory", "[UpdateHandoff]")
{
    clear_updater_cache_dir();
    const fs::path shared = fs::path(updater_cache_dir()) / "helper";

    CHECK(fs::path(updater_helper_dir()) == shared);

    set_updater_install_root("/opt/OrcaSlicer");
    const fs::path own = fs::path(updater_helper_dir());
    CHECK(own.parent_path() == shared);
    CHECK(own.filename() == install_identity_hex("/opt/OrcaSlicer"));

    set_updater_install_root("/opt/OrcaSlicer/client");
    CHECK(fs::path(updater_helper_dir()) != own);

    clear_updater_cache_dir();
    CHECK(fs::path(updater_helper_dir()) == shared);
}

TEST_CASE("A download is named after the asset, inside its own directory", "[UpdateHandoff]")
{
    // A path-like asset name is reduced to its file name, so it cannot leave the directory.
    const DownloadPaths paths = download_paths("cache", "../OrcaSlicer_V2.5.0.AppImage");

    CHECK(fs::path(paths.part) == fs::path("cache") / "OrcaSlicer_V2.5.0.AppImage.part");
    CHECK(fs::path(paths.marker) == fs::path("cache") / "OrcaSlicer_V2.5.0.AppImage.part.digest");
    CHECK(fs::path(paths.artifact) == fs::path("cache") / "OrcaSlicer_V2.5.0.AppImage");
    // The verified marker is its own file, beside the artifact it describes.
    CHECK(fs::path(paths.verified) == fs::path("cache") / "OrcaSlicer_V2.5.0.AppImage.part.verified");
    CHECK(fs::path(paths.verified) != fs::path(paths.part));
    CHECK(fs::path(paths.verified) != fs::path(paths.marker));
    CHECK(fs::path(paths.verified) != fs::path(paths.artifact));

    // No asset name to go by: the artifact still lands in the directory.
    CHECK(fs::path(download_paths("cache", "").artifact) == fs::path("cache") / "update.part");
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

TEST_CASE("Another candidate's partial download is left alone", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    write_partial_download(dir.path(), kDigest, 400);
    // A second helper is fetching a different release into the same per-install directory.
    const fs::path other = dir.path() / "OrcaSlicer_Linux_AppImage_Ubuntu2404_V2.4.0.AppImage.part";
    const fs::path other_marker = dir.path() / "OrcaSlicer_Linux_AppImage_Ubuntu2404_V2.4.0.AppImage.part.digest";
    write_text(other, std::string(50, 'y'));
    write_text(other_marker, "sha256:3333");

    const DownloadPrep prep = prepare_download_dir(dir.string(), kAsset, kDigest, kSize);
    CHECK(prep.resumable);
    CHECK(prep.dropped.empty());
    CHECK(fs::exists(other));
    CHECK(fs::exists(other_marker));
    CHECK(read_text(other_marker) == "sha256:3333");
    CHECK(fs::exists(part_path(dir.path())));
}

TEST_CASE("A verified artifact of the prepared candidate is still swept for another build", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    // This candidate's own artifact, recorded for a build other than the one being prepared.
    write_text(dir.path() / kAsset, std::string(kSize, 'x'));
    write_text(marker_path(dir.path()), "sha256:2222");
    // A different release another helper is fetching.
    const fs::path other = dir.path() / "OrcaSlicer_Linux_AppImage_Ubuntu2404_V2.4.0.AppImage.part";
    write_text(other, std::string(50, 'y'));

    const DownloadPrep prep = prepare_download_dir(dir.string(), kAsset, kDigest, kSize);
    CHECK_FALSE(prep.complete);
    CHECK(contains(prep.dropped, dir.path() / kAsset));
    CHECK_FALSE(fs::exists(dir.path() / kAsset));
    CHECK(fs::exists(other));
}

TEST_CASE("A verified artifact is kept for the build it belongs to", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    // The helper renames a verified download to its asset name and leaves the digest marker.
    write_text(dir.path() / kAsset, std::string(kSize, 'x'));
    write_text(marker_path(dir.path()), kDigest);

    const DownloadPrep prep = prepare_download_dir(dir.string(), kAsset, kDigest, kSize);
    CHECK(prep.complete);
    CHECK_FALSE(prep.resumable);
    CHECK(fs::exists(dir.path() / kAsset));
    CHECK(prep.dropped.empty());
}

TEST_CASE("A verified artifact recorded for another build is dropped", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    write_text(dir.path() / kAsset, std::string(kSize, 'x'));
    write_text(marker_path(dir.path()), "sha256:2222");

    const DownloadPrep prep = prepare_download_dir(dir.string(), kAsset, kDigest, kSize);
    CHECK_FALSE(prep.complete);
    CHECK_FALSE(fs::exists(dir.path() / kAsset));
}

TEST_CASE("A download the verifier rejected is fetched again", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    // The helper renames a verified download to its asset name and keeps the digest marker,
    // then records the digest it verified.
    write_text(dir.path() / kAsset, std::string(kSize, 'x'));
    write_text(marker_path(dir.path()), kDigest);
    CHECK(mark_download_verified(dir.string(), kAsset, kDigest));
    CHECK(prepare_download_dir(dir.string(), kAsset, kDigest, kSize).complete);

    // A prefetch of another build is still writing its .part.
    const fs::path other = dir.path() / "OrcaSlicer_Linux_AppImage_Ubuntu2404_V2.4.0.AppImage.part";
    write_text(other, std::string(50, 'y'));

    discard_download(dir.string(), kAsset);
    CHECK_FALSE(fs::exists(dir.path() / kAsset));
    CHECK_FALSE(fs::exists(marker_path(dir.path())));
    CHECK_FALSE(fs::exists(verified_path(dir.path())));
    CHECK(fs::exists(other));

    // Nothing is left for the next run to take as complete, so it downloads the file again.
    CHECK_FALSE(prepare_download_dir(dir.string(), kAsset, kDigest, kSize).complete);
}

TEST_CASE("A download needs the artifact at its expected size before it can be verified", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;

    // Nothing downloaded yet.
    CHECK_FALSE(download_is_verified(dir.string(), kAsset, kDigest, kSize));

    // The helper has recorded a verification, but the artifact is not there.
    CHECK(mark_download_verified(dir.string(), kAsset, kDigest));
    CHECK_FALSE(download_is_verified(dir.string(), kAsset, kDigest, kSize));

    // The final name is there, at the wrong size: the download was interrupted.
    write_text(artifact_path(dir.path()), std::string(kSize - 1, 'x'));
    CHECK_FALSE(download_is_verified(dir.string(), kAsset, kDigest, kSize));

    // A release without a size can never be reported ready.
    write_text(artifact_path(dir.path()), std::string(kSize, 'x'));
    CHECK_FALSE(download_is_verified(dir.string(), kAsset, kDigest, 0));
}

TEST_CASE("A download at the right size is verified only by a marker naming its digest", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    write_text(artifact_path(dir.path()), std::string(kSize, 'x'));

    // fetch_artifact() renames before verifying, so the artifact alone proves nothing.
    CHECK_FALSE(download_is_verified(dir.string(), kAsset, kDigest, kSize));

    CHECK(mark_download_verified(dir.string(), kAsset, kDigest));
    CHECK(read_text(verified_path(dir.path())) == kDigest);
    CHECK(download_is_verified(dir.string(), kAsset, kDigest, kSize));

    // A nightly reuses one asset name, so another build's digest leaves this download unverified.
    write_text(verified_path(dir.path()), "sha256:2222");
    CHECK_FALSE(download_is_verified(dir.string(), kAsset, kDigest, kSize));
}

TEST_CASE("A recorded digest with a trailing line ending still verifies", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    write_text(artifact_path(dir.path()), std::string(kSize, 'x'));
    write_text(verified_path(dir.path()), std::string(kDigest) + "\r\n");

    CHECK(download_is_verified(dir.string(), kAsset, kDigest, kSize));
}

TEST_CASE("A release without a digest verifies against an empty marker only", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    write_text(artifact_path(dir.path()), std::string(kSize, 'x'));

    // No marker at all: nothing recorded this download as verified.
    CHECK_FALSE(download_is_verified(dir.string(), kAsset, "", kSize));

    write_text(verified_path(dir.path()), "");
    CHECK(download_is_verified(dir.string(), kAsset, "", kSize));
    // An empty marker does not stand in for a digest.
    CHECK_FALSE(download_is_verified(dir.string(), kAsset, kDigest, kSize));

    write_text(verified_path(dir.path()), kDigest);
    CHECK_FALSE(download_is_verified(dir.string(), kAsset, "", kSize));
}

TEST_CASE("Removing a download takes the artifact and every marker beside it", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    write_text(part_path(dir.path()), std::string(50, 'y'));
    write_text(marker_path(dir.path()), kDigest);
    write_text(artifact_path(dir.path()), std::string(kSize, 'x'));
    CHECK(mark_download_verified(dir.string(), kAsset, kDigest));

    remove_download(dir.string(), kAsset);

    CHECK_FALSE(fs::exists(part_path(dir.path())));
    CHECK_FALSE(fs::exists(marker_path(dir.path())));
    CHECK_FALSE(fs::exists(artifact_path(dir.path())));
    CHECK_FALSE(fs::exists(verified_path(dir.path())));
}

TEST_CASE("Removing a download from a directory that is not there does nothing", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    const fs::path     absent = dir.path() / "absent";

    remove_download(absent.string(), kAsset);
    CHECK_FALSE(fs::exists(absent));
}

TEST_CASE("The verified marker is swept with the artifact of another build", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    write_text(artifact_path(dir.path()), std::string(kSize, 'x'));
    write_text(marker_path(dir.path()), "sha256:2222");
    write_text(verified_path(dir.path()), "sha256:2222");

    const DownloadPrep prep = prepare_download_dir(dir.string(), kAsset, kDigest, kSize);
    CHECK_FALSE(prep.complete);
    CHECK(contains(prep.dropped, artifact_path(dir.path())));
    CHECK_FALSE(fs::exists(artifact_path(dir.path())));
    CHECK_FALSE(fs::exists(verified_path(dir.path())));
}

TEST_CASE("The verified marker of this build's artifact is kept for the next launch", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    write_text(artifact_path(dir.path()), std::string(kSize, 'x'));
    write_text(marker_path(dir.path()), kDigest);
    CHECK(mark_download_verified(dir.string(), kAsset, kDigest));

    const DownloadPrep prep = prepare_download_dir(dir.string(), kAsset, kDigest, kSize);
    CHECK(prep.complete);
    CHECK(fs::exists(verified_path(dir.path())));
    CHECK(download_is_verified(dir.string(), kAsset, kDigest, kSize));
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

TEST_CASE("A release download of the named asset from the OrcaSlicer repository is accepted", "[UpdateHandoff]")
{
    const std::string release = "https://github.com/OrcaSlicer/OrcaSlicer/releases/download/v2.5.0/";
    CHECK(is_release_download_url(release + kAsset, kAsset));
    CHECK_FALSE(is_release_download_url(release + "other.AppImage", kAsset));
    CHECK_FALSE(is_release_download_url(release + kAsset, ""));
}

TEST_CASE("Release URLs that leave the release path or name another file are rejected", "[UpdateHandoff]")
{
    const std::string release = "https://github.com/OrcaSlicer/OrcaSlicer/releases/download/";
    const std::string asset   = kAsset;
    const std::string url     = GENERATE_COPY(as<std::string>{},
                                              "",
                                              "http://github.com/OrcaSlicer/OrcaSlicer/releases/download/v2.5.0/" + asset,
                                              "https://github.com/someone/OrcaSlicer/releases/download/v2.5.0/" + asset,
                                              "https://example.com/https://github.com/OrcaSlicer/OrcaSlicer/releases/download/v2.5.0/" + asset,
                                              release + "../../../evil/x/releases/download/v1/" + asset,
                                              release + "../" + asset,
                                              release + "%2e%2e/" + asset,
                                              release + "v2.5.0\\evil/" + asset,
                                              release + "v2.5.0@evil.example/" + asset,
                                              release + "v2.5.0/" + asset + "?query=1",
                                              release + "v2.5.0/" + asset + "#frag",
                                              release + "v2.5.0/" + asset + "/extra",
                                              release + "/" + asset,
                                              release + "v2.5.0/",
                                              release + "v2.5.0/other.AppImage");
    CHECK_FALSE(is_release_download_url(url, asset));
}

TEST_CASE("A second install lock on the same path is refused while the first is held", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    const std::string  lock = (dir.path() / "install.lock").string();

    CHECK(acquire_install_lock(lock));
    CHECK_FALSE(acquire_install_lock(lock));

    release_install_lock(lock);
    CHECK_FALSE(fs::exists(lock));
    CHECK(acquire_install_lock(lock));
    release_install_lock(lock);
}

TEST_CASE("A leftover install lock file that nobody holds is taken over", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    const std::string  lock = (dir.path() / "install.lock").string();
    write_text(lock, "99999");

    CHECK(acquire_install_lock(lock));
    CHECK_FALSE(acquire_install_lock(lock));
    release_install_lock(lock);
    CHECK_FALSE(fs::exists(lock));
}

TEST_CASE("Releasing an install lock this process does not hold leaves the file in place", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    const std::string  lock = (dir.path() / "install.lock").string();
    write_text(lock, "1234");

    release_install_lock(lock);
    CHECK(fs::exists(lock));
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

TEST_CASE("A prefetch and an install job do not overwrite or consume each other", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    set_updater_cache_dir(dir.path().string());
    set_updater_install_root("/opt/OrcaSlicer");

    UpdateHandoff prefetch;
    prefetch.version = "from the prefetch";
    UpdateHandoff install;
    install.version = "from the install";
    CHECK(write_handoff(prefetch, "prefetch"));
    CHECK(write_handoff(install, "install"));
    CHECK(updater_handoff_path("prefetch") != updater_handoff_path("install"));

    // Each helper removes only its own file, so the other job stays readable.
    remove_handoff("prefetch");
    UpdateHandoff missing;
    CHECK_FALSE(read_handoff(missing, "prefetch"));
    UpdateHandoff read;
    CHECK(read_handoff(read, "install"));
    CHECK(read.version == "from the install");

    clear_updater_cache_dir();
}

TEST_CASE("An install record round-trips with the install it describes", "[UpdateHandoff]")
{
    InstallRecord written;
    written.digest  = kDigest;
    written.channel = "stable";
    written.version = "2.5.0";
    written.target  = "/opt/OrcaSlicer";

    InstallRecord read;
    CHECK(install_record_from_json(install_record_to_json(written), read));
    CHECK(read.digest == written.digest);
    CHECK(read.channel == written.channel);
    CHECK(read.version == written.version);
    CHECK(read.target == written.target);
}

TEST_CASE("An install record without a target parses with an empty target", "[UpdateHandoff]")
{
    InstallRecord read;
    CHECK(install_record_from_json(R"({"digest":"sha256:1111","channel":"stable","version":"2.5.0"})", read));
    CHECK(read.digest == kDigest);
    CHECK(read.target.empty());
}

TEST_CASE("Reading the install record leaves it in place and removing it deletes it", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    set_updater_cache_dir(dir.string());

    InstallRecord written;
    written.digest = kDigest;
    written.target = "/opt/OrcaSlicer";
    CHECK(write_install_record(written));

    InstallRecord first;
    CHECK(read_install_record(first));
    CHECK(first.target == written.target);
    CHECK(fs::exists(updater_install_record_path()));

    InstallRecord second;
    CHECK(read_install_record(second));
    CHECK(second.digest == kDigest);

    remove_install_record();
    CHECK_FALSE(fs::exists(updater_install_record_path()));
    InstallRecord gone;
    CHECK_FALSE(read_install_record(gone));

    clear_updater_cache_dir();
}

TEST_CASE("The same install root always gives the same identity key", "[UpdateHandoff]")
{
    const std::string key = install_identity_key("/opt/OrcaSlicer");
    CHECK(install_identity_key("/opt/OrcaSlicer") == key);
}

TEST_CASE("A trailing separator does not change the identity key", "[UpdateHandoff]")
{
    const std::string key = install_identity_key("/opt/OrcaSlicer");
    CHECK(install_identity_key("/opt/OrcaSlicer/") == key);
    CHECK(install_identity_key("/opt/OrcaSlicer//") == key);
}

TEST_CASE("Copies installed in different roots get different identity keys", "[UpdateHandoff]")
{
    const std::string parent = "/opt/OrcaSlicer";
    const std::string client = "/opt/OrcaSlicer/client";
    CHECK(install_identity_key(parent) != install_identity_key(client));
}

TEST_CASE("The identity key is prefixed and free of whitespace", "[UpdateHandoff]")
{
    // AppConfig asserts that a key has no whitespace, so a path such as Program Files
    // cannot be the key itself.
    const std::string key = install_identity_key("C:\\Program Files\\OrcaSlicer");
    CHECK(key.rfind("installed_artifact:", 0) == 0);
    CHECK(key.find_first_of(" \t\r\n\v\f") == std::string::npos);
    CHECK(key.size() == std::string("installed_artifact:").size() + 16);
    CHECK(key.find_first_not_of("0123456789abcdef", std::string("installed_artifact:").size()) == std::string::npos);
}

TEST_CASE("The install hex is the identity key without its prefix", "[UpdateHandoff]")
{
    const std::string root = "/opt/OrcaSlicer";
    CHECK(install_identity_key(root) == "installed_artifact:" + install_identity_hex(root));
    CHECK(install_identity_hex("").empty());
    // Usable as a directory name: the hash never carries a separator.
    CHECK(install_identity_hex(root).find_first_of("/\\") == std::string::npos);
}

TEST_CASE("Two install roots record their own channel and artifact", "[UpdateHandoff]")
{
    AppConfig config;
    config.set_installed_build("/opt/OrcaSlicer", "stable", "sha256:1111");
    config.set_installed_build("/opt/OrcaSlicer/client", "nightly", "sha256:2222");

    CHECK(config.installed_channel("/opt/OrcaSlicer") == "stable");
    CHECK(config.installed_channel("/opt/OrcaSlicer/client") == "nightly");
    CHECK(config.installed_artifact("/opt/OrcaSlicer") == "sha256:1111");
    CHECK(config.installed_artifact("/opt/OrcaSlicer/client") == "sha256:2222");
    CHECK(config.installed_channel("/opt/other").empty());
}

TEST_CASE("A build recorded before the keying is moved to the first root that claims it", "[UpdateHandoff]")
{
    AppConfig config;
    config.set("app", "installed_artifact", "sha256:1111");
    config.set("app", "installed_channel", "nightly");

    config.migrate_installed_artifact("/opt/OrcaSlicer");
    CHECK(config.installed_artifact("/opt/OrcaSlicer") == "sha256:1111");
    CHECK(config.installed_channel("/opt/OrcaSlicer") == "nightly");

    // Claimed once: a second copy of the application must not read it as its own build.
    config.migrate_installed_artifact("/opt/OrcaSlicer/client");
    CHECK(config.installed_artifact("/opt/OrcaSlicer/client").empty());
    CHECK(config.installed_channel("/opt/OrcaSlicer/client").empty());
}

TEST_CASE("A recorded update failure is read once and removed", "[UpdateHandoff]")
{
    ScopedTemporaryDir dir;
    set_updater_cache_dir(dir.string());

    std::string message;
    CHECK_FALSE(read_update_failure(message));

    CHECK(write_update_failure("The downloaded file is 12 bytes, expected 1000."));
    CHECK(fs::exists(updater_failure_path()));
    CHECK(read_update_failure(message));
    CHECK(message == "The downloaded file is 12 bytes, expected 1000.");

    remove_update_failure();
    CHECK_FALSE(fs::exists(updater_failure_path()));
    CHECK_FALSE(read_update_failure(message));

    clear_updater_cache_dir();
}
