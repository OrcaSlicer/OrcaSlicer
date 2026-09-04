#include "TextureLibrary.hpp"

#include <algorithm>
#include <fstream>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <wx/image.h>
#include <wx/mstream.h>

#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Utils.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r::GUI {

namespace {

// Extensions the picker will list. The shipped folder only ever contains .png; the rest are here
// so a user who drops a .jpg straight into their own folder still sees it (load_texture_image_data()
// converts anything it can open).
bool is_image_file(const boost::filesystem::path &path)
{
    std::string ext = path.extension().string();
    boost::algorithm::to_lower(ext);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp";
}

void scan_dir(const boost::filesystem::path &dir, bool is_user, std::vector<TextureLibraryEntry> &out)
{
    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(dir, ec))
        return;

    const size_t first = out.size();
    for (boost::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!boost::filesystem::is_regular_file(it->path(), ec) || !is_image_file(it->path()))
            continue;
        out.push_back({ it->path().stem().string(), it->path().string(), is_user });
    }
    std::sort(out.begin() + first, out.end(),
              [](const TextureLibraryEntry &a, const TextureLibraryEntry &b) { return a.name < b.name; });
}

// True if the image carries any colour at all, i.e. some pixel's channels are not all equal. A
// photo saved as RGB but actually grey should still be stored as a grayscale PNG - it is a third of
// the bytes, and the colour feature keys off has_color(), so storing a grey image as RGB would offer
// the user a "colour" that is a row of identical greys.
bool image_has_color(const wxImage &image)
{
    const unsigned char *rgb = image.GetData();
    if (rgb == nullptr)
        return false;
    const size_t n = size_t(image.GetWidth()) * size_t(image.GetHeight());
    for (size_t i = 0; i < n; ++i)
        if (rgb[i * 3] != rgb[i * 3 + 1] || rgb[i * 3 + 1] != rgb[i * 3 + 2])
            return true;
    return false;
}

// Colour images are stored as-is (as an RGB PNG) so the texture can colour the model as well as
// displace it; decode_height_texture() takes the height from their luminance, with the same
// coefficients ConvertToGreyscale() uses, so the relief is identical either way. wxImage writes the
// PNG here rather than Slic3r::png, which only encodes grayscale.
bool encode_color_png_bytes(const wxImage &image, std::vector<unsigned char> &out, std::string &error)
{
    if (image.GetWidth() <= 0 || image.GetHeight() <= 0) {
        error = _u8L("The selected image is empty.");
        return false;
    }
    wxMemoryOutputStream stream;
    // Alpha would be lost on the way into DecodedHeightTexture anyway, and a partly transparent
    // texture reading as black relief is worse than reading as its own colour over the background.
    wxImage opaque = image;
    if (opaque.HasAlpha())
        opaque.ClearAlpha();
    if (!opaque.SaveFile(stream, wxBITMAP_TYPE_PNG)) {
        error = _u8L("Failed to prepare the texture for use.");
        return false;
    }
    const size_t size = size_t(stream.GetLength());
    out.resize(size);
    stream.CopyTo(out.data(), size);
    if (out.empty()) {
        error = _u8L("Failed to prepare the texture for use.");
        return false;
    }
    return true;
}

