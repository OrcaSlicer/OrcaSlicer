#include "OrcaFilesPanel.h"

#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "ImageGrid.h"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/StateColor.hpp"

#include <boost/make_shared.hpp>
#include <wx/colour.h>
#include <wx/sizer.h>
#include <utility>

namespace Slic3r {
namespace GUI {

OrcaFilesPanel::OrcaFilesPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
    , m_bmp_loading(this, "media_loading", 0)
    , m_bmp_failed(this, "media_failed", 0)
    , m_bmp_empty(this, "media_empty", 0)
{
    const wxColour light_background("#EEEEEE");
    SetBackgroundColour(wxGetApp().dark_mode() ? StateColor::darkModeColorFor(light_background) : light_background);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* toolbar = new wxBoxSizer(wxHORIZONTAL);
    toolbar->SetMinSize({-1, 75 * em_unit(this) / 10});

    m_root_label = new Label(this, _L("gcodes"));
    m_root_label->SetFont(Label::Head_16);
    toolbar->Add(m_root_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 24);

    m_time_panel = new ::StaticBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_time_panel->SetCornerRadius(0);
    m_button_year = new ::Button(m_time_panel, _L("Year"), "", wxBORDER_NONE);
    m_button_month = new ::Button(m_time_panel, _L("Month"), "", wxBORDER_NONE);
    m_button_all = new ::Button(m_time_panel, _L("All Files"), "", wxBORDER_NONE);
    m_button_year->SetToolTip(_L("Group files by year, recent first."));
    m_button_month->SetToolTip(_L("Group files by month, recent first."));
    m_button_all->SetToolTip(_L("Show all files, recent first."));
    for (auto* button : {m_button_year, m_button_month, m_button_all}) {
        button->SetBackgroundColor(StateColor(
            std::make_pair(0xEEEEEE, static_cast<int>(StateColor::Checked)),
            std::make_pair(*wxLIGHT_GREY, static_cast<int>(StateColor::Hovered)),
            std::make_pair(*wxWHITE, static_cast<int>(StateColor::Normal))));
        button->SetTextColor(StateColor(
            std::make_pair(0x3B4446, static_cast<int>(StateColor::Checked)),
            std::make_pair(*wxLIGHT_GREY, static_cast<int>(StateColor::Hovered)),
            std::make_pair(0xABACAC, static_cast<int>(StateColor::Normal))));
        button->SetCanFocus(false);
    }
    auto* time_sizer = new wxBoxSizer(wxHORIZONTAL);
    time_sizer->Add(m_button_year, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 24);
    time_sizer->Add(m_button_month, 0, wxALIGN_CENTER_VERTICAL);
    time_sizer->Add(m_button_all, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 24);
    m_time_panel->SetSizer(time_sizer);
    toolbar->AddStretchSpacer(1);
    toolbar->Add(m_time_panel, 0, wxALIGN_CENTER_VERTICAL);
    toolbar->AddStretchSpacer(1);

    m_button_refresh = new ::Button(this, _L("Refresh"));
    m_button_refresh->SetToolTip(_L("Reload file list from printer."));
    m_button_refresh->SetFont(Label::Body_12);
    m_button_refresh->SetCornerRadius(12);
    m_button_refresh->SetPaddingSize({10, 6});
    m_button_refresh->SetCanFocus(false);
    m_button_refresh->SetBorderWidth(0);
    toolbar->Add(m_button_refresh, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 24);
    sizer->Add(toolbar, 0, wxEXPAND);

    m_image_grid = new ImageGrid(this);
    // Theme this grid instance without changing the shared Bambu defaults.
    m_image_grid->SetBackgroundColour(GetBackgroundColour());
    m_image_grid->SetSelecting(false);
    m_image_grid->ShowDownload(false);
    m_image_grid->SetStatus(m_bmp_failed, _L("Please confirm if the printer is connected."));
    sizer->Add(m_image_grid, 1, wxEXPAND);
    SetSizer(sizer);
    applyThemeColors();
    updateGroupButtons(m_group_mode);

    auto group_clicked = [this](wxCommandEvent& event) {
        const FileGridGroup mode = event.GetEventObject() == m_button_year ? FileGridGroup::Year :
                                   event.GetEventObject() == m_button_month ? FileGridGroup::Month : FileGridGroup::All;
        m_image_grid->SetGroupMode(static_cast<int>(mode));
        updateGroupButtons(mode);
    };
    m_button_year->Bind(wxEVT_COMMAND_BUTTON_CLICKED, group_clicked);
    m_button_month->Bind(wxEVT_COMMAND_BUTTON_CLICKED, group_clicked);
    m_button_all->Bind(wxEVT_COMMAND_BUTTON_CLICKED, group_clicked);

    m_button_refresh->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) {
        event.Skip();
        if (m_model)
            m_model->Refresh();
    });

