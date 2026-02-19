#ifndef slic3r_GUI_PRE_PRINT_CHECK_hpp_
#define slic3r_GUI_PRE_PRINT_CHECK_hpp_

#include <set>
#include <wx/wx.h>
#include "Widgets/Label.hpp"

class Button;
class ScalableButton;

namespace Slic3r { namespace GUI {

enum prePrintInfoLevel {
    Normal,
    Warning,
    Error
};

enum prePrintInfoType {
    Printer,
    Filament
};

enum class prePrintInfoStyle : int
{
    Default = 0,
    NozzleState = 0x001,
    BtnNozzleRefresh = 0x002,
    BtnConfirmNotShowAgain = 0x004,
    BtnInstallFanF000 = 0x008,
    BtnJumpToUpgrade = 0x010,
};

inline constexpr prePrintInfoStyle operator|(prePrintInfoStyle a, prePrintInfoStyle b) noexcept
{
    return static_cast<prePrintInfoStyle>(static_cast<int>(a) | static_cast<int>(b));
}

struct prePrintInfo
{
    prePrintInfoLevel level;
    prePrintInfoType  type;
    prePrintInfoStyle m_style = prePrintInfoStyle::Default;
    wxString msg;
    wxString tips;
    wxString wiki_url;
    int index;

public:
    bool operator==(const prePrintInfo& other) const {
        return level == other.level && type == other.type &&
               msg == other.msg && tips == other.tips &&
               wiki_url == other.wiki_url && index == other.index &&
               m_style == other.m_style;
    }

    bool operator<(const prePrintInfo& other) const {
        if (level != other.level)
            return level < other.level;
        if (type != other.type)
            return type < other.type;
        if (msg != other.msg)
            return msg < other.msg;
        if (tips != other.tips)
            return tips < other.tips;
        if (wiki_url != other.wiki_url)
            return wiki_url < other.wiki_url;
        if (index != other.index)
            return index < other.index;
        return m_style < other.m_style;
    }

    bool testStyle(prePrintInfoStyle style) const {
        return (static_cast<int>(m_style) & static_cast<int>(style)) == static_cast<int>(style);
    }
};

enum PrintDialogStatus : unsigned int {

    PrintStatusErrorBegin,//->start error<-

    // Errors for printer, Block Print
    PrintStatusPrinterErrorBegin,
    PrintStatusInit,
    PrintStatusNoUserLogin,
    PrintStatusInvalidPrinter,
    PrintStatusConnectingServer,
    PrintStatusReadingTimeout,
    PrintStatusReading,
    PrintStatusConnecting,
    PrintStatusReconnecting,
    PrintStatusInUpgrading,
    PrintStatusFirmwareNotSupportTpuAtLeft,
    PrintStatusModeNotFDM,
    PrintStatusInSystemPrinting,
    PrintStatusInPrinting,
    PrintStatusNozzleMatchInvalid,
    PrintStatusNozzleNoMatchedHotends,
    PrintStatusNozzleRackMaximumInstalled,
    PrintStatusNozzleDataInvalid,
    PrintStatusNozzleDiameterMismatch,
    PrintStatusNozzleTypeMismatch,
    PrintStatusRefreshingMachineList,
    PrintStatusSending,
    PrintStatusLanModeNoSdcard,
    PrintStatusNoSdcard,
    PrintStatusLanModeSDcardNotAvailable,
    PrintStatusNeedForceUpgrading,
    PrintStatusNeedConsistencyUpgrading,
    PrintStatusNotSupportedPrintAll,
    PrintStatusBlankPlate,
    PrintStatusUnsupportedPrinter,
    PrintStatusRackReading,
    PrintStatusRackNozzleMappingWaiting,
    PrintStatusRackNozzleMappingError,
    PrintStatusInvalidMapping,
    PrintStatusPrinterErrorEnd,

    // Errors for filament, Block Print
    PrintStatusFilamentErrorBegin,
    PrintStatusAmsOnSettingup,
    PrintStatusAmsMappingInvalid,
    PrintStatusAmsMappingU0Invalid,
    PrintStatusAmsMappingMixInvalid,
    PrintStatusTPUUnsupportAutoCali,
    PrintStatusHasFilamentInBlackListError,
    PrintStatusColorQuantityExceed,
    PrintStatusFilamentErrorEnd,

