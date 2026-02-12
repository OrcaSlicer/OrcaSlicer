#include "PrePrintChecker.hpp"

#include "MainFrame.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "SelectMachine.hpp"

#include "DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"
#include "slic3r/GUI/DeviceCore/DevNozzleRack.h"
#include "slic3r/GUI/DeviceCore/DevUpgrade.h"

#include <set>

namespace Slic3r { namespace GUI {

std::string PrePrintChecker::get_print_status_info(PrintDialogStatus status)
{
    switch (status)
    {
    case PrintStatusInit: return "PrintStatusInit";
    case PrintStatusNoUserLogin: return "PrintStatusNoUserLogin";
    case PrintStatusInvalidPrinter: return "PrintStatusInvalidPrinter";
    case PrintStatusConnectingServer: return "PrintStatusConnectingServer";
    case PrintStatusReadingTimeout: return "PrintStatusReadingTimeout";
    case PrintStatusReading: return "PrintStatusReading";
    case PrintStatusConnecting: return "PrintStatusConnecting";
    case PrintStatusReconnecting: return "PrintStatusReconnecting";
    case PrintStatusInUpgrading: return "PrintStatusInUpgrading";
    case PrintStatusModeNotFDM: return "PrintStatusModeNotFDM";
    case PrintStatusInSystemPrinting: return "PrintStatusInSystemPrinting";
    case PrintStatusInPrinting: return "PrintStatusInPrinting";
    case PrintStatusNozzleMatchInvalid: return "PrintStatusNozzleMatchInvalid";
    case PrintStatusNozzleNoMatchedHotends: return "PrintStatusNozzleNoMatchedHotends";
    case PrintStatusNozzleRackMaximumInstalled: return "PrintStatusNozzleRackMaximumInstalled";
    case PrintStatusNozzleDataInvalid: return "PrintStatusNozzleDataInvalid";
    case PrintStatusNozzleDiameterMismatch: return "PrintStatusNozzleDiameterMismatch";
    case PrintStatusNozzleTypeMismatch: return "PrintStatusNozzleTypeMismatch";
    case PrintStatusRefreshingMachineList: return "PrintStatusRefreshingMachineList";
    case PrintStatusSending: return "PrintStatusSending";
    case PrintStatusLanModeNoSdcard: return "PrintStatusLanModeNoSdcard";
    case PrintStatusNoSdcard: return "PrintStatusNoSdcard";
    case PrintStatusLanModeSDcardNotAvailable: return "PrintStatusLanModeSDcardNotAvailable";
    case PrintStatusNeedForceUpgrading: return "PrintStatusNeedForceUpgrading";
    case PrintStatusNeedConsistencyUpgrading: return "PrintStatusNeedConsistencyUpgrading";
    case PrintStatusNotSupportedPrintAll: return "PrintStatusNotSupportedPrintAll";
    case PrintStatusBlankPlate: return "PrintStatusBlankPlate";
    case PrintStatusUnsupportedPrinter: return "PrintStatusUnsupportedPrinter";
    case PrintStatusColorQuantityExceed: return "PrintStatusColorQuantityExceed";
    // Handle filament errors
    case PrintStatusRackReading: return "PrintStatusRackReading";
    case PrintStatusRackNozzleMappingWaiting: return "PrintStatusRackNozzleMappingWaiting";
    case PrintStatusRackNozzleMappingError: return "PrintStatusRackNozzleMappingError";
    case PrintStatusInvalidMapping: return "PrintStatusInvalidMapping";
    case PrintStatusAmsOnSettingup: return "PrintStatusAmsOnSettingup";
    case PrintStatusAmsMappingInvalid: return "PrintStatusAmsMappingInvalid";
    case PrintStatusAmsMappingU0Invalid: return "PrintStatusAmsMappingU0Invalid";
    case PrintStatusAmsMappingMixInvalid: return "PrintStatusAmsMappingMixInvalid";
    case PrintStatusTPUUnsupportAutoCali: return "PrintStatusTPUUnsupportAutoCali";
    case PrintStatusHasFilamentInBlackListError: return "PrintStatusHasFilamentInBlackListError";
    case PrintStatusTimelapseNoSdcard: return "PrintStatusTimelapseNoSdcard";
    case PrintStatusTimelapseWarning: return "PrintStatusTimelapseWarning";
    case PrintStatusMixAmsAndVtSlotWarning: return "PrintStatusMixAmsAndVtSlotWarning";
    case PrintStatusToolHeadCoolingFanWarning: return "PrintStatusToolHeadCoolingFanWarning";
    case PrintStatusHasUnreliableNozzleWarning: return "PrintStatusRackHasUnreliableNozzleWarning";
    case PrintStatusRackNozzleNumUnmeetWarning: return "PrintStatusRackNozzleNumUnmeetWarning";
    case PrintStatusRackNozzleMappingWarning: return "PrintStatusRackNozzleMappingWarning";
    case PrintStatusWarningKvalueNotUsed: return "PrintStatusWarningKvalueNotUsed";
    case PrintStatusHasFilamentInBlackListWarning: return "PrintStatusHasFilamentInBlackListWarning";
    case PrintStatusFilamentWarningHighChamberTemp: return "PrintStatusFilamentWarningHighChamberTemp";
    case PrintStatusFilamentWarningHighChamberTempCloseDoor: return "PrintStatusFilamentWarningHighChamberTempCloseDoor";
    case PrintStatusFilamentWarningHighChamberTempSoft: return "PrintStatusFilamentWarningHighChamberTempSoft";
    case PrintStatusFilamentWarningUnknownHighChamberTempSoft: return "PrintStatusFilamentWarningUnknownHighChamberTempSoft";
    case PrintStatusReadingFinished: return "PrintStatusReadingFinished";
    case PrintStatusSendingCanceled: return "PrintStatusSendingCanceled";
    case PrintStatusAmsMappingSuccess: return "PrintStatusAmsMappingSuccess";
    case PrintStatusReadyToGo: return "PrintStatusReadyToGo";
    case PrintStatusNotOnTheSameLAN: return "PrintStatusNotOnTheSameLAN";
    case PrintStatusNotSupportedSendToSDCard: return "PrintStatusNotSupportedSendToSDCard";
    case PrintStatusPublicInitFailed: return "PrintStatusPublicInitFailed";
    case PrintStatusPublicUploadFiled: return "PrintStatusPublicUploadFiled";
    default: return "Unknown status";
    }
}

wxString PrePrintChecker::get_pre_state_msg(PrintDialogStatus status)
{
    switch (status) {
    case PrintStatusNoUserLogin: return _L("No login account, only printers in LAN mode are displayed");
    case PrintStatusConnectingServer: return _L("Connecting to server");
    case PrintStatusReading: return _L("Synchronizing device information");
    case PrintStatusReadingTimeout: return _L("Synchronizing device information time out");
    case PrintStatusModeNotFDM: return _L("Cannot send the print job when the printer is not at FDM mode");
    case PrintStatusInUpgrading: return _L("Cannot send the print job when the printer is updating firmware");
    case PrintStatusInSystemPrinting: return _L("The printer is executing instructions. Please restart printing after it ends");
    case PrintStatusInPrinting: return _L("The printer is busy on other print job");
    case PrintStatusAmsOnSettingup: return _L("AMS is setting up. Please try again later.");
    case PrintStatusAmsMappingInvalid: return _L("Not all filaments used in slicing are mapped to the printer. Please check the mapping of filaments.");
    case PrintStatusAmsMappingMixInvalid: return _L("Please do not mix-use the Ext with AMS.");
    case PrintStatusNozzleDataInvalid: return _L("Invalid nozzle information, please refresh or manually set nozzle information.");
    case PrintStatusLanModeNoSdcard: return _L("Storage needs to be inserted before printing via LAN.");
    case PrintStatusLanModeSDcardNotAvailable: return _L("Storage is not available or is in read-only mode.");
    case PrintStatusNoSdcard: return _L("Storage needs to be inserted before printing.");
    case PrintStatusNeedForceUpgrading: return _L("Cannot send the print job to a printer whose firmware is required to get updated.");
    case PrintStatusNeedConsistencyUpgrading: return _L("Cannot send the print job to a printer whose firmware is required to get updated.");
    case PrintStatusBlankPlate: return _L("Cannot send the print job for empty plate");
    case PrintStatusTimelapseNoSdcard: return _L("Storage needs to be inserted to record timelapse.");
    case PrintStatusMixAmsAndVtSlotWarning: return _L("You have selected both external and AMS filaments for an extruder. You will need to manually switch the external filament during printing.");
    case PrintStatusTPUUnsupportAutoCali: return _L("TPU 90A/TPU 85A is too soft and does not support automatic Flow Dynamics calibration.");
    case PrintStatusWarningKvalueNotUsed: return _L("Set dynamic flow calibration to 'OFF' to enable custom dynamic flow value.");
    case PrintStatusNotSupportedPrintAll: return _L("This printer does not support printing all plates.");
    case PrintStatusColorQuantityExceed: return _L("The current firmware supports a maximum of 16 materials. You can either reduce the number of materials to 16 or fewer on the Preparation Page, or try updating the firmware. If you are still restricted after the update, please wait for subsequent firmware support.");
    case PrintStatusHasUnreliableNozzleWarning: return _L("Please check if the required nozzle diameter and flow rate match the current display.");
    }
    return wxEmptyString;
}

void PrePrintChecker::clear()
{
    printerList.clear();
    filamentList.clear();
}

void PrePrintChecker::add(PrintDialogStatus state, wxString msg, wxString tip, const wxString& wiki_url, prePrintInfoStyle style)
{
    prePrintInfo info;

    if (is_error(state)) {
        info.level = prePrintInfoLevel::Error;
    } else if (is_warning(state)) {
        info.level = prePrintInfoLevel::Warning;
    } else {
        info.level = prePrintInfoLevel::Normal;
    }

    if (is_error_printer(state)) {
        info.type = prePrintInfoType::Printer;
    } else if (is_error_filament(state)) {
        info.type = prePrintInfoType::Filament;
    } else if (is_warning_printer(state)) {
        info.type = prePrintInfoType::Printer;
    } else if (is_warning_filament(state)) {
        info.type = prePrintInfoType::Filament;
    }

    if (!msg.IsEmpty()) {
        info.msg  = msg;
        info.tips = tip;
    } else {
        info.msg  = get_pre_state_msg(state);
        info.tips = wxEmptyString;
    }

    info.wiki_url = wiki_url;
    info.m_style = style;

    switch (info.type) {
    case prePrintInfoType::Filament:
        if (std::find(filamentList.begin(), filamentList.end(), info) == filamentList.end()) {
            filamentList.push_back(info);
        }
        break;
    case prePrintInfoType::Printer:
        if (std::find(printerList.begin(), printerList.end(), info) == printerList.end()) {
            printerList.push_back(info);
        }
        break;
    default: break;
    }
}


//void PrePrintMsgBoard::add(const wxString &msg, const wxString &tips, bool is_error)
//{
//    if (msg.IsEmpty()) { return; }
//
//    /*message*/
//    // create label
//    if (!m_sizer->IsEmpty()) { m_sizer->AddSpacer(FromDIP(10)); }
//    Label *msg_label = new Label(this, wxEmptyString);
//    m_sizer->Add(msg_label, 0, wxLEFT, 0);
//
//    // set message
//    msg_label->SetLabel(msg);
//    msg_label->SetMinSize(wxSize(FromDIP(420), -1));
//    msg_label->SetMaxSize(wxSize(FromDIP(420), -1));
//    msg_label->Wrap(FromDIP(420));
//
//    // font color
//    auto colour = is_error ? wxColour("#D01B1B") : wxColour(0xFF, 0x6F, 0x00);
//    msg_label->SetForegroundColour(colour);
//
//    /*tips*/
//    if (!tips.IsEmpty()) { /*Not supported yet*/
//    }
//
//    Layout();
//    Fit();
//}

PrinterMsgPanel::PrinterMsgPanel(wxWindow *parent, SelectMachineDialog* select_dialog)
    : wxPanel(parent), m_select_dialog(select_dialog)
{
    m_sizer = new wxBoxSizer(wxVERTICAL);
    this->SetSizer(m_sizer);
}

void PrinterMsgPanel::Clear()
{
    m_infos.clear();
    m_not_show_again_infos.clear();
    ClearGUI();
}

void PrinterMsgPanel::ClearGUI()
{
    m_sizer->Clear(true);
    m_scale_btns.clear();
    m_ctrl_btns.clear();
}

static wxColour _GetLabelColour(const prePrintInfo& info)
{
    if (info.level == Error)
    {
        return wxColour("#D01B1B");
    }
    else if (info.level == Warning)
    {
        return wxColour("#FF6F00");
    }

    return *wxBLACK; // Default colour for normal messages
}

bool PrinterMsgPanel::UpdateInfos(const std::vector<prePrintInfo>& infos)
{
    if (m_infos == infos)
    {
        return false;
    }
    m_infos = infos;

    m_sizer->Clear(true);
    for (const prePrintInfo& info : infos)
    {
        if (m_not_show_again_infos.count(info) != 0) {
            continue;
        };

        if (!info.msg.empty())
        {
            Label* label = new Label(this);
            label->SetFont(::Label::Body_13);
            label->SetForegroundColour(_GetLabelColour(info));


            if (info.wiki_url.empty())
            {
                label->SetLabel(info.msg);
            }
            else
            {
                label->SetLabel(info.msg + " " + _L("Please refer to Wiki before use->"));
                label->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) { SetCursor(wxCURSOR_HAND); });
                label->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) { SetCursor(wxCURSOR_ARROW); });
                label->Bind(wxEVT_LEFT_DOWN, [info](wxMouseEvent& event) { wxLaunchDefaultBrowser(info.wiki_url); });
            }

