#ifndef slic3r_GUI_PRE_PRINT_CHECK_hpp_
#define slic3r_GUI_PRE_PRINT_CHECK_hpp_

#include <functional>
#include <wx/wx.h>
#include "Widgets/Label.hpp"
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

struct prePrintInfo
{
    prePrintInfoLevel level;
    prePrintInfoType  type;
    wxString msg;
    wxString tips;
    wxString wiki_url;
    wxString              link_label;      // optional: clickable text appended after msg
    std::function<void()> link_callback;   // optional: internal action for link_label click
    int index{0};

public:
    bool operator==(const prePrintInfo& other) const {
        return level == other.level && type == other.type &&
               msg == other.msg && tips == other.tips &&
               wiki_url == other.wiki_url && link_label == other.link_label &&
               index == other.index;
        // link_callback excluded: std::function is not comparable
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
    PrintStatusNozzleDataInvalid,
    PrintStatusNozzleDiameterMismatch,
    PrintStatusNozzleTypeMismatch,
    PrintStatusNozzleNoMatchedHotends,
    PrintStatusNozzleRackMaximumInstalled,
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
    PrintStatusRackNozzleMappingWaiting,
    PrintStatusRackNozzleMappingError,
    PrintStatusRackReading,
    PrintStatusFilaSwitcherError,
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
    PrintStatusTimelapseStorageLow,
    PrintStatusMixAmsAndVtSlotWarning,
    PrintStatusToolHeadCoolingFanWarning,
    PrintStatusRackNozzleMappingWarning,
    PrintStatusFilaSwitcherSlicingNotMatch,
    PrintStatusRackNozzleNumUnmeetWarning,
    PrintStatusHasUnreliableNozzleWarning,
    PrintStatusPrinterWarningEnd,

    // Warnings for filament
    PrintStatusFilamentWarningBegin,
    PrintStatusWarningKvalueNotUsed,
    PrintStatusHasFilamentInBlackListWarning,
    PrintStatusFilamentWarningHighChamberTemp,
    PrintStatusFilamentWarningHighChamberTempCloseDoor,
    PrintStatusFilamentWarningHighChamberTempSoft,
    PrintStatusFilamentWarningUnknownHighChamberTempSoft,
    PrintStatusWarningExtFilamentNotMatch,
    PrintStatusFilamentWarningNozzleHRC,
    PrintStatusFilamentCrossExtruderWarning,
    // Non-blocking advisories. FilamentWarningRemainNotEnough is declared but not wired (missing
    // device-model surface); PrintTimeEstimateWarning has no call site or message and is kept only
    // to match the reference enum table.
    PrintStatusTPUUnsupportCaliOn,
    PrintStatusTPUUnsuggestCali,
    PrintStatusSmartNozzleBlobNeedAuto,
    PrintStatusFilamentWarningRemainNotEnough,
    PrintStatusPrintTimeEstimateWarning,
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

class PrePrintChecker
{
public:
    std::vector<prePrintInfo> printerList;
    std::vector<prePrintInfo> filamentList;

public:
    void clear();
    /*auto merge*/
    void add(PrintDialogStatus state, wxString msg, wxString tip, const wxString& wiki_url);
    // Orca: minimal callback-link render path instead of the full style-bitmask machinery.
    void add_with_link(PrintDialogStatus state, wxString msg, wxString link_label, std::function<void()> link_callback);
    static ::std::string get_print_status_info(PrintDialogStatus status);

	wxString get_pre_state_msg(PrintDialogStatus status);
    static bool is_error(PrintDialogStatus status) { return (PrintStatusErrorBegin < status) && (PrintStatusErrorEnd > status); };
    static bool is_error_printer(PrintDialogStatus status) { return (PrintStatusPrinterErrorBegin < status) && (PrintStatusPrinterErrorEnd > status); };
    static bool is_error_filament(PrintDialogStatus status) { return (PrintStatusFilamentErrorBegin < status) && (PrintStatusFilamentErrorEnd > status); };
    static bool is_warning(PrintDialogStatus status) { return (PrintStatusWarningBegin < status) && (PrintStatusWarningEnd > status); };
    static bool is_warning_printer(PrintDialogStatus status) { return (PrintStatusPrinterWarningBegin < status) && (PrintStatusPrinterWarningEnd > status); };
    static bool is_warning_filament(PrintDialogStatus status) { return (PrintStatusFilamentWarningBegin < status) && (PrintStatusFilamentWarningEnd > status); };
};
//class PrePrintMsgBoard : public wxWindow
//{
//public:
//    PrePrintMsgBoard(wxWindow * parent,
//        wxWindowID      winid = wxID_ANY,
//        const wxPoint & pos   = wxDefaultPosition,
//        const wxSize &  size  = wxDefaultSize,
//        long            style = wxTAB_TRAVERSAL | wxNO_BORDER,
//        const wxString &name  = wxASCII_STR(wxPanelNameStr)
//    );
//
//public:
//    // Operations
//    void addError(const wxString &msg, const wxString &tips = wxEmptyString) { Add(msg, tips, true); };
//    void addWarning(const wxString &msg, const wxString &tips = wxEmptyString) { Add(msg, tips, false); };
//    void clear() { m_sizer->Clear(); };
//
//    // Const Access
//    bool isEmpty() const { return m_sizer->IsEmpty(); }
//
//private:
//    void add(const wxString &msg, const wxString &tips, bool is_error);
//
//private:
//    wxBoxSizer *m_sizer{nullptr};
//};



class PrinterMsgPanel : public wxPanel
{
public:
    PrinterMsgPanel(wxWindow *parent);

public:
    bool  UpdateInfos(const std::vector<prePrintInfo>& infos);

 private:
    wxBoxSizer*  m_sizer = nullptr;
    std::vector<prePrintInfo> m_infos;

};


}} // namespace Slic3r::GUI

#endif
