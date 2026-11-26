#pragma once

#include <memory>

#include <wx/bitmap.h>
#include <wx/panel.h>
#include <wx/string.h>

#include "GUI_Utils.hpp"
#include "libslic3r/BoundingBox.hpp"

class Label;
class LabeledCheckBox;
class wxMouseEvent;
class wxPaintEvent;
class wxStaticBitmap;
class wxWindow;

namespace Slic3r {
class Model;

namespace GUI {

// One choice of MultipartImportDialog: a picture of the plate as it will look after loading, a title and a description.
// A click anywhere on the card selects it; a double click also confirms the dialog.
class MultipartImportCard : public wxPanel
{
public:
    MultipartImportCard(wxWindow* parent, const wxString& title, const wxString& detail);

    void set_preview(const wxBitmap& preview);
    void set_detail(const wxString& detail);
    void select(bool selected);

private:
    void on_paint(wxPaintEvent& event);
    void on_hover(wxMouseEvent& event);
    void update_colours();

    wxStaticBitmap* m_preview;
    Label*          m_title;
    Label*          m_detail;
    bool            m_hover{false};
    bool            m_selected{false};
};

// Asks whether the objects of an import load as one object with several parts or as separate objects.
// Instead of a yes/no question, each choice shows the plate as it will look after loading.
class MultipartImportDialog : public DPIDialog
{
public:
    // bed is the printable area the previews show. show_auto_drop adds the Auto-Drop check box offered when several
    // files are loaded at once.
    MultipartImportDialog(wxWindow* parent, const Model& model, const BoundingBoxf& bed, const wxString& title, const wxString& question,
                          bool show_auto_drop);
    ~MultipartImportDialog() override;

    bool single_object() const { return m_single_object; }
    bool auto_drop() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_sys_color_changed() override;

private:
    struct Scene;

    void select(bool single_object);
    void update_previews();

    std::unique_ptr<Scene> m_scene;
    MultipartImportCard*   m_single_card;
    MultipartImportCard*   m_separate_card;
    LabeledCheckBox*       m_auto_drop{nullptr};
    bool                   m_single_object{true};
};

} // namespace GUI
} // namespace Slic3r