// Slic3r::png only writes PNGs to a file, so the encode round-trips through a temp file rather than
// staying in memory. It happens once per import / per texture pick, not per frame, so the I/O is
// not worth avoiding with a second PNG encoder.
bool encode_gray_png_bytes(const wxImage &image, std::vector<unsigned char> &out, std::string &error)
{
    const wxImage gray = image.ConvertToGreyscale();
    const int     w    = gray.GetWidth();
    const int     h    = gray.GetHeight();
    if (w <= 0 || h <= 0) {
        error = _u8L("The selected image is empty.");
        return false;
    }

    // wxImage always stores 3 bytes per pixel; after ConvertToGreyscale the three are equal.
    std::vector<uint8_t> pixels(size_t(w) * size_t(h));
    const unsigned char *rgb = gray.GetData();
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = rgb[i * 3];

    const boost::filesystem::path tmp = boost::filesystem::temp_directory_path()
        / boost::filesystem::unique_path("orca_texdisp_%%%%%%%%.png");
    if (!Slic3r::png::write_gray_to_file(tmp.string(), size_t(w), size_t(h), pixels)) {
        error = _u8L("Failed to prepare the texture for use.");
        return false;
    }

    {
        std::ifstream ifs(tmp.string(), std::ios::binary);
        out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    boost::system::error_code ec;
    boost::filesystem::remove(tmp, ec);

    if (out.empty()) {
        error = _u8L("Failed to prepare the texture for use.");
        return false;
    }
    return true;
}

// Grayscale in, grayscale out; colour in, colour out.
bool encode_png_bytes(const wxImage &image, std::vector<unsigned char> &out, std::string &error)
{
    return image_has_color(image) ? encode_color_png_bytes(image, out, error)
                                  : encode_gray_png_bytes(image, out, error);
}

std::vector<unsigned char> read_file_bytes(const std::string &path)
{
    std::ifstream ifs(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

// True if these bytes are a PNG libslic3r can decode, i.e. can be stored on a layer as-is. Mirrors
// exactly what decode_height_texture() accepts: 8-bit grayscale, or colour (whose luminance is the
// height and whose RGB is available to colour the model).
bool is_supported_height_map(const std::vector<unsigned char> &bytes)
{
    if (bytes.empty())
        return false;
    const png::ReadBuf rbuf{ bytes.data(), bytes.size() };
    if (!png::is_png(rbuf))
        return false;
    png::ImageGreyscale gray;
    if (png::decode_png(rbuf, gray) && gray.cols > 0 && gray.rows > 0)
        return true;
    png::ImageColorscale color;
    return png::decode_colored_png(rbuf, color) && color.cols > 0 && color.rows > 0 &&
           color.bytes_per_pixel >= 3;
}

std::vector<TextureLibraryEntry> g_library;
bool                             g_library_scanned = false;

} // namespace

std::string user_texture_dir()
{
    const boost::filesystem::path dir = boost::filesystem::path(Slic3r::data_dir()) / "textures" / "displacement";
    boost::system::error_code     ec;
    boost::filesystem::create_directories(dir, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "Could not create the user texture directory " << dir.string() << ": " << ec.message();
        return {};
    }
    return dir.string();
}

const std::vector<TextureLibraryEntry> &texture_library(bool force_rescan)
{
    if (g_library_scanned && !force_rescan)
        return g_library;

    g_library.clear();
    scan_dir(boost::filesystem::path(Slic3r::resources_dir()) / "textures" / "displacement", false, g_library);
    const std::string user_dir = user_texture_dir();
    if (!user_dir.empty())
        scan_dir(boost::filesystem::path(user_dir), true, g_library);

    g_library_scanned = true;
    return g_library;
}

std::optional<TextureLibraryEntry> import_texture_to_library(const std::string &source_path, std::string &error)
{
    const std::string user_dir = user_texture_dir();
    if (user_dir.empty()) {
        error = _u8L("Could not create the folder for imported textures.");
        return std::nullopt;
    }

    wxImage image;
    if (!image.LoadFile(from_u8(source_path)) || !image.IsOk()) {
        error = _u8L("Could not load the selected image.");
        return std::nullopt;
    }

    std::vector<unsigned char> bytes;
    if (!encode_png_bytes(image, bytes, error))
        return std::nullopt;

    // Never overwrite an existing texture (the user's or, if they picked the same name twice, their
    // own earlier import) - uniquify instead.
    const std::string       stem = boost::filesystem::path(source_path).stem().string();
    boost::filesystem::path dest = boost::filesystem::path(user_dir) / (stem + ".png");
    for (int i = 2; boost::filesystem::exists(dest); ++i)
        dest = boost::filesystem::path(user_dir) / (stem + " (" + std::to_string(i) + ").png");

    {
        boost::nowide::ofstream ofs(dest.string(), std::ios::binary);
        ofs.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
        if (!ofs.good()) {
            error = _u8L("Failed to save the imported texture.");
            return std::nullopt;
        }
    }

    texture_library(true); // pick the new file up
    const std::string dest_str = dest.string();
    for (const TextureLibraryEntry &e : g_library)
        if (e.path == dest_str)
            return e;

    error = _u8L("Failed to save the imported texture.");
    return std::nullopt;
}

std::shared_ptr<std::vector<unsigned char>> load_texture_image_data(const std::string &path, std::string &error)
{
    std::vector<unsigned char> bytes = read_file_bytes(path);
    if (bytes.empty()) {
        error = _u8L("Could not read the texture file.");
        return nullptr;
    }
    if (is_supported_height_map(bytes))
        return std::make_shared<std::vector<unsigned char>>(std::move(bytes));

    // Not a PNG at all (a jpg somebody copied into the folder by hand, say): convert it the same way
    // an import would - keeping its colour if it has any - but leave the file on disk alone.
    wxImage image;
    if (!image.LoadFile(from_u8(path)) || !image.IsOk()) {
        error = _u8L("Could not load the selected image.");
        return nullptr;
    }
    std::vector<unsigned char> converted;
    if (!encode_png_bytes(image, converted, error))
        return nullptr;
    return std::make_shared<std::vector<unsigned char>>(std::move(converted));
}

} // namespace Slic3r::GUI
