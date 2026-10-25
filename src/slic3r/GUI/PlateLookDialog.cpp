#include "PlateLookDialog.hpp"

#include <wx/filedlg.h>
#include <wx/sizer.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "format.hpp"

namespace Slic3r { namespace GUI {

namespace {

constexpr int ICON_PREVIEW_PIXELS = 80;
constexpr int SKIN_PREVIEW_WIDTH  = 240;
constexpr int SKIN_PREVIEW_HEIGHT = 160;

wxString image_text(const std::string &image_path)
{
    return image_path.empty() ? _L("Built-in") : wxString::FromUTF8(image_path);
}

wxString icon_requirement()
{
    return format_wxstr(_L("PNG, %1% x %2% px, square."), ICON_PIXELS, ICON_PIXELS);
}

wxString skin_requirement()
{
    const wxSize size = skin_pixel_size();
    return format_wxstr(_L("PNG, %1% x %2% px. It is stretched over the whole bed."), size.GetWidth(), size.GetHeight());
}

wxStaticText *new_title(wxWindow *parent, const wxString &text)
{
    wxStaticText *title = new wxStaticText(parent, wxID_ANY, text);
    title->SetFont(title->GetFont().Bold());
    return title;
}

} // namespace

PlateLookDialog::PlateLookDialog(wxWindow *parent, const std::vector<BedType> &bed_types)
    : DPIDialog(parent, wxID_ANY, _L("Customize plates"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_bed_types(bed_types)
{
    SetBackgroundColour(*wxWHITE);
    for (BedType bed_type : m_bed_types)
        m_looks.push_back(get_plate_look(bed_type));

    // Plate type
    m_plate_combo = new ComboBox(this, wxID_ANY, wxString(""), wxDefaultPosition, wxSize(FromDIP(240), -1), 0, nullptr, wxCB_READONLY);
    for (BedType bed_type : m_bed_types)
        m_plate_combo->AppendString(builtin_plate_label(bed_type));
    m_plate_combo->SetSelection(0);
    m_plate_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { select_plate(m_plate_combo->GetSelection()); });

    wxBoxSizer *plate_row = new wxBoxSizer(wxHORIZONTAL);
    plate_row->Add(new wxStaticText(this, wxID_ANY, _L("Plate type")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    plate_row->Add(m_plate_combo, 1, wxALIGN_CENTER_VERTICAL);

    // Label
    m_label_input = new TextInput(this, "", "", "", wxDefaultPosition, wxSize(FromDIP(240), -1));
    m_label_input->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &) {
        m_looks[m_selected].label = m_label_input->GetTextCtrl()->GetValue().utf8_string();
    });
    wxButton *clear_label_button = new wxButton(this, wxID_ANY, _L("Clear"));
    clear_label_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { m_label_input->GetTextCtrl()->Clear(); });

    wxBoxSizer *label_row = new wxBoxSizer(wxHORIZONTAL);
    label_row->Add(new wxStaticText(this, wxID_ANY, _L("Label")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    label_row->Add(m_label_input, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    label_row->Add(clear_label_button, 0, wxALIGN_CENTER_VERTICAL);

    // Icon and skin: a preview on the left, the file and what to upload on the right
    const auto add_image_section = [&](wxBoxSizer *parent_sizer, const wxString &title, const wxString &requirement, const wxSize &preview_size,
                                       wxStaticBitmap *&preview, wxStaticText *&path_text, std::string PlateLook::*image_path) {
        preview   = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, FromDIP(preview_size));
        path_text = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(240), -1), wxST_ELLIPSIZE_MIDDLE);

        wxButton *choose_button = new wxButton(this, wxID_ANY, _L("Choose..."));
        wxButton *clear_button  = new wxButton(this, wxID_ANY, _L("Clear"));
        choose_button->Bind(wxEVT_BUTTON, [this, image_path](wxCommandEvent &) { choose_image(m_looks[m_selected].*image_path); });
        clear_button->Bind(wxEVT_BUTTON, [this, image_path](wxCommandEvent &) {
            (m_looks[m_selected].*image_path).clear();
            show_look();
        });

        wxBoxSizer *buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(choose_button, 0, wxRIGHT, FromDIP(4));
        buttons->Add(clear_button);

        wxBoxSizer *details = new wxBoxSizer(wxVERTICAL);
        details->Add(path_text, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        details->Add(new wxStaticText(this, wxID_ANY, requirement), 0, wxBOTTOM, FromDIP(6));
        details->Add(buttons);

        wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(preview, 0, wxRIGHT, FromDIP(12));
        row->Add(details, 1, wxALIGN_CENTER_VERTICAL);

        parent_sizer->Add(new_title(this, title), 0, wxTOP | wxBOTTOM, FromDIP(6));
        parent_sizer->Add(row, 0, wxEXPAND);
    };

    wxBoxSizer *main = new wxBoxSizer(wxVERTICAL);
    main->Add(plate_row, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    main->Add(label_row, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    add_image_section(main, _L("Icon"), icon_requirement(), wxSize(ICON_PREVIEW_PIXELS, ICON_PREVIEW_PIXELS), m_icon_preview, m_icon_path,
                      &PlateLook::icon_path);
    main->AddSpacer(FromDIP(10));
    add_image_section(main, _L("Skin"), skin_requirement(), wxSize(SKIN_PREVIEW_WIDTH, SKIN_PREVIEW_HEIGHT), m_skin_preview, m_skin_path,
                      &PlateLook::skin_path);

    wxButton *reset_button = new wxButton(this, wxID_ANY, _L("Reset this plate"));
    reset_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { reset_selected_plate(); });

    wxBoxSizer *bottom = new wxBoxSizer(wxHORIZONTAL);
    bottom->Add(reset_button, 0, wxALIGN_CENTER_VERTICAL);
    bottom->AddStretchSpacer(1);
    bottom->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxALIGN_CENTER_VERTICAL);

    wxBoxSizer *frame = new wxBoxSizer(wxVERTICAL);
    frame->Add(main, 1, wxEXPAND | wxALL, FromDIP(16));
    frame->Add(bottom, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    SetSizerAndFit(frame);

    show_look();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void PlateLookDialog::select_plate(size_t index)
{
    m_selected = index;
    show_look();
}

void PlateLookDialog::show_look()
{
    const PlateLook &look = m_looks[m_selected];

    m_label_input->GetTextCtrl()->ChangeValue(wxString::FromUTF8(look.label));
    m_label_input->GetTextCtrl()->SetHint(builtin_plate_label(selected_bed_type()));

    m_icon_path->SetLabel(image_text(look.icon_path));
    m_icon_preview->SetBitmap(icon_bitmap(selected_bed_type(), look.icon_path, this, FromDIP(ICON_PREVIEW_PIXELS), true));

    m_skin_path->SetLabel(image_text(look.skin_path));
    m_skin_preview->SetBitmap(skin_preview_bitmap(look.skin_path, FromDIP(wxSize(SKIN_PREVIEW_WIDTH, SKIN_PREVIEW_HEIGHT))));
}

void PlateLookDialog::choose_image(std::string &image_path)
{
    wxFileDialog dialog(this, _L("Choose a PNG image"), "", "", "PNG (*.png)|*.png", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;

    image_path = dialog.GetPath().utf8_string();
    show_look();
}

void PlateLookDialog::reset_selected_plate()
{
    m_looks[m_selected] = {};
    show_look();
}

void edit_plate_looks(wxWindow *parent)
{
    const std::vector<BedType> bed_types = wxGetApp().plater()->sidebar().get_cur_combox_bed_types();
    if (bed_types.empty())
        return;

    PlateLookDialog dialog(parent, bed_types);
    if (dialog.ShowModal() != wxID_OK)
        return;

    for (size_t index = 0; index < bed_types.size(); ++index)
        set_plate_look(bed_types[index], dialog.looks()[index]);
    wxGetApp().plater()->sidebar().refresh_plate_looks();
}

}} // namespace Slic3r::GUI
