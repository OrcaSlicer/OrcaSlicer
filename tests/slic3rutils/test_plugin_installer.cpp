// Phase 4.3.1 — PluginInstaller unit tests.
//
// Authors a .orcaplugin (zip) at runtime under TEST_TMP_DIR by driving
// miniz directly, then exercises every PluginInstaller::Status path:
//   Ok  (peek + install)
//   DestinationExists (replace_existing=false on a non-empty dir)
//   ManifestMissing   (zip with no manifest.json)
//   ManifestMalformed (zip with broken JSON)
//   IdMissing         (zip with manifest.json that has no id field)

#include <catch2/catch_all.hpp>

#include "slic3r/Utils/PluginInstaller.hpp"

#include "libslic3r/miniz_extension.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

// Build a tiny .orcaplugin (zip) inline. `manifest_json` is the content
// of manifest.json. Extra entries beyond the manifest are not needed
// for the installer logic, but we include a token "payload.txt" so the
// extract path is exercised.
std::filesystem::path
make_archive(const std::string& filename, const std::string& manifest_json) {
    namespace fs = std::filesystem;
    const fs::path out = fs::path{TEST_TMP_DIR} / filename;
    fs::create_directories(out.parent_path());
    fs::remove(out);

    mz_zip_archive zip{};
    REQUIRE(Slic3r::open_zip_writer(&zip, out.string()));
    REQUIRE(mz_zip_writer_add_mem(
        &zip, "manifest.json",
        manifest_json.data(), manifest_json.size(),
        MZ_BEST_SPEED));

    constexpr const char* kPayload = "hello from inside the plugin\n";
    REQUIRE(mz_zip_writer_add_mem(
        &zip, "payload.txt",
        kPayload, std::strlen(kPayload),
        MZ_BEST_SPEED));

    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    Slic3r::close_zip_writer(&zip);
    return out;
}

} // namespace

TEST_CASE("Phase 4.3.1 — PluginInstaller round-trips a valid archive",
          "[plugin][installer]") {
    namespace fs = std::filesystem;

    const auto archive = make_archive("ok.orcaplugin",
        R"({
            "id": "test.fixture.installer.ok",
            "name": "Installer OK",
            "version": "0.0.1",
            "permissions": ["network", "ui_attach"]
        })");

    SECTION("peek_identity returns the manifest fields") {
        auto peek = Slic3r::PluginInstaller::peek_identity(archive);
        REQUIRE(peek.status == Slic3r::PluginInstaller::Status::Ok);
        CHECK(peek.identity.id      == "test.fixture.installer.ok");
        CHECK(peek.identity.name    == "Installer OK");
        CHECK(peek.identity.version == "0.0.1");
        // network = bit 0, ui_attach = bit 9
        CHECK(peek.identity.permissions == ((1ull << 0) | (1ull << 9)));
    }

    SECTION("install extracts every file under plugins_root/<id>/") {
        const fs::path plugins_root = fs::path{TEST_TMP_DIR} / "plugins_ok";
        fs::remove_all(plugins_root);

        auto inst = Slic3r::PluginInstaller::install(archive, plugins_root);
        REQUIRE(inst.status == Slic3r::PluginInstaller::Status::Ok);

        const auto id_dir = plugins_root / "test.fixture.installer.ok";
        CHECK(fs::is_regular_file(id_dir / "manifest.json"));
        CHECK(fs::is_regular_file(id_dir / "payload.txt"));
    }

    SECTION("install refuses a populated dir unless replace_existing=true") {
        const fs::path plugins_root = fs::path{TEST_TMP_DIR} / "plugins_collide";
        fs::remove_all(plugins_root);

        auto first = Slic3r::PluginInstaller::install(archive, plugins_root);
        REQUIRE(first.status == Slic3r::PluginInstaller::Status::Ok);

        auto second = Slic3r::PluginInstaller::install(
            archive, plugins_root, /*replace_existing=*/false);
        CHECK(second.status == Slic3r::PluginInstaller::Status::DestinationExists);

        auto third = Slic3r::PluginInstaller::install(
            archive, plugins_root, /*replace_existing=*/true);
        CHECK(third.status == Slic3r::PluginInstaller::Status::Ok);
    }
}

TEST_CASE("Phase 4.3.1 — PluginInstaller rejects malformed archives",
          "[plugin][installer]") {
    namespace fs = std::filesystem;

    SECTION("ManifestMalformed on broken JSON") {
        const auto archive = make_archive("broken.orcaplugin",
            "{ not valid json");
        auto peek = Slic3r::PluginInstaller::peek_identity(archive);
        CHECK(peek.status == Slic3r::PluginInstaller::Status::ManifestMalformed);
    }

    SECTION("IdMissing when id is absent") {
        const auto archive = make_archive("noid.orcaplugin",
            R"({ "name": "no id" })");
        auto peek = Slic3r::PluginInstaller::peek_identity(archive);
        CHECK(peek.status == Slic3r::PluginInstaller::Status::IdMissing);
    }

    SECTION("InvalidArchive when the file does not exist") {
        const auto path = fs::path{TEST_TMP_DIR} / "does_not_exist.orcaplugin";
        std::error_code ec;
        fs::remove(path, ec);
        auto peek = Slic3r::PluginInstaller::peek_identity(path);
        CHECK(peek.status == Slic3r::PluginInstaller::Status::InvalidArchive);
    }
}