    m_image_grid->Bind(EVT_ITEM_ACTION, [this](wxCommandEvent& event) {
        if (event.GetInt() != 0)   // only Delete is wired for this grid
            return;
        if (!m_model || m_device_id.empty())
            return;

        const size_t index = static_cast<size_t>(event.GetExtraLong());
        if (index >= m_model->GetCount())
            return;
        const FileGridCard& card = m_model->GetFile(index);
        if (card.id.empty())
            return;
        const std::string path = card.id;
        const std::string name = card.name;

        MessageDialog dlg(this,
            wxString::Format(_L("Do you want to delete the file '%s' from printer?"), from_u8(name)),
            _L("Delete file"), wxYES_NO | wxICON_WARNING);
        if (dlg.ShowModal() != wxID_YES)
            return;

        m_model->DeleteFile(path, [this](bool ok) {
            if (ok)
                return;
            MessageDialog(this, _L("Failed to delete the file from printer."), _L("Delete file"),
                          wxOK | wxICON_ERROR).ShowModal();
        });
    });
}

OrcaFilesPanel::~OrcaFilesPanel()
{
    if (m_model)
        m_model->SetStatusHandler({});
    if (m_model)
        m_model->SetGroupModeHandler({});
    m_image_grid->SetModel(nullptr);
    m_model.reset();
}

void OrcaFilesPanel::UpdateByObj(MachineObject* obj)
{
    const std::string device_id = obj ? obj->get_dev_id() : std::string();
    if (device_id == m_device_id)
        return;

    m_device_id = device_id;
    if (m_model)
        m_model->SetStatusHandler({});
    if (m_model)
        m_model->SetGroupModeHandler({});
    m_image_grid->SetModel(nullptr);
    m_model.reset();

    if (m_device_id.empty()) {
        m_button_refresh->Enable(false);
        m_image_grid->SetStatus(m_bmp_failed, _L("Please confirm if the printer is connected."));
        return;
    }

    m_model = boost::make_shared<RemoteFileGridModel>(m_device_id);
    m_model->SetStatusHandler([this](RemoteFileGridModel::Status status) { updateStatus(status); });
    m_model->SetGroupModeHandler([this](FileGridGroup mode) { updateGroupButtons(mode); });
    m_model->SetGroupMode(m_group_mode);
    m_image_grid->SetModel(m_model);
    m_image_grid->SetFileType(static_cast<int>(FileGridType::Model), {});
    m_button_refresh->Enable(true);
    m_model->Refresh();
}

void OrcaFilesPanel::updateStatus(RemoteFileGridModel::Status status)
{
    switch (status) {
    case RemoteFileGridModel::Status::Loading:
        m_image_grid->SetStatus(m_bmp_loading, _L("Loading file list..."));
        break;
    case RemoteFileGridModel::Status::Ready:
        break;
    case RemoteFileGridModel::Status::Empty:
        m_image_grid->SetStatus(m_bmp_empty, _L("No files"));
        break;
    case RemoteFileGridModel::Status::Failed:
        m_image_grid->SetStatus(m_bmp_failed, _L("Load failed"));
        break;
    }
}

void OrcaFilesPanel::Rescale()
{
    m_bmp_loading.msw_rescale();
    m_bmp_failed.msw_rescale();
    m_bmp_empty.msw_rescale();

    auto* toolbar = GetSizer()->GetItem(static_cast<size_t>(0))->GetSizer();
    toolbar->SetMinSize({-1, 75 * em_unit(this) / 10});
    m_root_label->SetFont(Label::Head_16);
    m_button_refresh->Rescale();
    m_button_year->Rescale();
    m_button_month->Rescale();
    m_button_all->Rescale();
    m_image_grid->Rescale();
}

void OrcaFilesPanel::on_sys_color_changed()
{
    applyThemeColors();
    Layout();
    Refresh();
}

void OrcaFilesPanel::applyThemeColors()
{
    const bool dark = wxGetApp().dark_mode();
    const wxColour light_background("#EEEEEE");
    const wxColour light_text("#262E30");
    const wxColour light_teal("#009688");
    const wxColour background = dark ? StateColor::darkModeColorFor(light_background) : light_background;

    SetBackgroundColour(background);
    m_root_label->SetBackgroundColour(background);
    m_root_label->SetForegroundColour(dark ? StateColor::darkModeColorFor(light_text) : light_text);
    m_time_panel->SetBackgroundColour(background);
    m_button_refresh->SetBackgroundColorNormal(dark ? StateColor::darkModeColorFor(light_teal) : light_teal);
    m_button_refresh->SetTextColorNormal(*wxWHITE);
    m_image_grid->SetBackgroundColour(background);
    m_image_grid->Refresh();
    m_button_year->Refresh();
    m_button_month->Refresh();
    m_button_all->Refresh();
}

void OrcaFilesPanel::updateGroupButtons(FileGridGroup mode)
{
    m_group_mode = mode;
    ::Button* buttons[] = {m_button_all, m_button_month, m_button_year};
    const int selected = static_cast<int>(mode);
    for (int i = 0; i < 3; ++i) {
        buttons[i]->SetValue(i == selected);
        buttons[i]->SetFont(i == selected ? Label::Head_14 : Label::Body_14);
    }
}

}}
