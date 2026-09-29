#include "PrintSendDialogEx.hpp"
#include "slic3r/GUI/ConfigWizard.hpp"
#include <string.h>
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r_version.h"
#include <boost/cast.hpp>
#include <boost/lexical_cast.hpp>
#include "slic3r/GUI/MainFrame.hpp"
#include <boost/dll.hpp>
#include <slic3r/GUI/Widgets/WebView.hpp>
#include <slic3r/Utils/Http.hpp>
#include <libslic3r/miniz_extension.hpp>
#include <libslic3r/Utils.hpp>
#include <wx/wx.h>
#include <wx/display.h>
#include <wx/fileconf.h>
#include <wx/file.h>
#include <wx/wfstream.h>
#include <wx/mstream.h>
#include <wx/base64.h>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <thread>
#include <memory>
#include "slic3r/Utils/ElegooLink.hpp"
#include "slic3r/Utils/WebviewIPCManager.h"
#include "libslic3r/PrinterNetworkResult.hpp"
#include "slic3r/Utils/JsonUtils.hpp"
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <fstream>

#define HAS_MMS_HEIGHT 800
#define NO_MMS_HEIGHT 650

using namespace nlohmann;

namespace Slic3r { namespace GUI {

PrintSendDialogEx::PrintSendDialogEx(Plater* plater, int printPlateIdx, const boost::filesystem::path& path)
    : DPIDialog(static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, _L("Send G-code to printer host"))
    , mPlater(plater)
    , mPrintPlateIdx(printPlateIdx)
    , mTimeLapse(0)
    , mHeatedBedLeveling(0)
    , mBedType(BedType::btPTE)
    , mPostUploadAction(PrintHostPostUploadAction::None)
    , mSwitchToDeviceTab(false)
    , mPath(path)
{
    Bind(wxEVT_CLOSE_WINDOW, &PrintSendDialogEx::OnCloseWindow, this);
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE) return;
        e.Skip();
    });
}

PrintSendDialogEx::~PrintSendDialogEx() {}

void PrintSendDialogEx::on_dpi_changed(const wxRect &suggested_rect) { Layout(); Refresh(); }

void PrintSendDialogEx::init()
{
    const AppConfig* app_config = wxGetApp().app_config;
    auto preset_bundle = wxGetApp().preset_bundle;
    SetIcon(wxNullIcon);
    mBrowser = WebView::CreateWebView(this, "");
    if (mBrowser == nullptr) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": could not init m_browser";
        return;
    }
    mIpc = std::make_unique<webviewIpc::WebviewIPCManager>(mBrowser);
    setupIPCHandlers();
    mBrowser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
    wxString TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/printer/print_send/index.html").make_preferred().string());
    TargetUrl = "file://" + TargetUrl;
    wxString strlang = wxGetApp().current_language_code_safe();
    if (strlang != "") TargetUrl = wxString::Format("%s?lang=%s", TargetUrl, strlang);
    if (wxGetApp().app_config->get_bool("developer_mode")) TargetUrl = TargetUrl + "&dev=true";
    mBrowser->LoadURL(TargetUrl);
    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(topsizer);
    topsizer->Add(mBrowser, wxSizerFlags().Expand().Proportion(1));
    wxSize pSize = FromDIP(wxSize(860, HAS_MMS_HEIGHT));
    SetSize(pSize);
    CenterOnParent();

    // Read persisted settings (Elegoo keys, fallback to Orca legacy elegoolink_* keys for migration)
    auto get_recent = [&](const char* elegoo_key, const char* orca_key) -> std::string {
        std::string v = app_config->get("recent", elegoo_key);
        if (v.empty() && orca_key) v = app_config->get("recent", orca_key);
        return v;
    };
    std::string uploadAndPrint = get_recent(CONFIG_KEY_UPLOADANDPRINT, "elegoolink_upload_and_print");
    if (!uploadAndPrint.empty()) mPostUploadAction = static_cast<PrintHostPostUploadAction>(std::stoi(uploadAndPrint));
    std::string timeLapse = get_recent(CONFIG_KEY_TIMELAPSE, "elegoolink_timelapse");
    if (!timeLapse.empty()) mTimeLapse = std::stoi(timeLapse);
    std::string heatedBedLeveling = get_recent(CONFIG_KEY_HEATEDBEDLEVELING, "elegoolink_heated_bed_leveling");
    if (!heatedBedLeveling.empty()) mHeatedBedLeveling = std::stoi(heatedBedLeveling);
    std::string bedType = get_recent(CONFIG_KEY_BEDTYPE, "elegoolink_bed_type");
    if (!bedType.empty()) mBedType = static_cast<BedType>(std::stoi(bedType));
    std::string autoRefill = get_recent(CONFIG_KEY_AUTO_REFILL, "elegoolink_auto_refill");
    if (!autoRefill.empty()) mAutoRefill = std::stoi(autoRefill);
    std::string switchToDeviceTab = get_recent(CONFIG_KEY_SWITCH_TO_DEVICE_TAB, nullptr);
    if (!switchToDeviceTab.empty()) mSwitchToDeviceTab = std::stoi(switchToDeviceTab);

    wxString recent_path = from_u8(app_config->get("recent", CONFIG_KEY_PATH));
    if (recent_path.Length() > 0 && recent_path[recent_path.Length() - 1] != '/') recent_path += '/';
    recent_path += mPath.filename().wstring();
    mModelName = recent_path;
    if (mModelName.size() >= 6 && mModelName.compare(mModelName.size() - 6, 6, ".gcode") == 0)
        mModelName = mModelName.substr(0, mModelName.size() - 6);
}

