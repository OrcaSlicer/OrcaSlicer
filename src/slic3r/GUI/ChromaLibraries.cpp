#include "ChromaLibraries.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <ios>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/dcmemory.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/pen.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/valtext.h>
#include <wx/window.h>

#include "libslic3r/ChromaStack.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/format.hpp"

#include "AssetGallery.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TextInput.hpp"
#include "format.hpp"

namespace Slic3r::GUI {

namespace {

// The thumbnail's own palette, written as the light colours the dark map knows, like every other
// Orca window's. A drawn thumbnail has to read in both themes, and it is drawn fresh on a theme
// change (AssetGalleryGrid::apply_theme), so one set of literals covers both.
const wxColour FRAME_LINE_COLOUR    = wxColour("#262E30"); // the plate's own edge
const wxColour FRAME_BAND_COLOUR    = wxColour("#DBDBDB"); // the band between the edge and the picture
const wxColour FRAME_PICTURE_COLOUR = wxColour("#FFFFFF"); // the picture's area, as a hole in the band
const wxColour FRAME_ACCENT_COLOUR  = wxColour("#009688"); // the picture's boundary and the section's fill
const wxColour FRAME_CARD_COLOUR    = wxColour("#F8F8F8"); // what the picker's own drawing sits on, as a tile does
const wxColour FRAME_DIM_COLOUR     = wxColour("#6B6B6B"); // the picker's captions

// What the thumbnail's plan view is drawn for. Only the proportions matter - the drawing is of a
// shape, not of a size - and 4:3 is what a photograph usually is.
constexpr float THUMB_PICTURE_W_MM = 100.f;
constexpr float THUMB_PICTURE_H_MM = 75.f;

// Extensions the picture library lists. Anything wxImage can open is accepted on import; these are
// what a file dropped into the folder by hand has to be named to be seen.
bool is_picture_file(const boost::filesystem::path &path)
{
    std::string ext = path.extension().string();
    boost::algorithm::to_lower(ext);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".webp";
}

// Files in `dir` matching `accept`, by name. Missing directories are not an error: a fresh
// installation has no user folder until the user adds something, and a resources folder can be
// missing in a partial build.
std::vector<boost::filesystem::path> scan_dir(const boost::filesystem::path                   &dir,
                                              const std::function<bool(const boost::filesystem::path &)> &accept)
{
    std::vector<boost::filesystem::path> found;
    boost::system::error_code            ec;
    if (!boost::filesystem::is_directory(dir, ec))
        return found;
    for (boost::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec))
        if (boost::filesystem::is_regular_file(it->path(), ec) && accept(it->path()))
            found.push_back(it->path());
    std::sort(found.begin(), found.end(),
              [](const boost::filesystem::path &a, const boost::filesystem::path &b) {
                  return a.filename().string() < b.filename().string();
              });
    return found;
}

// A folder under the data directory, created if it does not exist yet.
std::string user_dir(const char *leaf)
{
    const boost::filesystem::path dir = boost::filesystem::path(Slic3r::data_dir()) / "chroma" / leaf;
    boost::system::error_code     ec;
    boost::filesystem::create_directories(dir, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "Could not create the ChromaStack folder " << dir.string() << ": " << ec.message();
        return {};
    }
    return dir.string();
}

// True if `path` is a file directly inside `dir`. The only place anything may be deleted from, so
// that a shipped asset can never be removed through the gallery however an id was built.
bool inside(const std::string &path, const std::string &dir)
{
    if (dir.empty())
        return false;
    boost::system::error_code ec;
    const bool                same = boost::filesystem::equivalent(boost::filesystem::path(path).parent_path(),
                                                                   boost::filesystem::path(dir), ec);
    return !ec && same;
}

// `stem` + `ext` inside `dir`, with a number appended until the name is free, so adding a second file
// of the same name never overwrites the first.
boost::filesystem::path unique_path(const std::string &dir, const std::string &stem, const std::string &ext)
{
    boost::filesystem::path dest = boost::filesystem::path(dir) / (stem + ext);
    for (int i = 2; boost::filesystem::exists(dest); ++i)
        dest = boost::filesystem::path(dir) / (stem + " (" + std::to_string(i) + ")" + ext);
    return dest;
}

bool copy_file_to(const std::string &source, const boost::filesystem::path &dest, std::string &error)
{
    boost::system::error_code ec;
    boost::filesystem::copy_file(boost::filesystem::path(source), dest, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "Could not copy " << source << " to " << dest.string() << ": " << ec.message();
        error = _u8L("The file could not be copied into your own folder.");
        return false;
    }
    return true;
}

// A thumbnail bitmap of `logical` logical pixels at the window's scale factor, and how many physical
// pixels that turned out to be.
//
// The two differ per platform - CreateWithLogicalSize gives physical = logical on MSW and
// logical * scale on GTK3 and macOS - and an image has to be resampled to the physical size to be
// sharp, so the bitmap is created first and asked how big it really is.
wxBitmap make_canvas(const wxSize &logical, double scale, wxSize &physical)
{
    wxBitmap bitmap;
    if (logical.x <= 0 || logical.y <= 0 || !bitmap.CreateWithLogicalSize(logical, scale))
        return wxBitmap();
    physical = bitmap.GetSize();
    return bitmap;
}

} // namespace

// ----------------------------------------------------------------------------------------------
// Pictures
// ----------------------------------------------------------------------------------------------

std::string chroma_user_picture_dir()
{
    return user_dir("pictures");
}

ChromaPictureLibrary &ChromaPictureLibrary::instance()
{
    static ChromaPictureLibrary library;
    return library;
}

wxString ChromaPictureLibrary::title() const
{
    return _L("Choose a picture");
}

std::vector<AssetCategory> ChromaPictureLibrary::categories() const
{
    std::vector<AssetCategory> out;
    out.push_back({ CHROMA_PICTURES_BUILTIN, _L("Built-in pictures"), {}, false, false });
    out.push_back({ CHROMA_PICTURES_USER, _L("My pictures"),
                    _L("PNG, JPG, BMP or WebP. A copy is kept in your own folder, so an update cannot remove it."),
                    true, true });
    return out;
}

const std::vector<AssetEntry> &ChromaPictureLibrary::entries(bool force_rescan)
{
    if (m_scanned && !force_rescan)
        return m_entries;

    m_entries.clear();
    const auto add = [this](const boost::filesystem::path &path, AssetOrigin origin) {
        AssetEntry entry;
        entry.id       = path.string();
        entry.name     = from_u8(path.stem().string());
        entry.category = origin == AssetOrigin::BuiltIn ? CHROMA_PICTURES_BUILTIN : CHROMA_PICTURES_USER;
        entry.origin   = origin;
        m_entries.push_back(std::move(entry));
    };

    for (const boost::filesystem::path &path :
         scan_dir(boost::filesystem::path(Slic3r::resources_dir()) / "chroma_pictures", is_picture_file))
        add(path, AssetOrigin::BuiltIn);
    const std::string user = chroma_user_picture_dir();
    if (!user.empty())
        for (const boost::filesystem::path &path : scan_dir(boost::filesystem::path(user), is_picture_file))
            add(path, AssetOrigin::User);

    m_scanned = true;
    return m_entries;
}

