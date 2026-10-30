#include "TextureLibrary.hpp"

#include <algorithm>
#include <atomic>
#include <boost/filesystem/operations.hpp>
#include <cstddef>
#include <boost/filesystem/directory.hpp>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <string>
#include <optional>
#include <memory>
#include <unordered_map>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <vector>
#include <wx/app.h>
#include <wx/gdicmn.h>
#include <utility>
#include <wx/image.h>
#include <wx/mstream.h>

#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/Utils.hpp"

#include "libslic3r/AppConfig.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"

#include "slic3r/plugin/PluginFsUtils.hpp"
#include "slic3r/plugin/host/PluginResourceTextures.hpp"

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

void scan_dir(const boost::filesystem::path &dir, TextureSource source, std::vector<TextureLibraryEntry> &out)
{
    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(dir, ec))
        return;

    const size_t first = out.size();
    for (boost::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!boost::filesystem::is_regular_file(it->path(), ec) || !is_image_file(it->path()))
            continue;
        out.push_back({ it->path().stem().string(), it->path().string(), source });
    }
    std::sort(out.begin() + first, out.end(),
              [](const TextureLibraryEntry &a, const TextureLibraryEntry &b) { return a.name < b.name; });
}

// Appends every loaded pack's textures, ordered by the pack's display name and then by file name. Each
// entry carries the contributing pack's display name, which the picker uses to group a pack's textures
// into one section; a pack name is unique among loaded packs of this capability type, so the name both
// groups the section and keeps its entries contiguous. A pack whose directory fails validation is
// skipped, so one bad pack never hides the others.
void scan_plugin_textures(std::vector<TextureLibraryEntry> &out)
{
    std::vector<TextureLibraryEntry> found;
    for (const ResourceTextureEntry &pack : PluginResourceTextures::instance().entries()) {
        std::string                    error;
        const std::vector<std::string> files =
            collect_resource_texture_files(pack.texture_dir, pack.plugin_root, error);
        if (!error.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "Texture pack '" << pack.display_name << "' was skipped: " << error;
            continue;
        }
        for (const std::string &file : files) {
            const boost::filesystem::path path(file);
            found.push_back({ path.stem().string(), file, TextureSource::Plugin, pack.display_name });
        }
    }

    std::stable_sort(found.begin(), found.end(), [](const TextureLibraryEntry &a, const TextureLibraryEntry &b) {
        if (a.pack_name != b.pack_name)
            return a.pack_name < b.pack_name;
        return a.name < b.name;
    });
    for (TextureLibraryEntry &entry : found)
        out.push_back(std::move(entry));
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

// A wxImage over decoded pixels, so the encoders above can be reached from an in-memory image as well as
// from a file. Grayscale is expanded to the three equal channels wxImage always stores, which is what
// image_has_color() then reads as "no colour" - so a grey image still encodes as a grayscale PNG.
//
// `scratch` holds the pixel buffer: the wxImage is constructed over it without taking ownership, so it
// has to outlive every use of the returned image.
wxImage image_from_decoded(const DecodedHeightTexture &image, std::vector<unsigned char> &scratch)
{
    if (image.empty())
        return wxImage();

    const size_t n = size_t(image.width) * size_t(image.height);
    scratch.resize(n * 3);
    if (image.has_color())
        scratch.assign(image.rgb.begin(), image.rgb.end());
    else
        for (size_t i = 0; i < n; ++i)
            scratch[i * 3] = scratch[i * 3 + 1] = scratch[i * 3 + 2] = image.pixels[i];

    return wxImage(image.width, image.height, scratch.data(), /* static_data */ true);
}

std::vector<unsigned char> read_file_bytes(const std::string &path)
{
    std::ifstream ifs(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

// True if decode_height_texture() can read these bytes, so they can be stored on a layer as-is.
// Anything else is converted on load.
bool is_supported_height_map(const std::vector<unsigned char> &bytes)
{
    return !bytes.empty() && png::is_decodable_8bit_png(png::ReadBuf{ bytes.data(), bytes.size() });
}

std::vector<TextureLibraryEntry> g_library;
bool                             g_library_scanned = false;
// The generation g_library was last built from, so texture_library() can rescan after a pack change.
uint64_t g_library_scan_generation = 0;

// Bumped by plugin callbacks, read by the GUI frame. Atomic so a read never races a write.
std::atomic<uint64_t> g_library_generation{ 0 };

// Writes `bytes` into the user's own texture folder as `stem`.png, uniquified so an existing texture -
// shipped-name collision, or a second edit of the same source - is never overwritten. Returns the new
// file's library entry. Shared by import_texture_to_library() and save_texture_to_library(), which
// differ only in where their bytes came from.
std::optional<TextureLibraryEntry> write_user_texture(const std::vector<unsigned char> &bytes, const std::string &stem,
                                                      std::string &error)
{
    const std::string user_dir = user_texture_dir();
    if (user_dir.empty()) {
        error = _u8L("Could not create the folder for imported textures.");
        return std::nullopt;
    }

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

// The user's own textures, most recently used first, kept in the app config. File names are stored rather
// than paths so the order survives a moved data directory; '/' separates them because no platform allows
// it inside a file name. Bounded so a long-lived config does not grow a line without end.
const char *const RECENT_SECTION = "texture_displacement";
const char *const RECENT_KEY     = "recent_textures";
const size_t      RECENT_MAX     = 200;

std::vector<std::string> recent_texture_names()
{
    std::vector<std::string> names;
    if (wxTheApp == nullptr) // headless or shutting down: there is no app config to read
        return names;
    if (wxGetApp().app_config == nullptr)
        return names;
    const std::string value = wxGetApp().app_config->get(RECENT_SECTION, RECENT_KEY);
    for (size_t begin = 0; begin <= value.size();) {
        const size_t end = std::min(value.find('/', begin), value.size());
        if (end > begin)
            names.push_back(value.substr(begin, end - begin));
        begin = end + 1;
    }
    return names;
}

void store_recent_texture_names(std::vector<std::string> names)
{
    if (wxTheApp == nullptr) // headless or shutting down: there is no app config to write
        return;
    if (wxGetApp().app_config == nullptr)
        return;
    if (names.size() > RECENT_MAX)
        names.resize(RECENT_MAX);
    std::string value;
    for (const std::string &name : names)
        value += (value.empty() ? "" : "/") + name;
    wxGetApp().app_config->set(RECENT_SECTION, RECENT_KEY, value);
}

// Reorders the user's entries by recent use. Stable, so textures never used keep the name order
// scan_dir() gave them, after every used one. Only the user's own range is touched: shipped and pack
// entries must keep the order their scans produced.
void sort_user_textures(std::vector<TextureLibraryEntry> &library)
{
    const std::vector<std::string>       names = recent_texture_names();
    std::unordered_map<std::string, int> rank;
    for (size_t i = 0; i < names.size(); ++i)
        rank.emplace(names[i], int(i));
    const auto rank_of = [&rank](const TextureLibraryEntry &e) {
        const auto it = rank.find(boost::filesystem::path(e.path).filename().string());
        return it == rank.end() ? std::numeric_limits<int>::max() : it->second;
    };
    const auto first_user = std::find_if(library.begin(), library.end(),
                                         [](const TextureLibraryEntry &e) { return e.source == TextureSource::User; });
    const auto user_end   = std::find_if(first_user, library.end(),
                                         [](const TextureLibraryEntry &e) { return e.source != TextureSource::User; });
    std::stable_sort(first_user, user_end,
                     [&rank_of](const TextureLibraryEntry &a, const TextureLibraryEntry &b) { return rank_of(a) < rank_of(b); });
}

// True if `path` is a file directly inside user_texture_dir() - the only place textures may be removed
// from, or have their use recorded.
bool in_user_texture_dir(const std::string &path)
{
    const std::string dir = user_texture_dir();
    if (dir.empty())
        return false;
    boost::system::error_code ec;
    const bool                same = boost::filesystem::equivalent(boost::filesystem::path(path).parent_path(),
                                                                   boost::filesystem::path(dir), ec);
    return !ec && same;
}

} // namespace

uint64_t texture_library_generation()
{
    return g_library_generation.load();
}

void bump_texture_library_generation()
{
    g_library_generation.fetch_add(1);
}

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
    const uint64_t generation = texture_library_generation();
    if (g_library_scanned && !force_rescan && generation == g_library_scan_generation)
        return g_library;

    g_library.clear();
    scan_dir(boost::filesystem::path(Slic3r::resources_dir()) / "textures" / "displacement", TextureSource::Shipped, g_library);
    const std::string user_dir = user_texture_dir();
    if (!user_dir.empty())
        scan_dir(boost::filesystem::path(user_dir), TextureSource::User, g_library);
    sort_user_textures(g_library);
    scan_plugin_textures(g_library);

    g_library_scan_generation = generation;
    g_library_scanned          = true;
    return g_library;
}

void touch_user_texture(const std::string &path)
{
    if (!in_user_texture_dir(path))
        return;
    const std::string        name  = boost::filesystem::path(path).filename().string();
    std::vector<std::string> names = recent_texture_names();
    names.erase(std::remove(names.begin(), names.end(), name), names.end());
    names.insert(names.begin(), name);
    store_recent_texture_names(std::move(names));
    if (g_library_scanned)
        sort_user_textures(g_library);
}

bool remove_user_texture(const std::string &path, std::string &error)
{
    if (!in_user_texture_dir(path)) {
        error = _u8L("Only your own imported textures can be removed.");
        return false;
    }
    boost::system::error_code ec;
    boost::filesystem::remove(boost::filesystem::path(path), ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "Could not delete texture " << path << ": " << ec.message();
        error = _u8L("Could not delete the texture file.");
        return false;
    }

    const std::string        name  = boost::filesystem::path(path).filename().string();
    std::vector<std::string> names = recent_texture_names();
    names.erase(std::remove(names.begin(), names.end(), name), names.end());
    store_recent_texture_names(std::move(names));
    texture_library(true); // drop it from the list
    return true;
}

std::optional<TextureLibraryEntry> import_texture_to_library(const std::string &source_path, std::string &error)
{
    wxImage image;
    if (!image.LoadFile(from_u8(source_path)) || !image.IsOk()) {
        error = _u8L("Could not load the selected image.");
        return std::nullopt;
    }

    std::vector<unsigned char> bytes;
    if (!encode_png_bytes(image, bytes, error))
        return std::nullopt;

    return write_user_texture(bytes, boost::filesystem::path(source_path).stem().string(), error);
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

std::optional<TextureLibraryEntry> save_texture_to_library(const DecodedHeightTexture &image, const std::string &stem,
                                                           std::string &error)
{
    std::vector<unsigned char> scratch;
    const wxImage              wx = image_from_decoded(image, scratch);
    if (!wx.IsOk()) {
        error = _u8L("The selected image is empty.");
        return std::nullopt;
    }

    std::vector<unsigned char> bytes;
    if (!encode_png_bytes(wx, bytes, error))
        return std::nullopt;

    return write_user_texture(bytes, stem.empty() ? std::string("texture") : stem, error);
}

} // namespace Slic3r::GUI
