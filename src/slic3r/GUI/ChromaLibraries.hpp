#pragma once

#include <optional>
#include <string>
#include <vector>

#include <wx/bitmap.h>
#include <wx/string.h>
#include <wx/window.h>

#include "libslic3r/ChromaStack.hpp"
#include "libslic3r/Point.hpp"

#include "AssetGallery.hpp"

namespace Slic3r::GUI {

// ----------------------------------------------------------------------------------------------
// Pictures
// ----------------------------------------------------------------------------------------------

// The section keys the picture gallery uses. A caller hides a section by key, so they are part of the
// API rather than a detail of the library.
inline constexpr const char *CHROMA_PICTURES_BUILTIN = "chroma_pictures_builtin";
inline constexpr const char *CHROMA_PICTURES_USER    = "chroma_pictures_user";

// The pictures a ChromaStack print can reproduce: photographs, posters, line art. Deliberately not
// the height-map texture library - a ChromaStack solves colours out of an image, and a displacement
// layer reads heights out of one, so an asset that is right for one is wrong for the other and
// neither picker should offer the other's.
//
// Built-in pictures come from resources/chroma_pictures, the user's own from
// <data_dir>/chroma/pictures. The two live apart for the same reason the texture library's two do: an
// app update rewrites the resources tree wholesale, so anything the user added has to sit where an
// update cannot reach it.
class ChromaPictureLibrary : public AssetLibrary
{
public:
    // One library per app run, because the scan is cached in it and the gallery is opened over and
    // over. Safe to call before the GUI exists; the scan happens on the first entries() call.
    static ChromaPictureLibrary &instance();

    std::string                key() const override { return "chroma_pictures"; }
    wxString                   title() const override;
    std::vector<AssetCategory> categories() const override;

    const std::vector<AssetEntry> &entries(bool force_rescan = false) override;
    wxBitmap                       thumbnail(const AssetEntry &entry, const AssetThumbnailStyle &style) override;

    wxString                   file_prompt() const override;
    wxString                   file_wildcard() const override;
    std::optional<std::string> add_from_file(const std::string &path, std::string &error) override;
    bool                       remove(const AssetEntry &entry, std::string &error) override;

    // Whether a picture an id names is still in the library, for a caller holding an id out of a
    // saved project: the id is the file's absolute path, and a file can be moved or deleted between
    // one session and the next. Only valid after a scan, i.e. after the first entries() call.
    const AssetEntry *find(const std::string &id) const;

private:
    std::vector<AssetEntry> m_entries;
    bool                    m_scanned = false;
};

// <data_dir>/chroma/pictures, created if it does not exist. Empty string on failure.
std::string chroma_user_picture_dir();

// ----------------------------------------------------------------------------------------------
// Frames
// ----------------------------------------------------------------------------------------------

// How a frame definition describes the plate's outline. The keywords are the shapes the mesh builder
// can already sweep; `Points` is any other shape, handed in as the loop itself.
enum class ChromaFrameOutline { Rectangle, Rounded, Ellipse, Points };

// How a frame's cross-section follows a width that is not the width it was drawn for.
enum class ChromaFrameScaling
{
    // The profile is stretched to the width it is given: a wider frame is a longer sweep of the same
    // curve. What the parametric frame did, and right for a bevel or a step.
    Stretch,
    // The profile keeps the proportions it was drawn with, and the width left over becomes a flat lip
    // at the outer edge. What a real moulding does, and what keeps a mount board's window at 45
    // degrees however wide the board is.
    Proportional,
};

// One frame in the gallery: everything needed to sweep a plate's edge, as data.
//
// The parametric frame it replaces was a shape enum crossed with a profile enum, which is why it
// could only ever look like one of eight things. The two polylines below are what the mesh builder
// actually consumes, so a definition can be any frame that can be swept, and a user's own frame is a
// file rather than a code change.
//
// Both polylines are in normalized units, because a definition is a shape and the plate is what has
// a size:
//  - `profile` samples are (inset, height): inset 0 at the plate's outer edge and 1 where the frame
//    meets the picture, height 0 at the picture's own surface and 1 at the full rise.
//  - `outline_points` is a closed loop whose bounding box is centred on the origin and no wider than
//    2 in either direction, i.e. drawn inside [-1, 1]; only its shape matters, and the picture is
//    what sets its size.
struct ChromaFrameDef
{
    // What a project stores to say which frame it used, so it has to be stable: the file's stem.
    std::string id;
    wxString    name;
    // The gallery section this is listed under: AssetCategory::key, i.e. one of the built-in category
    // keys or CHROMA_FRAMES_USER.
    std::string category;
    AssetOrigin origin = AssetOrigin::BuiltIn;
    std::string path; // where it was read from, for the error message when it stops being readable

