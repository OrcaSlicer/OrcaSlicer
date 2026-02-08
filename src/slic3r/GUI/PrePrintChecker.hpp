#ifndef slic3r_GUI_PRE_PRINT_CHECK_hpp_
#define slic3r_GUI_PRE_PRINT_CHECK_hpp_

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
    int index;

public:
    bool operator==(const prePrintInfo& other) const {
        return level == other.level && type == other.type &&
               msg == other.msg && tips == other.tips &&
               wiki_url == other.wiki_url && index == other.index;
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
    PrintStatusModeNotFDM,
    PrintStatusInSystemPrinting,
    PrintStatusInPrinting,
    PrintStatusNozzleMatchInvalid,
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

class PrePrintChecker
{
public:
    std::vector<prePrintInfo> printerList;
    std::vector<prePrintInfo> filamentList;

public:
    void clear();
    /*auto merge*/
    void add(PrintDialogStatus state, wxString msg, wxString tip, const wxString& wiki_url);
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