wxBitmap ChromaPictureLibrary::thumbnail(const AssetEntry &entry, const AssetThumbnailStyle &style)
{
    wxImage image;
    // The whole reason the gallery loads thumbnails lazily: this reads and decodes a photograph, on
    // the GUI thread, and a folder of them would stall the window on open.
    if (!image.LoadFile(from_u8(entry.id)) || !image.IsOk())
        return wxBitmap();
    if (image.GetWidth() <= 0 || image.GetHeight() <= 0)
        return wxBitmap();

    // Fitted inside the tile rather than centre-cropped: a photograph's framing is part of the
    // picture, and the gallery centres whatever it is handed.
    const double k = std::min(double(style.size.x) / double(image.GetWidth()),
                              double(style.size.y) / double(image.GetHeight()));
    const wxSize logical(std::max(1, int(std::lround(image.GetWidth() * k))),
                         std::max(1, int(std::lround(image.GetHeight() * k))));
    wxSize       physical;
    const wxBitmap canvas = make_canvas(logical, style.scale, physical);
    if (!canvas.IsOk())
        return wxBitmap();
    return wxBitmap(image.Scale(physical.x, physical.y, wxIMAGE_QUALITY_HIGH), -1, canvas.GetScaleFactor());
}

wxString ChromaPictureLibrary::file_prompt() const
{
    return _L("Choose a picture to add");
}

wxString ChromaPictureLibrary::file_wildcard() const
{
    return _L("Images (*.png;*.jpg;*.jpeg;*.bmp;*.webp)|*.png;*.jpg;*.jpeg;*.bmp;*.webp");
}

std::optional<std::string> ChromaPictureLibrary::add_from_file(const std::string &path, std::string &error)
{
    // Opened before it is copied: a file that cannot be decoded would otherwise be added as a tile
    // with no picture and no explanation.
    wxImage image;
    if (!image.LoadFile(from_u8(path)) || !image.IsOk()) {
        error = _u8L("That file could not be read as a picture.");
        return std::nullopt;
    }

    const std::string dir = chroma_user_picture_dir();
    if (dir.empty()) {
        error = _u8L("Could not create the folder for your own pictures.");
        return std::nullopt;
    }

    const boost::filesystem::path source = boost::filesystem::path(path);
    // Copied rather than converted, and with its own extension: the solve reads colours out of the
    // picture, so anything done to it here would be done to the print.
    const boost::filesystem::path dest = unique_path(dir, source.stem().string(), source.extension().string());
    if (!copy_file_to(path, dest, error))
        return std::nullopt;

    entries(/* force_rescan */ true);
    return dest.string();
}

bool ChromaPictureLibrary::remove(const AssetEntry &entry, std::string &error)
{
    if (!inside(entry.id, chroma_user_picture_dir())) {
        error = _u8L("Only pictures you added yourself can be removed.");
        return false;
    }
    boost::system::error_code ec;
    boost::filesystem::remove(boost::filesystem::path(entry.id), ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "Could not delete the picture " << entry.id << ": " << ec.message();
        error = _u8L("The picture file could not be deleted.");
        return false;
    }
    entries(/* force_rescan */ true);
    return true;
}

const AssetEntry *ChromaPictureLibrary::find(const std::string &id) const
{
    for (const AssetEntry &entry : m_entries)
        if (entry.id == id)
            return &entry;
    return nullptr;
}

// ----------------------------------------------------------------------------------------------
// Frames: the definition format
// ----------------------------------------------------------------------------------------------

