#include "AutoFilamentSync.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <boost/log/trivial.hpp>
#include <wx/event.h>
#include <wx/timer.h>

#include <set>
#include <string>

namespace Slic3r { namespace GUI {

namespace {

// Lets start-up finish loading and selecting presets before the sync changes them.
constexpr int SYNC_DELAY_MS = 3000;

std::set<std::string> s_synced_devices;

bool ams_list_has_filament()
{
    for (const auto& entry : wxGetApp().preset_bundle->filament_ams_list) {
        const auto& tray = entry.second;
        if (!tray.opt_string("filament_type", 0u).empty() || tray.opt_bool("filament_exist", 0u))
            return true;
    }
    return false;
}

void sync_now(const std::string& dev_id)
{
    if (wxGetApp().is_closing() || !wxGetApp().plater())
        return;
    DeviceManager* dev = wxGetApp().getDeviceManager();
    NetworkAgent*  agent = dev ? dev->get_agent() : nullptr;
    if (!agent || agent->get_filament_sync_mode() != FilamentSyncMode::pull)
        return;
    MachineObject* obj = dev->get_selected_machine();
    if (!obj || obj->get_dev_id() != dev_id)
        return;
    Plater* plater = wxGetApp().plater();
    if (!plater->is_same_printer_for_connected_and_selected(false))
        return;

    // Overwriting the filaments of a project with objects, or unsaved filament edits, needs the user's say.
    if (!plater->model().objects.empty() || wxGetApp().preset_bundle->filaments.get_edited_preset().is_dirty) {
        BOOST_LOG_TRIVIAL(info) << "auto_sync_filaments_on_connect: project not empty, offering sync for " << dev_id;
        s_synced_devices.insert(dev_id);
        plater->get_notification_manager()->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel,
            _u8L("The printer is connected. The filament list was not synced because the project is not empty."),
            _u8L("Sync filaments"), [](wxEvtHandler*) {
                wxGetApp().sidebar().sync_ams_list();
                return true;
            });
        return;
    }

    // Fetch first so a failed or empty read stays silent instead of popping Sync's warnings.
    wxGetApp().sidebar().load_ams_list(obj);
    if (!ams_list_has_filament()) {
        BOOST_LOG_TRIVIAL(info) << "auto_sync_filaments_on_connect: no filament reported by " << dev_id;
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "auto_sync_filaments_on_connect: syncing filaments from " << dev_id;
    s_synced_devices.insert(dev_id);
    wxGetApp().sidebar().sync_ams_list();
}

} // namespace

void auto_sync_filaments_on_connect(const std::string& dev_id)
{
    if (!wxGetApp().app_config->get_bool("auto_sync_filaments_on_connect") || s_synced_devices.count(dev_id))
        return;

    auto* timer = new wxTimer();
    timer->Bind(wxEVT_TIMER, [timer, dev_id](wxTimerEvent&) {
        wxGetApp().CallAfter([timer] { delete timer; });
        sync_now(dev_id);
    });
    timer->StartOnce(SYNC_DELAY_MS);
}

}} // namespace Slic3r::GUI
