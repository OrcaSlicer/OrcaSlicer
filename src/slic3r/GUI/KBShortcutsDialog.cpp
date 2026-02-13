#include "libslic3r/libslic3r.h"
#include "KBShortcutsDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/Utils.hpp"
#include "GUI.hpp"
#include "Notebook.hpp"
#include <wx/scrolwin.h>
#include <wx/display.h>
#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "MainFrame.hpp"
#include <wx/notebook.h>

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_PREFERENCES_SELECT_TAB, wxCommandEvent);

KBShortcutsDialog::KBShortcutsDialog()
    : DPIDialog(static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY,_L("Keyboard Shortcuts"),
    wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    // fonts
    const wxFont& font = wxGetApp().normal_font();
    const wxFont& bold_font = wxGetApp().bold_font();
    SetFont(font);

    this->SetSizeHints(wxDefaultSize, wxDefaultSize);
    this->SetBackgroundColour(wxColour(255, 255, 255));

    wxBoxSizer *m_sizer_top = new wxBoxSizer(wxVERTICAL);

    auto m_top_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_top_line->SetBackgroundColour(wxColour(166, 169, 170));

    m_sizer_top->Add(m_top_line, 0, wxEXPAND, 0);
    m_sizer_body = new wxBoxSizer(wxHORIZONTAL);

    m_panel_selects = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_panel_selects->SetBackgroundColour(wxColour(248, 248, 248));
    wxBoxSizer *m_sizer_left = new wxBoxSizer(wxVERTICAL);

    m_sizer_left->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(20));

    m_sizer_left->Add(create_button(0, _L("Global")), 0, wxEXPAND, 0);
    m_sizer_left->Add(create_button(1, _L("Prepare")), 0, wxEXPAND, 0);
    m_sizer_left->Add(create_button(2, _L("Toolbar")), 0, wxEXPAND, 0);
    m_sizer_left->Add(create_button(3, _L("Objects list")), 0, wxEXPAND, 0);
    m_sizer_left->Add(create_button(4, _L("Preview")), 0, wxEXPAND, 0);

    m_panel_selects->SetSizer(m_sizer_left);
    m_panel_selects->Layout();
    m_sizer_left->Fit(m_panel_selects);
    m_sizer_body->Add(m_panel_selects, 0, wxEXPAND, 0);

    m_sizer_right = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_right->Add(0, 0, 0, wxEXPAND | wxLEFT,  FromDIP(12));

    m_simplebook = new wxSimplebook(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(870), FromDIP(500)), 0);

    m_sizer_right->Add(m_simplebook, 1, wxEXPAND, 0);
    m_sizer_body->Add(m_sizer_right, 1, wxEXPAND, 0);
    m_sizer_top->Add(m_sizer_body, 1, wxEXPAND, 0);

    fill_shortcuts();
    for (size_t i = 0; i < m_full_shortcuts.size(); ++i) {
        wxPanel *page = create_page(m_simplebook, m_full_shortcuts[i], font, bold_font);
        m_pages.push_back(page);
        m_simplebook->AddPage(page, m_full_shortcuts[i].first.first, i == 0);
    }

    Bind(EVT_PREFERENCES_SELECT_TAB, &KBShortcutsDialog::OnSelectTabel, this);

    SetSizer(m_sizer_top);
    Layout();
    Fit();
    CenterOnParent();

    // select first
    auto event = wxCommandEvent(EVT_PREFERENCES_SELECT_TAB);
    event.SetInt(0);
    event.SetEventObject(this);
    wxPostEvent(this, event);
    wxGetApp().UpdateDlgDarkUI(this);
}

