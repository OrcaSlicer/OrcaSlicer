#ifndef slic3r_PrinterAssetRef_hpp_
#define slic3r_PrinterAssetRef_hpp_

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace Slic3r {

// A printer preset's bed_custom_model / bed_custom_texture can name a file in an installed
// resource-printer-asset plugin (a "printer asset pack") instead of a local path:
//
//     orcacloud-asset/<plugin id>/<file>
//
// The value has no drive letter, colon or backslash, so it syncs, versions and shares like any
// other setting; each machine resolves it to its own installed copy of the pack. <plugin id> is the
// Orca Cloud plugin uuid (a local plugin key for packs installed by hand), <file> a file directly
// inside the pack's asset folder.
inline constexpr std::string_view PRINTER_ASSET_REF_PREFIX = "orcacloud-asset/";

struct PrinterAssetRef
{
    std::string plugin_id;
    std::string filename;
};

namespace printer_asset_ref_detail {

// [A-Za-z0-9._-], like Orca Cloud's sanitized plugin file names; never "." or "..".
inline bool is_segment(std::string_view segment)
{
    if (segment.empty() || segment.size() > 128 || segment == "." || segment == "..")
        return false;
    for (const char ch : segment) {
        const bool allowed = std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '.' || ch == '_' || ch == '-';
        if (!allowed)
            return false;
    }
    return true;
}

// Windows opens CON, PRN, AUX, NUL, COM0-9 and LPT0-9, with any extension, as devices: a pack file of
// that name could not be read there.
inline bool is_windows_device_name(std::string_view filename)
{
    std::string stem(filename.substr(0, filename.find('.')));
    for (char &ch : stem)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
        return true;
    return stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
           std::isdigit(static_cast<unsigned char>(stem[3])) != 0;
}

inline bool is_file_segment(std::string_view segment) { return is_segment(segment) && !is_windows_device_name(segment); }

inline bool iends_with(std::string_view value, std::string_view suffix)
{
    if (value.size() < suffix.size())
        return false;
    const std::string_view tail = value.substr(value.size() - suffix.size());
    for (size_t i = 0; i < suffix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(tail[i])) != std::tolower(static_cast<unsigned char>(suffix[i])))
            return false;
    return true;
}

} // namespace printer_asset_ref_detail

// The reference in `value`, or nullopt when it is a plain path (or malformed).
inline std::optional<PrinterAssetRef> parse_printer_asset_ref(std::string_view value)
{
    using namespace printer_asset_ref_detail;
    if (value.substr(0, PRINTER_ASSET_REF_PREFIX.size()) != PRINTER_ASSET_REF_PREFIX)
        return std::nullopt;
    value.remove_prefix(PRINTER_ASSET_REF_PREFIX.size());

    const size_t slash = value.find('/');
    if (slash == std::string_view::npos)
        return std::nullopt;
    const std::string_view plugin_id = value.substr(0, slash);
    const std::string_view filename  = value.substr(slash + 1);
    if (!is_segment(plugin_id) || !is_file_segment(filename))
        return std::nullopt;
    return PrinterAssetRef{std::string(plugin_id), std::string(filename)};
}

// Empty when the id or file name cannot be stored in a reference.
inline std::string make_printer_asset_ref(std::string_view plugin_id, std::string_view filename)
{
    using namespace printer_asset_ref_detail;
    if (!is_segment(plugin_id) || !is_file_segment(filename))
        return {};
    return std::string(PRINTER_ASSET_REF_PREFIX) + std::string(plugin_id) + "/" + std::string(filename);
}

// The Bed Shape dialog's file kinds: an STL bed model, a PNG or SVG bed texture.
inline bool is_printer_bed_model_file(std::string_view filename)
{
    return printer_asset_ref_detail::iends_with(filename, ".stl");
}

inline bool is_printer_bed_texture_file(std::string_view filename)
{
    return printer_asset_ref_detail::iends_with(filename, ".png") || printer_asset_ref_detail::iends_with(filename, ".svg");
}

} // namespace Slic3r

#endif // slic3r_PrinterAssetRef_hpp_