    PrintStatusErrorEnd,//->end error<-


    PrintStatusWarningBegin,//->start warning<-

    // Warnings for printer
    PrintStatusPrinterWarningBegin,
    PrintStatusTimelapseNoSdcard,
    PrintStatusTimelapseWarning,
    PrintStatusMixAmsAndVtSlotWarning,
    PrintStatusToolHeadCoolingFanWarning,
    PrintStatusHasUnreliableNozzleWarning,
    PrintStatusRackNozzleNumUnmeetWarning,
    PrintStatusRackNozzleMappingWarning,
    PrintStatusNozzleHRCMismatch,
    PrintStatusPrinterWarningEnd,

    // Warnings for filament
    PrintStatusFilamentWarningBegin,
    PrintStatusWarningKvalueNotUsed,
    PrintStatusHasFilamentInBlackListWarning,
    PrintStatusFilamentWarningHighChamberTemp,
    PrintStatusFilamentWarningHighChamberTempCloseDoor,
    PrintStatusFilamentWarningHighChamberTempSoft,
    PrintStatusFilamentWarningUnknownHighChamberTempSoft,
    PrintStatusFilamentWarningEnd,

    PrintStatusWarningEnd,//->end error<-

    /*success*/
    // printer
    PrintStatusReadingFinished,
    PrintStatusSendingCanceled,
    PrintStatusReadyToGo,

    // filament
    PrintStatusAmsMappingSuccess,

    /*Other, SendToPrinterDialog*/
    PrintStatusNotOnTheSameLAN,
    PrintStatusNotSupportedSendToSDCard,
    PrintStatusPublicInitFailed,
    PrintStatusPublicUploadFiled,
};

class NozzleStatePanel;
class PrePrintChecker
{
public:
    std::vector<prePrintInfo> printerList;
    std::vector<prePrintInfo> filamentList;

public:
    void clear();
    /*auto merge*/
    void add(PrintDialogStatus state, wxString msg, wxString tip, const wxString& wiki_url, prePrintInfoStyle style);
    static ::std::string get_print_status_info(PrintDialogStatus status);

	wxString get_pre_state_msg(PrintDialogStatus status);
    static bool is_error(PrintDialogStatus status) { return (PrintStatusErrorBegin < status) && (PrintStatusErrorEnd > status); };
    static bool is_error_printer(PrintDialogStatus status) { return (PrintStatusPrinterErrorBegin < status) && (PrintStatusPrinterErrorEnd > status); };
    static bool is_error_filament(PrintDialogStatus status) { return (PrintStatusFilamentErrorBegin < status) && (PrintStatusFilamentErrorEnd > status); };
    static bool is_warning(PrintDialogStatus status) { return (PrintStatusWarningBegin < status) && (PrintStatusWarningEnd > status); };
    static bool is_warning_printer(PrintDialogStatus status) { return (PrintStatusPrinterWarningBegin < status) && (PrintStatusPrinterWarningEnd > status); };
    static bool is_warning_filament(PrintDialogStatus status) { return (PrintStatusFilamentWarningBegin < status) && (PrintStatusFilamentWarningEnd > status); };
};

class SelectMachineDialog;
class PrinterMsgPanel : public wxPanel
{
public:
    PrinterMsgPanel(wxWindow *parent, SelectMachineDialog* select_dialog);

public:
    void  Clear();
    bool  UpdateInfos(const std::vector<prePrintInfo>& infos);
    void  Rescale();

private:
    void  ClearGUI();
    void  AppendStyles(const prePrintInfo& info);
    ScalableButton* CreateTypeButton(const prePrintInfo& info);

    // events
    void OnNotShowAgain(const prePrintInfo& info);
    void OnRefreshNozzleBtnClicked(wxMouseEvent& event);
    void OnUpgradeBtnClicked(wxMouseEvent& event);

 private:
    SelectMachineDialog* m_select_dialog = nullptr;

    wxBoxSizer*  m_sizer = nullptr;
    std::vector<prePrintInfo> m_infos;
    std::vector<ScalableButton*> m_scale_btns;
    std::vector<Button*> m_ctrl_btns;

    std::set<prePrintInfo> m_not_show_again_infos;
};


}} // namespace Slic3r::GUI

#endif
