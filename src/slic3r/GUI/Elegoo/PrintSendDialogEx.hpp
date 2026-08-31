#pragma once

#include <boost/filesystem/path.hpp>
#include <wx/dialog.h>
#include <wx/webview.h>
#include <wx/colour.h>
#include <wx/string.h>
#include <wx/event.h>

#include <nlohmann/json.hpp>
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PrinterNetworkInfo.hpp"
#include "slic3r/Utils/WebviewIPCManager.h"

#if wxUSE_WEBVIEW_IE
#include "wx/msw/webview_ie.h"
#endif
#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

namespace webviewIpc {
    class WebviewIPCManager;
}

namespace Slic3r { namespace GUI {

// Elegoo-style host type keys (match ElegooSlicer for AppConfig persistence)
#ifndef CONFIG_KEY_UPLOADANDPRINT
#define CONFIG_KEY_UPLOADANDPRINT      "printsend_upload_and_print"
#define CONFIG_KEY_TIMELAPSE           "printsend_timelapse"
#define CONFIG_KEY_HEATEDBEDLEVELING   "printsend_heated_bed_leveling"
#define CONFIG_KEY_BEDTYPE             "printsend_bed_type"
#define CONFIG_KEY_AUTO_REFILL         "printsend_auto_refill"
#define CONFIG_KEY_SELECTED_PRINTER_ID "printsend_selected_printer_id"
#define CONFIG_KEY_SWITCH_TO_DEVICE_TAB "printsend_switch_to_device_tab"
#define CONFIG_KEY_MMS_FILAMENT_MAPPING "mms_filament_mapping"
#endif

class PrintSendDialogEx : public GUI::DPIDialog
{
public:
    PrintSendDialogEx(Plater* plater, int printPlateIdx, const boost::filesystem::path& path);
    ~PrintSendDialogEx();

    void init();
    boost::filesystem::path filename() const { return into_path(mModelName); }

    virtual void EndModal(int ret) override;

    std::map<std::string, std::string> getExtendedInfo() const;
    PrintHostPostUploadAction getPostAction() const;
    bool getSwitchToDeviceTab() const;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void OnCloseWindow(wxCloseEvent& event);

    BedType getCurrentBedType() const;
private:
    void setupIPCHandlers();
    IPCResult getPrinterList();
    IPCResult preparePrintTask(const std::string &printerId);
    IPCResult getPrinterMmsInfo(const std::string &printerId);
    IPCResult onPrint(const nlohmann::json &printInfo);
    void onCancel();
    std::string getCurrentProjectName();
    BedType appBedType() const;
    void refresh();

    // Helpers for exact Elegoo UX without full PrinterManager SDK
    std::string imageFileToBase64DataURI(const std::string &img_path) const;
    void buildMmsGroupFromCanvasSlots(const std::vector<ElegooCanvasSlot> &slots, PrinterMmsGroup &group) const;
    void autoMapFilamentsToMms();

    wxWebView* mBrowser{nullptr};
    std::unique_ptr<webviewIpc::WebviewIPCManager> mIpc;
    Plater*  mPlater{nullptr};
    int mPrintPlateIdx;

    bool    mTimeLapse{false};
    bool    mHeatedBedLeveling{false};
    BedType mBedType{BedType::btPTE};
    bool    mAutoRefill{false};
    PrintHostPostUploadAction mPostUploadAction{PrintHostPostUploadAction::None};
    bool mSwitchToDeviceTab{false};
    wxString mModelName;
    boost::filesystem::path mPath;
    std::string mSelectedPrinterId;
    std::string mProjectName;
    std::vector<PrintFilamentMmsMapping> mPrintFilamentList;
    bool mHasMms{false};
    PrinterMmsGroup mMmsGroup;
};

}} // namespace Slic3r::GUI
