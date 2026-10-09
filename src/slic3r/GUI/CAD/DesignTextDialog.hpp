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

    // The vectorised text, centred on the origin, in mm. Empty when there is nothing to insert.
    const ImportRegions& regions() const { return m_regions; }
    wxString             text() const;
    Spec                 spec() const;

    std::function<void()> on_change;   // the outline changed (text, font or height)
    std::function<void()> on_accept;   // Enter / OK with something to insert
    std::function<void()> on_cancel;   // Esc / Cancel / closed

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_sys_color_changed() override;

private:
    void     load_font();          // m_face/m_bold/m_italic -> m_font_file
    void     update_preview();     // text/font/height -> m_regions + size label
    wxFont   current_font() const;
    void     draw_preview(wxWindow* canvas);
    void     accept();
    void     cancel();
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