            label->Wrap(this->GetMinSize().GetWidth());
            label->Show();
            m_sizer->Add(label, 0, wxBOTTOM, FromDIP(4));
        }
    }

    this->Show();
    this->Layout();

    Fit();

    return true;
}

void PrinterMsgPanel::Rescale()
{
    for (auto item : m_scale_btns) {
        item->msw_rescale();
    }

    for (auto item : m_ctrl_btns) {
        item->Rescale();
    }

    Layout();
    Fit();
}

ScalableButton* PrinterMsgPanel::CreateTypeButton(const prePrintInfo& info)
{
    ScalableButton* btn = nullptr;
    if (info.level == Error)         {
        btn = new ScalableButton(this, wxID_ANY, "dev_error");
    } else if (info.level == Warning) {
        btn = new ScalableButton(this, wxID_ANY, "dev_warning");
    } else {
        btn = new ScalableButton(this, wxID_ANY, "dev_warning");
    }

    btn->SetBackgroundColour(*wxWHITE);
    btn->SetMaxSize(wxSize(FromDIP(16), FromDIP(16)));
    btn->SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
    btn->SetSize(wxSize(FromDIP(16), FromDIP(16)));
    wxGetApp().UpdateDarkUI(btn);
    m_scale_btns.push_back(btn);
    return btn;
};

