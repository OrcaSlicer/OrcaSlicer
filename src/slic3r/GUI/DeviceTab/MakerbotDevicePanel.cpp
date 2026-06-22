#include "MakerbotDevicePanel.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/DeviceCore/DevFirmware.h"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/MakerbotLink.hpp"

#include <wx/msgdlg.h>
#include <wx/choicdlg.h>
#include <wx/log.h>
#include <boost/log/trivial.hpp>
#include <algorithm>

namespace Slic3r {
namespace GUI {

namespace {

// Anzahl konfigurierter Extruder (Düsendurchmesser-Array-Länge) - dient zur
// Single-/Dual-Extruder-Unterscheidung innerhalb derselben Baureihe, z.B. um
// bei Legacy-Druckern zwischen "Replicator Single" und "Replicator Dual" /
// "Replicator 2X" zu unterscheiden, ohne dass dafür eine eigene Kategorie
// nötig wäre.
int extruder_count(const DynamicPrintConfig& config)
{
    if (const auto* opt = config.option<ConfigOptionFloats>("nozzle_diameter"))
        return std::max<int>(1, static_cast<int>(opt->values.size()));
    return 1;
}

} // namespace

// -----------------------------------------------------------------------------------------
// Constructor: Initialize the main UI container and placeholder variables
// -----------------------------------------------------------------------------------------
MakerbotDevicePanel::MakerbotDevicePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY), m_active_config(nullptr)
{
    m_main_sizer = new wxBoxSizer(wxVERTICAL);

    // Initialize a default placeholder image for the camera before the stream connects
    m_raw_camera_frame = wxImage(FromDIP(640), FromDIP(480), true);
    m_raw_camera_frame.InitAlpha();

    this->SetSizer(m_main_sizer);

    // Bind the telemetry timer to the tick event handler
    m_telemetry_timer.SetOwner(this);
    this->Bind(wxEVT_TIMER, &MakerbotDevicePanel::on_telemetry_tick, this);
}

MakerbotDevicePanel::~MakerbotDevicePanel() {
    stop_telemetry_polling();
}

// -----------------------------------------------------------------------------------------
// Öffnet die persistente Klartext-kaiten-Session (Port 9999) bei Bedarf.
// Nur für Birdwing relevant - Lava/Method und UltiMaker nutzen andere
// Protokolle (HTTP/REST) und sind hier nicht eingebunden.
// -----------------------------------------------------------------------------------------
bool MakerbotDevicePanel::ensure_kaiten_session(std::string& error) {
    if (m_kaiten_session && m_kaiten_session->is_open())
        return true;
    if (!m_active_config) { error = "No active printer configuration."; return false; }

    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(const_cast<DynamicPrintConfig*>(m_active_config)));
    auto* mb = dynamic_cast<MakerbotLink*>(host.get());
    if (!mb) { error = "Active printer is not a MakerbotLink host."; return false; }

    m_kaiten_session = mb->open_kaiten_session(error);
    m_capability_checked = false; // neue Sitzung - Capability-Check erneut nötig
    return m_kaiten_session != nullptr;
}

// -----------------------------------------------------------------------------------------
// Kategorie-Zuordnung: bestimmt, welche der vier Baureihen aktiv ist.
// -----------------------------------------------------------------------------------------
MBDeviceCategory MakerbotDevicePanel::category_for_config(const DynamicPrintConfig& config)
{
    const auto* opt = config.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor");
    const GCodeFlavor gcf = opt ? opt->value : gcfMakerBotLegacy;

    switch (gcf) {
        case gcfMakerBotBirdwing: return MBDeviceCategory::Birdwing;
        case gcfMakerBotLava:     return MBDeviceCategory::Lava;
        case gcfUltiGCode:        return MBDeviceCategory::UltiMaker;
        case gcfMakerBotLegacy:
        default:                  return MBDeviceCategory::Legacy;
    }
}

