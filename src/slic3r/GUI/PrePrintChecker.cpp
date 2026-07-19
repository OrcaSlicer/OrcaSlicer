#include "PrePrintChecker.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
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
    case PrintStatusRackNozzleMappingWaiting: return "PrintStatusRackNozzleMappingWaiting";
    case PrintStatusRackNozzleMappingError: return "PrintStatusRackNozzleMappingError";
    case PrintStatusFilaSwitcherError: return "PrintStatusFilaSwitcherError";
    case PrintStatusColorQuantityExceed: return "PrintStatusColorQuantityExceed";
    // Handle filament errors
    case PrintStatusAmsOnSettingup: return "PrintStatusAmsOnSettingup";
    case PrintStatusAmsMappingInvalid: return "PrintStatusAmsMappingInvalid";
    case PrintStatusAmsMappingU0Invalid: return "PrintStatusAmsMappingU0Invalid";
    case PrintStatusAmsMappingMixInvalid: return "PrintStatusAmsMappingMixInvalid";
    case PrintStatusTPUUnsupportAutoCali: return "PrintStatusTPUUnsupportAutoCali";
    case PrintStatusHasFilamentInBlackListError: return "PrintStatusHasFilamentInBlackListError";
    case PrintStatusTimelapseNoSdcard: return "PrintStatusTimelapseNoSdcard";
    case PrintStatusTimelapseWarning: return "PrintStatusTimelapseWarning";
    case PrintStatusMixAmsAndVtSlotWarning: return "PrintStatusMixAmsAndVtSlotWarning";
    case PrintStatusRackNozzleMappingWarning: return "PrintStatusRackNozzleMappingWarning";
    case PrintStatusFilaSwitcherSlicingNotMatch: return "PrintStatusFilaSwitcherSlicingNotMatch";
    case PrintStatusWarningKvalueNotUsed: return "PrintStatusWarningKvalueNotUsed";
    case PrintStatusHasFilamentInBlackListWarning: return "PrintStatusHasFilamentInBlackListWarning";
    case PrintStatusFilamentWarningHighChamberTemp: return "PrintStatusFilamentWarningHighChamberTemp";
    case PrintStatusFilamentWarningHighChamberTempCloseDoor: return "PrintStatusFilamentWarningHighChamberTempCloseDoor";
    case PrintStatusFilamentWarningHighChamberTempSoft: return "PrintStatusFilamentWarningHighChamberTempSoft";
    case PrintStatusFilamentWarningUnknownHighChamberTempSoft: return "PrintStatusFilamentWarningUnknownHighChamberTempSoft";
    case PrintStatusNozzleNoMatchedHotends: return "PrintStatusNozzleNoMatchedHotends";
    case PrintStatusNozzleRackMaximumInstalled: return "PrintStatusNozzleRackMaximumInstalled";
    case PrintStatusRackReading: return "PrintStatusRackReading";
    case PrintStatusRackNozzleNumUnmeetWarning: return "PrintStatusRackNozzleNumUnmeetWarning";
    case PrintStatusHasUnreliableNozzleWarning: return "PrintStatusHasUnreliableNozzleWarning";
    case PrintStatusWarningExtFilamentNotMatch: return "PrintStatusWarningExtFilamentNotMatch";
    case PrintStatusFilamentWarningNozzleHRC: return "PrintStatusFilamentWarningNozzleHRC";
    case PrintStatusTPUUnsupportCaliOn: return "PrintStatusTPUUnsupportCaliOn";
    case PrintStatusTPUUnsuggestCali: return "PrintStatusTPUUnsuggestCali";
    case PrintStatusSmartNozzleBlobNeedAuto: return "PrintStatusSmartNozzleBlobNeedAuto";
    case PrintStatusFilamentWarningRemainNotEnough: return "PrintStatusFilamentWarningRemainNotEnough";
    case PrintStatusPrintTimeEstimateWarning: return "PrintStatusPrintTimeEstimateWarning";
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
    case PrintStatusNoUserLogin: return _L("No login account, only printers in LAN mode are displayed.");
    case PrintStatusConnectingServer: return _L("Connecting to server...");
    case PrintStatusReading: return _L("Synchronizing device information...");
    case PrintStatusReadingTimeout: return _L("Synchronizing device information timed out.");
    case PrintStatusModeNotFDM: return _L("Cannot send a print job when the printer is not at FDM mode.");
    case PrintStatusInUpgrading: return _L("Cannot send a print job while the printer is updating firmware.");
    case PrintStatusInSystemPrinting: return _L("The printer is executing instructions. Please restart printing after it ends.");
    case PrintStatusInPrinting: return _L("The printer is busy with another print job.");
    case PrintStatusAmsOnSettingup: return _L("AMS is setting up. Please try again later.");
    case PrintStatusAmsMappingInvalid: return _L("Not all filaments used in slicing are mapped to the printer. Please check the mapping of filaments.");
    case PrintStatusAmsMappingMixInvalid: return _L("Please do not mix-use the Ext with AMS.");
    case PrintStatusNozzleDataInvalid: return _L("Invalid nozzle information, please refresh or manually set nozzle information.");
    case PrintStatusLanModeNoSdcard: return _L("Storage needs to be inserted before printing via LAN.");
    case PrintStatusLanModeSDcardNotAvailable: return _L("Storage is in abnormal state or is in read-only mode.");
    case PrintStatusNoSdcard: return _L("Storage needs to be inserted before printing.");
    case PrintStatusNeedForceUpgrading: return _L("Cannot send the print job to a printer whose firmware must be updated.");
    case PrintStatusNeedConsistencyUpgrading: return _L("Cannot send the print job to a printer whose firmware must be updated.");
    case PrintStatusBlankPlate: return _L("Cannot send a print job for an empty plate.");
    case PrintStatusTimelapseNoSdcard: return _L("Storage needs to be inserted to record timelapse.");
    case PrintStatusMixAmsAndVtSlotWarning: return _L("You have selected both external and AMS filaments for an extruder. You will need to manually switch the external filament during printing.");
    case PrintStatusTPUUnsupportAutoCali: return _L("TPU 90A/TPU 85A is too soft and does not support automatic Flow Dynamics calibration.");
    case PrintStatusWarningKvalueNotUsed: return _L("Set dynamic flow calibration to 'OFF' to enable custom dynamic flow value.");
    case PrintStatusNotSupportedPrintAll: return _L("This printer does not support printing all plates.");
    // Orca: %s-parameterized so the real per-printer max color count can be filled in by the caller
    // (SelectMachine formats it via get_pre_state_msg before add()); default templates keep the wording.
    case PrintStatusColorQuantityExceed: return _L("The current firmware supports a maximum of %s materials. You can either reduce the number of materials to %s or fewer on the Preparation Page, or try updating the firmware. If you are still restricted after the update, please wait for subsequent firmware support.");
    case PrintStatusWarningExtFilamentNotMatch: return _L("The type of external filament is unknown or does not match with the filament type in the slicing file. Please make sure you have installed the correct filament in the external spool.");
    // Same text as the blocking "auto" case (PrintStatusTPUUnsupportAutoCali); CaliOn is a
    // non-blocking advisory (Send stays enabled).
    case PrintStatusTPUUnsupportCaliOn: return _L("TPU 90A/TPU 85A are too soft. It is recommended to perform manual flow calibration on the 'Calibration' page. If 'Dynamic Flow Calibration' is set to auto/on, the system will use the previous calibration value and skip the flow calibration process.");
    case PrintStatusFilamentWarningRemainNotEnough: return _L("The filament in the AMS may be insufficient for this print. Please refill or replace it.");
    // SmartNozzleBlobNeedAuto text is passed via add_with_link (clickable "Switch"), not here.
    // PrintTimeEstimateWarning has no call site and no message string.
    }
    return wxEmptyString;
}

