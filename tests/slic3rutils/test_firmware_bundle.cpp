#include <catch2/catch_all.hpp>

#include <algorithm>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/miniz_extension.hpp"
#include "slic3r/Utils/FirmwareBundle.hpp"

using namespace Slic3r;

namespace {

// Builds an in-memory zip. A name ending in "/" is written as a directory entry, the way the
// published UnlayeredWebControl-SD.zip carries css/, fonts/ and js/.
std::vector<uint8_t> make_zip(const std::vector<std::pair<std::string, std::string>> &entries)
{
    mz_zip_archive arch;
    mz_zip_zero_struct(&arch);
    REQUIRE(mz_zip_writer_init_heap(&arch, 0, 64 * 1024));
    for (const auto &e : entries)
        REQUIRE(mz_zip_writer_add_mem(&arch, e.first.c_str(), e.second.data(), e.second.size(),
                                      MZ_DEFAULT_COMPRESSION));
    void  *buf  = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&arch, &buf, &size));
    std::vector<uint8_t> out(static_cast<uint8_t *>(buf), static_cast<uint8_t *>(buf) + size);
    mz_zip_writer_end(&arch);
    return out;
}

std::vector<std::string> paths_of(const std::vector<WebInterfaceFile> &files)
{
    std::vector<std::string> out;
    for (const auto &f : files)
        out.push_back(f.path);
    return out;
}

WebInterfaceFile wf(const std::string &path)
{
    WebInterfaceFile f;
    f.path = path;
    f.data = {1};
    return f;
}

const char *MANIFEST_19 = R"({
  "version": "3.7.0-beta.1+unlayered.19",
  "buildDate": "2026-10-08",
  "board": "Mini5plus",
  "images": [
    { "role": "iap",       "file": "Duet3_SDiap32_Mini5plus.bin", "sha256": "B531" },
    { "role": "mainboard", "file": "Duet3Firmware_Mini5plus.uf2", "sha256": "3aab" },
    { "role": "wifi",      "file": "DuetWiFiServer.bin",          "sha256": "cd8d" },
    { "role": "paneldue",  "file": "PanelDueFirmware.bin",        "sha256": "dd6d" },
    { "role": "panelota",  "file": "unlayered-panel.bin",         "sha256": "5fa2" },
    { "role": "www",       "file": "UnlayeredWebControl-SD.zip",  "sha256": "430a" }
  ]
})";

} // namespace

TEST_CASE("The manifest's www role is the web interface", "[FirmwareBundle]")
{
    FirmwarePackage pkg;
    std::string     err;
    REQUIRE(parse_manifest(MANIFEST_19, pkg, err));
    CHECK(pkg.version == "3.7.0-beta.1+unlayered.19");
    REQUIRE(pkg.images.size() == 6);
    const FirmwareImage *www = pkg.find(FirmwareRole::Www);
    REQUIRE(www != nullptr);
    CHECK(www->filename == "UnlayeredWebControl-SD.zip");
    CHECK(www->sha256 == "430a");
    CHECK(pkg.find(FirmwareRole::Iap)->sha256 == "b531"); // lowercased
}

TEST_CASE("www round-trips and is never an M997 module", "[FirmwareBundle]")
{
    CHECK(firmware_role_from_string("www") == FirmwareRole::Www);
    CHECK(firmware_role_from_string("WWW") == FirmwareRole::Www);
    CHECK(std::string(firmware_role_to_string(FirmwareRole::Www)) == "www");
    CHECK(firmware_role_to_module(FirmwareRole::Www) == -1);
    CHECK(firmware_role_to_module(FirmwareRole::Mainboard) == 0);
}

TEST_CASE("A manifest entry with a role this version does not know is skipped, not fatal", "[FirmwareBundle]")
{
    // Rejecting the whole manifest would fall back to name matching with no checksums.
    const char *json = R"({ "version": "3.7.0-beta.1+unlayered.20",
        "images": [ { "role": "mainboard", "file": "Duet3Firmware_Mini5plus.uf2" },
                    { "role": "hologram",  "file": "future.bin" } ] })";
    FirmwarePackage pkg;
    std::string     err;
    REQUIRE(parse_manifest(json, pkg, err));
    REQUIRE(pkg.images.size() == 1);
    CHECK(pkg.images[0].role == FirmwareRole::Mainboard);

    const char *only_unknown = R"({ "version": "1", "images": [ { "role": "hologram", "file": "x.bin" } ] })";
    CHECK_FALSE(parse_manifest(only_unknown, pkg, err));
}