void KBShortcutsDialog::OnSelectTabel(wxCommandEvent &event)
{
    auto                   id = event.GetInt();
    SelectHash::iterator i  = m_hash_selector.begin();
    while (i != m_hash_selector.end()) {
        Select *sel = i->second;
        if (id == sel->m_index) {
            sel->m_tab_button->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#BFE1DE"))); // ORCA color for selected tab background
            sel->m_tab_text->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#BFE1DE"))); // ORCA color for selected tab background
            sel->m_tab_text->SetFont(::Label::Head_13);
            sel->m_tab_button->Refresh();
            sel->m_tab_text->Refresh();

            m_simplebook->SetSelection(id);
        } else {
            sel->m_tab_button->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F8F8")));
            sel->m_tab_text->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F8F8")));
            sel->m_tab_text->SetFont(::Label::Body_13);
            sel->m_tab_button->Refresh();
            sel->m_tab_text->Refresh();
        }
        i++;
    }
    wxGetApp().UpdateDlgDarkUI(this);
}

wxWindow *KBShortcutsDialog::create_button(int id, wxString text)
{
    auto tab_button = new wxWindow(m_panel_selects, wxID_ANY, wxDefaultPosition, wxSize( FromDIP(150),  FromDIP(28)), wxTAB_TRAVERSAL);

    wxBoxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);

    sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, FromDIP(22));

    auto stext = new wxStaticText(tab_button, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, 0);
    stext->SetFont(::Label::Body_13);
    stext->SetForegroundColour(wxColour(38, 46, 48));
    stext->Wrap(-1);
    sizer->Add(stext, 1, wxALIGN_CENTER, 0);

    tab_button->Bind(wxEVT_LEFT_DOWN, [this, id](auto &e) {
        auto event = wxCommandEvent(EVT_PREFERENCES_SELECT_TAB);
        event.SetInt(id);
        event.SetEventObject(this);
        wxPostEvent(this, event);
    });

    stext->Bind(wxEVT_LEFT_DOWN, [this, id](wxMouseEvent &e) {
        auto event = wxCommandEvent(EVT_PREFERENCES_SELECT_TAB);
        event.SetInt(id);
        event.SetEventObject(this);
        wxPostEvent(this, event);
    });

    Select *sel                   = new Select;
    sel->m_index                  = id;
    sel->m_tab_button             = tab_button;
    sel->m_tab_text               = stext;
    m_hash_selector[sel->m_index] = sel;

    tab_button->SetSizer(sizer);
    tab_button->Layout();
    return tab_button;
}

void KBShortcutsDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    m_logo_bmp.msw_rescale();
    m_header_bitmap->SetBitmap(m_logo_bmp.bmp());
    msw_buttons_rescale(this, em_unit(), { wxID_OK });

    Layout();
    Fit();
    Refresh();
}

