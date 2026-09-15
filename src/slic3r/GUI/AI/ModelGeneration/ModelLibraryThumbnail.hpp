#pragma once

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <wx/image.h>
#include <wx/imagjpeg.h>
#include <wx/imagpng.h>
#include <wx/wfstream.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <ios>
#include <list>
#include <string>

namespace Slic3r::GUI {

// History only needs file identity while listing. Full image validation belongs
// to opening a design, not to walking every historical record on the UI thread.
inline bool is_library_image_file(const boost::filesystem::path& path)
{
    boost::system::error_code ec;
    if (path.empty() || !boost::filesystem::is_regular_file(path, ec) || ec) return false;
    boost::filesystem::ifstream file(path, std::ios::binary);
    unsigned char signature[8] {};
    file.read(reinterpret_cast<char*>(signature), sizeof(signature));
    return (file.gcount() == 8 && signature[0] == 0x89 && signature[1] == 'P' &&
            signature[2] == 'N' && signature[3] == 'G' && signature[4] == 13 &&
            signature[5] == 10 && signature[6] == 26 && signature[7] == 10) ||
           (file.gcount() >= 3 && signature[0] == 0xff && signature[1] == 0xd8 && signature[2] == 0xff);
}

// Inspect dimensions before allocating a decoded image. A pathological source
// must not consume unbounded RAM or keep shutdown waiting for a huge decode.
inline wxSize library_image_dimensions(const boost::filesystem::path& path, bool& png)
{
    boost::filesystem::ifstream file(path, std::ios::binary);
    unsigned char header[24] {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (file.gcount() != sizeof(header)) return {};
    png = header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G' &&
          header[4] == 13 && header[5] == 10 && header[6] == 26 && header[7] == 10;
    if (png) {
        if (header[8] != 0 || header[9] != 0 || header[10] != 0 || header[11] != 13 ||
            header[12] != 'I' || header[13] != 'H' || header[14] != 'D' || header[15] != 'R') return {};
        const auto value = [&](int offset) {
            return (uint32_t(header[offset]) << 24) | (uint32_t(header[offset + 1]) << 16) |
                   (uint32_t(header[offset + 2]) << 8) | uint32_t(header[offset + 3]);
        };
        const auto width = value(16), height = value(20);
        if (width == 0 || height == 0 || width > 65535 || height > 65535) return {};
        return {int(width), int(height)};
    }
    if (header[0] != 0xff || header[1] != 0xd8) return {};
    file.clear();
    file.seekg(2);
    // JPEG metadata can contain many segments. Bound header work as well as
    // decoded pixels; SOF must precede the image scan.
    while (file && file.tellg() < std::streamoff(1024 * 1024)) {
        if (file.get() != 0xff) return {};
        int marker = file.get();
        while (marker == 0xff && file) marker = file.get();
        if (marker < 0 || marker == 0xda || marker == 0xd9) return {};
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        const int high = file.get(), low = file.get();
        if (high < 0 || low < 0) return {};
        const int length = high * 256 + low;
        if (length < 2) return {};
        const bool frame = (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc);
        if (frame) {
            if (length < 8) return {};
            unsigned char size[5] {};
            file.read(reinterpret_cast<char*>(size), sizeof(size));
            if (file.gcount() != sizeof(size)) return {};
            return {int(size[3]) * 256 + size[4], int(size[1]) * 256 + size[2]};
        }
        file.seekg(length - 2, std::ios::cur);
    }
    return {};
}

inline wxImage load_library_thumbnail(const boost::filesystem::path& path, int edge,
                                      const std::atomic<bool>& cancelled)
{
    if (cancelled || edge <= 0 || edge > 1024) return {};
    boost::system::error_code ec;
    const auto bytes = boost::filesystem::file_size(path, ec);
    if (ec || bytes == 0 || bytes > 20 * 1024 * 1024) return {};
    bool png = false;
    const auto dimensions = library_image_dimensions(path, png);
    if (dimensions.x <= 0 || dimensions.y <= 0 ||
        uint64_t(dimensions.x) * uint64_t(dimensions.y) > 16 * 1024 * 1024 || cancelled) return {};
    wxFFileInputStream stream(path.wstring());
    wxImage image;
    // Private handler instances avoid sharing decoder state with UI previews.
    wxPNGHandler png_handler;
    wxJPEGHandler jpeg_handler;
    wxImageHandler& handler = png ? static_cast<wxImageHandler&>(png_handler)
                                  : static_cast<wxImageHandler&>(jpeg_handler);
    if (!stream.IsOk() || !handler.LoadFile(&image, stream, false) || !image.IsOk() || cancelled) return {};
    const double scale = double(edge) / std::max(image.GetWidth(), image.GetHeight());
    return image.Scale(std::max(1, int(image.GetWidth() * scale)),
                       std::max(1, int(image.GetHeight() * scale)), wxIMAGE_QUALITY_HIGH);
}

class ModelLibraryThumbnailCache
{
public:
    wxImage load(const boost::filesystem::path& primary, const boost::filesystem::path& fallback,
                 int edge, const std::atomic<bool>& cancelled)
    {
        if (cancelled) return {};
        auto display = primary;
        if (!display.empty()) display += ".display.png";
        const std::wstring key = stamp(primary) + L"|" + stamp(display) + L"|" + stamp(fallback) + L"|" + std::to_wstring(edge);
        const auto cached = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& entry) { return entry.key == key; });
        if (cached != m_entries.end()) {
            m_entries.splice(m_entries.begin(), m_entries, cached);
            return m_entries.front().image.IsOk() ? m_entries.front().image.Copy() : wxImage();
        }
        wxImage image;
        boost::system::error_code ec;
        const auto source_time = boost::filesystem::last_write_time(primary, ec);
        if (!ec) {
            const auto display_time = boost::filesystem::last_write_time(display, ec);
            if (!ec && display_time >= source_time) image = load_library_thumbnail(display, edge, cancelled);
        }
        if (!image.IsOk()) image = load_library_thumbnail(primary, edge, cancelled);
        if (!image.IsOk()) image = load_library_thumbnail(fallback, edge, cancelled);
        if (!cancelled) {
            // Cache and UI result must not share wxImage reference data across
            // threads (some supported wx builds use a non-atomic refcount).
            m_entries.push_front({key, image.IsOk() ? image.Copy() : wxImage()});
            if (m_entries.size() > 48) m_entries.pop_back();
        }
        return image;
    }

private:
    static std::wstring stamp(const boost::filesystem::path& path)
    {
        if (path.empty()) return {};
        boost::system::error_code ec;
        const auto bytes = boost::filesystem::file_size(path, ec);
        if (ec) return path.wstring() + L":missing";
        const auto modified = boost::filesystem::last_write_time(path, ec);
        return path.wstring() + L":" + std::to_wstring(bytes) + L":" + (ec ? L"unknown" : std::to_wstring(modified));
    }
    struct Entry { std::wstring key; wxImage image; };
    std::list<Entry> m_entries;
};

} // namespace Slic3r::GUI