// -----------------------------------------------------------------------------------------
// Core UI Builder: baut die UI ausschließlich aus den Sektionen zusammen, die
// für die aktive Kategorie tatsächlich Sinn ergeben.
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::update_ui_for_printer(const DynamicPrintConfig& config) {
    m_active_config = &config;
    m_category = category_for_config(config);

    // Clear existing UI elements to prevent stacking during printer switch
    m_main_sizer->Clear(true);
    m_camera_bitmap = nullptr;
    m_zoom_slider = nullptr;
    m_z_offset_slider = nullptr;
    m_z_offset_text = nullptr;
    m_btn_z_calib = m_btn_load_fil = m_btn_unload_fil = m_btn_firmware_update = nullptr;

    // Eine offene Sitzung gehört zum VORHERIGEN Drucker - sonst würden wir
    // nach einem Druckerwechsel stillschweigend weiter mit dem alten Host
    // sprechen.
    if (m_kaiten_session) {
        m_kaiten_session->close();
        m_kaiten_session.reset();
    }
    m_capability_checked = false;

    const bool is_networked = (m_category != MBDeviceCategory::Legacy);

    // Netzwerk-Druckerfamilien: Kamera + Z-Offset + Live-Status + Steuerung
    if (is_networked) {
        build_camera_section();
        build_z_offset_section();
        build_extruder_and_telemetry_section();
        build_hardware_controls_section();
    } else {
        // Legacy (Cupcake...Replicator 2X): kein Netzwerk, keine Kamera,
        // kein RPC - nur statische Infos + Firmware-Flash via avrdude.
        build_legacy_static_info_section();
        build_firmware_section();
    }

    // Refresh UI Layout hierarchy to display the updated nodes
    this->Layout();

    // Live-Polling ergibt nur bei Netzwerk-Druckern einen Sinn.
    if (is_networked)
        start_telemetry_polling();
    else
        stop_telemetry_polling();
}