std::string PrintSendDialogEx::getCurrentProjectName()
{
    wxString filename = mPlater->get_export_gcode_filename("", true, mPrintPlateIdx == PLATE_ALL_IDX ? true : false);
    if (mPrintPlateIdx == PLATE_ALL_IDX && filename.empty()) filename = _L("Untitled");
    if (filename.empty()) {
        filename = mPlater->get_export_gcode_filename("", true);
        if (filename.empty()) filename = _L("Untitled");
    }
    fs::path filenamePath(filename.c_str());
    std::string projectName = filenamePath.filename().string();
    if (from_u8(projectName).find(_L("Untitled")) != wxString::npos) {
        PartPlate* partPlate = mPlater->get_partplate_list().get_plate(mPrintPlateIdx);
        if (partPlate) {
            if (std::vector<ModelObject*> objects = partPlate->get_objects_on_this_plate(); objects.size() > 0) {
                projectName = objects[0]->name;
                for (size_t i = 1; i < objects.size(); i++) projectName += (" + " + objects[i]->name);
            }
            if (projectName.size() > 100) projectName = projectName.substr(0, 97) + "...";
        }
    }
    const std::string invalidChars = "<>[]:\\/|?*\"";
    projectName.erase(std::remove_if(projectName.begin(), projectName.end(),
                        [&invalidChars](char c){ return invalidChars.find(c) != std::string::npos; }), projectName.end());
    return projectName;
}

void PrintSendDialogEx::setupIPCHandlers()
{
    if (!mIpc) return;
    mIpc->onRequestAsync("request_print_task", [this](const IPCRequest& request, std::function<void(const IPCResult&)> sendResponse) {
        std::string printerId = request.params.value("printerId", "");
        try { sendResponse(this->preparePrintTask(printerId)); }
        catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error in request_print_task: %s") % e.what();
            sendResponse(IPCResult::error(std::string("Print task preparation failed: ") + e.what()));
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": unknown error in request_print_task";
            sendResponse(IPCResult::error("Print task preparation failed: Unknown error"));
        }
    });
    mIpc->onRequest("request_printer_list", [this](const IPCRequest& request) {
        try { return getPrinterList(); }
        catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error in request_printer_list: %s") % e.what();
            return IPCResult::error("Failed to get printer list");
        }
    });
    mIpc->onEvent("cancel_print", [this](const IPCEvent& event) {
        try { wxGetApp().CallAfter([this](){ onCancel(); }); } catch (const std::exception& e) { BOOST_LOG_TRIVIAL(error) << "Error in cancel_print: " << e.what(); }
    });
    mIpc->onRequestAsync("start_upload", [this](const IPCRequest& request, std::function<void(const IPCResult&)> sendResponse) {
        try {
            auto result = onPrint(request.params);
            if (result.code == 0) wxGetApp().CallAfter([this](){ EndModal(wxID_OK); });
            else sendResponse(result);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Error in start_upload: " << e.what();
            sendResponse(IPCResult::error(std::string("Upload failed: ") + e.what()));
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Unknown error in start_upload";
            sendResponse(IPCResult::error("Upload failed: Unknown error"));
        }
    });
    mIpc->onEvent("expand_window", [this](const IPCEvent& event) {
        try {
            bool expand = event.data.value("expand", false);
            wxGetApp().CallAfter([this, expand](){
                wxSize pSize = FromDIP(wxSize(860, expand ? HAS_MMS_HEIGHT : NO_MMS_HEIGHT));
                SetSize(pSize);
            });
        } catch (const std::exception& e) { BOOST_LOG_TRIVIAL(error) << "Error in expand_window: " << e.what(); }
    });
    mIpc->onRequest("get_current_bed_type", [this](const IPCRequest& request) {
        try {
            int bedType = getCurrentBedType();
            std::string bedTypeStr = (bedType == (int)BedType::btPC) ? "btPC" : (bedType == (int)BedType::btPTE ? "btPTE" : "unknown");
            nlohmann::json response = nlohmann::json::object();
            response["bedType"] = bedTypeStr;
            return IPCResult::success(response);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error in get_current_bed_type: %s") % e.what();
            return IPCResult::error("Failed to get bed type");
        }
    });
    mIpc->onRequestAsync("request_mms_info", [this](const IPCRequest& request, std::function<void(const IPCResult&)> sendResponse) {
        std::string printerId = request.params.value("printerId", "");
        try { sendResponse(this->getPrinterMmsInfo(printerId)); }
        catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error in request_mms_info: %s") % e.what();
            sendResponse(IPCResult::error(std::string("MMS info request failed: ") + e.what()));
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": unknown error in request_mms_info";
            sendResponse(IPCResult::error("MMS info request failed: Unknown error"));
        }
    });
}