namespace {

// The version this build writes and reads. A file without it is not a frame file at all, which is
// worth refusing loudly: the folder is a place users will drop things.
constexpr int CHROMA_FRAME_FORMAT_VERSION = 1;

const std::map<std::string, ChromaFrameOutline> &outline_keywords()
{
    static const std::map<std::string, ChromaFrameOutline> map{
        { "rectangle", ChromaFrameOutline::Rectangle },
        { "rounded", ChromaFrameOutline::Rounded },
        { "ellipse", ChromaFrameOutline::Ellipse },
        { "points", ChromaFrameOutline::Points },
    };
    return map;
}

// The eight shapes the parametric frame had, by name, for the "legacy_profile" field. They are only
// a fallback for a mesh builder that still sweeps the enum; the polyline above is the real profile.
const std::map<std::string, ChromaFrame::Profile> &legacy_profiles()
{
    static const std::map<std::string, ChromaFrame::Profile> map{
        { "flat", ChromaFrame::Profile::Flat },     { "bevel", ChromaFrame::Profile::Bevel },
        { "step", ChromaFrame::Profile::Step },     { "round", ChromaFrame::Profile::Round },
        { "ogee", ChromaFrame::Profile::Ogee },     { "mat", ChromaFrame::Profile::Mat },
        { "cove", ChromaFrame::Profile::Cove },     { "double_step", ChromaFrame::Profile::Step2 },
    };
    return map;
}

// Reads a number that may be written as an integer in the file. A missing or unusable value leaves
// `out` at the default it came in with, so nothing downstream ever sees a NaN from a typo.
void read_float(const nlohmann::json &node, const char *field, float &out)
{
    const auto it = node.find(field);
    if (it == node.end() || !it->is_number())
        return;
    const float value = it->get<float>();
    if (std::isfinite(value))
        out = value;
}

// Everything the sweep assumes about a definition, checked once, here, where there is a file name to
// blame and a user to tell. The mesh builder has no way to refuse a frame: it would sweep a torn one.
bool validate(const ChromaFrameDef &def, const std::string &name, std::string &error)
{
    if (def.name.empty()) {
        error = Slic3r::format(_u8L("The frame file %1% does not say what the frame is called."), name);
        return false;
    }
    if (!(def.width_mm > 0.f)) {
        error = Slic3r::format(_u8L("The frame file %1% has a frame width of zero or less."), name);
        return false;
    }
    if (!(def.rise_mm >= 0.f)) {
        error = Slic3r::format(_u8L("The frame file %1% has a negative rise."), name);
        return false;
    }
    if (def.profile.size() < 2) {
        error = Slic3r::format(_u8L("The frame file %1% needs a cross-section of at least two samples."), name);
        return false;
    }
    for (const Vec2f &sample : def.profile)
        if (!std::isfinite(sample.x()) || !std::isfinite(sample.y()) || sample.x() < 0.f || sample.x() > 1.f ||
            sample.y() < 0.f || sample.y() > 1.f) {
            error = Slic3r::format(_u8L("The frame file %1% has a cross-section sample outside the 0 to 1 range."),
                                   name);
            return false;
        }
    // Swept from the outer edge inwards, which is what makes a ring per sample possible at all: the
    // insets have to run one way, start at the edge and end on the picture's own surface.
    for (size_t i = 1; i < def.profile.size(); ++i)
        if (def.profile[i].x() < def.profile[i - 1].x()) {
            error = Slic3r::format(
                _u8L("The frame file %1% has a cross-section that does not run from the plate's edge inwards."), name);
            return false;
        }
    if (def.profile.front().x() > 1e-4f || def.profile.back().x() < 1.f - 1e-4f ||
        def.profile.back().y() > 1e-4f) {
        error = Slic3r::format(
            _u8L("The frame file %1% has a cross-section that must start at inset 0 and end at inset 1, height 0."),
            name);
        return false;
    }
    if (def.outline == ChromaFrameOutline::Points && def.outline_points.size() < 3) {
        error = Slic3r::format(_u8L("The frame file %1% needs at least three outline points."), name);
        return false;
    }
    if (def.outline == ChromaFrameOutline::Points) {
        Vec2f lo = def.outline_points.front(), hi = def.outline_points.front();
        for (const Vec2f &p : def.outline_points) {
            if (!std::isfinite(p.x()) || !std::isfinite(p.y())) {
                error = Slic3r::format(_u8L("The frame file %1% has an outline point that is not a number."), name);
                return false;
            }
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
        }
        if (hi.x() - lo.x() <= 1e-4f || hi.y() - lo.y() <= 1e-4f) {
            error = Slic3r::format(_u8L("The frame file %1% has a flat outline, with no area to sweep."), name);
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<ChromaFrameDef> chroma_frame_parse_file(const std::string &path, AssetOrigin origin, std::string &error)
{
    const boost::filesystem::path file(path);
    const std::string             name = file.filename().string();

    boost::nowide::ifstream ifs(path, std::ios::binary);
    if (!ifs.good()) {
        error = Slic3r::format(_u8L("The frame file %1% could not be read."), name);
        return std::nullopt;
    }
    // No exceptions: a file in a folder the user can write to is input, not a programming error.
    const nlohmann::json root = nlohmann::json::parse(ifs, nullptr, /* allow_exceptions */ false);
    if (root.is_discarded() || !root.is_object()) {
        error = Slic3r::format(_u8L("The frame file %1% is not valid JSON."), name);
        return std::nullopt;
    }

    const auto version = root.find("chroma_frame");
    if (version == root.end() || !version->is_number_integer() ||
        version->get<int>() != CHROMA_FRAME_FORMAT_VERSION) {
        error = Slic3r::format(_u8L("%1% is not a ChromaStack frame file this version can read."), name);
        return std::nullopt;
    }

    ChromaFrameDef def;
    def.id     = file.stem().string();
    def.origin = origin;
    def.path   = path;
    if (const auto it = root.find("name"); it != root.end() && it->is_string())
        def.name = from_u8(it->get<std::string>());
    // A user's frame is listed among the user's frames whatever its file says: that section is the
    // one that can be removed, and a file that hid itself among the built-ins could not be.
    if (origin == AssetOrigin::User)
        def.category = CHROMA_FRAMES_USER;
    else if (const auto it = root.find("category"); it != root.end() && it->is_string())
        def.category = it->get<std::string>();
    if (def.category.empty())
        def.category = "plain";

    read_float(root, "width_mm", def.width_mm);
    read_float(root, "rise_mm", def.rise_mm);

    if (const auto it = root.find("outline"); it != root.end() && it->is_object()) {
        if (const auto shape = it->find("shape"); shape != it->end() && shape->is_string()) {
            const auto keyword = outline_keywords().find(shape->get<std::string>());
            if (keyword == outline_keywords().end()) {
                error = Slic3r::format(_u8L("The frame file %1% names an outline shape this version does not know."),
                                       name);
                return std::nullopt;
            }
            def.outline = keyword->second;
        }
        read_float(*it, "corner", def.corner);
        if (const auto points = it->find("points"); points != it->end() && points->is_array()) {
            for (const nlohmann::json &point : *points) {
                if (!point.is_array() || point.size() != 2 || !point[0].is_number() || !point[1].is_number()) {
                    error = Slic3r::format(_u8L("The frame file %1% has an outline point that is not a pair of "
                                                "numbers."),
                                           name);
                    return std::nullopt;
                }
                def.outline_points.emplace_back(point[0].get<float>(), point[1].get<float>());
            }
            // A loop was given, so that is the shape, whatever the keyword said.
            if (!def.outline_points.empty())
                def.outline = ChromaFrameOutline::Points;
        }
    }

    if (const auto it = root.find("profile"); it != root.end() && it->is_array()) {
        for (const nlohmann::json &sample : *it) {
            if (!sample.is_array() || sample.size() != 2 || !sample[0].is_number() || !sample[1].is_number()) {
                error = Slic3r::format(
                    _u8L("The frame file %1% has a cross-section sample that is not a pair of numbers."), name);
                return std::nullopt;
            }
            def.profile.emplace_back(sample[0].get<float>(), sample[1].get<float>());
        }
    }

    if (const auto it = root.find("profile_scaling"); it != root.end() && it->is_string()) {
        const std::string scaling = it->get<std::string>();
        if (scaling == "proportional")
            def.scaling = ChromaFrameScaling::Proportional;
        else if (scaling == "stretch")
            def.scaling = ChromaFrameScaling::Stretch;
        else {
            error = Slic3r::format(_u8L("The frame file %1% names a profile scaling this version does not know."),
                                   name);
            return std::nullopt;
        }
    }

    if (const auto it = root.find("legacy_profile"); it != root.end() && it->is_string()) {
        const auto legacy = legacy_profiles().find(it->get<std::string>());
        if (legacy != legacy_profiles().end())
            def.legacy_profile = legacy->second;
    }

    if (!validate(def, name, error))
        return std::nullopt;
    return def;
}

// ----------------------------------------------------------------------------------------------
// Frames: geometry
// ----------------------------------------------------------------------------------------------

std::vector<Vec2f> chroma_frame_profile_points(const ChromaFrameDef &def, float width_mm, float rise_mm,
                                               float base_mm)
{
    if (width_mm <= 0.f || def.profile.size() < 2)
        return {}; // no frame: the picture runs to the edge of the plate, which is what the sweep reads
    const float rise = std::max(0.f, rise_mm);

    std::vector<Vec2f> out;
    out.reserve(def.profile.size() + 1);
    if (def.scaling == ChromaFrameScaling::Proportional && def.rise_mm > 1e-4f && rise > 0.f) {
        // The moulding keeps the proportions it was drawn with and the leftover width becomes a flat
        // lip at the outer edge, which is what a wider frame of the same moulding really is - and the
        // only way a mount board's 45 degree window stays at 45 degrees when the board is widened.
        const float k    = rise / def.rise_mm;
        const float used = std::min(width_mm, def.width_mm * k);
        if (used > 1e-4f) {
            const float lip = width_mm - used;
            for (const Vec2f &sample : def.profile)
                out.emplace_back(lip + sample.x() * used, base_mm + sample.y() * rise);
            if (lip > 1e-4f)
                out.insert(out.begin(), Vec2f(0.f, out.front().y()));
        }
    }
    if (out.empty())
        for (const Vec2f &sample : def.profile)
            out.emplace_back(sample.x() * width_mm, base_mm + sample.y() * rise);

    // Exactly at the edge and exactly on the picture's surface. The sweep hangs its outer wall off
    // the first sample and the picture's own face off the last, so a micron of rounding error here is
    // a visible step in the mesh.
    out.front().x() = 0.f;
    out.back()      = Vec2f(width_mm, base_mm);
    return out;
}

void chroma_frame_to_plate(const ChromaFrameDef &def, float width_mm, float rise_mm, ChromaPlate &plate)
{
    ChromaFrame &frame = plate.frame;
    frame.width_mm     = std::max(0.f, width_mm);
    frame.rise_mm      = std::max(0.f, rise_mm);
    frame.profile      = def.legacy_profile;
    switch (def.outline) {
    case ChromaFrameOutline::Rounded: frame.shape = ChromaFrame::Shape::Rounded; break;
    case ChromaFrameOutline::Ellipse: frame.shape = ChromaFrame::Shape::Ellipse; break;
    case ChromaFrameOutline::Points: frame.shape = ChromaFrame::Shape::Outline; break;
    case ChromaFrameOutline::Rectangle:
    default: frame.shape = ChromaFrame::Shape::Rectangle; break;
    }
    // Normalized units are handed over as they are: chroma_frame_outline() scales a Shape::Outline
    // loop about its own centre to fit the picture, so what it needs is the shape, not a size.
    frame.outline = def.outline == ChromaFrameOutline::Points ? def.outline_points : std::vector<Vec2f>{};
    // The corner radius is a fraction of the plate's shorter side, so one definition looks the same
    // on a postcard and on a poster; the mesh builder wants it in millimetres at the plate's edge.
    const float shorter = std::min(plate.width_mm, plate.height_mm) + 2.f * frame.width_mm;
    frame.corner_mm     = std::clamp(def.corner, 0.f, 0.5f) * shorter;
}

std::vector<Vec2f> chroma_frame_ring(const ChromaFrameDef &def, float picture_w_mm, float picture_h_mm,
                                     float width_mm, float grow_mm)
{
    // Through libslic3r's own outline function rather than a copy of it: the mesh is swept from those
    // rings, and two implementations of the same loop would drift apart at the first bug fix.
    ChromaPlate plate;
    plate.width_mm  = picture_w_mm;
    plate.height_mm = picture_h_mm;
    chroma_frame_to_plate(def, width_mm, 0.f, plate); // the outline does not depend on the rise
    return Slic3r::chroma_frame_outline(plate, grow_mm);
}

ChromaFrameGeometry chroma_frame_geometry(const ChromaFrameDef &def, float width_mm, float rise_mm,
                                          float picture_w_mm, float picture_h_mm, float base_mm)
{
    ChromaFrameGeometry out;
    out.width_mm = std::max(0.f, width_mm);
    out.rise_mm  = std::max(0.f, rise_mm);
    out.outline  = chroma_frame_ring(def, picture_w_mm, picture_h_mm, out.width_mm, out.width_mm);
    out.profile  = chroma_frame_profile_points(def, out.width_mm, out.rise_mm, base_mm);
    return out;
}

// ----------------------------------------------------------------------------------------------
// Frames: the thumbnail
// ----------------------------------------------------------------------------------------------

namespace {

// Maps a loop or a polyline into a rectangle, keeping its proportions and centring it: a frame that
// is wider than it is tall has to look that way in the tile.
struct FitToRect
{
    FitToRect(const std::vector<Vec2f> &points, const wxRect &into, int pad)
    {
        if (points.empty())
            return;
        Vec2f lo = points.front(), hi = points.front();
        for (const Vec2f &p : points) {
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
        }
        const float span_x = std::max(1e-3f, hi.x() - lo.x());
        const float span_y = std::max(1e-3f, hi.y() - lo.y());
        const float room_x = float(std::max(1, into.width - 2 * pad));
        const float room_y = float(std::max(1, into.height - 2 * pad));
        scale              = std::min(room_x / span_x, room_y / span_y);
        centre             = 0.5f * (lo + hi);
        target             = wxPoint(into.x + into.width / 2, into.y + into.height / 2);
    }

    // y is flipped: the plate's y and the frame's height both grow upwards, and a device context's
    // grows downwards.
    wxPoint operator()(const Vec2f &p) const
    {
        return wxPoint(target.x + int(std::lround((p.x() - centre.x()) * scale)),
                       target.y - int(std::lround((p.y() - centre.y()) * scale)));
    }

    float   scale  = 1.f;
    Vec2f   centre = Vec2f::Zero();
    wxPoint target = wxPoint(0, 0);
};

std::vector<wxPoint> to_device(const std::vector<Vec2f> &points, const FitToRect &fit)
{
    std::vector<wxPoint> out;
    out.reserve(points.size());
    for (const Vec2f &p : points)
        out.push_back(fit(p));
    return out;
}

// The cross-section at one width and rise, fitted into `into`: the swept polyline closed into a slab
// of backing, with the picture's own surface as the line it lands on. Shared by the tile and by the
// picker's larger drawing, which have to agree about what a frame looks like.
void draw_frame_section(wxGCDC &gdc, const ChromaFrameDef &def, float width_mm, float rise_mm, const wxRect &into,
                        int pad, const wxColour &line, const wxColour &band, const wxColour &accent)
{
    const std::vector<Vec2f> profile = chroma_frame_profile_points(def, width_mm, rise_mm, 0.f);
    if (profile.size() < 2)
        return;

    // A slab under the profile, so a flat frame is a flat band rather than a bare line, and so the
    // drawing says which way is up.
    const float        slab = std::max(0.25f * std::max(rise_mm, 0.1f), 0.12f * width_mm);
    std::vector<Vec2f> body = profile;
    body.emplace_back(width_mm, -slab);
    body.emplace_back(0.f, -slab);

    const FitToRect            fit(body, into, pad);
    const std::vector<wxPoint> shape = to_device(body, fit);
    gdc.SetBrush(wxBrush(band));
    gdc.SetPen(wxPen(line, 1));
    gdc.DrawPolygon(int(shape.size()), shape.data());
    // The picture's own surface, as the line the profile lands on: how far the frame stands off the
    // picture is the whole point of a cross-section.
    const wxPoint on_picture = fit(Vec2f(width_mm, 0.f));
    gdc.SetPen(wxPen(accent, 1));
    gdc.DrawLine(on_picture.x, on_picture.y, into.GetRight() - pad, on_picture.y);
}

// "No frame": the picture's edge with nothing around it, dashed because there is nothing there. Drawn
// rather than written, so the None cell reads as one of the frames rather than as a button that
// wandered into the grid.
void draw_no_frame(wxGCDC &gdc, const wxRect &into, int pad, const wxColour &line)
{
    const int room_x = std::max(1, into.width - 2 * pad);
    const int room_y = std::max(1, into.height - 2 * pad);
    int       w      = room_x;
    int       h      = int(std::lround(w * double(THUMB_PICTURE_H_MM) / double(THUMB_PICTURE_W_MM)));
    if (h > room_y) {
        h = room_y;
        w = int(std::lround(h * double(THUMB_PICTURE_W_MM) / double(THUMB_PICTURE_H_MM)));
    }
    gdc.SetBrush(*wxTRANSPARENT_BRUSH);
    gdc.SetPen(wxPen(line, 1, wxPENSTYLE_SHORT_DASH));
    gdc.DrawRectangle(into.x + (into.width - w) / 2, into.y + (into.height - h) / 2, w, h);
}

} // namespace

wxBitmap chroma_frame_thumbnail(const ChromaFrameDef &def, const AssetThumbnailStyle &style)
{
    wxSize   physical;
    wxBitmap bitmap = make_canvas(style.size, style.scale, physical);
    if (!bitmap.IsOk())
        return wxBitmap();

    const wxColour line    = StateColor::darkModeColorFor(FRAME_LINE_COLOUR);
    const wxColour band    = StateColor::darkModeColorFor(FRAME_BAND_COLOUR);
    const wxColour picture = StateColor::darkModeColorFor(FRAME_PICTURE_COLOUR);
    const wxColour accent  = StateColor::darkModeColorFor(FRAME_ACCENT_COLOUR);

    wxMemoryDC mdc(bitmap);
    mdc.SetBackground(wxBrush(style.background));
    mdc.Clear();
    {
        // Rounded corners, a hole in a filled loop and a hairline all need anti-aliasing, which a
        // plain memory DC does not have on MSW.
        wxGCDC gdc(mdc);

        // The drawing is in logical pixels, which is what the bitmap's own scale factor makes of the
        // memory DC's coordinates.
        const wxSize size(style.size);
        const int    pad      = std::max(2, size.y / 16);
        const int    split    = size.y * 62 / 100; // the plan view above, the cross-section below
        const wxRect plan(0, 0, size.x, split);
        const wxRect section(0, split, size.x, size.y - split);

        // ---- Plan: the plate's edge, with the picture's own boundary inside it ----
        const std::vector<Vec2f> edge = chroma_frame_ring(def, THUMB_PICTURE_W_MM, THUMB_PICTURE_H_MM, def.width_mm,
                                                          def.width_mm);
        const std::vector<Vec2f> hole = chroma_frame_ring(def, THUMB_PICTURE_W_MM, THUMB_PICTURE_H_MM, def.width_mm,
                                                          0.f);
        if (edge.size() >= 3) {
            const FitToRect           fit(edge, plan, pad);
            const std::vector<wxPoint> outer = to_device(edge, fit);
            gdc.SetBrush(wxBrush(band));
            gdc.SetPen(wxPen(line, 1));
            gdc.DrawPolygon(int(outer.size()), outer.data());
            if (hole.size() >= 3) {
                // The picture as a hole in the band: the frame is the part that is left, which is what
                // the drawing has to show.
                const std::vector<wxPoint> inner = to_device(hole, fit);
                gdc.SetBrush(wxBrush(picture));
                gdc.SetPen(wxPen(accent, 1));
                gdc.DrawPolygon(int(inner.size()), inner.data());
            }
        }

        gdc.SetPen(wxPen(band, 1));
        gdc.DrawLine(pad, split, size.x - pad, split);

        // ---- Cross-section: the polyline the frame is swept from, on a slab of backing ----
        draw_frame_section(gdc, def, def.width_mm, def.rise_mm, section, pad, line, band, accent);
    }
    mdc.SelectObject(wxNullBitmap); // the bitmap cannot be handed on while it is still selected
    return bitmap;
}

namespace {

// The None cell's tile: the same canvas as a frame's, with the empty frame on it.
wxBitmap chroma_frame_none_thumbnail(const AssetThumbnailStyle &style)
{
    wxSize   physical;
    wxBitmap bitmap = make_canvas(style.size, style.scale, physical);
    if (!bitmap.IsOk())
        return wxBitmap();

    wxMemoryDC mdc(bitmap);
    mdc.SetBackground(wxBrush(style.background));
    mdc.Clear();
    {
        wxGCDC gdc(mdc); // a dashed hairline is another thing a plain memory DC cannot do on MSW
        const wxSize size(style.size);
        draw_no_frame(gdc, wxRect(0, 0, size.x, size.y), std::max(2, size.y / 8),
                      StateColor::darkModeColorFor(FRAME_LINE_COLOUR));
    }
    mdc.SelectObject(wxNullBitmap);
    return bitmap;
}

} // namespace

// ----------------------------------------------------------------------------------------------
// Frames: the library
// ----------------------------------------------------------------------------------------------

std::string chroma_user_frame_dir()
{
    return user_dir("frames");
}

ChromaFrameLibrary &ChromaFrameLibrary::instance()
{
    static ChromaFrameLibrary library;
    return library;
}

wxString ChromaFrameLibrary::title() const
{
    return _L("Choose a frame");
}

std::vector<AssetCategory> ChromaFrameLibrary::categories() const
{
    // The built-in sections come from the shipped files' own "category", in a fixed order so adding a
    // file never reshuffles the gallery, with any unknown key after the known ones and the user's own
    // frames last.
    const std::vector<std::pair<const char *, wxString>> known{
        { "plain", _L("Plain") },
        { "classic", _L("Classic") },
        { "mouldings", _L("Mouldings") },
        { "gallery", _L("Gallery") },
    };

    std::vector<AssetCategory> out;
    // First, before any frame: the user looking for the one they want has to be able to see that not
    // having one is on the same list.
    out.push_back({ CHROMA_FRAMES_NONE, _L("No frame"), {}, false, false });

    std::vector<std::string> used;
    for (const ChromaFrameDef &def : m_definitions)
        if (def.origin == AssetOrigin::BuiltIn &&
            std::find(used.begin(), used.end(), def.category) == used.end())
            used.push_back(def.category);

    for (const auto &[key, title] : known)
        if (std::find(used.begin(), used.end(), key) != used.end())
            out.push_back({ key, title, {}, false, false });
    std::sort(used.begin(), used.end());
    for (const std::string &key : used) {
        const bool is_known = std::any_of(known.begin(), known.end(),
                                          [&key](const auto &pair) { return key == pair.first; });
        if (!is_known)
            // A category this build has no name for is shown under its own key rather than hidden: a
            // frame the user can see is a frame they can pick.
            out.push_back({ key, from_u8(key), {}, false, false });
    }

    out.push_back({ CHROMA_FRAMES_USER, _L("My frames"),
                    _L("Frame definitions you added, as .json files in your own folder."), true, true });
    return out;
}

void ChromaFrameLibrary::scan()
{
    m_definitions.clear();
    m_entries.clear();
    m_errors.clear();

    const auto is_json = [](const boost::filesystem::path &path) {
        std::string ext = path.extension().string();
        boost::algorithm::to_lower(ext);
        return ext == ".json";
    };

    const auto load = [this, &is_json](const boost::filesystem::path &dir, AssetOrigin origin) {
        for (const boost::filesystem::path &path : scan_dir(dir, is_json)) {
            std::string                         error;
            const std::optional<ChromaFrameDef> def = chroma_frame_parse_file(path.string(), origin, error);
            if (!def) {
                // One broken file never hides the rest of the folder; the gallery can report what it
                // skipped, and the frames that did load are still there to pick from.
                BOOST_LOG_TRIVIAL(warning) << "ChromaStack frame skipped: " << error;
                m_errors.push_back(error);
                continue;
            }
            // A user file whose stem collides with a shipped frame would make two frames answer to
            // one id, and a project could then resolve to either. The user's copy keeps its file and
            // loses the duplicate id.
            if (std::any_of(m_definitions.begin(), m_definitions.end(),
                            [&def](const ChromaFrameDef &other) { return other.id == def->id; })) {
                m_errors.push_back(Slic3r::format(_u8L("Two frames are called %1%; only the first was loaded."),
                                                  def->id));
                continue;
            }
            m_definitions.push_back(*def);
        }
    };

    load(boost::filesystem::path(Slic3r::resources_dir()) / "chroma_frames", AssetOrigin::BuiltIn);
    const std::string user = chroma_user_frame_dir();
    if (!user.empty())
        load(boost::filesystem::path(user), AssetOrigin::User);

    m_entries.reserve(m_definitions.size() + 1);
    {
        // No definition behind this one, and none needed: the picker turns the sentinel id back into
        // an empty frame id, and every other caller only ever resolves an id it was given.
        AssetEntry none;
        none.id       = CHROMA_FRAME_NONE_ID;
        none.name     = _L("None");
        none.category = CHROMA_FRAMES_NONE;
        none.tooltip  = _L("No frame, so the picture runs to the edge of the plate.");
        m_entries.push_back(std::move(none));
    }
    for (const ChromaFrameDef &def : m_definitions) {
        AssetEntry entry;
        entry.id       = def.id;
        entry.name     = def.name;
        entry.category = def.category;
        entry.origin   = def.origin;
        // The numbers the frame was drawn for. Mouldings look much alike at tile size, and what the
        // user is really choosing between is how wide the band is and how far it stands proud.
        entry.tooltip = format_wxstr(_L("%1%: %2% mm wide, standing %3% mm proud"), def.name,
                                     wxString::Format("%.1f", def.width_mm),
                                     wxString::Format("%.1f", def.rise_mm));
        m_entries.push_back(std::move(entry));
    }
    m_scanned = true;
}

const std::vector<ChromaFrameDef> &ChromaFrameLibrary::definitions(bool force_rescan)
{
    if (!m_scanned || force_rescan)
        scan();
    return m_definitions;
}

const std::vector<AssetEntry> &ChromaFrameLibrary::entries(bool force_rescan)
{
    if (!m_scanned || force_rescan)
        scan();
    return m_entries;
}

const ChromaFrameDef *ChromaFrameLibrary::find(const std::string &id)
{
    for (const ChromaFrameDef &def : definitions())
        if (def.id == id)
            return &def;
    return nullptr;
}

wxBitmap ChromaFrameLibrary::thumbnail(const AssetEntry &entry, const AssetThumbnailStyle &style)
{
    if (entry.id == CHROMA_FRAME_NONE_ID)
        return chroma_frame_none_thumbnail(style);
    const ChromaFrameDef *def = find(entry.id);
    return def != nullptr ? chroma_frame_thumbnail(*def, style) : wxBitmap();
}

wxString ChromaFrameLibrary::file_prompt() const
{
    return _L("Choose a frame definition to add");
}

wxString ChromaFrameLibrary::file_wildcard() const
{
    return _L("Frame definitions (*.json)|*.json");
}

std::optional<std::string> ChromaFrameLibrary::add_from_file(const std::string &path, std::string &error)
{
    // Parsed before it is copied, so a file that cannot be swept is refused with a reason instead of
    // becoming a tile that fails when it is picked.
    if (!chroma_frame_parse_file(path, AssetOrigin::User, error))
        return std::nullopt;

    const std::string dir = chroma_user_frame_dir();
    if (dir.empty()) {
        error = _u8L("Could not create the folder for your own frames.");
        return std::nullopt;
    }
    // Unique as an id as well as as a file name: the id is the stem, and a stem that a shipped frame
    // already answers to would be dropped by the next scan as a duplicate. The None cell's sentinel
    // is taken too, or the gallery would list two cells answering to one id.
    const std::string base = boost::filesystem::path(path).stem().string();
    std::string       stem = base;
    for (int i = 2; find(stem) != nullptr || stem == CHROMA_FRAME_NONE_ID; ++i)
        stem = base + " (" + std::to_string(i) + ")";
    const boost::filesystem::path dest = unique_path(dir, stem, ".json");
    if (!copy_file_to(path, dest, error))
        return std::nullopt;

    scan(); // the id is the file's stem, which uniquifying may have changed
    return dest.stem().string();
}

bool ChromaFrameLibrary::remove(const AssetEntry &entry, std::string &error)
{
    const ChromaFrameDef *def = find(entry.id);
    if (def == nullptr || !inside(def->path, chroma_user_frame_dir())) {
        error = _u8L("Only frames you added yourself can be removed.");
        return false;
    }
    boost::system::error_code ec;
    boost::filesystem::remove(boost::filesystem::path(def->path), ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "Could not delete the frame " << def->path << ": " << ec.message();
        error = _u8L("The frame file could not be deleted.");
        return false;
    }
    scan();
    return true;
}

// ----------------------------------------------------------------------------------------------
// The two calls a card makes
// ----------------------------------------------------------------------------------------------

std::optional<std::string> pick_chroma_picture(wxWindow *parent, const std::string &selected_path)
{
    AssetGalleryOptions options;
    options.selected_id = selected_path;
    return pick_asset(parent, ChromaPictureLibrary::instance(), options);
}

// ----------------------------------------------------------------------------------------------
// Frames: what the gallery decides besides the moulding
// ----------------------------------------------------------------------------------------------

namespace {

// What the fields take, the same bounds the card's own fields had.
constexpr float FRAME_WIDTH_MAX_MM = 100.f;
constexpr float FRAME_RISE_MAX_MM  = 50.f;

// The drawing beside the fields, in logical pixels. A tile's thumbnail is this tall, and this one has
// only the cross-section in it, so the same square shows it several times over.
constexpr int FRAME_SECTION_SIDE = 92;

wxString format_mm(float value)
{
    return wxString::Format("%.1f", value);
}

// Reads a millimetre field, clamped to [0, max_mm]. Text that is not a number leaves `out` alone: a
// half-typed number is not a number, and a field that fell back to zero under the user's hands would
// be a bug of its own.
bool read_mm(const ::TextInput *input, float max_mm, float &out)
{
    const wxString text = input->GetTextCtrl()->GetValue();
    double         value = 0.;
    // Either separator: the field is written back with the C one, and the user may well type theirs.
    if (!text.ToDouble(&value) && !text.ToCDouble(&value))
        return false;
    out = std::clamp(float(value), 0.f, max_mm);
    return true;
}

// The cross-section of the frame being chosen, drawn large and redrawn from the fields rather than
// cached per definition: the tiles say what a moulding looks like, and this says what the two numbers
// under it do to it.
class ChromaFrameSectionView : public wxWindow
{
public:
    explicit ChromaFrameSectionView(wxWindow *parent) : wxWindow(parent, wxID_ANY)
    {
        SetMinSize(wxSize(FromDIP(FRAME_SECTION_SIDE), FromDIP(FRAME_SECTION_SIDE)));
        // on_paint covers every pixel with the blit at the end of it, so the erase step is pure
        // flicker here.
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &ChromaFrameSectionView::on_paint, this);
    }

    // `def` is nullptr for no frame, which is drawn as the empty frame the None cell shows. The
    // definition is copied because a rescan replaces the library's own vector.
    void set_frame(const ChromaFrameDef *def, float width_mm, float rise_mm)
    {
        m_def.reset();
        if (def != nullptr)
            m_def = *def;
        m_width_mm = width_mm;
        m_rise_mm  = rise_mm;
        Refresh();
    }

private:
    void on_paint(wxPaintEvent &event)
    {
        (void) event;
        wxPaintDC dc(this);

        const wxSize client = GetClientSize();
        if (client.x <= 0 || client.y <= 0)
            return;
        // A back buffer rather than a buffered paint DC, because the drawing goes through wxGCDC: on
        // MSW a plain paint DC is GDI, with no alpha and no anti-aliasing.
        wxBitmap buffer;
        if (!buffer.CreateWithLogicalSize(client, GetDPIScaleFactor()))
            return;

        wxMemoryDC mdc(buffer);
        mdc.SetBackground(wxBrush(StateColor::darkModeColorFor(FRAME_CARD_COLOUR)));
        mdc.Clear();
        {
            wxGCDC       gdc(mdc);
            const wxRect into(0, 0, client.x, client.y);
            const int    pad = FromDIP(10);
            if (m_def)
                draw_frame_section(gdc, *m_def, m_width_mm, m_rise_mm, into, pad,
                                   StateColor::darkModeColorFor(FRAME_LINE_COLOUR),
                                   StateColor::darkModeColorFor(FRAME_BAND_COLOUR),
                                   StateColor::darkModeColorFor(FRAME_ACCENT_COLOUR));
            else
                draw_no_frame(gdc, into, pad, StateColor::darkModeColorFor(FRAME_LINE_COLOUR));
        }
        mdc.SelectObject(wxNullBitmap); // the buffer cannot be blitted while it is still selected
        dc.DrawBitmap(buffer, 0, 0);
    }

    std::optional<ChromaFrameDef> m_def;
    float                         m_width_mm = 0.f;
    float                         m_rise_mm  = 0.f;
};

// The frame's two dimensions, its cross-section and the outline button: everything about the frame
// that is not which moulding it is.
//
// They are in the gallery because this is where the frame is being looked at. A width means something
// next to the profile it widens and very little next to a number on a card, and the outline is the
// same choice from the other side: the moulding swept around a shape nobody shipped.
class ChromaFrameOptions : public wxPanel
{
public:
    ChromaFrameOptions(wxWindow *parent, float width_mm, float rise_mm)
        : wxPanel(parent)
        , m_width_mm(std::clamp(width_mm, 0.f, FRAME_WIDTH_MAX_MM))
        , m_rise_mm(std::clamp(rise_mm, 0.f, FRAME_RISE_MAX_MM))
        // Nothing of the caller's to keep, so a pick may bring the moulding's own dimensions with it.
        , m_seed_width(width_mm <= 0.f)
        , m_seed_rise(rise_mm <= 0.f)
    {
        // The light colour the dialog itself is given; UpdateDlgDarkUI maps both at the end of its
        // constructor.
        SetBackgroundColour(*wxWHITE);

        auto *sizer = new wxBoxSizer(wxVERTICAL);

        auto *upper = new wxBoxSizer(wxHORIZONTAL);
        auto *left  = new wxBoxSizer(wxVERTICAL);

        auto *fields = new wxFlexGridSizer(2, 2, FromDIP(6), FromDIP(10));
        m_width      = add_field(fields, _L("Frame width"), m_width_mm);
        m_rise       = add_field(fields, _L("Stands above"), m_rise_mm);
        left->Add(fields, 0, wxALIGN_LEFT);

        left->Add(caption(_L("The same moulding at any width. The picture loses this much on every side."),
                          FromDIP(300)),
                  0, wxTOP, FromDIP(8));

        upper->Add(left, 1, wxALIGN_TOP);
        m_section = new ChromaFrameSectionView(this);
        upper->Add(m_section, 0, wxALIGN_TOP | wxLEFT, FromDIP(14));
        sizer->Add(upper, 0, wxEXPAND);

        auto *svg = new Button(this, _L("Cut the plate to an SVG outline"));
        svg->SetStyle(ButtonStyle::Regular, ButtonType::Expanded);
        svg->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { choose_svg(); });
        sizer->Add(svg, 0, wxEXPAND | wxTOP, FromDIP(12));
        sizer->Add(caption(_L("Keeps the moulding, swept around whatever shape the file holds."), FromDIP(520)), 0,
                   wxTOP, FromDIP(4));

        SetSizer(sizer);
    }

    // The gallery's selection moved to `id`; the None cell's sentinel means no frame.
    void set_frame(const std::string &id)
    {
        m_frame_id                = id == CHROMA_FRAME_NONE_ID ? std::string{} : id;
        const ChromaFrameDef *def = m_frame_id.empty() ? nullptr : ChromaFrameLibrary::instance().find(m_frame_id);
        // A frame picked where there was none takes its own dimensions, because those are the ones it
        // was drawn at; a width the user already had stays, because that is a dimension and the frame
        // is a design.
        if (def != nullptr) {
            if (m_seed_width)
                m_width_mm = def->width_mm;
            if (m_seed_rise)
                m_rise_mm = def->rise_mm;
        }
        write_fields();
        // Nothing to be wide or proud of without a frame, and a field that did nothing would be the
        // window lying about what it will do.
        m_width->Enable(def != nullptr);
        m_rise->Enable(def != nullptr);
        m_section->set_frame(def, m_width_mm, m_rise_mm);
    }

    // Reads both fields, for a caller about to take the numbers: a field can be left mid-edit, and on
    // GTK a button does not always take the focus a text control would hand over on the way out.
    void commit()
    {
        read_mm(m_width, FRAME_WIDTH_MAX_MM, m_width_mm);
        read_mm(m_rise, FRAME_RISE_MAX_MM, m_rise_mm);
    }

    float              width_mm() const { return m_width_mm; }
    float              rise_mm() const { return m_rise_mm; }
    const std::string &svg_path() const { return m_svg_path; }

private:
    wxStaticText *caption(const wxString &text, int wrap)
    {
        auto *label = new Label(this, Label::Body_12, text);
        label->SetForegroundColour(StateColor::darkModeColorFor(FRAME_DIM_COLOUR));
        // Wrapped at a width rather than left to the sizer: the gallery is resizable, and a caption
        // that re-wraps while the window is being fitted can leave the dialog a line short of its
        // own text.
        label->Wrap(wrap);
        return label;
    }

    ::TextInput *add_field(wxFlexGridSizer *into, const wxString &label, float value)
    {
        into->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        // TRN The unit beside a millimetre field in the ChromaStack frame gallery.
        auto *input = new ::TextInput(this, format_mm(value), _L("mm"), wxEmptyString, wxDefaultPosition,
                                      wxSize(FromDIP(90), -1), wxTE_PROCESS_ENTER);
        input->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        into->Add(input, 0, wxALIGN_CENTER_VERTICAL);

        // Every keystroke that parses: the drawing is the answer to the number, so it has to arrive
        // with it. The text is left exactly as typed until the field is left, because rewriting it
        // under the cursor moves the caret.
        input->GetTextCtrl()->Bind(wxEVT_TEXT, [this, input](wxCommandEvent &event) {
            on_edited(input);
            event.Skip();
        });
        input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this, input](wxCommandEvent &event) {
            write_field(input);
            event.Skip();
        });
        input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [this, input](wxFocusEvent &event) {
            write_field(input); // the clamped value, so the field says what will be used
            event.Skip();
        });
        return input;
    }

