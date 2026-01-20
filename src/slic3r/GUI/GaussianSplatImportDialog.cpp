#include "GaussianSplatImportDialog.hpp"

#include "I18N.hpp"

#include "libslic3r/Format/GaussianSplatTaichiLang.hpp"

#include <wx/button.h>
#include <wx/dir.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/listbox.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <string>

namespace Slic3r {
namespace GUI {

static std::string wx_to_u8(const wxString& str)
{
    const wxCharBuffer buf = str.ToUTF8();
    return buf ? std::string(buf.data()) : std::string();
}

wxBEGIN_EVENT_TABLE(GaussianSplatImportDialog, DPIDialog)
wxEND_EVENT_TABLE()

GaussianSplatImportDialog::GaussianSplatImportDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Import Gaussian Splat"), wxDefaultPosition, wxSize(700, 420), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto* topsizer = new wxBoxSizer(wxVERTICAL);

    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, _L("Object name:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        m_object_name = new wxTextCtrl(this, wxID_ANY, _L("Gaussian Splat"));
        row->Add(m_object_name, 1, wxEXPAND);
        topsizer->Add(row, 0, wxEXPAND | wxALL, 8);
    }

    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, _L("Preview cube (mm):")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

        m_preview_cube_mm = new wxSpinCtrlDouble(this, wxID_ANY);
        m_preview_cube_mm->SetRange(1.0, 500.0);
        m_preview_cube_mm->SetIncrement(1.0);
        m_preview_cube_mm->SetDigits(1);
        m_preview_cube_mm->SetValue(20.0);

        row->Add(m_preview_cube_mm, 0, wxRIGHT, 12);
        row->AddStretchSpacer(1);
        topsizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    }

    {
        auto* content = new wxBoxSizer(wxHORIZONTAL);

        m_images = new wxListBox(this, wxID_ANY);
        content->Add(m_images, 1, wxEXPAND | wxRIGHT, 10);

        auto* buttons = new wxBoxSizer(wxVERTICAL);
        m_btn_add_images = new wxButton(this, wxID_ANY, _L("Add images…"));
        m_btn_add_folder = new wxButton(this, wxID_ANY, _L("Add folder…"));
        m_btn_remove = new wxButton(this, wxID_ANY, _L("Remove selected"));
        m_btn_clear = new wxButton(this, wxID_ANY, _L("Clear"));

        buttons->Add(m_btn_add_images, 0, wxEXPAND | wxBOTTOM, 6);
        buttons->Add(m_btn_add_folder, 0, wxEXPAND | wxBOTTOM, 12);
        buttons->Add(m_btn_remove, 0, wxEXPAND | wxBOTTOM, 6);
        buttons->Add(m_btn_clear, 0, wxEXPAND);
        buttons->AddStretchSpacer(1);

        content->Add(buttons, 0, wxEXPAND);
        topsizer->Add(content, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    }

    topsizer->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 8);

    SetSizer(topsizer);

    Bind(wxEVT_BUTTON, &GaussianSplatImportDialog::on_add_images, this, m_btn_add_images->GetId());
    Bind(wxEVT_BUTTON, &GaussianSplatImportDialog::on_add_folder, this, m_btn_add_folder->GetId());
    Bind(wxEVT_BUTTON, &GaussianSplatImportDialog::on_remove_selected, this, m_btn_remove->GetId());
    Bind(wxEVT_BUTTON, &GaussianSplatImportDialog::on_clear, this, m_btn_clear->GetId());

    Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { refresh_buttons(); }, m_images->GetId());

    refresh_buttons();
}

void GaussianSplatImportDialog::add_paths(const wxArrayString& paths)
{
    for (const wxString& p : paths) {
        if (p.empty())
            continue;
        if (m_images->FindString(p) == wxNOT_FOUND)
            m_images->Append(p);
    }

    refresh_buttons();
}

void GaussianSplatImportDialog::refresh_buttons()
{
    const bool has_any = (m_images->GetCount() > 0);
    const bool has_sel = (m_images->GetSelection() != wxNOT_FOUND);

    if (m_btn_remove)
        m_btn_remove->Enable(has_any && has_sel);
    if (m_btn_clear)
        m_btn_clear->Enable(has_any);
}

void GaussianSplatImportDialog::on_add_images(wxCommandEvent&)
{
    wxFileDialog dlg(this,
        _L("Select images for Gaussian Splat"),
        wxEmptyString,
        wxEmptyString,
        _L("Image files (*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff)|*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff|All files (*.*)|*.*"),
        wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);

    if (dlg.ShowModal() != wxID_OK)
        return;

    wxArrayString paths;
    dlg.GetPaths(paths);
    add_paths(paths);
}

void GaussianSplatImportDialog::on_add_folder(wxCommandEvent&)
{
    wxDirDialog dlg(this, _L("Select a folder containing images"));
    if (dlg.ShowModal() != wxID_OK)
        return;

    wxArrayString paths;

    wxString folder = dlg.GetPath();
    if (folder.empty())
        return;

    wxDir dir(folder);
    if (!dir.IsOpened())
        return;

    wxString filename;
    bool cont = dir.GetFirst(&filename);
    while (cont) {
        const wxString full = folder + wxFILE_SEP_PATH + filename;
        const wxString lower = filename.Lower();
        if (lower.EndsWith(".png") || lower.EndsWith(".jpg") || lower.EndsWith(".jpeg") || lower.EndsWith(".bmp") || lower.EndsWith(".tif") || lower.EndsWith(".tiff"))
            paths.Add(full);
        cont = dir.GetNext(&filename);
    }

    add_paths(paths);
}

void GaussianSplatImportDialog::on_remove_selected(wxCommandEvent&)
{
    int sel = m_images->GetSelection();
    if (sel == wxNOT_FOUND)
        return;

    m_images->Delete((unsigned)sel);
    refresh_buttons();
}

void GaussianSplatImportDialog::on_clear(wxCommandEvent&)
{
    m_images->Clear();
    refresh_buttons();
}

GaussianSplatImportSpec GaussianSplatImportDialog::get_spec() const
{
    GaussianSplatImportSpec spec;
    spec.object_name = wx_to_u8(m_object_name ? m_object_name->GetValue() : wxString());
    spec.preview_cube_size_mm = (m_preview_cube_mm != nullptr) ? m_preview_cube_mm->GetValue() : 20.0;

    if (m_images != nullptr) {
        spec.image_paths.reserve((size_t)m_images->GetCount());
        for (unsigned i = 0; i < m_images->GetCount(); ++i)
            spec.image_paths.push_back(wx_to_u8(m_images->GetString(i)));
    }

    return spec;
}

} // namespace GUI
} // namespace Slic3r