TEST_CASE("A package's www image must be a zip", "[FirmwareBundle]")
{
    FirmwarePackage pkg;
    FirmwareImage   img;
    img.role     = FirmwareRole::Www;
    img.filename = "UnlayeredWebControl-SD.zip";
    img.data     = {'n', 'o', 't', 'z', 'i', 'p'};
    pkg.images.push_back(img);
    std::string err;
    CHECK_FALSE(validate_package(pkg, "Mini5plus", err));

    pkg.images[0].data = make_zip({{"index.html.gz", "x"}});
    CHECK(validate_package(pkg, "Mini5plus", err)); // a www-only package is installable
}

TEST_CASE("The web interface zip unpacks with index.html.gz last", "[FirmwareBundle][www]")
{
    const auto zip = make_zip({
        {"css/", ""},
        {"css/app-Bl8JMW6g.css.gz", "css"},
        {"index.html.gz", "index"},
        {"favicon.ico.gz", "ico"},
        {"js/", ""},
        {"js/app-D_vsAdMJ.js.gz", "js"},
        {"service-worker.js.gz", "sw"},
        {"fonts/montserrat-latin-400-normal-BLhwKU8k.woff2", "font"},
    });
    std::vector<WebInterfaceFile> files;
    std::string                   err;
    REQUIRE(extract_web_interface(zip, files, err));
    CHECK(paths_of(files) == std::vector<std::string>{
                                 "css/app-Bl8JMW6g.css.gz",
                                 "favicon.ico.gz",
                                 "fonts/montserrat-latin-400-normal-BLhwKU8k.woff2",
                                 "js/app-D_vsAdMJ.js.gz",
                                 "service-worker.js.gz",
                                 "index.html.gz",
                             });
    CHECK(std::string(files.back().data.begin(), files.back().data.end()) == "index");
    CHECK(web_interface_dirs(files) == std::vector<std::string>{"css", "fonts", "js"});
}

TEST_CASE("index.html goes after everything but index.html.gz", "[FirmwareBundle][www]")
{
    std::vector<WebInterfaceFile> files = {wf("index.html.gz"), wf("index.html"), wf("z.js"), wf("a/b.css")};
    web_interface_upload_order(files);
    CHECK(paths_of(files) == std::vector<std::string>{"a/b.css", "z.js", "index.html", "index.html.gz"});
}

TEST_CASE("Folders are created parents first", "[FirmwareBundle][www]")
{
    const std::vector<WebInterfaceFile> files = {wf("a/b/c/d.js"), wf("a/x.js"), wf("e/f.js"), wf("index.html.gz")};
    CHECK(web_interface_dirs(files) == std::vector<std::string>{"a", "e", "a/b", "a/b/c"});
}

TEST_CASE("A zip that is not a web interface is refused before anything is written", "[FirmwareBundle][www]")
{
    std::vector<WebInterfaceFile> files;
    std::string                   err;

    SECTION("no index at the top level, e.g. everything inside a wrapper folder")
    {
        CHECK_FALSE(extract_web_interface(make_zip({{"dwc/index.html.gz", "i"}, {"dwc/js/a.js.gz", "a"}}), files, err));
    }
    SECTION("a path that climbs out of 0:/www")
    {
        CHECK_FALSE(extract_web_interface(make_zip({{"index.html.gz", "i"}, {"../sys/config.g", "M999"}}), files, err));
    }
    SECTION("characters that cannot travel in an unquoted G-code path")
    {
        CHECK_FALSE(extract_web_interface(make_zip({{"index.html.gz", "i"}, {"js/a;b.js", "x"}}), files, err));
        CHECK_FALSE(extract_web_interface(make_zip({{"index.html.gz", "i"}, {"js/a b.js", "x"}}), files, err));
    }
    SECTION("not a zip at all")
    {
        const std::vector<uint8_t> uf2 = {0x55, 0x46, 0x32, 0x0A, 0x57, 0x51, 0x5D, 0x9E};
        CHECK_FALSE(extract_web_interface(uf2, files, err));
    }
    CHECK(files.empty());
}