void KBShortcutsDialog::fill_shortcuts()
{
    const std::string ctrl  = GUI::shortkey_ctrl_prefix();
    const std::string alt   = GUI::shortkey_alt_prefix();
    const std::string shift = L("Shift+");

    if (wxGetApp().is_editor()) {
        Shortcuts global_shortcuts = {
            // File
            { ctrl + "N", L("New Project") },
            { ctrl + "O", L("Open Project") },
            { ctrl + "S", L("Save Project") },
            { ctrl + shift + "S", L("Save Project as")},
            // File>Import
            { ctrl + "I", L("Import geometry data from STL/STEP/3MF/OBJ/AMF files") },
            // File>Export
            { ctrl + "G", L("Export plate sliced file")},
            // Slice plate
            { ctrl + "R", L("Slice plate")},
            // Send to Print
            { ctrl + shift + "G", L("Print plate")},
            // Edit
            { ctrl + "X", L("Cut") },
            { ctrl + "C", L("Copy to clipboard") },
            { ctrl + "V", L("Paste from clipboard") },
            // Configuration
            { ctrl + "P", L("Preferences") },
            //3D control
#ifdef __APPLE__
            { ctrl + shift + "M", L("Show/Hide 3Dconnexion devices settings dialog") },
#else
            { ctrl + "M", L("Show/Hide 3Dconnexion devices settings dialog") },
#endif // __APPLE

            // Switch table page
            { ctrl + L("Tab"), L("Switch table page")},
            //DEL
            #ifdef __APPLE__
                {"fn+⌫", L("Delete selected")},
            #else
                {L("Del"), L("Delete selected")},
            #endif
            // Help
            { "?", L("Show keyboard shortcuts list") }
        };
        m_full_shortcuts.push_back({{_L("Global shortcuts"), ""}, global_shortcuts});
        
        bool swap_mouse_buttons = wxGetApp().app_config->get_bool("swap_mouse_buttons");

        Shortcuts plater_shortcuts = {
            { L("Left mouse button"), swap_mouse_buttons ? L("Pan View") : L("Rotate View") },
            { L("Right mouse button"), swap_mouse_buttons ? L("Rotate View") : L("Pan View") },
            { L("Mouse wheel"), L("Zoom View") },
            { "A", L("Arrange all objects") },
            { shift + "A", L("Arrange objects on selected plates") },

            { "Q", L("Auto orients selected objects or all objects. If there are selected objects, it just orients the selected ones. Otherwise, it will orient all objects in the current project.") },
            { shift + "Q", L("Auto orients all objects on the active plate.") },

            {shift + L("Tab"), L("Collapse/Expand the sidebar")},
            {ctrl + L("Any arrow"), L("Movement in camera space")},
            {alt + L("Left mouse button"), L("Select a part")},
            {ctrl + L("Left mouse button"), L("Select multiple objects")},
            {shift + L("Left mouse button"), L("Select objects by rectangle")},
            {L("Arrow Up"), L("Move selection 10 mm in positive Y direction")},
            {L("Arrow Down"), L("Move selection 10 mm in negative Y direction")},
            {L("Arrow Left"), L("Move selection 10 mm in negative X direction")},
            {L("Arrow Right"), L("Move selection 10 mm in positive X direction")},
            {shift + L("Any arrow"), L("Movement step set to 1 mm")},
            {L("Esc"), L("Deselect all")},
            {"1-9", L("keyboard 1-9: set filament for object/part")},
            {ctrl + "0", L("Camera view - Default")},
            {ctrl + "1", L("Camera view - Top")},
            {ctrl + "2", L("Camera view - Bottom")},
            {ctrl + "3", L("Camera view - Front")},
            {ctrl + "4", L("Camera view - Behind")},
            {ctrl + "5", L("Camera Angle - Left side")},
            {ctrl + "6", L("Camera Angle - Right side")},

            {ctrl + "A", L("Select all objects")},
            {ctrl + "D", L("Delete all")},
            {ctrl + "Z", L("Undo")},
            {ctrl + "Y", L("Redo")},
            { "M", L("Gizmo move") },
            { "R", L("Gizmo rotate") },
            { "S", L("Gizmo scale") },
            { "F", L("Gizmo place face on bed") },
            { "C", L("Gizmo cut") },
            { "B", L("Gizmo mesh boolean") },
            { "H", L("Gizmo FDM paint-on fuzzy skin") },
            { "L", L("Gizmo SLA support points") },
            { "P", L("Gizmo FDM paint-on seam") },
            { "T", L("Gizmo text emboss/engrave") },
            { "U", L("Gizmo measure") },
            { "Y", L("Gizmo assemble") },
            { "E", L("Gizmo brim ears") },
            { "I", L("Zoom in") },
            { "O", L("Zoom out") },
            { L("Tab"), L("Switch between Prepare/Preview") },

        };
        m_full_shortcuts.push_back({ { _L("Plater"), "" }, plater_shortcuts });

        Shortcuts gizmos_shortcuts = {
            {L("Esc"), L("Deselect all")},
            {shift, L("Move: press to snap by 1mm")},
            {ctrl + L("Mouse wheel"), L("Support/Color Painting: adjust pen radius")},
            {alt + L("Mouse wheel"), L("Support/Color Painting: adjust section position")},
        };
        m_full_shortcuts.push_back({{_L("Gizmo"), ""}, gizmos_shortcuts});

        Shortcuts object_list_shortcuts = {
            {"1-9", L("Set extruder number for the objects and parts") },
            {L("Del"), L("Delete objects, parts, modifiers")},
            {L("Esc"), L("Deselect all")},
            {ctrl + "C", L("Copy to clipboard")},
            {ctrl + "V", L("Paste from clipboard")},
            {ctrl + "X", L("Cut")},
            {ctrl + "A", L("Select all objects")},
            {ctrl + "K", L("Clone selected")},
            {ctrl + "Z", L("Undo")},
            {ctrl + "Y", L("Redo")},
            {L("Space"), L("Select the object/part and press space to change the name")},
            {L("Mouse click"), L("Select the object/part and mouse click to change the name")},
        };
        m_full_shortcuts.push_back({ { _L("Objects List"), "" }, object_list_shortcuts });
    }

    Shortcuts preview_shortcuts = {
        { L("Arrow Up"),    L("Vertical slider - Move active thumb Up")},
        { L("Arrow Down"),  L("Vertical slider - Move active thumb Down")},
        { L("Arrow Left"),  L("Horizontal slider - Move active thumb Left")},
        { L("Arrow Right"), L("Horizontal slider - Move active thumb Right")},
        { "L", L("On/Off one layer mode of the vertical slider")},
        { "C", L("On/Off G-code window")},
        { L("Tab"), L("Switch between Prepare/Preview")},
        {shift + L("Any arrow"), L("Move slider 5x faster")},
        {shift + L("Mouse wheel"), L("Move slider 5x faster")},
        {ctrl + L("Any arrow"), L("Move slider 5x faster")},
        {ctrl + L("Mouse wheel"), L("Move slider 5x faster")},
        { L("Home"),        L("Horizontal slider - Move to start position")},
        { L("End"),         L("Horizontal slider - Move to last position")},
    };
    m_full_shortcuts.push_back({ { _L("Preview"), "" }, preview_shortcuts });
}

