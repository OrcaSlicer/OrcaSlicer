#include "TaichiDialog.hpp"

#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"

#include "PartPlate.hpp"

#include "GaussianSplatImportDialog.hpp"

#include "libslic3r/Format/TaichiLang.hpp"
#include "libslic3r/Format/GaussianSplatTaichiLang.hpp"

#include <wx/button.h>
#include <wx/choicdlg.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/msgdlg.h>
#include <wx/file.h>

namespace Slic3r {
namespace GUI {

wxBEGIN_EVENT_TABLE(TaichiDialog, DPIDialog)
    EVT_CLOSE(TaichiDialog::on_close)
wxEND_EVENT_TABLE()

TaichiDialog::TaichiDialog(MainFrame* parent)
    : DPIDialog(parent, wxID_ANY, _L("Taichi"), wxDefaultPosition, wxSize(900, 600), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_main_frame(parent)
{
    m_backend_label = _L("CPU");

    auto* topsizer = new wxBoxSizer(wxVERTICAL);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    m_btn_from_model = new wxButton(this, wxID_ANY, _L("Import from Orca"));
    m_btn_gaussian_splat_import = new wxButton(this, wxID_ANY, _L("Gaussian Splat Import…"));
    m_btn_apply_to_model = new wxButton(this, wxID_ANY, _L("Export to Orca (New Plate)"));
    m_btn_add_new_models = new wxButton(this, wxID_ANY, _L("Export to Orca (Current Plate)"));
    m_btn_config = new wxButton(this, wxID_ANY, _L("Config…"));
    m_btn_load = new wxButton(this, wxID_ANY, _L("Load .tai"));
    m_btn_save = new wxButton(this, wxID_ANY, _L("Save .tai"));

    btns->Add(m_btn_from_model, 0, wxRIGHT, 6);
    btns->Add(m_btn_gaussian_splat_import, 0, wxRIGHT, 12);
    btns->Add(m_btn_apply_to_model, 0, wxRIGHT, 12);
    btns->Add(m_btn_add_new_models, 0, wxRIGHT, 12);
    btns->AddStretchSpacer(1);
    btns->Add(m_btn_config, 0, wxRIGHT, 12);
    btns->Add(m_btn_load, 0, wxRIGHT, 6);
    btns->Add(m_btn_save, 0);

    topsizer->Add(btns, 0, wxALL, 8);

    m_text = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
        wxTE_MULTILINE | wxTE_RICH2 | wxTE_DONTWRAP);
    topsizer->Add(m_text, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    SetSizer(topsizer);

    Bind(wxEVT_BUTTON, &TaichiDialog::on_from_model, this, m_btn_from_model->GetId());
    Bind(wxEVT_BUTTON, &TaichiDialog::on_gaussian_splat_import, this, m_btn_gaussian_splat_import->GetId());
    Bind(wxEVT_BUTTON, &TaichiDialog::on_apply_to_model, this, m_btn_apply_to_model->GetId());
    Bind(wxEVT_BUTTON, &TaichiDialog::on_add_new_models, this, m_btn_add_new_models->GetId());
    Bind(wxEVT_BUTTON, &TaichiDialog::on_config, this, m_btn_config->GetId());
    Bind(wxEVT_BUTTON, &TaichiDialog::on_load, this, m_btn_load->GetId());
    Bind(wxEVT_BUTTON, &TaichiDialog::on_save, this, m_btn_save->GetId());
}

static bool append_tai_to_plate(MainFrame* main_frame, const std::string& tai_text, bool create_new_plate, std::string& error)
{
    if (main_frame == nullptr || main_frame->plater() == nullptr) {
        error = "Taichi: plater not available";
        return false;
    }

    auto* plater = main_frame->plater();

    plater->take_snapshot(create_new_plate ? "Taichi: Gaussian Splat import (new plate)" : "Taichi: Gaussian Splat import (current plate)");

    std::vector<size_t> added_objects;
    if (!Slic3r::append_taichi_lang_to_model(plater->model(), tai_text, added_objects, error))
        return false;

    if (added_objects.empty()) {
        error = "No new objects were generated.";
        return false;
    }

    PartPlateList& plate_list = plater->get_partplate_list();

    int target_plate_index = plate_list.get_curr_plate_index();
    if (create_new_plate) {
        target_plate_index = plate_list.create_plate();
        if (target_plate_index < 0) {
            error = "Failed to create a new build plate.";
            return false;
        }
        plater->select_plate(target_plate_index, false);
    }

    for (size_t obj_idx : added_objects) {
        if (obj_idx >= plater->model().objects.size() || plater->model().objects[obj_idx] == nullptr)
            continue;

        ModelObject* obj = plater->model().objects[obj_idx];
        for (size_t inst_idx = 0; inst_idx < obj->instances.size(); ++inst_idx)
            plate_list.add_to_plate((int)obj_idx, (int)inst_idx, target_plate_index);
    }

    plater->object_list_changed();
    for (size_t idx : added_objects)
        plater->changed_object((int)idx);

    return true;
}

bool TaichiDialog::prompt_backend_config()
{
    wxArrayString choices;
    choices.Add(_L("CPU"));
    choices.Add(_L("CUDA"));
    choices.Add(_L("Vulkan"));
    choices.Add(_L("OpenGL"));
    choices.Add(_L("Metal"));
    choices.Add(_L("DX12"));
    choices.Add(_L("OpenCL (if supported)"));

    wxSingleChoiceDialog dlg(this, _L("Choose Taichi backend"), _L("Taichi Config"), choices);
    if (!m_backend_label.empty()) {
        int sel = choices.Index(m_backend_label);
        if (sel != wxNOT_FOUND)
            dlg.SetSelection(sel);
    }

    if (dlg.ShowModal() != wxID_OK)
        return false;

    m_backend_label = dlg.GetStringSelection();
    SetTitle(_L("Taichi") + " - " + m_backend_label);
    return true;
}

void TaichiDialog::on_close(wxCloseEvent& evt)
{
    // Behave like a tool window: user close == hide, so it can be shown again.
    if (evt.CanVeto()) {
        evt.Veto();
        Hide();
        return;
    }

    // Non-vetoable close (typically shutdown): allow destruction and clear pointer.
    if (m_main_frame != nullptr)
        m_main_frame->on_taichi_dialog_closed();

    evt.Skip();
    Destroy();
}

void TaichiDialog::on_from_model(wxCommandEvent& /*evt*/)
{
    if (m_main_frame == nullptr || m_main_frame->plater() == nullptr)
        return;

    const Model& model = m_main_frame->plater()->model();
    m_text->SetValue(from_u8(Slic3r::model_to_taichi_lang(model)));
}

void TaichiDialog::on_gaussian_splat_import(wxCommandEvent& /*evt*/)
{
    GaussianSplatImportDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK)
        return;

    const Slic3r::GaussianSplatImportSpec spec = dlg.get_spec();
    if (spec.image_paths.empty()) {
        wxMessageDialog empty(this, _L("No images selected."), _L("Gaussian Splat"), wxOK | wxICON_INFORMATION);
        empty.ShowModal();
        return;
    }

    const std::string tai = Slic3r::gaussian_splat_to_taichi_lang(spec);
    m_text->SetValue(from_u8(tai));

    wxMessageDialog ask(this,
        _L("Generated Taichi (.tai) text. Create a preview object now?"),
        _L("Gaussian Splat"),
        wxYES_NO | wxCANCEL | wxICON_QUESTION);
    ask.SetYesNoCancelLabels(_L("New plate"), _L("Current plate"), _L("Cancel"));

    const int res = ask.ShowModal();
    if (res == wxID_CANCEL)
        return;

    std::string error;
    const bool ok = append_tai_to_plate(m_main_frame, tai, /*create_new_plate=*/(res == wxID_YES), error);
    if (!ok) {
        wxMessageDialog fail(this, from_u8(error), _L("Gaussian Splat"), wxOK | wxICON_ERROR);
        fail.ShowModal();
    }
}

void TaichiDialog::on_apply_to_model(wxCommandEvent& /*evt*/)
{
    if (m_main_frame == nullptr || m_main_frame->plater() == nullptr)
        return;

    auto* plater = m_main_frame->plater();

    // User-requested workflow: do not edit the current plate/model objects in-place.
    // Instead, compile/import Taichi text into *new* objects and place them onto a new plate.
    if (!prompt_backend_config())
        return;

    plater->take_snapshot("Taichi: Apply to new plate");

    std::string error;
    std::vector<size_t> added_objects;
    if (!Slic3r::append_taichi_lang_to_model(plater->model(), into_u8(m_text->GetValue()), added_objects, error)) {
        wxMessageDialog dlg(this, from_u8(error), _L("Taichi"), wxOK | wxICON_ERROR);
        dlg.ShowModal();
        return;
    }

    if (added_objects.empty()) {
        wxMessageDialog dlg(this, _L("No new objects were generated."), _L("Taichi"), wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }

    PartPlateList& plate_list = plater->get_partplate_list();
    const int new_plate_index = plate_list.create_plate();
    if (new_plate_index < 0) {
        wxMessageDialog dlg(this, _L("Failed to create a new build plate."), _L("Taichi"), wxOK | wxICON_ERROR);
        dlg.ShowModal();
        return;
    }

    plater->select_plate(new_plate_index, false);

    for (size_t obj_idx : added_objects) {
        if (obj_idx >= plater->model().objects.size() || plater->model().objects[obj_idx] == nullptr)
            continue;

        ModelObject* obj = plater->model().objects[obj_idx];
        for (size_t inst_idx = 0; inst_idx < obj->instances.size(); ++inst_idx)
            plate_list.add_to_plate((int)obj_idx, (int)inst_idx, new_plate_index);
    }

    plater->object_list_changed();
    for (size_t idx : added_objects)
        plater->changed_object((int)idx);
}

void TaichiDialog::on_add_new_models(wxCommandEvent& /*evt*/)
{
    if (m_main_frame == nullptr || m_main_frame->plater() == nullptr)
        return;

    auto* plater = m_main_frame->plater();

    // This action appends new object(s) onto the *current* plate.
    // Only the explicit Export-to-New-Plate action should create a new plate.
    plater->take_snapshot("Taichi: Export to current plate");

    std::string error;
    std::vector<size_t> added_objects;

    if (!Slic3r::append_taichi_lang_to_model(plater->model(), into_u8(m_text->GetValue()), added_objects, error)) {
        wxMessageDialog dlg(this, from_u8(error), _L("Taichi"), wxOK | wxICON_ERROR);
        dlg.ShowModal();
        return;
    }

    if (added_objects.empty()) {
        wxMessageDialog dlg(this, _L("No new objects were generated."), _L("Taichi"), wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }

    PartPlateList& plate_list = plater->get_partplate_list();
    const int target_plate_index = plate_list.get_curr_plate_index();

    // Place each generated instance onto the currently selected plate.
    for (size_t obj_idx : added_objects) {
        if (obj_idx >= plater->model().objects.size() || plater->model().objects[obj_idx] == nullptr)
            continue;

        ModelObject* obj = plater->model().objects[obj_idx];
        for (size_t inst_idx = 0; inst_idx < obj->instances.size(); ++inst_idx)
            plate_list.add_to_plate((int)obj_idx, (int)inst_idx, target_plate_index);
    }

    plater->object_list_changed();
    for (size_t idx : added_objects)
        plater->changed_object((int)idx);
}

void TaichiDialog::on_config(wxCommandEvent& /*evt*/)
{
    (void)prompt_backend_config();
}

void TaichiDialog::on_load(wxCommandEvent& /*evt*/)
{
    wxFileDialog dlg(this, _L("Load Taichi file"), wxEmptyString, wxEmptyString,
        _L("Taichi files (*.tai)|*.tai|All files (*.*)|*.*"),
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dlg.ShowModal() != wxID_OK)
        return;

    wxFile file(dlg.GetPath());
    if (!file.IsOpened())
        return;

    wxString content;
    if (!file.ReadAll(&content))
        return;

    m_text->SetValue(content);
}

void TaichiDialog::on_save(wxCommandEvent& /*evt*/)
{
    wxFileDialog dlg(this, _L("Save Taichi file"), wxEmptyString, wxEmptyString,
        _L("Taichi files (*.tai)|*.tai|All files (*.*)|*.*"),
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    if (dlg.ShowModal() != wxID_OK)
        return;

    wxFile file(dlg.GetPath(), wxFile::write);
    if (!file.IsOpened())
        return;

    file.Write(m_text->GetValue());
}

} // namespace GUI
} // namespace Slic3r