// -----------------------------------------------------------------------------------------
// 1. WEBCAM & DIGITAL ZOOM (Birdwing/Lava/UltiMaker)
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_camera_section() {
    wxStaticBoxSizer* camera_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _L("Live Camera"));
    m_camera_bitmap = new wxStaticBitmap(this, wxID_ANY, wxBitmap(m_raw_camera_frame));
    camera_sizer->Add(m_camera_bitmap, 1, wxEXPAND | wxALL, FromDIP(5));

    wxBoxSizer* zoom_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText* zoom_lbl = new wxStaticText(this, wxID_ANY, _L("Digital Zoom:"));
    m_zoom_slider = new wxSlider(this, wxID_ANY, 100, 100, 300, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
    zoom_sizer->Add(zoom_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
    zoom_sizer->Add(m_zoom_slider, 1, wxEXPAND | wxALL, FromDIP(5));
    camera_sizer->Add(zoom_sizer, 0, wxEXPAND | wxALL, FromDIP(2));

    m_main_sizer->Add(camera_sizer, 0, wxEXPAND | wxALL, FromDIP(10));

    m_zoom_slider->Bind(wxEVT_SLIDER, &MakerbotDevicePanel::on_zoom_changed, this);
}

// -----------------------------------------------------------------------------------------
// 2. GLOBAL Z-OFFSET (Birdwing/Lava/UltiMaker)
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_z_offset_section() {
    wxStaticBoxSizer* z_offset_sizer = new wxStaticBoxSizer(wxHORIZONTAL, this, _L("Global Z-Offset Calibration"));

    // Slider values range from -200 to 200, representing -2.00 mm to +2.00 mm
    m_z_offset_slider = new wxSlider(this, wxID_ANY, 0, -200, 200, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
    m_z_offset_text = new wxTextCtrl(this, wxID_ANY, "0.00", wxDefaultPosition, wxSize(FromDIP(60), -1), wxTE_PROCESS_ENTER | wxTE_RIGHT);
    wxStaticText* z_unit = new wxStaticText(this, wxID_ANY, "mm");

    z_offset_sizer->Add(m_z_offset_slider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    z_offset_sizer->Add(m_z_offset_text, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    z_offset_sizer->Add(z_unit, 0, wxALIGN_CENTER_VERTICAL, FromDIP(5));

    m_main_sizer->Add(z_offset_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    m_z_offset_text->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& e){
        wxCommandEvent dummy; on_z_offset_slider_changed(dummy);
    });
    m_z_offset_text->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e){
        wxCommandEvent dummy; on_z_offset_slider_changed(dummy); e.Skip();
    });
}

// -----------------------------------------------------------------------------------------
// 3. EXTRUDER-INFO & TELEMETRIE (Birdwing/Lava/UltiMaker - Legacy hat eigene
//    statische Sektion, siehe build_legacy_static_info_section())
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_extruder_and_telemetry_section() {
    m_extruder_info_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _L("Printer Status & Hardware"));

    // Dual-Extrusion (Lava/Method, UltiMaker S-Serie) vs. Single Smart
    // Extruder (Birdwing/Z18). Beide Familien sind reine Netzwerkdrucker mit
    // Smart-Extruder-Generation - "Smart Extruder" ist hier korrekt, im
    // Gegensatz zur Legacy-Baureihe (siehe unten).
    if (m_category == MBDeviceCategory::Lava || m_category == MBDeviceCategory::UltiMaker) {
        m_lbl_extruder_1 = new wxStaticText(this, wxID_ANY, _L("Extruder 1 (Model): Syncing..."));
        m_lbl_extruder_2 = new wxStaticText(this, wxID_ANY, _L("Extruder 2 (Support): Syncing..."));
        m_extruder_info_sizer->Add(m_lbl_extruder_1, 0, wxALL, FromDIP(2));
        m_extruder_info_sizer->Add(m_lbl_extruder_2, 0, wxALL, FromDIP(2));
    } else {
        m_lbl_extruder_1 = new wxStaticText(this, wxID_ANY, _L("Smart Extruder: Syncing..."));
        m_extruder_info_sizer->Add(m_lbl_extruder_1, 0, wxALL, FromDIP(2));
    }

    m_lbl_telemetry_temp = new wxStaticText(this, wxID_ANY, _L("Temperatures (Extruder / Chamber): -- °C / -- °C"));
    m_lbl_telemetry_status = new wxStaticText(this, wxID_ANY, _L("Status: Connecting..."));
    m_lbl_telemetry_progress = new wxStaticText(this, wxID_ANY, _L("Progress: --"));

    m_extruder_info_sizer->Add(m_lbl_telemetry_temp, 0, wxTOP | wxBOTTOM, FromDIP(5));
    m_extruder_info_sizer->Add(m_lbl_telemetry_status, 0, wxBOTTOM, FromDIP(2));
    m_extruder_info_sizer->Add(m_lbl_telemetry_progress, 0, wxBOTTOM, FromDIP(2));

    m_main_sizer->Add(m_extruder_info_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
}

// -----------------------------------------------------------------------------------------
// 4. HARDWARE-STEUERUNG (Birdwing/Lava/UltiMaker)
//    Die Buttons existieren, lösen aber bewusst noch keine echten Kommandos
//    aus: die kaiten-/REST-Kommando-Namen für Z-Offset-Push, Filament
//    Load/Unload sind noch nicht bestätigt (siehe execute_printer_action()).
//    Auf realer Hardware blind geratene RPC-Aufrufe zu senden ist riskanter
//    als nur Telemetrie falsch anzuzeigen, daher hier bewusst zurückhaltend.
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_hardware_controls_section() {
    wxBoxSizer* controls_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_z_calib = new wxButton(this, wxID_ANY, _L("Run Z-Calibration"));
    m_btn_load_fil = new wxButton(this, wxID_ANY, _L("Load Filament"));
    m_btn_unload_fil = new wxButton(this, wxID_ANY, _L("Unload Filament"));

    controls_sizer->Add(m_btn_z_calib, 1, wxRIGHT, FromDIP(5));
    controls_sizer->Add(m_btn_load_fil, 1, wxRIGHT, FromDIP(5));
    controls_sizer->Add(m_btn_unload_fil, 1, 0);

    m_main_sizer->Add(controls_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    m_btn_z_calib->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("z_calibration"); });
    m_btn_load_fil->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("load_filament"); });
    m_btn_unload_fil->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("unload_filament"); });
}