    void on_edited(const ::TextInput *input)
    {
        if (input == m_width) {
            if (read_mm(m_width, FRAME_WIDTH_MAX_MM, m_width_mm))
                m_seed_width = false; // typed by hand, so the next pick leaves it alone
        } else if (read_mm(m_rise, FRAME_RISE_MAX_MM, m_rise_mm))
            m_seed_rise = false;
        m_section->set_frame(m_frame_id.empty() ? nullptr : ChromaFrameLibrary::instance().find(m_frame_id),
                             m_width_mm, m_rise_mm);
    }

    // Silently, with ChangeValue: SetValue would send wxEVT_TEXT and re-enter the handler that calls
    // this. Only when the text really differs, because a rewrite moves the caret and drops the
    // selection in a field the user may have just tabbed into.
    void write_field(::TextInput *input)
    {
        const wxString text = format_mm(input == m_width ? m_width_mm : m_rise_mm);
        if (input->GetTextCtrl()->GetValue() != text)
            input->GetTextCtrl()->ChangeValue(text);
    }

    void write_fields()
    {
        write_field(m_width);
        write_field(m_rise);
    }

    void choose_svg()
    {
        wxFileDialog dialog(this, _L("Choose the outline to cut the plate to"), wxEmptyString, wxEmptyString,
                            _L("SVG files (*.svg)|*.svg"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK)
            return; // the user backed out of the file dialog, not out of the gallery

        commit();
        m_svg_path = into_u8(dialog.GetPath());
        // The outline is the last thing to decide here, so choosing a file is choosing: the frame on
        // the screen is the moulding that shape will be swept with.
        if (auto *dlg = dynamic_cast<wxDialog *>(wxGetTopLevelParent(this)); dlg != nullptr)
            dlg->EndModal(wxID_OK);
    }

    ::TextInput            *m_width   = nullptr;
    ::TextInput            *m_rise    = nullptr;
    ChromaFrameSectionView *m_section = nullptr;

    std::string m_frame_id; // empty for no frame
    std::string m_svg_path;
    float       m_width_mm = 0.f;
    float       m_rise_mm  = 0.f;
    bool        m_seed_width = false;
    bool        m_seed_rise  = false;
};

} // namespace