void PrePrintChecker::clear()
{
    printerList.clear();
    filamentList.clear();
}

void PrePrintChecker::add(PrintDialogStatus state, wxString msg, wxString tip, const wxString& wiki_url)
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

// Orca: minimal callback-link variant of add(): stores an internal click action
// (link_callback) rendered as a trailing link instead of the wiki-url launcher.
void PrePrintChecker::add_with_link(PrintDialogStatus state, wxString msg, wxString link_label, std::function<void()> link_callback)
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

    info.msg           = msg;
    info.link_label    = link_label;
    info.link_callback = link_callback;

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

PrinterMsgPanel::PrinterMsgPanel(wxWindow *parent)
    : wxPanel(parent)
{
    m_sizer = new wxBoxSizer(wxVERTICAL);
    this->SetSizer(m_sizer);
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
        if (!info.msg.empty())
        {
            Label* label = new Label(this);
            label->SetFont(::Label::Body_13);
            label->SetForegroundColour(_GetLabelColour(info));


            if (!info.link_label.empty())
            {
                // Orca: internal callback link (e.g. "Clean up files") instead of a wiki url
                label->SetLabel(info.msg + " " + info.link_label);
                label->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) { SetCursor(wxCURSOR_HAND); });
                label->Bind(wxEVT_LEAVE_WINDOW, [this](auto& e) { SetCursor(wxCURSOR_ARROW); });
                label->Bind(wxEVT_LEFT_DOWN, [info](wxMouseEvent& event) { if (info.link_callback) info.link_callback(); });
            }
            else if (info.wiki_url.empty())
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


}
};


