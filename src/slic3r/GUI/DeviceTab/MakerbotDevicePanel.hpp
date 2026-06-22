#ifndef slic3r_MakerbotDevicePanel_hpp_
#define slic3r_MakerbotDevicePanel_hpp_

#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statbmp.h>
#include <wx/slider.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/timer.h>
#include <wx/image.h>
#include <memory>

namespace Slic3r {

class DynamicPrintConfig;
class KaitenSession;

namespace GUI {

// Welche der vier unterstützten MakerBot/UltiMaker-Druckerfamilien gerade
// aktiv ist - bestimmt, welche UI-Sektionen überhaupt sinnvoll sind:
//   Legacy     Cupcake...Replicator 2X: nur USB/seriell, kein Netzwerk,
//              keine Kamera, kein RPC. Braucht avrdude-Firmware-Flash.
//   Birdwing   Z18 & Co: SSL/kaiten-RPC, Smart Extruder, Kamera.
//   Lava       Method/Sketch: HTTP-RPC, Dual-Extrusion (Model/Support), Kamera.
//   UltiMaker  S-Serie/Cura-Familie: REST-API, i.d.R. Dual-Extrusion.
enum class MBDeviceCategory { Legacy, Birdwing, Lava, UltiMaker };

class MakerbotDevicePanel : public wxPanel {
private:
    // --- UI Layout Containers ---
    wxBoxSizer* m_main_sizer;
    wxStaticBoxSizer* m_extruder_info_sizer;

    // --- Webcam & Digital Zoom (nur Birdwing/Lava/UltiMaker) ---
    wxStaticBitmap* m_camera_bitmap   = nullptr;
    wxSlider*       m_zoom_slider     = nullptr;
    wxImage         m_raw_camera_frame;

    // --- Global Z-Offset Calibration (nur Birdwing/Lava/UltiMaker) ---
    wxSlider*   m_z_offset_slider = nullptr;
    wxTextCtrl* m_z_offset_text   = nullptr;

    // --- Telemetry & Extruder Information (alle Familien, Inhalt variiert) ---
    wxStaticText* m_lbl_extruder_1        = nullptr;
    wxStaticText* m_lbl_extruder_2        = nullptr;
    wxStaticText* m_lbl_telemetry_temp    = nullptr;
    wxStaticText* m_lbl_telemetry_status  = nullptr;
    wxStaticText* m_lbl_telemetry_progress= nullptr;

    // --- Hardware Controls (nur Birdwing/Lava/UltiMaker) ---
    wxButton* m_btn_z_calib     = nullptr;
    wxButton* m_btn_load_fil    = nullptr;
    wxButton* m_btn_unload_fil  = nullptr;

    // --- Firmware-Flash via avrdude (nur Legacy: Cupcake...Replicator 2X) ---
    wxButton* m_btn_firmware_update = nullptr;

    // --- Background Tasks & State ---
    wxTimer m_telemetry_timer;
    const DynamicPrintConfig* m_active_config;
    MBDeviceCategory m_category = MBDeviceCategory::Legacy;

    // Persistent plaintext kaiten session (port 9999, Birdwing only - see
    // MakerbotLink.hpp/.cpp). Opened lazily on the first telemetry tick,
    // closed in stop_telemetry_polling()/destructor.
    std::shared_ptr<KaitenSession> m_kaiten_session;
    bool m_z_calibration_supported = false; // gated via has_z_calibration_routine
    bool m_capability_checked = false;      // reset whenever a new session opens

    // --- Event Handlers ---
    void on_zoom_changed(wxCommandEvent& event);
    void on_z_offset_slider_changed(wxCommandEvent& event);
    void on_firmware_update_clicked(wxCommandEvent& event);
    void on_telemetry_tick(wxTimerEvent& event);

    // --- Helper Methods ---
    static MBDeviceCategory category_for_config(const DynamicPrintConfig& config);
    bool ensure_kaiten_session(std::string& error); // lazily opens m_kaiten_session
    void sync_z_offset_to_hardware(double offset_mm);
    void execute_printer_action(const std::string& action_id);
    void update_telemetry_ui(const std::string& status, int temp_ext, int temp_bed, int progress);

    // UI-Bausteinmethoden - eine pro Sektion, jeweils nur aufgerufen wenn die
    // aktive Kategorie sie tatsächlich unterstützt.
    void build_camera_section();
    void build_z_offset_section();
    void build_extruder_and_telemetry_section();
    void build_hardware_controls_section();
    void build_firmware_section();
    void build_legacy_static_info_section();

public:
    MakerbotDevicePanel(wxWindow* parent);
    ~MakerbotDevicePanel() override;

    // Rebuilds the UI dynamically based on the selected printer's category
    void update_ui_for_printer(const DynamicPrintConfig& config);

    // Manage background network requests for live data (no-op for Legacy:
    // Cupcake...Replicator 2X have no network connection in this architecture)
    void start_telemetry_polling();
    void stop_telemetry_polling();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_MakerbotDevicePanel_hpp_