std::string PrintSendDialogEx::imageFileToBase64DataURI(const std::string &img_path) const
{
    try {
        std::ifstream file(img_path, std::ios::binary);
        if (!file) return "";
        std::vector<unsigned char> buffer(std::istreambuf_iterator<char>(file), {});
        if (buffer.empty()) return "";
        wxMemoryBuffer memBuf(buffer.size());
        memcpy(memBuf.GetData(), buffer.data(), buffer.size());
        wxString b64 = wxBase64Encode(memBuf.GetData(), buffer.size());
        std::string ext = boost::filesystem::path(img_path).extension().string();
        std::string mime = (ext == ".svg") ? "image/svg+xml" : "image/png";
        return "data:" + mime + ";base64," + b64.ToStdString();
    } catch (...) { return ""; }
}

void PrintSendDialogEx::buildMmsGroupFromCanvasSlots(const std::vector<ElegooCanvasSlot> &slots, PrinterMmsGroup &group) const
{
    group = PrinterMmsGroup();
    group.connected = !slots.empty();
    group.connectNum = slots.empty() ? 0 : 1;
    group.mmsSystemName = "CANVAS";
    group.autoRefill = mAutoRefill;
    if (slots.empty()) return;
    PrinterMms mms;
    mms.mmsId = "0";
    mms.mmsName = "CANVAS";
    mms.connected = true;
    for (auto &s : slots) {
        PrinterMmsTray tray;
        tray.trayId = std::to_string(s.tray_id);
        tray.mmsId = std::to_string(s.canvas_id);
        tray.trayName = "Tray " + std::to_string(s.tray_id);
        tray.filamentType = s.filament_type;
        tray.filamentName = s.filament_name;
        tray.filamentColor = s.filament_color;
        tray.vendor = s.brand;
        tray.filamentId = s.filament_type;
        tray.minNozzleTemp = s.min_nozzle_temp;
        tray.maxNozzleTemp = s.max_nozzle_temp;
        // Elegoo tray status: 1 = preloaded, 3 = loaded are selectable; 0 = disconnected
        tray.status = s.has_filament ? TRAY_STATUS_LOADED : TRAY_STATUS_DISCONNECTED;
        mms.trayList.push_back(tray);
    }
    group.mmsList.push_back(mms);
    group.activeMmsId = "0";
    if (!slots.empty()) group.activeTrayId = std::to_string(slots[0].tray_id);
}

void PrintSendDialogEx::autoMapFilamentsToMms()
{
    if (mMmsGroup.mmsList.empty() || mPrintFilamentList.empty()) return;
    // Simple match as in Elegoo's PrinterMmsManager: try to match by filamentType (case-insensitive), fallback to first available
    auto &trays = mMmsGroup.mmsList[0].trayList;
    if (trays.empty()) return;
    auto to_lower = [](std::string s){ std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; };
    // Track used trays
    std::set<std::string> used;
    for (auto &fil : mPrintFilamentList) {
        bool mapped = false;
        // Try exact type match to unused tray
        for (auto &tray : trays) {
            if (tray.status == TRAY_STATUS_DISCONNECTED) continue;
            if (used.count(tray.trayId)) continue;
            if (!tray.filamentType.empty() && to_lower(tray.filamentType) == to_lower(fil.filamentType)) {
                fil.mappedMmsFilament = tray;
                used.insert(tray.trayId);
                mapped = true;
                break;
            }
        }
        if (!mapped) {
            // Fallback to first unused loaded tray
            for (auto &tray : trays) {
                if (tray.status == TRAY_STATUS_DISCONNECTED) continue;
                if (used.count(tray.trayId)) continue;
                fil.mappedMmsFilament = tray;
                used.insert(tray.trayId);
                mapped = true;
                break;
            }
        }
        if (!mapped) {
            // No tray available, leave empty (user must pick)
            fil.mappedMmsFilament = PrinterMmsTray();
        }
    }
}