wxPanel* KBShortcutsDialog::create_page(wxWindow* parent, const ShortcutsItem& shortcuts, const wxFont& font, const wxFont& bold_font)
{
    wxPanel* main_page = new wxPanel(parent);
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    if (!shortcuts.first.second.empty()) {
        main_sizer->AddSpacer(FromDIP(10));
        wxBoxSizer* info_sizer = new wxBoxSizer(wxHORIZONTAL);
        info_sizer->AddStretchSpacer();
        info_sizer->Add(new wxStaticText(main_page, wxID_ANY, shortcuts.first.second), 0);
        info_sizer->AddStretchSpacer();
        main_sizer->Add(info_sizer, 0, wxEXPAND);
        main_sizer->AddSpacer(FromDIP(10));
    }

    int items_count = (int) shortcuts.second.size();
    wxScrolledWindow *scrollable_panel = new wxScrolledWindow(main_page);
    wxGetApp().UpdateDarkUI(scrollable_panel);
    scrollable_panel->SetScrollbars(20, 20, 50, 50);
    scrollable_panel->SetInitialSize(wxSize(FromDIP(850), FromDIP(450)));

    wxBoxSizer *     scrollable_panel_sizer = new wxBoxSizer(wxVERTICAL);
    wxFlexGridSizer *grid_sizer             = new wxFlexGridSizer(items_count, 2, FromDIP(10), FromDIP(20));

    for (int i = 0; i < items_count; ++i) {
        const auto &[shortcut, description] = shortcuts.second[i];
        auto key                            = new wxStaticText(scrollable_panel, wxID_ANY, _(shortcut));
        key->SetForegroundColour(wxColour(50, 58, 61));
        key->SetFont(bold_font);
        grid_sizer->Add(key, 0, wxALIGN_CENTRE_VERTICAL);

        auto desc = new wxStaticText(scrollable_panel, wxID_ANY, _(description));
        desc->SetFont(font);
        desc->SetForegroundColour(wxColour(50, 58, 61));
        desc->Wrap(FromDIP(600));
        grid_sizer->Add(desc, 0, wxALIGN_CENTRE_VERTICAL);
    }

    scrollable_panel_sizer->Add(grid_sizer, 1, wxEXPAND | wxALL, FromDIP(20));
    scrollable_panel->SetSizer(scrollable_panel_sizer);

    main_sizer->Add(scrollable_panel, 1, wxEXPAND);
    main_page->SetSizer(main_sizer);

    return main_page;
}

} // namespace GUI
} // namespace Slic3r