    ChromaFrameOutline outline = ChromaFrameOutline::Rectangle;
    // Rounded only: the corner radius at the plate's edge, as a fraction of the plate's shorter side.
    //
    // Worth keeping it larger than the frame's own width, though it is no longer fatal not to. Every
    // ring of the sweep shares the corner's centre, so the radius shrinks by the distance travelled
    // inwards; chroma_frame_outline() floors it rather than letting it reach zero, so the inner rings
    // keep their point count and the sweep holds together, and a corner tighter than the frame is wide
    // simply reads as a sharp one from there in.
    float corner = 0.f;
    // Points only: the loop itself.
    std::vector<Vec2f> outline_points;

    std::vector<Vec2f> profile;
    float              width_mm = 8.f; // what the gallery offers until the user changes it
    float              rise_mm  = 3.f;
    ChromaFrameScaling scaling  = ChromaFrameScaling::Stretch;

    // The nearest shape in ChromaFrame::Profile, carried only so a plate built by the current
    // chroma_plate_mesh() - which sweeps the enum, not a polyline - still comes out close to what the
    // gallery drew. Once the mesh builder takes the polyline this is dead weight.
    ChromaFrame::Profile legacy_profile = ChromaFrame::Profile::Flat;
};

// The two things chroma_plate_mesh() consumes, resolved for one plate.
struct ChromaFrameGeometry
{
    // The plate's edge: a closed loop in millimetres, centred on the picture. The sweep's other rings
    // are chroma_frame_ring() at a smaller growth.
    std::vector<Vec2f> outline;
    // The cross-section: (inset in mm from the plate's edge, height in mm above z = 0), outer edge
    // first, ending at (width_mm, base_mm) where the frame meets the picture's surface. Exactly the
    // shape libslic3r's chroma_frame_profile() returns, so it drops straight into the same sweep.
    std::vector<Vec2f> profile;
    float              width_mm = 0.f;
    float              rise_mm  = 0.f;
};

// The frame resolved against one plate: `picture_w_mm` x `picture_h_mm` is the picture's own area and
// `base_mm` the solid backing under it, i.e. the height the frame's profile lands on.
//
// A width of 0 gives an empty profile, which is the mesh builder's own way of saying "no frame": the
// picture then runs to the edge of the plate.
ChromaFrameGeometry chroma_frame_geometry(const ChromaFrameDef &def, float width_mm, float rise_mm,
                                          float picture_w_mm, float picture_h_mm, float base_mm);

// The cross-section on its own, in millimetres, for a caller that only draws it.
std::vector<Vec2f> chroma_frame_profile_points(const ChromaFrameDef &def, float width_mm, float rise_mm,
                                               float base_mm);

// One ring of the sweep: the plate's loop `grow_mm` outside the picture's own boundary, which is the
// frame's width at the plate's edge and less than that further in. Produced by libslic3r's
// chroma_frame_outline() through a filled-in ChromaPlate, so every ring the gallery draws is the ring
// the mesh is built from.
std::vector<Vec2f> chroma_frame_ring(const ChromaFrameDef &def, float picture_w_mm, float picture_h_mm,
                                     float width_mm, float grow_mm);

// Writes the definition into a plate's ChromaFrame: the outline keyword, the corner radius in
// millimetres, the loop for a Points outline, the width, the rise and the nearest legacy profile.
// After this, chroma_frame_outline(plate, g) and chroma_plate_mesh(plate) agree with the gallery
// about the plate's shape; only the cross-section is approximated, until the mesh builder can be
// handed chroma_frame_geometry().profile instead.
void chroma_frame_to_plate(const ChromaFrameDef &def, float width_mm, float rise_mm, ChromaPlate &plate);

// Reads one frame definition. Returns nullopt with `error` set to one translated sentence naming the
// file and what is wrong with it: a frame that cannot be swept must be refused on load, where there
// is somewhere to say so, rather than crashing the mesh builder later.
std::optional<ChromaFrameDef> chroma_frame_parse_file(const std::string &path, AssetOrigin origin,
                                                      std::string &error);

// A drawing of the frame: the plate's outline with the picture's boundary inside it, and the
// cross-section under it. Generated rather than shipped, so a user's own frame gets a thumbnail for
// free and so both themes get their own.
wxBitmap chroma_frame_thumbnail(const ChromaFrameDef &def, const AssetThumbnailStyle &style);

// The section key the user's own frames are listed under. The built-in sections' keys are whatever
// the shipped files' "category" says, and the known ones get a translated title (see
// ChromaFrameLibrary::categories()).
inline constexpr const char *CHROMA_FRAMES_USER = "chroma_frames_user";

// The first section of the frame gallery and the one cell in it: no frame at all. Taking the frame
// off is one of the choices made while looking at frames, so it is a cell and not a button
// somewhere else, and a caller who may not refuse a frame hides the section by this key.
//
// The cell has no definition behind it, so its id is a sentinel rather than a file's stem: a caller
// gets an empty frame id back, and ChromaFrameLibrary::find() answers nullptr for it.
inline constexpr const char *CHROMA_FRAMES_NONE   = "chroma_frames_none";
inline constexpr const char *CHROMA_FRAME_NONE_ID = "chroma_frame_none";

// The picture frames the plate's edge can be swept into: a few shipped in resources/chroma_frames,
// plus whatever the user dropped into <data_dir>/chroma/frames.
class ChromaFrameLibrary : public AssetLibrary
{
public:
    static ChromaFrameLibrary &instance();