IPCResult PrintSendDialogEx::preparePrintTask(const std::string& printerId)
{
    auto preset_bundle = wxGetApp().preset_bundle;
    const Print& print = mPlater->get_partplate_list().get_current_fff_print();
    const auto& stats = print.print_statistics();
    nlohmann::json printInfo = json::object();
    printInfo["printTime"] = stats.estimated_normal_print_time;
    printInfo["totalWeight"] = stats.total_weight;
    int layerCount = 0;
    for (const PrintObject* object : print.objects()) layerCount = std::max(layerCount, (int)object->layer_count());
    printInfo["layerCount"] = layerCount;
    std::string modelName = mModelName.ToUTF8().data();
    printInfo["modelName"] = modelName;
    printInfo["timeLapse"] = mTimeLapse == 1;
    printInfo["heatedBedLeveling"] = mHeatedBedLeveling == 1;
    printInfo["switchToDeviceTab"] = mSwitchToDeviceTab;
    printInfo["uploadAndPrint"] = mPostUploadAction == PrintHostPostUploadAction::StartPrint;
    printInfo["autoRefill"] = mAutoRefill == 1;
    printInfo["bedType"] = (mBedType == BedType::btPC) ? "btPC" : "btPTE";

    ThumbnailData& data = mPlater->get_partplate_list().get_curr_plate()->thumbnail_data;
    if (data.is_valid()) {
        wxImage image(data.width, data.height);
        image.InitAlpha();
        for (unsigned int r = 0; r < data.height; ++r) {
            unsigned int rr = (data.height - 1 - r) * data.width;
            for (unsigned int c = 0; c < data.width; ++c) {
                unsigned char* px = (unsigned char*) data.pixels.data() + 4 * (rr + c);
                image.SetRGB((int)c, (int)r, px[0], px[1], px[2]);
                image.SetAlpha((int)c, (int)r, px[3]);
            }
        }
        image = image.Rescale(FromDIP(256), FromDIP(256));
        wxMemoryOutputStream mem;
        image.SaveFile(mem, wxBITMAP_TYPE_PNG);
        size_t len = mem.GetSize();
        wxMemoryBuffer buffer(len);
        mem.CopyTo(buffer.GetData(), len);
        printInfo["thumbnail"] = wxBase64Encode(buffer.GetData(), len).ToStdString();
    }

    std::map<std::string, Preset*> nameToPreset;
    for (size_t i = 0; i < preset_bundle->filaments.size(); ++i) {
        Preset* p = &preset_bundle->filaments.preset(i);
        nameToPreset[p->name] = p;
    }
    mMmsGroup = PrinterMmsGroup();
    std::vector<PrintFilamentMmsMapping> projectFilamentList;
    mPrintFilamentList.clear();
    for (auto &filamentName : preset_bundle->filament_presets) {
        auto it = nameToPreset.find(filamentName);
        if (it != nameToPreset.end()) {
            PrintFilamentMmsMapping filament;
            Preset* preset = it->second;
            std::string displayedFilamentType;
            std::string filamentType = preset->config.get_filament_type(displayedFilamentType);
            float density = 1.24f;
            const ConfigOptionFloats* dens = preset->config.option<ConfigOptionFloats>("filament_density");
            if (dens && !dens->values.empty()) density = dens->values[0];
            filament.filamentType = filamentType;
            filament.filamentId = preset->filament_id;
            filament.settingId = preset->setting_id;
            const ConfigOptionStrings* vendors = preset->config.option<ConfigOptionStrings>("filament_vendor");
            filament.vendor = (vendors && !vendors->values.empty()) ? vendors->values[0] : "";
            filament.filamentName = filamentName;
            filament.filamentAlias = preset->alias;
            filament.filamentWeight = 0;
            filament.filamentDensity = density;
            projectFilamentList.push_back(filament);
        }
    }
    auto extruders = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_used_filaments();
    for (size_t i = 0; i < extruders.size(); i++) {
        int extruderIdx = extruders[i] - 1;
        if (extruderIdx < 0 || extruderIdx >= (int)projectFilamentList.size()) continue;
        auto info = projectFilamentList[extruderIdx];
        info.index = extruderIdx;
        auto colour = wxGetApp().preset_bundle->project_config.opt_string("filament_colour", (unsigned int)extruderIdx);
        info.filamentColor = colour;
        const auto& printStats = print.print_statistics();
        double model_volume_mm3 = 0.0;
        auto mit = printStats.filament_stats.find(extruderIdx);
        if (mit != printStats.filament_stats.end()) model_volume_mm3 = mit->second;
        double wipe_tower_volume_mm3 = 0.0, support_volume_mm3 = 0.0, flush_per_filament = 0.0;
        auto current_plate = mPlater->get_partplate_list().get_curr_plate();
        if (current_plate && current_plate->get_slice_result()) {
            const auto& gcode_stats = current_plate->get_slice_result()->print_statistics;
            auto wit = gcode_stats.wipe_tower_volumes_per_extruder.find(extruderIdx);
            if (wit != gcode_stats.wipe_tower_volumes_per_extruder.end()) wipe_tower_volume_mm3 = wit->second;
            auto sit = gcode_stats.support_volumes_per_extruder.find(extruderIdx);
            if (sit != gcode_stats.support_volumes_per_extruder.end()) support_volume_mm3 = sit->second;
            auto fit = gcode_stats.flush_per_filament.find(extruderIdx);
            if (fit != gcode_stats.flush_per_filament.end()) flush_per_filament = fit->second;
        }
        double total_volume_mm3 = model_volume_mm3 + wipe_tower_volume_mm3 + support_volume_mm3 + flush_per_filament;
        double total_weight = 0.0;
        if (total_volume_mm3 > 0) {
            double raw_weight = total_volume_mm3 * info.filamentDensity * 0.001;
            if (raw_weight > 0) {
                double magnitude = pow(10, floor(log10(raw_weight)));
                double normalized = raw_weight / magnitude;
                total_weight = round(normalized * 100) / 100 * magnitude;
            }
        }
        info.filamentWeight = total_weight;
        mPrintFilamentList.push_back(info);
    }
    auto cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    std::string printerModel = "";
    auto printerModelValue = cfg.option<ConfigOptionString>("printer_model");
    if (printerModelValue) printerModel = printerModelValue->value;
    printInfo["currentProjectPrinterModel"] = printerModel;
    mHasMms = false;
    nlohmann::json filamentList = json::array();
    for (auto& f : mPrintFilamentList) filamentList.push_back(convertPrintFilamentMmsMappingToJson(f));
    printInfo["filamentList"] = filamentList;
    IPCResult result;
    result.data = printInfo;
    result.code = 0;
    result.message = getErrorMessage(PrinterNetworkErrorCode::SUCCESS);
    return result;
}