static Label* s_create_btn_label(PrinterMsgPanel* panel, const wxString& btn_name)
{
    Label* btn = new Label(panel, btn_name);
    btn->SetFont(Label::Body_13);
    auto font = btn->GetFont();
    font.SetUnderlined(true);
    btn->SetFont(font);
    btn->SetBackgroundColour(*wxWHITE);
    btn->SetForegroundColour(wxColour("#00AE42"));

    btn->Bind(wxEVT_ENTER_WINDOW, [panel](auto &e) { panel->SetCursor(wxCURSOR_HAND); });
    btn->Bind(wxEVT_LEAVE_WINDOW, [panel](auto &e) { panel->SetCursor(wxCURSOR_ARROW); });

    wxGetApp().UpdateDarkUI(btn);
    return btn;
}

void PrinterMsgPanel::AppendStyles(const prePrintInfo& info)
{
    // special styles
    if (info.testStyle(prePrintInfoStyle::BtnNozzleRefresh) ||
        info.testStyle(prePrintInfoStyle::BtnConfirmNotShowAgain) ||
        info.testStyle(prePrintInfoStyle::BtnInstallFanF000) ||
        info.testStyle(prePrintInfoStyle::BtnJumpToUpgrade)) {
        wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
        if (info.testStyle(prePrintInfoStyle::BtnNozzleRefresh)){
            auto btn = s_create_btn_label(this, _L("Refresh"));
            btn->Bind(wxEVT_LEFT_DOWN, &PrinterMsgPanel::OnRefreshNozzleBtnClicked, this);
            btn_sizer->Add(btn, 0, wxLEFT, FromDIP(16));
        }

        if (info.testStyle(prePrintInfoStyle::BtnConfirmNotShowAgain)) {
            auto btn = s_create_btn_label(this, _L("Confirm"));
            btn->Bind(wxEVT_LEFT_DOWN, [this, info](auto& e) {
                this->OnNotShowAgain(info);
            });

            btn_sizer->Add(btn, 0, wxLEFT, FromDIP(16));
        }

        if (info.testStyle(prePrintInfoStyle::BtnInstallFanF000)) {
            auto btn = s_create_btn_label(this, _L("How to install"));
            btn->Bind(wxEVT_LEFT_DOWN, [this, info](auto& e) {
                wxLaunchDefaultBrowser("https://e.bambulab.com/t?c=l3T7caKGeNt3omA9");
            });

            btn_sizer->Add(btn, 0, wxLEFT, FromDIP(16));
        }

        if (info.testStyle(prePrintInfoStyle::BtnJumpToUpgrade)) {
            auto btn = s_create_btn_label(this, _L("Upgrade"));
            btn->Bind(wxEVT_LEFT_DOWN, &PrinterMsgPanel::OnUpgradeBtnClicked, this);
            btn_sizer->Add(btn, 0, wxLEFT, FromDIP(16));
        }

        m_sizer->Add(btn_sizer, 0, wxLEFT);
        m_sizer->AddSpacer(FromDIP(4));
    }

    if (info.testStyle(prePrintInfoStyle::NozzleState)) {
        NozzleStatePanel* nozzle_info = new NozzleStatePanel(this);
        nozzle_info->UpdateInfoBy(m_select_dialog->get_plater(), m_select_dialog->get_current_machine());
        m_sizer->Add(nozzle_info, 0, wxLEFT, FromDIP(16));
        m_sizer->AddSpacer(FromDIP(4));
    }
}

void PrinterMsgPanel::OnRefreshNozzleBtnClicked(wxMouseEvent& event)
{
    auto obj_ = m_select_dialog->get_current_machine();
    if (obj_) {
        obj_->GetNozzleSystem()->GetNozzleRack()->CtrlRackReadAll(true);
    }
}

void PrinterMsgPanel::OnNotShowAgain(const prePrintInfo& info)
{
    m_not_show_again_infos.insert(info);

    auto cp_infos = m_infos;
    m_infos.clear();
    UpdateInfos(cp_infos);
}

void PrinterMsgPanel::OnUpgradeBtnClicked(wxMouseEvent& event)
{
    auto obj_ = m_select_dialog ? m_select_dialog->get_current_machine() : nullptr;
    if (!obj_) {
        return;
    }

    m_select_dialog->Hide();
    if (Slic3r::GUI::wxGetApp().mainframe && Slic3r::GUI::wxGetApp().mainframe->m_monitor) {
        Slic3r::GUI::wxGetApp().mainframe->jump_to_monitor();
        Slic3r::GUI::wxGetApp().mainframe->m_monitor->jump_to_Upgrade();
    }
}

}
};


