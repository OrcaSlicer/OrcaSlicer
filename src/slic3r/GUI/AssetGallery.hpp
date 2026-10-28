#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <wx/bitmap.h>
#include <wx/colour.h>
#include <wx/gdicmn.h>
#include <wx/string.h>
#include <wx/window.h>

#include "GUI_Utils.hpp"

class Button;
class TextInput;
class wxStaticText;

namespace Slic3r::GUI {

class DialogButtons;

// Where an asset came from. Only two cases matter to the window: what the app ships, which can be
// listed but not changed, and what the user added, which can be removed again.
enum class AssetOrigin { BuiltIn, User };

// One selectable thing in a library: a picture, a frame, anything a gallery can draw a thumbnail of.
//
// `id` is the whole contract between the window and its caller. The window returns it and nothing
// else, so it has to be something the caller can resolve on its own later - a file path for a file
// asset, a definition's own id for a definition - and it has to stay the same across a rescan, or a
// selection saved in a project stops resolving.
struct AssetEntry
{
    std::string id;
    wxString    name;
    // The key of the AssetCategory this entry is listed under. An entry whose category is not in the
    // library's own list, or is one the caller hid, is never shown.
    std::string category;
    AssetOrigin origin = AssetOrigin::BuiltIn;
    // Shown when the pointer rests on the tile. Empty means the name is tooltip enough.
    wxString tooltip;
};

// One section of the gallery. Sections are what makes a single window able to serve a ChromaStack
// card and a relief layer without either of them offering the other's assets: the library declares
// what it holds in named groups, and the caller hides the groups that would be wrong for it.
struct AssetCategory
{
    std::string key;
    wxString    title;
    // One short line under the heading, for what the user cannot guess from the tiles: where the
    // files live, what formats are taken. Empty for none.
    wxString hint;
    // This section carries the "Add from file" tile, and the window's Add button is enabled while it
    // is visible.
    bool accepts_files = false;
    // Entries in this section carry a remove button when the pointer is over them.
    bool removable = false;
};

// What a thumbnail has to fit and look right in. A library that loads an image scales it to `size`;
// one that draws its asset (a frame definition has no image to load) draws into a bitmap of this
// size, on this background, in the colours this theme needs. The window centres whatever it gets in
// the tile, so a thumbnail smaller than `size` is fine and letterboxing by hand is not needed.
struct AssetThumbnailStyle
{
    wxSize   size;
    double   scale = 1.0; // the window's DPI scale factor, for wxBitmap::CreateWithLogicalSize
    bool     dark  = false;
    wxColour background;
};

// What the gallery window knows about assets. One window serves any number of libraries, so
// everything specific to a kind of asset - where the files are, what a thumbnail looks like, what
// adding one means - lives behind this and nothing else.
//
// Every method is called on the GUI thread only.
class AssetLibrary
{
public:
    virtual ~AssetLibrary() = default;

    // Identifies the library in state that outlives one window: which sections were collapsed, and
    // anything a library itself keeps in the app config. Stable across releases, like a config key:
    // "chroma_pictures", "chroma_frames".
    virtual std::string key() const = 0;
    // The window's title, e.g. "Choose a picture".
    virtual wxString title() const = 0;

    // The sections, in the order they are listed. A library may report a different set after a
    // rescan - a frame library's sections come from the files it found - so the window re-reads this
    // whenever it re-reads the entries.
    virtual std::vector<AssetCategory> categories() const = 0;

    // Every entry, in the order each section lists them. Held by the library, which is what lets the
    // window keep pointers to them for the length of one layout pass.
    virtual const std::vector<AssetEntry> &entries(bool force_rescan = false) = 0;

    // The tile picture. Called lazily, for tiles that are about to be drawn and a few per timer tick,
    // because this is where a library decodes an image and that cost is what a gallery must not pay
    // for its whole library on the first paint. An invalid bitmap is drawn as a placeholder, which is
    // also the honest answer for a file that turned out not to be readable.
    virtual wxBitmap thumbnail(const AssetEntry &entry, const AssetThumbnailStyle &style) = 0;