// -----------------------------------------------------------------------------------------
// 5. FIRMWARE-FLASH via avrdude (NUR Legacy: Cupcake...Replicator 2X - diese
//    Baureihe nutzt AVR/Sailfish-Firmware über USB-Seriell. Birdwing/Lava/
//    UltiMaker aktualisieren ihre Firmware übers Netzwerk, nicht über
//    avrdude - das ist ein separates, noch nicht begonnenes Feature
//    ("WiFi-Setup via USB", siehe HANDOVER.md) und hier bewusst nicht
//    nachgebaut.)
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_firmware_section() {
    wxStaticBoxSizer* fw_sizer = new wxStaticBoxSizer(wxHORIZONTAL, this, _L("Maintenance"));
    m_btn_firmware_update = new wxButton(this, wxID_ANY, _L("Flash Firmware (USB/Serial)"));
    fw_sizer->Add(m_btn_firmware_update, 1, wxALL, FromDIP(2));

    m_main_sizer->Add(fw_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    m_btn_firmware_update->Bind(wxEVT_BUTTON, &MakerbotDevicePanel::on_firmware_update_clicked, this);
}

// -----------------------------------------------------------------------------------------
// 6. STATISCHE INFO-SEKTION (NUR Legacy)
//    Cupcake...Replicator 2X sprechen in dieser Architektur ausschließlich
//    über USB/seriell mit Sailfish/MightyBoard-Firmware - es gibt keinen
//    RPC-/REST-Kanal für Live-Status. Statt einer vorgetäuschten Telemetrie
//    zeigen wir hier nur, was aus der aktiven Konfiguration tatsächlich
//    bekannt ist (Modellname, Extruderzahl).
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_legacy_static_info_section() {
    wxStaticBoxSizer* info_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _L("Printer Info"));

    std::string model = "MakerBot Legacy";
    if (m_active_config) {
        if (const auto* opt = m_active_config->option<ConfigOptionString>("printer_model"))
            if (!opt->value.empty())
                model = opt->value;
    }
    const int extruders = m_active_config ? extruder_count(*m_active_config) : 1;

    auto* lbl_model = new wxStaticText(this, wxID_ANY,
        wxString::Format(_L("Model: %s"), model.c_str()));
    auto* lbl_ext = new wxStaticText(this, wxID_ANY,
        wxString::Format(_L("Extruders: %d"), extruders));
    auto* lbl_note = new wxStaticText(this, wxID_ANY,
        _L("This printer connects via USB/serial only. Live status, camera and\n"
           "remote control are not available for this generation - use the\n"
           "firmware tool below or your printer's own display panel."));

    info_sizer->Add(lbl_model, 0, wxALL, FromDIP(2));
    info_sizer->Add(lbl_ext, 0, wxALL, FromDIP(2));
    info_sizer->Add(lbl_note, 0, wxALL | wxTOP, FromDIP(6));

    m_main_sizer->Add(info_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM | wxTOP, FromDIP(10));
}

// -----------------------------------------------------------------------------------------
// Digital Zoom Handler: Crops and rescales the raw camera frame dynamically
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::on_zoom_changed(wxCommandEvent& event) {
    if (!m_raw_camera_frame.IsOk() || !m_camera_bitmap || !m_zoom_slider) return;

    int orig_w = m_raw_camera_frame.GetWidth();
    int orig_h = m_raw_camera_frame.GetHeight();
    double zoom_factor = m_zoom_slider->GetValue() / 100.0;

    int crop_w = static_cast<int>(orig_w / zoom_factor);
    int crop_h = static_cast<int>(orig_h / zoom_factor);
    int x_offset = (orig_w - crop_w) / 2;
    int y_offset = (orig_h - crop_h) / 2;

    wxImage zoomed_img = m_raw_camera_frame.GetSubImage(wxRect(x_offset, y_offset, crop_w, crop_h));
    zoomed_img.Rescale(orig_w, orig_h, wxIMAGE_QUALITY_HIGH);

    m_camera_bitmap->SetBitmap(wxBitmap(zoomed_img));
    m_camera_bitmap->Refresh();
}