IPCResult PrintSendDialogEx::getPrinterMmsInfo(const std::string &printerId)
{
    IPCResult result;
    mMmsGroup = PrinterMmsGroup();
    // Find printer host for this printerId
    std::string host, token;
    bool isCC2 = false;
    {
        // Resolve printerId -> host via PhysicalPrinterCollection
        auto *preset_bundle = wxGetApp().preset_bundle;
        for (size_t i = 0; i < preset_bundle->physical_printers.size(); ++i) {
            auto &phys = preset_bundle->physical_printers[i];
            // printerId in our scheme is host (ip) for now; we store physical printer name as id
            // We try to match by stored printerId or host
            std::string cfg_host = phys.config.opt_string("printhost_host");
            std::string cfg_name = phys.preset_name; // use preset name as id
            if (cfg_name == printerId || cfg_host == printerId) {
                host = cfg_host;
                break;
            }
        }
        // Fallback: if printerId looks like IP/host, use it directly
        if (host.empty() && printerId.find('.') != std::string::npos) host = printerId;
        // Detect model for MMS capability - check edited preset printer_model
        auto cfg = preset_bundle->printers.get_edited_preset().config;
        auto modelOpt = cfg.option<ConfigOptionString>("printer_model");
        std::string model = modelOpt ? modelOpt->value : "";
        isCC2 = (model.find("CC2") != std::string::npos || model.find("CC") != std::string::npos);
        // CC2 supports MMS/CANVAS, CC may not
        if (model == "Elegoo-C" || model == "Elegoo_C") isCC2 = false;
    }
    // If not CC2 with MMS, return empty (no filament section)
    // We still try to fetch CANVAS only for CC2; for others, systemCapabilities.supportsMultiFilament false hides section
    PrinterNetworkInfo dummy;
    dummy.systemCapabilities.supportsMultiFilament = false;
    if (!isCC2) {
        result.data = json::object();
        result.data["mmsInfo"] = json::object();
        result.data["mmsInfo"]["mmsList"] = json::array();
        result.data["mmsInfo"]["connected"] = false;
        result.data["mmsInfo"]["mmsSystemName"] = "CANVAS";
        result.data["mappedFilamentList"] = json::array();
        result.code = 0;
        result.message = getErrorMessage(PrinterNetworkErrorCode::SUCCESS);
        return result;
    }
    // For CC2, try to fetch CANVAS slots via ElegooLink (use host from printer or prompt)
    // Find ElegooLink instance that matches host
    std::vector<ElegooCanvasSlot> slots;
    wxString msg;
    bool fetched = false;
    if (!host.empty()) {
        // Create temp ElegooLink to fetch (uses config's host)
        DynamicPrintConfig cfg;
        cfg.set_key_value("printhost_host", new ConfigOptionString(host));
        cfg.set_key_value("printhost_port", new ConfigOptionString("3030"));
        ElegooLink tmpLink(std::move(cfg));
        // We need to know if host is reachable; try fetch
        // Need printer model for classify
        tmpLink.set_printer_model("Elegoo Centauri Carbon 2");
        fetched = tmpLink.fetch_canvas_slots(slots, msg);
    }
    if (!fetched) {
        // No MMS or fetch failed -> return not connected, JS will hide filament section
        result.data = json::object();
        nlohmann::json mmsInfo = json::object();
        mmsInfo["mmsSystemName"] = "CANVAS";
        mmsInfo["mmsList"] = json::array();
        mmsInfo["connected"] = false;
        mmsInfo["connectNum"] = 0;
        result.data["mmsInfo"] = mmsInfo;
        nlohmann::json filamentList = json::array();
        for (auto& f : mPrintFilamentList) filamentList.push_back(convertPrintFilamentMmsMappingToJson(f));
        result.data["mappedFilamentList"] = filamentList;
        result.code = 0;
        result.message = getErrorMessage(PrinterNetworkErrorCode::SUCCESS);
        mHasMms = false;
        return result;
    }
    buildMmsGroupFromCanvasSlots(slots, mMmsGroup);
    if (mMmsGroup.mmsList.empty() || !mMmsGroup.connected) mHasMms = false; else mHasMms = true;
    autoMapFilamentsToMms();
    nlohmann::json mmsInfo = convertPrinterMmsGroupToJson(mMmsGroup);
    result.data = json::object();
    result.data["mmsInfo"] = mmsInfo;
    nlohmann::json filamentList = json::array();
    for (auto& f : mPrintFilamentList) filamentList.push_back(convertPrintFilamentMmsMappingToJson(f));
    result.data["mappedFilamentList"] = filamentList;
    result.code = 0;
    result.message = getErrorMessage(PrinterNetworkErrorCode::SUCCESS);
    return result;
}