    // --- Adding and removing. A library that has no user section leaves all of this alone. ---

    // The file dialog's title and wildcard. Empty wildcard means the library takes no files, and the
    // window then shows no Add button and no add tile.
    virtual wxString file_prompt() const { return {}; }
    virtual wxString file_wildcard() const { return {}; }

    // Takes a copy of `path` into the library's own folder and returns the new entry's id, or
    // nullopt with `error` set to one translated sentence. The file at `path` is only read.
    virtual std::optional<std::string> add_from_file(const std::string &path, std::string &error)
    {
        (void) path;
        error.clear();
        return std::nullopt;
    }

    // Deletes one of the user's own entries. False with `error` set on failure. The window asks the
    // user to confirm first and never calls this for an entry whose section is not `removable`.
    virtual bool remove(const AssetEntry &entry, std::string &error)
    {
        (void) entry;
        error.clear();
        return false;
    }

    // Called with the entry the user chose, before the window closes, so a library that keeps a
    // recently-used order can record the pick.
    virtual void touch(const AssetEntry &entry) { (void) entry; }
};

// How one gallery window differs from another over the same library.
struct AssetGalleryOptions
{
    // Shown with its ring on, and scrolled into view when the window opens. Empty for no selection,
    // which leaves the OK button disabled until the user picks something.
    std::string selected_id;
    // Sections the caller does not want offered here, by AssetCategory::key. This is the whole
    // mechanism behind "a ChromaStack card never offers height maps": one library, and each caller
    // hides what is not its business.
    std::vector<std::string> hidden_categories;
    // Overrides AssetLibrary::title() for a caller that wants to say what the pick is for.
    wxString heading;
    // Built with the window as its parent and placed between the grid and the buttons, for a caller
    // whose question is wider than which asset: a control that only means something beside the tiles
    // belongs beside the tiles. Returning nullptr adds nothing.
    std::function<wxWindow *(wxWindow *parent)> build_extra;
    // The id that is selected now, whenever that changes and once while the window is being built, so
    // extra content can follow the tiles. The same id can come twice - a search re-reads the
    // selection without changing it - so a handler that does anything irreversible has to compare.
    std::function<void(const std::string &id)> on_selection_changed;
};

// The scrolling tile grid. Declared here only so the window can hold one; it is drawn, not built out
// of child widgets, because a library of a few hundred assets is a few hundred windows otherwise.
class AssetGalleryGrid;

// A modal picker over one AssetLibrary: a search box, an Add button, and a scrolling grid of
// thumbnails grouped into collapsible sections.
//
// Thumbnails are loaded off the first paint on purpose. Only tiles inside the visible scroll area are
// asked for, a couple per timer tick, and the rest fill in over the following ticks; the texture
// picker decoded its whole expanded library in one frame and stalled on open for it (see
// "Keep the texture picker from stalling on open", caade80624).
class AssetGallery : public DPIDialog
{
public:
    AssetGallery(wxWindow *parent, AssetLibrary &library, const AssetGalleryOptions &options = {});

    // The id the user chose, valid once ShowModal() returned wxID_OK. Empty otherwise.
    const std::string &selected_id() const { return m_selected; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void on_sys_color_changed() override;

private:
    void rebuild(const std::string &select_id, bool reveal);
    void add_from_file();
    void remove_entry(const AssetEntry &entry);
    void accept();
    void update_footer();

    AssetLibrary      &m_library;
    AssetGalleryGrid  *m_grid    = nullptr;
    TextInput         *m_search  = nullptr;
    Button            *m_add     = nullptr;
    wxStaticText      *m_footer  = nullptr;
    DialogButtons     *m_buttons = nullptr;
    std::string        m_selected;

    std::function<void(const std::string &id)> m_on_selection_changed;
};

// Shows the gallery and returns what the user chose, or nullopt if they cancelled. The one call a
// caller needs; the class above is for a caller that wants to keep the window around.
std::optional<std::string> pick_asset(wxWindow *parent, AssetLibrary &library, const AssetGalleryOptions &options = {});

} // namespace Slic3r::GUI