// -----------------------------------------------------------------------------------------
// Z-Offset Synchronization Handlers
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::on_z_offset_slider_changed(wxCommandEvent& event) {
    if (!m_z_offset_slider || !m_z_offset_text) return;
    double value = m_z_offset_slider->GetValue() / 100.0;

    m_z_offset_text->ChangeValue(wxString::Format("%.2f", value));
    sync_z_offset_to_hardware(value);
}

void MakerbotDevicePanel::sync_z_offset_to_hardware(double offset_mm) {
    if (m_category != MBDeviceCategory::Birdwing) {
        // set_z_adjusted_offset wurde nur für Birdwing/Z18 bestätigt (Capture
        // vom 2026-06). Für Lava/UltiMaker bewusst kein geratener Aufruf.
        BOOST_LOG_TRIVIAL(info) << "MakerbotDevicePanel: Z-Offset control not confirmed for this printer family, not sent.";
        return;
    }
    std::string error;
    if (!ensure_kaiten_session(error)) {
        BOOST_LOG_TRIVIAL(warning) << "MakerbotDevicePanel: Z-Offset not sent, no session: " << error;
        return;
    }
    nlohmann::json resp;
    const nlohmann::json params = {{"offset", offset_mm}};
    if (!m_kaiten_session->call("set_z_adjusted_offset", params, resp, error)) {
        BOOST_LOG_TRIVIAL(warning) << "MakerbotDevicePanel: set_z_adjusted_offset failed: " << error;
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "MakerbotDevicePanel: Z-Offset set to " << offset_mm << "mm";
}

void MakerbotDevicePanel::execute_printer_action(const std::string& action_id) {
    if (m_category != MBDeviceCategory::Birdwing) {
        wxMessageDialog(this,
            _L("This control has only been confirmed for Birdwing (Z18/Replicator+) "
               "printers so far. No command was sent."),
            _L("Not available for this printer"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }

    if (action_id == "unload_filament") {
        // KEIN bestätigter Methodenname (siehe MakerbotLink.hpp/.cpp Kommentar).
        // process_method/"stop_filament" ist ein Kandidat aus dem Capture,
        // aber seine genaue Wirkung ist nicht eindeutig belegt - daher
        // bewusst nicht verdrahtet.
        wxMessageDialog(this,
            _L("Unload Filament has no confirmed command yet - the capture "
               "didn't show this action being triggered. No command was sent. "
               "If you can capture clicking this on the printer's own screen, "
               "send it over and this gets wired up."),
            _L("Not yet implemented"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }

    std::string error;
    if (!ensure_kaiten_session(error)) {
        wxMessageDialog(this, wxString::Format(_L("Could not connect: %s"), error.c_str()),
            _L("Connection Error"), wxOK | wxICON_ERROR).ShowModal();
        return;
    }

    nlohmann::json resp;
    bool ok = false;
    if (action_id == "z_calibration") {
        ok = m_kaiten_session->call("calibrate_z_offset", nlohmann::json::object(), resp, error);
    } else if (action_id == "load_filament") {
        // tool_index 0: einziger bestätigter Fall im Capture (Single-
        // Extruder-Z18). Für Dual-Extrusion (Lava/UltiMaker) ohnehin oben
        // schon ausgeschlossen - kommt erst mit eigener Bestätigung dazu.
        const nlohmann::json params = {{"tool_index", 0}};
        ok = m_kaiten_session->call("load_filament", params, resp, error);
    } else {
        BOOST_LOG_TRIVIAL(warning) << "MakerbotDevicePanel: unknown action_id '" << action_id << "'";
        return;
    }

    if (!ok) {
        wxMessageDialog(this, wxString::Format(_L("Command failed: %s"), error.c_str()),
            _L("Error"), wxOK | wxICON_ERROR).ShowModal();
    }
    BOOST_LOG_TRIVIAL(info) << "MakerbotDevicePanel: action '" << action_id << "' -> " << (ok ? "OK" : "FAILED: " + error);
}

// -----------------------------------------------------------------------------------------
// Firmware Updater (Legacy only, via avrdude)
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::on_firmware_update_clicked(wxCommandEvent& event) {
    if (!m_active_config) {
        wxMessageDialog(this, _L("No active printer configuration found."), _L("Error"), wxOK | wxICON_ERROR).ShowModal();
        return;
    }

    std::string fw_base_dir = resources_dir() + "/firmware/makerbot/";

    wxArrayString choices;
    choices.Add(_L("MakerBot Original Firmware (Latest)"));
    choices.Add(_L("Sailfish Custom Firmware"));

    wxSingleChoiceDialog dialog(this,
        _L("Select the firmware version to flash to the connected printer.\nWarning: Do not disconnect the USB cable during this process."),
        _L("Firmware Selection"), choices);

    if (dialog.ShowModal() == wxID_OK) {
        std::string selected = dialog.GetStringSelection().ToStdString();
        std::string hex_path;

        if (selected.find("Original") != std::string::npos) {
            hex_path = fw_base_dir + "legacy/MightyBoard_RevE_v7.5.hex";
        } else {
            hex_path = fw_base_dir + "sailfish/sailfish_v7.7.hex";
        }

        std::string serial_port = m_active_config->opt_string("serial_port");
        if (serial_port.empty()) {
            wxMessageDialog(this, _L("No serial port configured. Please check your connection settings before flashing."), _L("Connection Error"), wxOK | wxICON_ERROR).ShowModal();
            return;
        }

        std::string flash_log;

        if (DevFirmware::flash_via_usb(hex_path, serial_port, flash_log)) {
            wxMessageDialog(this, _L("Firmware successfully flashed to the printer!"), _L("Success"), wxOK | wxICON_INFORMATION).ShowModal();
        } else {
            wxMessageDialog(this, wxString::Format(_L("Firmware flash failed. Details:\n\n%s"), flash_log), _L("Flash Error"), wxOK | wxICON_ERROR).ShowModal();
        }
    }
}

// -----------------------------------------------------------------------------------------
// Telemetry & MJPEG Polling Logic (nur Birdwing/Lava/UltiMaker)
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::start_telemetry_polling() {
    if (!m_telemetry_timer.IsRunning()) {
        m_telemetry_timer.Start(2000);
        BOOST_LOG_TRIVIAL(info) << "MakerBot/UltiMaker Telemetry polling routine started.";
    }
}

void MakerbotDevicePanel::stop_telemetry_polling() {
    if (m_telemetry_timer.IsRunning()) {
        m_telemetry_timer.Stop();
        BOOST_LOG_TRIVIAL(info) << "MakerBot/UltiMaker Telemetry polling routine stopped.";
    }
    if (m_kaiten_session) {
        m_kaiten_session->close();
        m_kaiten_session.reset();
    }
}

void MakerbotDevicePanel::on_telemetry_tick(wxTimerEvent& event) {
    if (!m_active_config || m_category == MBDeviceCategory::Legacy) return;

    if (m_category != MBDeviceCategory::Birdwing) {
        // Lava/Method (HTTP) und UltiMaker (REST): kein bestätigtes Schema -
        // noch kein Capture für diese Familien.
        if (m_lbl_telemetry_status)
            m_lbl_telemetry_status->SetLabel(_L("Status: Live telemetry not yet implemented for this printer family"));
        return;
    }

    std::string error;
    if (!ensure_kaiten_session(error)) {
        if (m_lbl_telemetry_status)
            m_lbl_telemetry_status->SetLabel(wxString::Format(_L("Status: %s"), error.c_str()));
        return;
    }

    // Capability-Gate für den Z-Kalibrierungs-Button - einmalig pro Sitzung,
    // nicht jeden Tick neu abfragen (nichts spricht dafür, dass sich das
    // während einer Sitzung ändert).
    if (!m_capability_checked) {
        nlohmann::json cap_resp;
        std::string cap_err;
        if (m_kaiten_session->call("has_z_calibration_routine", nlohmann::json::object(), cap_resp, cap_err)) {
            try { m_z_calibration_supported = cap_resp.at("result").get<bool>(); }
            catch (...) { m_z_calibration_supported = true; /* unklare Antwort: nicht vorsorglich sperren */ }
        }
        if (m_btn_z_calib)
            m_btn_z_calib->Enable(m_z_calibration_supported);
        m_capability_checked = true;
    }

    nlohmann::json resp;
    if (!m_kaiten_session->call("get_system_information", nlohmann::json::object(), resp, error, 5)) {
        if (m_lbl_telemetry_status)
            m_lbl_telemetry_status->SetLabel(wxString::Format(_L("Status: %s"), error.c_str()));
        return;
    }

    // Bestätigtes Schema (Capture Z18 / MakerBot Desktop 4.10.1, 2026-06):
    //   result.current_process.{step, progress, error, ...}
    //   result.toolheads.extruder[] : {current_temperature, target_temperature, tool_id, error}
    //   result.toolheads.chamber[]  : {current_temperature, door_open}
    try {
        const auto& result = resp.at("result");

        std::string status_str = "Connected";
        int progress = -1;
        if (result.contains("current_process") && result["current_process"].is_object()) {
            const auto& proc = result["current_process"];
            if (proc.contains("step") && proc["step"].is_string())
                status_str = proc["step"].get<std::string>();
            if (proc.contains("progress") && proc["progress"].is_number())
                progress = proc["progress"].get<int>();
        } else {
            status_str = "Idle";
        }

        int temp_ext = -1, temp_chamber = -1;
        if (result.contains("toolheads") && result["toolheads"].is_object()) {
            const auto& th = result["toolheads"];
            if (th.contains("extruder") && th["extruder"].is_array() && !th["extruder"].empty()
                && th["extruder"][0].contains("current_temperature"))
                temp_ext = th["extruder"][0]["current_temperature"].get<int>();
            if (th.contains("chamber") && th["chamber"].is_array() && !th["chamber"].empty()
                && th["chamber"][0].contains("current_temperature"))
                temp_chamber = th["chamber"][0]["current_temperature"].get<int>();
        }

        update_telemetry_ui(status_str, temp_ext, temp_chamber, progress);
    } catch (const std::exception& e) {
        if (m_lbl_telemetry_status)
            m_lbl_telemetry_status->SetLabel(wxString::Format(_L("Status: parse error (%s)"), e.what()));
    }
}

void MakerbotDevicePanel::update_telemetry_ui(const std::string& status, int temp_ext, int temp_bed, int progress) {
    if (m_lbl_telemetry_temp) {
        if (temp_ext >= 0 && temp_bed >= 0)
            m_lbl_telemetry_temp->SetLabel(wxString::Format(_L("Temperatures (Extruder / Chamber): %d °C / %d °C"), temp_ext, temp_bed));
        else
            m_lbl_telemetry_temp->SetLabel(_L("Temperatures (Extruder / Chamber): -- °C / -- °C"));
        m_lbl_telemetry_temp->Refresh();
    }
    if (m_lbl_telemetry_status) {
        m_lbl_telemetry_status->SetLabel(wxString::Format(_L("Status: %s"), status.c_str()));
        m_lbl_telemetry_status->Refresh();
    }
    if (m_lbl_telemetry_progress) {
        if (progress >= 0)
            m_lbl_telemetry_progress->SetLabel(wxString::Format(_L("Progress: %d%%"), progress));
        else
            m_lbl_telemetry_progress->SetLabel(_L("Progress: --"));
        m_lbl_telemetry_progress->Refresh();
    }
}

} // namespace GUI
} // namespace Slic3r