IPCResult PrintSendDialogEx::getPrinterList()
{
    IPCResult result;
    nlohmann::json printers = json::array();
    auto *preset_bundle = wxGetApp().preset_bundle;
    auto &physical_printers = preset_bundle->physical_printers;
    // Build list from Orca physical printers filtered to ElegooLink host_type
    for (size_t i = 0; i < physical_printers.size(); ++i) {
        auto &phys = physical_printers[i];
        std::string host_type = phys.config.opt_string("host_type");
        if (host_type != "elegoolink" && host_type != "ElegooLink" && host_type != "htElegooLink") {
            // Orca stores as enum string; check numeric as well
            // Fallback: check if host_type contains elegoo
            std::string lower = host_type; std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.find("elegoo") == std::string::npos) continue;
        }
        std::string host = phys.config.opt_string("printhost_host");
        std::string name = phys.preset_name;
        if (name.empty()) name = host;
        if (host.empty()) continue;

        PrinterNetworkInfo info;
        info.printerId = name; // use preset name as stable id
        info.printerName = name;
        info.host = host;
        info.vendor = "Elegoo";
        // Determine model from associated printer preset
        std::string model = "Elegoo-CC2";
        if (phys.preset_names.size() > 0) {
            auto preset = preset_bundle->find_preset(phys.preset_names[0]);
            if (preset) {
                auto *opt = preset->config.option<ConfigOptionString>("printer_model");
                if (opt) model = opt->value;
            }
        }
        info.printerModel = model;
        info.networkType = NETWORK_TYPE_LAN;
        info.connectStatus = PRINTER_CONNECT_STATUS_CONNECTED; // assume connected if configured
        info.printerStatus = PRINTER_STATUS_IDLE;
        info.isPhysicalPrinter = true;
        // Capabilities: CC2 has MMS, both have bed leveling and timelapse per Elegoo docs
        info.printCapabilities.supportsAutoBedLeveling = true;
        info.printCapabilities.supportsTimeLapse = true;
        info.printCapabilities.supportsHeatedBedSwitching = true;
        // Only CC2 supports MMS/CANVAS
        bool isCC2 = (model.find("CC2") != std::string::npos || model == "Elegoo-CC2" || model == "Elegoo Centauri Carbon 2");
        info.systemCapabilities.supportsMultiFilament = isCC2;
        info.printCapabilities.supportsFilamentMapping = isCC2;
        info.printCapabilities.supportsAutoRefill = isCC2;

        nlohmann::json j = convertPrinterNetworkInfoToJson(info);
        boost::filesystem::path resources_path(Slic3r::resources_dir());
        std::string img_path = resources_path.string() + "/profiles/Elegoo/" + model + "_cover.png";
        // Try svg fallback
        if (!boost::filesystem::exists(img_path)) {
            img_path = resources_path.string() + "/profiles/Elegoo/" + model + ".png";
            if (!boost::filesystem::exists(img_path))
                img_path = resources_path.string() + "/profiles/Elegoo/cover.png";
        }
        j["printerImg"] = imageFileToBase64DataURI(img_path);
        j["selected"] = false;
        printers.push_back(j);
    }
    // If no physical printers configured, add a placeholder from current config host for testing
    if (printers.empty()) {
        // Try to infer from current AppConfig recent host
        std::string recent_host = wxGetApp().app_config->get("recent", "printhost_host");
        if (!recent_host.empty()) {
            PrinterNetworkInfo info;
            info.printerId = recent_host;
            info.printerName = recent_host;
            info.host = recent_host;
            info.vendor = "Elegoo";
            auto cfg = preset_bundle->printers.get_edited_preset().config;
            auto *opt = cfg.option<ConfigOptionString>("printer_model");
            std::string model = opt ? opt->value : "Elegoo-CC2";
            info.printerModel = model;
            info.networkType = NETWORK_TYPE_LAN;
            info.connectStatus = PRINTER_CONNECT_STATUS_CONNECTED;
            info.printerStatus = PRINTER_STATUS_IDLE;
            info.printCapabilities.supportsAutoBedLeveling = true;
            info.printCapabilities.supportsTimeLapse = true;
            info.printCapabilities.supportsHeatedBedSwitching = true;
            info.systemCapabilities.supportsMultiFilament = (model.find("CC2") != std::string::npos);
            info.printCapabilities.supportsFilamentMapping = info.systemCapabilities.supportsMultiFilament;
            info.printCapabilities.supportsAutoRefill = info.systemCapabilities.supportsMultiFilament;
            nlohmann::json j = convertPrinterNetworkInfoToJson(info);
            boost::filesystem::path resources_path(Slic3r::resources_dir());
            std::string img_path = resources_path.string() + "/profiles/Elegoo/" + model + "_cover.png";
            j["printerImg"] = imageFileToBase64DataURI(img_path);
            j["selected"] = true;
            printers.push_back(j);
        }
    }
    std::string selectedPrinterId = wxGetApp().app_config->get("recent", CONFIG_KEY_SELECTED_PRINTER_ID);
    if (selectedPrinterId.empty()) selectedPrinterId = wxGetApp().app_config->get("recent", "elegoolink_selected_printer_id");
    // Auto-select: prefer selected id, else first
    bool hasSelected = false;
    for (auto &p : printers) {
        if (p["printerId"].get<std::string>() == selectedPrinterId) { p["selected"] = true; hasSelected = true; }
        else p["selected"] = false;
    }
    if (!hasSelected && !printers.empty()) printers[0]["selected"] = true;

    result.data = printers;
    result.code = 0;
    result.message = "success";
    return result;
}