    std::string                key() const override { return "chroma_frames"; }
    wxString                   title() const override;
    std::vector<AssetCategory> categories() const override;

    const std::vector<AssetEntry> &entries(bool force_rescan = false) override;
    wxBitmap                       thumbnail(const AssetEntry &entry, const AssetThumbnailStyle &style) override;

    wxString                   file_prompt() const override;
    wxString                   file_wildcard() const override;
    std::optional<std::string> add_from_file(const std::string &path, std::string &error) override;
    bool                       remove(const AssetEntry &entry, std::string &error) override;

    // The definition behind an id, or nullptr if no file defines it any more - which is what happens
    // to a project that names a frame the user has since deleted, so the caller has to handle it.
    const ChromaFrameDef *find(const std::string &id);

    // Every definition, in the order the gallery lists them.
    const std::vector<ChromaFrameDef> &definitions(bool force_rescan = false);

    // What went wrong while scanning, one translated sentence per rejected file. Read after a scan so
    // the GUI can report a broken file instead of silently listing one frame fewer.
    const std::vector<std::string> &load_errors() const { return m_errors; }

private:
    void scan();

    std::vector<ChromaFrameDef> m_definitions;
    std::vector<AssetEntry>     m_entries;
    std::vector<std::string>    m_errors;
    bool                        m_scanned = false;
};

// <data_dir>/chroma/frames, created if it does not exist. Empty string on failure.
std::string chroma_user_frame_dir();

// ----------------------------------------------------------------------------------------------
// The two calls a card makes
// ----------------------------------------------------------------------------------------------

// Opens the picture gallery and returns the chosen picture's absolute path, or nullopt if the user
// cancelled. `selected_path` is shown as the current pick and scrolled into view.
//
// Both of these put a modal dialog up, so neither may be called from inside a gizmo's ImGui render
// pass: that would run a second event loop under the frame being drawn. Defer it with
// wxGetApp().CallAfter() from wherever the click is noticed.
std::optional<std::string> pick_chroma_picture(wxWindow *parent, const std::string &selected_path = {});

// Everything the frame gallery decides, which is the whole of the frame and not only which moulding
// it is: the two dimensions are there because a width means something next to the profile it widens
// and nothing next to a number on a panel somewhere else.
struct ChromaFramePick
{
    bool chosen = false; // false when the user cancelled, and then nothing else here means anything
    // What a project stores, or empty for no frame at all - the None cell, and also what a caller
    // should read as "the picture runs to the edge of the plate".
    std::string frame_id;
    // Both 0 when there is no frame, so a caller can write them straight into the plate.
    float width_mm = 0.f;
    float rise_mm  = 0.f;
    // The user asked for the plate to be cut to an outline and chose the file. The frame above still
    // applies: the moulding is swept around whatever shape the file holds.
    bool        svg_requested = false;
    std::string svg_path;
};

// Opens the frame gallery on `selected_id` (empty for the None cell) with `width_mm` and `rise_mm` in
// its fields. Both 0 means there is no frame yet, and then picking one takes that definition's own
// dimensions; a width the caller passed is kept across a pick, because a width is a dimension and the
// frame is a design.
ChromaFramePick pick_chroma_frame(wxWindow *parent, const std::string &selected_id = {}, float width_mm = 0.f,
                                  float rise_mm = 0.f);

} // namespace Slic3r::GUI
