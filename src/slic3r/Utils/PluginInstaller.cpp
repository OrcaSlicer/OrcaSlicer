// Phase 4.3.1 — PluginInstaller implementation.

#include "PluginInstaller.hpp"

#include "libslic3r/miniz_extension.hpp"

#include "nlohmann/json.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace Slic3r {

namespace {

// Returns the file_index of the named entry within the zip, or -1 when
// absent. Names are matched case-sensitively against the entry's
// in-archive path.
int find_entry(mz_zip_archive* zip, const char* name) {
    const auto count = mz_zip_reader_get_num_files(zip);
    for (mz_uint i = 0; i < count; ++i) {
        char buf[512];
        const auto len = mz_zip_reader_get_filename(zip, i, buf, sizeof(buf));
        if (len > 0 && std::strcmp(buf, name) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

std::uint64_t parse_permissions(const nlohmann::json& j) {
    if (!j.contains("permissions") || !j["permissions"].is_array()) return 0;
    static const std::pair<const char*, std::uint64_t> kPerms[] = {
        {"network",          1ull << 0},
        {"filesystem_read",  1ull << 1},
        {"filesystem_write", 1ull << 2},
        {"settings_read",    1ull << 3},
        {"settings_write",   1ull << 4},
        {"profiles_install", 1ull << 5},
        {"device_control",   1ull << 6},
        {"slice_intercept",  1ull << 7},
        {"gcode_modify",     1ull << 8},
        {"ui_attach",        1ull << 9},
        {"events_raw",       1ull << 10},
    };
    std::uint64_t bits = 0;
    for (const auto& v : j["permissions"]) {
        if (!v.is_string()) continue;
        const auto s = v.get<std::string>();
        for (const auto& [name, bit] : kPerms)
            if (s == name) { bits |= bit; break; }
    }
    return bits;
}

PluginInstaller::Result read_manifest_from_zip(const std::filesystem::path& archive,
                                               PluginInstaller::Identity*    out_id,
                                               mz_zip_archive*               zip_inout)
{
    using R = PluginInstaller::Result;
    using S = PluginInstaller::Status;

    if (!open_zip_reader(zip_inout, archive.string()))
        return R{S::InvalidArchive, {}, {}, "cannot open " + archive.string()};

    const int idx = find_entry(zip_inout, "manifest.json");
    if (idx < 0) {
        close_zip_reader(zip_inout);
        return R{S::ManifestMissing, {}, {}, "manifest.json missing from archive"};
    }

    std::size_t out_size = 0;
    void* raw = mz_zip_reader_extract_to_heap(
        zip_inout, static_cast<mz_uint>(idx), &out_size, 0);
    if (!raw) {
        close_zip_reader(zip_inout);
        return R{S::ExtractFailed, {}, {}, "manifest.json extract failed"};
    }
    std::string manifest_text(static_cast<const char*>(raw), out_size);
    mz_free(raw);

    nlohmann::json j;
    try { j = nlohmann::json::parse(manifest_text); }
    catch (const std::exception& e) {
        close_zip_reader(zip_inout);
        return R{S::ManifestMalformed, {}, {}, e.what()};
    }

    PluginInstaller::Identity id;
    id.id          = j.value("id",          std::string{});
    id.name        = j.value("name",        std::string{});
    id.version     = j.value("version",     std::string{});
    id.author      = j.value("author",      std::string{});
    id.description = j.value("description", std::string{});
    id.kind        = j.value("kind",        std::string{});
    id.permissions = parse_permissions(j);

    if (id.id.empty()) {
        close_zip_reader(zip_inout);
        return R{S::IdMissing, id, {}, "manifest.id is empty"};
    }

    if (out_id) *out_id = id;
    return R{S::Ok, std::move(id), {}, {}};
}

} // namespace

PluginInstaller::Result
PluginInstaller::peek_identity(const std::filesystem::path& archive_path)
{
    mz_zip_archive zip{};
    Identity id;
    auto rc = read_manifest_from_zip(archive_path, &id, &zip);
    if (rc.status == Status::Ok)
        close_zip_reader(&zip);
    return rc;
}

PluginInstaller::Result
PluginInstaller::install(const std::filesystem::path& archive_path,
                         const std::filesystem::path& plugins_root,
                         bool replace_existing)
{
    namespace fs = std::filesystem;

    mz_zip_archive zip{};
    Identity id;
    auto manifest_rc = read_manifest_from_zip(archive_path, &id, &zip);
    if (manifest_rc.status != Status::Ok) return manifest_rc;
    // manifest_rc.status == Ok means zip is still open; we'll close on exit.

    Result out;
    out.identity    = id;
    out.install_dir = plugins_root / id.id;

    std::error_code ec;
    if (fs::exists(out.install_dir, ec) && !fs::is_empty(out.install_dir, ec)) {
        if (!replace_existing) {
            close_zip_reader(&zip);
            out.status        = Status::DestinationExists;
            out.error_message = "already installed: " + out.install_dir.string();
            return out;
        }
        // Replace: drop the existing tree before extracting fresh.
        fs::remove_all(out.install_dir, ec);
    }

    fs::create_directories(out.install_dir, ec);
    if (ec) {
        close_zip_reader(&zip);
        out.status        = Status::DestinationCreateFailed;
        out.error_message = ec.message();
        return out;
    }

    // Extract every file entry under install_dir, preserving the
    // archive's directory layout.
    const auto count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;

        char buf[1024];
        const auto len = mz_zip_reader_get_filename(&zip, i, buf, sizeof(buf));
        if (len == 0) continue;

        const fs::path rel(buf);
        // Defensively reject paths that escape the install_dir.
        if (rel.is_absolute() || rel.string().find("..") != std::string::npos) {
            close_zip_reader(&zip);
            out.status        = Status::ExtractFailed;
            out.error_message = "rejected unsafe path: " + rel.string();
            return out;
        }

        const auto dest_path = out.install_dir / rel;
        fs::create_directories(dest_path.parent_path(), ec);

        std::size_t out_size = 0;
        void* raw = mz_zip_reader_extract_to_heap(&zip, i, &out_size, 0);
        if (!raw) {
            close_zip_reader(&zip);
            out.status        = Status::ExtractFailed;
            out.error_message = "extract failed for " + rel.string();
            return out;
        }
        std::ofstream of(dest_path, std::ios::binary | std::ios::trunc);
        of.write(static_cast<const char*>(raw),
                 static_cast<std::streamsize>(out_size));
        mz_free(raw);
        if (!of) {
            close_zip_reader(&zip);
            out.status        = Status::ExtractFailed;
            out.error_message = "write failed for " + dest_path.string();
            return out;
        }
    }

    close_zip_reader(&zip);
    out.status = Status::Ok;
    return out;
}

} // namespace Slic3r