ChromaFramePick pick_chroma_frame(wxWindow *parent, const std::string &selected_id, float width_mm, float rise_mm)
{
    ChromaFrameOptions *controls = nullptr; // the window's child, alive as long as the window is

    AssetGalleryOptions options;
    // No frame is the None cell being selected, so OK always has something to take and taking the
    // frame off is a pick rather than a button outside the gallery.
    options.selected_id = selected_id.empty() ? CHROMA_FRAME_NONE_ID : selected_id;
    options.build_extra = [&controls, width_mm, rise_mm](wxWindow *dialog) -> wxWindow * {
        controls = new ChromaFrameOptions(dialog, width_mm, rise_mm);
        return controls;
    };
    options.on_selection_changed = [&controls](const std::string &id) {
        if (controls != nullptr)
            controls->set_frame(id);
    };

    ChromaFramePick pick;
    AssetGallery    gallery(parent, ChromaFrameLibrary::instance(), options);
    if (gallery.ShowModal() != wxID_OK || controls == nullptr)
        return pick;

    controls->commit();
    pick.chosen = true;
    // The gallery owns the selection, so the id comes from it rather than from the controls that were
    // told about it.
    const std::string chosen = gallery.selected_id();
    pick.frame_id            = chosen == CHROMA_FRAME_NONE_ID ? std::string{} : chosen;
    if (!pick.frame_id.empty()) {
        pick.width_mm = controls->width_mm();
        pick.rise_mm  = controls->rise_mm();
    }
    pick.svg_path      = controls->svg_path();
    pick.svg_requested = !pick.svg_path.empty();
    return pick;
}

} // namespace Slic3r::GUI