IPCResult PrintSendDialogEx::onPrint(const nlohmann::json& printInfo)
{
    IPCResult result;
    result.data = nlohmann::json::object();
    PrinterNetworkErrorCode errorCode = PrinterNetworkErrorCode::SUCCESS;
    try {
        mSelectedPrinterId = "";
        mTimeLapse = printInfo.value("timeLapse", false);
        mHeatedBedLeveling = printInfo.value("heatedBedLeveling", false);
        mAutoRefill = printInfo.value("autoRefill", false);
        bool uploadAndPrint = printInfo.value("uploadAndPrint", false);
        mSwitchToDeviceTab = printInfo.value("switchToDeviceTab", false);
        mSelectedPrinterId = printInfo.value("selectedPrinterId", "");
        std::string bedType = printInfo.value("bedType", "btPTE");
        mBedType = (bedType == "btPC") ? BedType::btPC : BedType::btPTE;
        if (mSelectedPrinterId.empty()) {
            errorCode = PrinterNetworkErrorCode::PRINTER_NOT_SELECTED;
            result.message = getErrorMessage(errorCode);
            result.code = static_cast<int>(errorCode);
            return result;
        }
        mPostUploadAction = uploadAndPrint ? PrintHostPostUploadAction::StartPrint : PrintHostPostUploadAction::None;
        wxString modelName = wxString::FromUTF8(printInfo.value("modelName", ""));
        if (!modelName.EndsWith(".gcode")) modelName += ".gcode";
        mModelName = modelName;
        if (uploadAndPrint && mHasMms) {
            for (auto& pf : mPrintFilamentList) {
                pf.mappedMmsFilament = PrinterMmsTray();
                for (size_t i = 0; i < printInfo["filamentList"].size(); i++) {
                    if (printInfo["filamentList"][i].value("index", -1) == pf.index) {
                        auto mapped = printInfo["filamentList"][i].value("mappedMmsFilament", nlohmann::json::object());
                        pf.mappedMmsFilament.trayName = mapped.value("trayName", "");
                        pf.mappedMmsFilament.mmsId = mapped.value("mmsId", "");
                        pf.mappedMmsFilament.trayId = mapped.value("trayId", "");
                        pf.mappedMmsFilament.filamentColor = mapped.value("filamentColor", "");
                        pf.mappedMmsFilament.filamentName = mapped.value("filamentName", "");
                        pf.mappedMmsFilament.filamentType = mapped.value("filamentType", "");
                        break;
                    }
                }
            }
            for (auto& pf : mPrintFilamentList) {
                if (pf.mappedMmsFilament.trayName.empty() || pf.mappedMmsFilament.mmsId.empty() ||
                    pf.mappedMmsFilament.trayId.empty() || pf.mappedMmsFilament.filamentColor.empty() ||
                    pf.mappedMmsFilament.filamentName.empty() || pf.mappedMmsFilament.filamentType.empty()) {
                    errorCode = PrinterNetworkErrorCode::PRINTER_MMS_FILAMENT_NOT_MAPPED;
                    result.message = getErrorMessage(errorCode);
                    result.code = static_cast<int>(errorCode);
                    return result;
                }
            }
            // Persist mapping to AppConfig for next time (Elegoo key)
            nlohmann::json arr = nlohmann::json::array();
            for (auto &f : mPrintFilamentList) arr.push_back(convertPrintFilamentMmsMappingToJson(f));
            wxGetApp().app_config->set("recent", CONFIG_KEY_MMS_FILAMENT_MAPPING, arr.dump());
        }
    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Print Error: " << e.what();
        errorCode = PrinterNetworkErrorCode::PRINTER_UNKNOWN_ERROR;
    }
    result.code = (errorCode == PrinterNetworkErrorCode::SUCCESS) ? 0 : static_cast<int>(errorCode);
    result.message = getErrorMessage(errorCode);
    return result;
}

