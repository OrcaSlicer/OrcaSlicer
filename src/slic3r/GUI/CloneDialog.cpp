#include "CloneDialog.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"

namespace Slic3r { namespace GUI {

CloneDialog::CloneDialog(wxWindow *parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Clone"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    m_plater = wxGetApp().plater();
    m_config = wxGetApp().app_config;
    m_cancel_process = false;

    auto v_sizer = new wxBoxSizer(wxVERTICAL);
    auto f_sizer = new wxFlexGridSizer(2, 2, FromDIP(4) , FromDIP(20));

    auto count_label = new wxStaticText(this, wxID_ANY, _L("Number of copies:"), wxDefaultPosition, wxDefaultSize, 0);
    m_count_spin = new SpinInput(this, wxEmptyString, "", wxDefaultPosition, wxSize(FromDIP(120), -1), wxSP_ARROW_KEYS, 1, 1000, 1);
    m_count_spin->GetTextCtrl()->SetFocus();
    f_sizer->Add(count_label  , 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_count_spin, 0, wxALIGN_CENTER_VERTICAL);

    auto arrange_label = new wxStaticText(this, wxID_ANY, _L("Auto arrange plate after cloning") + ":", wxDefaultPosition, wxDefaultSize, 0);
    arrange_label->Wrap(FromDIP(300));
    m_arrange_cb = new ::CheckBox(this);
    m_arrange_cb->SetValue(m_config->get("auto_arrange") == "true");
    f_sizer->Add(arrange_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_arrange_cb , 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(5));

    v_sizer->Add(f_sizer, 1, wxEXPAND | wxALL, FromDIP(10));

    auto bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_progress = new ProgressBar(this, wxID_ANY, 100);
    m_progress->SetHeight(FromDIP(8));
    m_progress->SetMaxSize(wxSize(-1, FromDIP(8)));
    m_progress->SetProgressForedColour(StateColor::darkModeColorFor(wxColour("#DFDFDF")));
    m_progress->SetDoubleBuffered(true);
    m_progress->Hide();
    bottom_sizer->Add(m_progress, 2, wxEXPAND | wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    auto dlg_btns = new DialogButtons(this, {"Fill", "OK", "Cancel"}, "", 1 /*left_aligned*/);

    dlg_btns->GetFIRST()->SetToolTip(_L("Fill bed with copies"));
    dlg_btns->GetFIRST()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        m_plater->fill_bed_with_instances();
        EndModal(wxID_OK);
    });

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this, dlg_btns, v_sizer](wxCommandEvent &e) {

        m_count_spin->Disable(); // also ensures input box value applied with wxEVT_KILL_FOCUS
        m_arrange_cb->Disable();

        m_count = m_count_spin->GetValue();

        m_progress->Show();

        dlg_btns->GetOK()->Hide();
        dlg_btns->GetFIRST()->Hide();

        this->Layout();
        v_sizer->Fit(this);
        Refresh();

        Selection& sel = m_plater->canvas3D()->get_selection();
        m_plater->take_snapshot(std::string("Selection-clone"));
        m_plater->Freeze(); // Better to stop rendering canvas while processing
        sel.copy_to_clipboard();
        for (int i = 0; i < m_count; i++) { // same method with Selection::clone()
            m_progress->SetValue(static_cast<int>(static_cast<double>(i) / m_count * 100)); // pass 0 / 100
            sel.paste_from_clipboard();
            if(m_cancel_process){
                m_plater->undo();
                return;
            }
            wxYield(); // Allow event loop to process updates
        }

        if(!m_cancel_process){
            if (m_arrange_cb->GetValue()){
                m_plater->set_prepare_state(Job::PREPARE_STATE_MENU);
                m_plater->arrange();
            }
            m_plater->Thaw();
            EndModal(wxID_OK);
        }
    });

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        m_cancel_process = true;
        if(m_plater->IsFrozen())
            m_plater->Thaw();
        EndModal(wxID_CANCEL);
    });

    bottom_sizer->Add(dlg_btns, 1, wxEXPAND);

    v_sizer->Add(bottom_sizer, 0, wxEXPAND);

    this->SetSizer(v_sizer);
    this->Layout();
    v_sizer->Fit(this);
    wxGetApp().UpdateDlgDarkUI(this);
}

CloneDialog::~CloneDialog() {}

}} // namespace Slic3r::GUI