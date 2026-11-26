#ifndef slic3r_GUI_DesignTextDialog_hpp_
#define slic3r_GUI_DesignTextDialog_hpp_

#include "slic3r/GUI/GUI_Utils.hpp"
#include "libslic3r/CAD/SketchImport.hpp"

#include <wx/font.h>
#include <wx/string.h>

#include <functional>
#include <memory>
#include <string>

class wxStaticText;
class TextInput;
class ComboBox;
class CheckBox;

namespace Slic3r {
namespace Emboss { struct FontFile; }
namespace GUI {

// Text for the Design tab: the words, the font (face, bold, italic) and the height, with the
// size in millimetres of exactly what will be inserted. MODELESS: the host draws the text in
// the canvas as it is typed (on_change), so the dialog stays out of the way and can be moved —
// a modal dialog is pinned over the middle of the window on GNOME. Enter or OK accepts, Esc,
// Cancel or closing it cancels (charter 4.2); each fires its callback once and the host then
// destroys the dialog. A new text starts from the last font and height used.
class DesignTextDialog : public DPIDialog
{
public:
    struct Spec {
        wxString    text;
        std::string font;       // WxFontUtils::store_wxFont descriptor (face, bold, italic)
        double      height{10.0};
    };
    // `initial` reopens an existing text for editing; nullptr starts a new one.
    DesignTextDialog(wxWindow* parent, const Spec* initial = nullptr);

    // What a new text starts from, here and in sketch_text: the font and height OK last stored, else the GUI font
    // and 10 mm.
    static wxFont last_font();
    static double last_height();
    // The face the Font list selects for `font`: its own when the list has it, else the plain sans-serif that
    // wxFONTFAMILY_SWISS maps to, else the list's first. macOS's GUI font is its UI font, .AppleSystemUIFont, which is
    // not an installed face; the first face there is a decorative one whose letters carry engraved holes.
    static wxString listed_face(const wxFont& font);

    // The vectorised text, centred on the origin, in mm. Empty when there is nothing to insert.
    const ImportRegions& regions() const { return m_regions; }
    wxString             text() const;
    Spec                 spec() const;

    std::function<void()> on_change;   // the outline changed (text, font or height)
    std::function<void()> on_accept;   // Enter / OK with something to insert
    std::function<void()> on_cancel;   // Esc / Cancel / closed

    // OK and Cancel for a caller with no mouse, as the buttons press them. OK takes only what it is enabled for, text
    // with an outline to insert; while it is not, the size line under the preview says why.
    bool     ok_enabled() const { return !m_regions.empty(); }
    wxString size_line() const;
    void     accept();
    void     cancel();

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_sys_color_changed() override;

private:
    void     load_font();          // m_face/m_bold/m_italic -> m_font_file
    void     update_preview();     // text/font/height -> m_regions + size label
    wxFont   current_font() const;
    void     draw_preview(wxWindow* canvas);
    bool     m_done{false};        // accept/cancel fire once

    double   height_mm() const;    // the typed height, or the last valid one while it is being typed

    ::TextInput*      m_text{nullptr};
    ::ComboBox*       m_face{nullptr};
    ::CheckBox*       m_bold{nullptr};
    ::CheckBox*       m_italic{nullptr};
    ::TextInput*      m_height{nullptr};   // mm; Orca's SpinInput is integer-only
    double            m_last_height{10.0};
    wxWindow*         m_preview{nullptr};
    wxStaticText*     m_size{nullptr};
    wxWindow*         m_ok{nullptr};

    std::shared_ptr<const Emboss::FontFile> m_font_file;
    ImportRegions                           m_regions;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_DesignTextDialog_hpp_