TEST_CASE("Empty files in the zip are skipped", "[FirmwareBundle][www]")
{
    std::vector<WebInterfaceFile> files;
    std::string                   err;
    REQUIRE(extract_web_interface(make_zip({{"index.html.gz", "i"}, {"empty.txt", ""}}), files, err));
    CHECK(paths_of(files) == std::vector<std::string>{"index.html.gz"});
}

TEST_CASE("Stale files are what was on the card and is not in the bundle", "[FirmwareBundle][www]")
{
    const std::vector<WebInterfaceFile> bundle = {wf("js/app-NEW.js.gz"), wf("css/app-NEW.css.gz"),
                                                  wf("favicon.ico.gz"), wf("index.html.gz")};
    const std::vector<std::string> on_card = {"js/app-OLD.js.gz", "css/app-OLD.css.gz", "favicon.ico.gz",
                                              "index.html.gz", "reprap.htm", "fonts/old.woff"};
    CHECK(stale_web_interface_files(on_card, bundle) ==
          std::vector<std::string>{"js/app-OLD.js.gz", "css/app-OLD.css.gz", "reprap.htm", "fonts/old.woff"});
}

TEST_CASE("Stale-file comparison ignores case, because FAT does", "[FirmwareBundle][www]")
{
    // "Index.html.gz" on the card IS the index.html.gz just uploaded; deleting it would take the
    // printer's web page down.
    const std::vector<WebInterfaceFile> bundle = {wf("index.html.gz"), wf("js/App.js.gz")};
    CHECK(stale_web_interface_files({"Index.HTML.gz", "JS/app.js.gz", "old.js"}, bundle) ==
          std::vector<std::string>{"old.js"});
}

TEST_CASE("Stale folders go deepest first and never a folder the bundle uses", "[FirmwareBundle][www]")
{
    const std::vector<WebInterfaceFile> bundle = {wf("js/a.js.gz"), wf("index.html.gz")};
    CHECK(stale_web_interface_dirs({"fonts", "JS", "img", "img/icons", "img/icons/x"}, bundle) ==
          std::vector<std::string>{"img/icons/x", "img/icons", "fonts", "img"});
}

TEST_CASE("A hand-picked web interface zip is recognised by name", "[FirmwareBundle][www]")
{
    namespace fs = boost::filesystem;
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("fwbundle-%%%%-%%%%");
    fs::create_directories(dir);
    const auto zip = make_zip({{"index.html.gz", "i"}});

    for (const std::string name : {"UnlayeredWebControl-SD.zip", "DuetWebControl-SD.zip"}) {
        const fs::path p = dir / name;
        {
            std::ofstream os(p.string(), std::ios::binary);
            os.write(reinterpret_cast<const char *>(zip.data()), std::streamsize(zip.size()));
        }
        LocalFileFirmwareSource src(p.string(), "Duet3Firmware_Mini5plus.uf2", "Duet3_SDiap32_Mini5plus.bin", "");
        FirmwarePackage         pkg;
        std::string             err;
        REQUIRE(src.fetch(pkg, nullptr, err));
        REQUIRE(pkg.images.size() == 1);
        CHECK(pkg.images[0].role == FirmwareRole::Www);
        CHECK(pkg.images[0].filename == name);
        CHECK(pkg.version.empty());
    }

    // Any other zip is not guessed at.
    const fs::path other = dir / "unlayered-1.9-sd-card.zip";
    {
        std::ofstream os(other.string(), std::ios::binary);
        os.write(reinterpret_cast<const char *>(zip.data()), std::streamsize(zip.size()));
    }
    LocalFileFirmwareSource src(other.string(), "Duet3Firmware_Mini5plus.uf2", "Duet3_SDiap32_Mini5plus.bin", "");
    FirmwarePackage         pkg;
    std::string             err;
    CHECK_FALSE(src.fetch(pkg, nullptr, err));

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}