void PrintSendDialogEx::onCancel() { EndModal(wxID_CANCEL); }

void PrintSendDialogEx::EndModal(int ret)
{
    if (ret == wxID_OK) {
        AppConfig* app_config = wxGetApp().app_config;
        app_config->set("recent", CONFIG_KEY_UPLOADANDPRINT, std::to_string(static_cast<int>(mPostUploadAction)));
        app_config->set("recent", CONFIG_KEY_TIMELAPSE, std::to_string(mTimeLapse));
        app_config->set("recent", CONFIG_KEY_HEATEDBEDLEVELING, std::to_string(mHeatedBedLeveling));
        app_config->set("recent", CONFIG_KEY_BEDTYPE, std::to_string(static_cast<int>(mBedType)));
        app_config->set("recent", CONFIG_KEY_AUTO_REFILL, std::to_string(mAutoRefill));
        app_config->set("recent", CONFIG_KEY_SELECTED_PRINTER_ID, mSelectedPrinterId);
        app_config->set("recent", CONFIG_KEY_SWITCH_TO_DEVICE_TAB, std::to_string(mSwitchToDeviceTab));
        // Also write legacy Orca keys for backward compat
        app_config->set("recent", "elegoolink_upload_and_print", std::to_string(static_cast<int>(mPostUploadAction)));
        app_config->set("recent", "elegoolink_timelapse", std::to_string(mTimeLapse));
        app_config->set("recent", "elegoolink_heated_bed_leveling", std::to_string(mHeatedBedLeveling));
        app_config->set("recent", "elegoolink_bed_type", std::to_string(static_cast<int>(mBedType)));
        app_config->set("recent", "elegoolink_auto_refill", std::to_string(mAutoRefill));
    }
    DPIDialog::EndModal(ret);
}

std::map<std::string, std::string> PrintSendDialogEx::getExtendedInfo() const
{
    nlohmann::json filamentList = json::array();
    if (mHasMms) for (auto& f : mPrintFilamentList) filamentList.push_back(convertPrintFilamentMmsMappingToJson(f));
    return {
        {"bedType", std::to_string(mBedType)},
        {"timeLapse", mTimeLapse ? "true" : "false"},
        {"heatedBedLeveling", mHeatedBedLeveling ? "true" : "false"},
        {"autoRefill", mAutoRefill ? "true" : "false"},
        {"hasMms", mHasMms ? "true" : "false"},
        {"selectedPrinterId", mSelectedPrinterId},
        {"filamentAmsMapping", filamentList.dump()}
    };
}

PrintHostPostUploadAction PrintSendDialogEx::getPostAction() const { return mPostUploadAction; }
bool PrintSendDialogEx::getSwitchToDeviceTab() const { return mSwitchToDeviceTab; }
void PrintSendDialogEx::OnCloseWindow(wxCloseEvent& event) { event.Skip(); }
BedType PrintSendDialogEx::getCurrentBedType() const {
    std::string s = wxGetApp().app_config->get("curr_bed_type");
    int v = atoi(s.c_str());
    return static_cast<BedType>(v);
}

}} // namespace Slic3r::GUI
