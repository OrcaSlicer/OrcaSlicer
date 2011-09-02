#include "libslic3r/libslic3r.h"
#include "libslic3r/PresetBundle.hpp"
#include "Mouse3DController.hpp"

#include "Camera.hpp"
#include "GUI_App.hpp"
#include "GLCanvas3D.hpp"
#include "Plater.hpp"
#include "NotificationManager.hpp"

#include <wx/glcanvas.h>

#include <boost/nowide/convert.hpp>
#include <boost/log/trivial.hpp>
#include "I18N.hpp"

#include <bitset>

//unofficial linux lib
#ifdef HAVE_SPNAV
#include <spnav.h>
#endif

// WARN: If updating these lists, please also update resources/udev/90-3dconnexion.rules

static const std::vector<int> _3DCONNEXION_VENDORS =
{
    0x046d,  // LOGITECH = 1133 // Logitech (3Dconnexion is made by Logitech)
    0x256F   // 3DCONNECTION = 9583 // 3Dconnexion
};

// See: https://github.com/FreeSpacenav/spacenavd/blob/39856625a6de1e8c4b57c5938e1bf29d13cf1a9f/src/dev.c#L63
static const std::vector<int> _3DCONNEXION_DEVICES =
{
    0xc603,	/* 50691 spacemouse plus XT */
    0xc605,	/* 50693 cadman */
    0xc606,	/* 50694 spacemouse classic */
    0xc621,	/* 50721 spaceball 5000 */
    0xc623,	/* 50723 space traveller */
    0xc625,	/* 50725 space pilot */
    0xc626,	/* 50726 space navigator *TESTED* */
    0xc627,	/* 50727 space explorer */
    0xc628,	/* 50728 space navigator for notebooks*/
    0xc629,	/* 50729 space pilot pro*/
    0xc62b,	/* 50731 space mouse pro*/
    0xc640,	/* 50752 nulooq */
    0xc62e,	/* 50734 spacemouse wireless (USB cable) *TESTED* */
    0xc62f,	/* 50735 spacemouse wireless receiver */
    0xc631,	/* 50737 spacemouse pro wireless *TESTED* */
    0xc632,	/* 50738 spacemouse pro wireless receiver */
    0xc633,	/* 50739 spacemouse enterprise */
    0xc635,	/* 50741 spacemouse compact *TESTED* */
    0xc636,	/* 50742 spacemouse module */
    0xc63a,	/* 60060 spacemouse wireless (Bluetooth) */
    0xc652, /* 50770 3Dconnexion universal receiver *TESTED* */
};

namespace Slic3r {
namespace GUI {

#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
template<typename T>
void update_maximum(std::atomic<T>& maximum_value, T const& value) noexcept
{
    T prev_value = maximum_value;
    while (prev_value < value && ! maximum_value.compare_exchange_weak(prev_value, value)) ;
}
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT

void Mouse3DController::State::append_translation(const Vec3d& translation, size_t input_queue_max_size)
{
    std::scoped_lock<std::mutex> lock(m_input_queue_mutex);
    while (m_input_queue.size() >= input_queue_max_size)
        m_input_queue.pop_front();
    m_input_queue.emplace_back(QueueItem::translation(translation));
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
    update_maximum(input_queue_max_size_achieved, m_input_queue.size());
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
}

void Mouse3DController::State::append_rotation(const Vec3f& rotation, size_t input_queue_max_size)
{
    std::scoped_lock<std::mutex> lock(m_input_queue_mutex);
    while (m_input_queue.size() >= input_queue_max_size)
        m_input_queue.pop_front();
    m_input_queue.emplace_back(QueueItem::rotation(rotation.cast<double>()));
#ifdef WIN32
	if (rotation.x() != 0.0f)
        ++ m_mouse_wheel_counter;
#endif // WIN32
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
    update_maximum(input_queue_max_size_achieved, m_input_queue.size());
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
}

void Mouse3DController::State::append_button(unsigned int id, size_t /* input_queue_max_size */)
{
    std::scoped_lock<std::mutex> lock(m_input_queue_mutex);
    m_input_queue.emplace_back(QueueItem::buttons(id));
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
    update_maximum(input_queue_max_size_achieved, m_input_queue.size());
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
}

#ifdef _WIN32
static std::string format_device_string(int vid, int pid)
{
    std::string ret;

    switch (vid)
    {
    case 0x046d: { ret = "LOGITECH"; break; }
    case 0x256F: { ret = "3DCONNECTION"; break; }
    default:     { ret = "UNKNOWN"; break; }
    }

    ret += "::";

    switch (pid)
    {
    case 0xc603: { ret += "spacemouse plus XT"; break; }
    case 0xc605: { ret += "cadman"; break; }
    case 0xc606: { ret += "spacemouse classic"; break; }
    case 0xc621: { ret += "spaceball 5000"; break; }
    case 0xc623: { ret += "space traveller"; break; }
    case 0xc625: { ret += "space pilot"; break; }
    case 0xc626: { ret += "space navigator"; break; }
    case 0xc627: { ret += "space explorer"; break; }
    case 0xc628: { ret += "space navigator for notebooks"; break; }
    case 0xc629: { ret += "space pilot pro"; break; }
    case 0xc62b: { ret += "space mouse pro"; break; }
    case 0xc62e: { ret += "spacemouse wireless (USB cable)"; break; }
    case 0xc62f: { ret += "spacemouse wireless receiver"; break; }
    case 0xc631: { ret += "spacemouse pro wireless"; break; }
    case 0xc632: { ret += "spacemouse pro wireless receiver"; break; }
    case 0xc633: { ret += "spacemouse enterprise"; break; }
    case 0xc635: { ret += "spacemouse compact"; break; }
    case 0xc636: { ret += "spacemouse module"; break; }
    case 0xc640: { ret += "nulooq"; break; }
    case 0xc652: { ret += "3Dconnexion universal receiver"; break; }
    default:     { ret += "UNKNOWN"; break; }
    }

    return ret;
}

static std::string detect_attached_device()
{
    std::string ret;
    // Initialize the hidapi library
    int res = hid_init();
    if (res != 0)
        BOOST_LOG_TRIVIAL(error) << "Unable to initialize hidapi library";
    else if (wxGetApp().app_config->get("skip_3dmouse_detect") == "true") {
        BOOST_LOG_TRIVIAL(info) << "do not detect 3d Mouse";
    }
    else {
        // Enumerates devices
        hid_device_info* devices = hid_enumerate(0, 0);
        if (devices == nullptr)
            BOOST_LOG_TRIVIAL(trace) << "detect_attached_device() - no HID device enumerated.";
        else {
            // Searches for 1st connected 3Dconnexion device
            struct DeviceData
            {
                unsigned short usage_page{ 0 };
                unsigned short usage{ 0 };

                DeviceData(unsigned short usage_page, unsigned short usage)
                    : usage_page(usage_page), usage(usage)
                {}

                // https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf
                // Usage page 1 - Generic Desktop Controls
                // Usage page 1, usage 8 - Multi-axis Controller
                bool has_valid_usage() const { return usage_page == 1 && usage == 8; }
            };

            // When using 3Dconnexion universal receiver, multiple devices are detected sharing the same vendor_id and product_id.
            // To choose from them the right one we use: usage_page == 1 and usage == 8
            // When only a single device is detected, as for wired connections, vendor_id and product_id are enough

            // First we count all the valid devices from the enumerated list,

            hid_device_info* current = devices;
            typedef std::pair<unsigned short, unsigned short> DeviceIds;
            typedef std::vector<DeviceData> DeviceDataList;
            typedef std::map<DeviceIds, DeviceDataList> DetectedDevices;
            DetectedDevices detected_devices;
            while (current != nullptr) {
                unsigned short vendor_id = 0;
                unsigned short product_id = 0;

                for (size_t i = 0; i < _3DCONNEXION_VENDORS.size(); ++i) {
                    if (_3DCONNEXION_VENDORS[i] == current->vendor_id) {
                        vendor_id = current->vendor_id;
                        break;
                    }
                }

                if (vendor_id != 0) {
                    for (size_t i = 0; i < _3DCONNEXION_DEVICES.size(); ++i) {
                        if (_3DCONNEXION_DEVICES[i] == current->product_id) {
                            product_id = current->product_id;
                            DeviceIds detected_device(vendor_id, product_id);
                            DetectedDevices::iterator it = detected_devices.find(detected_device);
                            if (it == detected_devices.end())
                                it = detected_devices.insert(DetectedDevices::value_type(detected_device, DeviceDataList())).first;

                            it->second.emplace_back(current->usage_page, current->usage);
                        }
                    }
                }

                current = current->next;
            }

            // Free enumerated devices
            hid_free_enumeration(devices);

            unsigned short vendor_id = 0;
            unsigned short product_id = 0;
            if (!detected_devices.empty()) {
                // Then we'll decide the choosing logic to apply in dependence of the device count and operating system
                for (const DetectedDevices::value_type& device : detected_devices) {
                    if (device.second.size() == 1) {
                        if (device.second.front().has_valid_usage()) {
                            vendor_id = device.first.first;
                            product_id = device.first.second;
                            break;
                        }
                    }
                    else {
                        bool found = false;
                        for (const DeviceData& data : device.second) {
                            if (data.has_valid_usage()) {
                                vendor_id = device.first.first;
                                product_id = device.first.second;
                                found = true;
                                break;
                            }
                        }

                        if (found)
                            break;
                    }
                }
            }

            if (vendor_id != 0 && product_id != 0) {
                ret = format_device_string(static_cast<int>(vendor_id), static_cast<int>(product_id));
                BOOST_LOG_TRIVIAL(trace) << "Detected device: " << std::hex << vendor_id << std::dec << "::" << std::hex << product_id << std::dec << " " << ret;
            }
            else
                BOOST_LOG_TRIVIAL(trace) << "No 3DConnexion device detected";
        }

        // Finalize the hidapi library
        hid_exit();
    }

    return ret;
}

// Called by Win32 HID enumeration callback.
void Mouse3DController::device_attached(const std::string &device)
{
	int vid = 0;
	int pid = 0;
	if (sscanf(device.c_str(), "\\\\?\\HID#VID_%x&PID_%x&", &vid, &pid) == 2) {
//    BOOST_LOG_TRIVIAL(trace) << boost::format("Mouse3DController::device_attached(VID_%04xxPID_%04x)") % vid % pid;
//    BOOST_LOG_TRIVIAL(trace) << "Mouse3DController::device_attached: " << device;
	    if (std::find(_3DCONNEXION_VENDORS.begin(), _3DCONNEXION_VENDORS.end(), vid) != _3DCONNEXION_VENDORS.end()) {
			// Signal the worker thread to wake up and enumerate HID devices, if not connected at the moment.
			// The message may come multiple times per each USB device. For example, some USB wireless dongles register as multiple HID sockets 
			// for multiple devices to connect to.
			// Never mind, enumeration will be performed until connected.
		    m_wakeup = true;
			m_stop_condition.notify_all();
            m_device_str = format_device_string(vid, pid);
            if (auto it_params = m_params_by_device.find(m_device_str); it_params != m_params_by_device.end()) {
                std::scoped_lock<std::mutex> lock(m_params_ui_mutex);
                m_params = m_params_ui = it_params->second;
            }
            else
                m_params_by_device[format_device_string(vid, pid)] = Params();
            m_connected = true;
        }
	}
}

void Mouse3DController::device_detached(const std::string& device)
{
    int vid = 0;
    int pid = 0;
    if (sscanf(device.c_str(), "\\\\?\\HID#VID_%x&PID_%x&", &vid, &pid) == 2) {
        if (std::find(_3DCONNEXION_VENDORS.begin(), _3DCONNEXION_VENDORS.end(), vid) != _3DCONNEXION_VENDORS.end()) {
            std::scoped_lock<std::mutex> lock(m_params_ui_mutex);
            m_params_by_device[format_device_string(vid, pid)] = m_params_ui;
        }
    }
    m_device_str = "";
    m_connected = false;
}

// Filter out mouse scroll events produced by the 3DConnexion driver.
bool Mouse3DController::State::process_mouse_wheel()
{
    std::scoped_lock<std::mutex> lock(m_input_queue_mutex);
    if (m_mouse_wheel_counter == 0)
        // No 3DConnexion rotation has been captured since the last mouse scroll event.
        return false;
    if (std::find_if(m_input_queue.begin(), m_input_queue.end(), [](const QueueItem &item){ return item.is_rotation(); }) != m_input_queue.end()) {
        // There is a rotation stored in the queue. Suppress one mouse scroll event.
        -- m_mouse_wheel_counter;
        return true;
    }
    m_mouse_wheel_counter = 0;
    return true;
}
#endif // _WIN32

bool Mouse3DController::State::apply(const Mouse3DController::Params &params, Camera& camera)
{
    if (! wxGetApp().IsActive())
        return false;

    int xmult = params.invert_x ? -1 : 1;
    int ymult = params.invert_y ? -1 : 1;
    int zmult = params.invert_z ? -1 : 1;
    int yawmult = params.invert_yaw ? -1 : 1;
    int pitchmult = params.invert_pitch ? -1 : 1;
    int rollmult = params.invert_roll ? -1 : 1;

    std::deque<QueueItem> input_queue;
    {
    	// Atomically move m_input_queue to input_queue.
    	std::scoped_lock<std::mutex> lock(m_input_queue_mutex);
    	input_queue = std::move(m_input_queue);
        m_input_queue.clear();
    }

    for (const QueueItem &input_queue_item : input_queue) {
    	if (input_queue_item.is_translation()) {
            Vec3d translation = params.swap_yz ? Vec3d(input_queue_item.vector.x() * xmult, - input_queue_item.vector.z() * zmult, input_queue_item.vector.y() * ymult) : Vec3d(input_queue_item.vector.x() * xmult, input_queue_item.vector.y() * ymult, input_queue_item.vector.z() * zmult);
            double zoom_factor = camera.min_zoom() / camera.get_zoom();
	        camera.set_target(camera.get_target() + zoom_factor * params.translation.scale * (translation.x() * camera.get_dir_right() + translation.z() * camera.get_dir_up()));
            if (translation.y() != 0.0)
                camera.update_zoom(params.zoom.scale * translation.y());
        } else if (input_queue_item.is_rotation()) {
            Vec3d rot = params.rotation.scale * input_queue_item.vector * (PI / 180.);
            if (params.swap_yz)
                rot = Vec3d(rot.x(), -rot.z(), rot.y());
            rot = Vec3d(rot.x() * pitchmult, rot.y() * yawmult, rot.z() * rollmult);
            camera.rotate_local_around_target(Vec3d(rot.x(), - rot.z(), rot.y()));
	    } else {
	    	assert(input_queue_item.is_buttons());
	        switch (input_queue_item.type_or_buttons) {
	        case 0: camera.update_zoom(1.0); break;
	        case 1: camera.update_zoom(-1.0); break;
            default: break;
	        }
    	}
    }

    return ! input_queue.empty();
}

// Load the device parameter database from appconfig. To be called on application startup.
void Mouse3DController::load_config(const AppConfig &appconfig)
{
	// We do not synchronize m_params_by_device with the background thread explicitely 
	// as there should be a full memory barrier executed once the background thread is started.
	m_params_by_device.clear();

	for (const std::string &device_name : appconfig.get_mouse_device_names()) {
	    double translation_speed 	= 4.0;
	    float  rotation_speed 		= 4.0;
	    double translation_deadzone = Params::DefaultTranslationDeadzone;
	    float  rotation_deadzone 	= Params::DefaultRotationDeadzone;
	    double zoom_speed 			= 2.0;
        bool   swap_yz              = false;
        bool   invert_x             = false;
        bool   invert_y             = false;
        bool   invert_z             = false;
        bool   invert_yaw           = false;
        bool   invert_pitch         = false;
        bool   invert_roll          = false;
        appconfig.get_mouse_device_translation_speed(device_name, translation_speed);
	    appconfig.get_mouse_device_translation_deadzone(device_name, translation_deadzone);
	    appconfig.get_mouse_device_rotation_speed(device_name, rotation_speed);
	    appconfig.get_mouse_device_rotation_deadzone(device_name, rotation_deadzone);
	    appconfig.get_mouse_device_zoom_speed(device_name, zoom_speed);
        appconfig.get_mouse_device_swap_yz(device_name, swap_yz);
        appconfig.get_mouse_device_invert_x(device_name, invert_x);
        appconfig.get_mouse_device_invert_y(device_name, invert_y);
        appconfig.get_mouse_device_invert_z(device_name, invert_z);
        appconfig.get_mouse_device_invert_yaw(device_name, invert_yaw);
        appconfig.get_mouse_device_invert_pitch(device_name, invert_pitch);
        appconfig.get_mouse_device_invert_roll(device_name, invert_roll);
        // clamp to valid values
	    Params params;
	    params.translation.scale = Params::DefaultTranslationScale * std::clamp(translation_speed, Params::MinTranslationScale, Params::MaxTranslationScale);
	    params.translation.deadzone = std::clamp(translation_deadzone, 0.0, Params::MaxTranslationDeadzone);
	    params.rotation.scale = Params::DefaultRotationScale * std::clamp(rotation_speed, 0.1f, 10.0f);
	    params.rotation.deadzone = std::clamp(rotation_deadzone, 0.0f, Params::MaxRotationDeadzone);
	    params.zoom.scale = Params::DefaultZoomScale * std::clamp(zoom_speed, 0.1, 10.0);
        params.swap_yz = swap_yz;
        params.invert_x = invert_x;
        params.invert_y = invert_y;
        params.invert_z = invert_z;
        params.invert_yaw = invert_yaw;
        params.invert_pitch = invert_pitch;
        params.invert_roll = invert_roll;
        m_params_by_device[device_name] = std::move(params);
	}
}

// Store the device parameter database back to appconfig. To be called on application closeup.
void Mouse3DController::save_config(AppConfig &appconfig) const
{
	// We do not synchronize m_params_by_device with the background thread explicitely 
	// as there should be a full memory barrier executed once the background thread is stopped.

    for (const auto &key_value_pair : m_params_by_device) {
		const std::string &device_name = key_value_pair.first;
		const Params      &params      = key_value_pair.second;
	    // Store current device parameters into the config
        appconfig.set_mouse_device(device_name, params.translation.scale / Params::DefaultTranslationScale, params.translation.deadzone,
            params.rotation.scale / Params::DefaultRotationScale, params.rotation.deadzone, params.zoom.scale / Params::DefaultZoomScale,
            params.swap_yz, params.invert_x, params.invert_y, params.invert_z, params.invert_yaw, params.invert_pitch, params.invert_roll);
    }
}

bool Mouse3DController::apply(Camera& camera)
{
    // check if the user unplugged the device
    if (! m_connected) {
        // hides the settings dialog if the user un-plug the device
        m_show_settings_dialog = false;
        m_settings_dialog_closed_by_user = false;
    }

#ifdef _WIN32
    {
        std::scoped_lock<std::mutex> lock(m_params_ui_mutex);
        if (m_params_ui_changed) {
            m_params = m_params_ui;
            m_params_ui_changed = false;
        }
    }
#endif // _WIN32

    return m_state.apply(m_params, camera);
}

void Mouse3DController::render_settings_dialog(GLCanvas3D& canvas) const
{
    if (! m_show_settings_dialog || ! m_connected)
        return;

    // when the user clicks on [X] or [Close] button we need to trigger
    // an extra frame to let the dialog disappear
    if (m_settings_dialog_closed_by_user) {
        m_show_settings_dialog = false;
        m_settings_dialog_closed_by_user = false;
        canvas.request_extra_frame();
        return;
    }

    Params params_copy;
    bool   params_changed = false;
    {
    	std::scoped_lock<std::mutex> lock(m_params_ui_mutex);
    	params_copy = m_params_ui;
    }

    Size cnv_size = canvas.get_canvas_size();

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    imgui.set_next_window_pos(0.5f * (float)cnv_size.get_width(), 0.5f * (float)cnv_size.get_height(), ImGuiCond_Always, 0.5f, 0.5f);

    float space_size = imgui.get_style_scaling() * 8;

    float speed_size = imgui.calc_text_size(_L("Speed:")).x + imgui.scaled(1.5f);
    float dead_size = imgui.calc_text_size(_L("Deadzone:")).x + imgui.scaled(1.5f);
    float option_size = imgui.calc_text_size(_L("Options:")).x + imgui.scaled(1.5f);
    float max_left_size = std::max(speed_size,std::max(dead_size,option_size)) + space_size;

    float trans_size = imgui.calc_text_size(_L("Translate")).x + space_size;
    float zoom_size = imgui.calc_text_size(_L("Zoom")).x + space_size;
    float rota_size = imgui.calc_text_size(_L("Rotation")).x + space_size;
    float trasn_zoom_size = imgui.calc_text_size(_L("Translation/Zoom")).x + space_size;
    float max_slider_txt_size = std::max(std::max(trans_size,zoom_size),std::max(rota_size,trasn_zoom_size));

    ImGuiWrapper::push_toolbar_style(wxGetApp().plater()->canvas3D()->get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 20.0f));
    static ImVec2 last_win_size(0.0f, 0.0f);
    bool shown = true;
    if (imgui.begin(_L("3Dconnexion settings"), &shown, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse || ImGuiWindowFlags_NoTitleBar)) {
        if (shown) {
            ImVec2 win_size = ImGui::GetWindowSize();
            if (last_win_size.x != win_size.x || last_win_size.y != win_size.y) {
                // when the user clicks on [X] button, the next time the dialog is shown 
                // has a dummy size, so we trigger an extra frame to let it have the correct size
                last_win_size = win_size;
                canvas.request_extra_frame();
            }

            const ImVec4& color = ImVec4(0.56f, 0.56f, 0.56f, 1.00f);
            /*imgui.text(_L("Device:"));
            ImGui::SameLine();
            imgui.text(m_device_str);*/
            ImGui::AlignTextToFramePadding();
            imgui.text_colored(color,_L("Speed:"));
            ImGui::SameLine(max_left_size + max_slider_txt_size - trans_size);
            imgui.text(_L("Translate"));
            ImGui::SameLine(max_left_size + max_slider_txt_size);

            ImGui::PushItemWidth(imgui.scaled(6.0f));
            float translation_scale = float(params_copy.translation.scale) / float(Params::DefaultTranslationScale);
            bool b_translation = imgui.bbl_slider_float_style("##Translate", &translation_scale, float(Params::MinTranslationScale), float(Params::MaxTranslationScale), "%.1f");
            ImGui::SameLine();
            ImGui::PushItemWidth(imgui.scaled(3.0f));
            bool b_translation_dragfloat = ImGui::BBLDragFloat("##Translate_input", &translation_scale, float(Params::MinTranslationScale), float(Params::MaxTranslationScale),0.0, "%.1f");
            if (b_translation || b_translation_dragfloat) {
                params_copy.translation.scale = Params::DefaultTranslationScale * double(translation_scale);
            	params_changed = true;
            }
            ImGui::SameLine();
            ImGui::Dummy({space_size, 0.0});

            ImGui::Dummy({0.0, 0.0});
            ImGui::SameLine(max_left_size + max_slider_txt_size - rota_size);
            ImGui::AlignTextToFramePadding();
            imgui.text(_L("Rotation"));
            ImGui::SameLine(max_left_size + max_slider_txt_size);
            ImGui::PushItemWidth(imgui.scaled(6.0f));
            float rotation_scale = params_copy.rotation.scale / Params::DefaultRotationScale;
            bool  b_rotation1     = imgui.bbl_slider_float_style("##Rotation1", &rotation_scale, 0.1f, 10.0f, "%.1f");
            ImGui::SameLine();
            ImGui::PushItemWidth(imgui.scaled(3.0f));
            bool b_rotation1_dragfloat = ImGui::BBLDragFloat("##Rotation1_input", &rotation_scale, 0.1f, 10.0f, 0.0, "%.1f");
            if (b_rotation1 || b_rotation1_dragfloat) {
            	params_copy.rotation.scale = Params::DefaultRotationScale * rotation_scale;
            	params_changed = true;
            }
            ImGui::SameLine();
            ImGui::Dummy({space_size, 0.0});

            ImGui::Dummy({0.0, 0.0});
            ImGui::SameLine(max_left_size + max_slider_txt_size - zoom_size);
            ImGui::AlignTextToFramePadding();
            imgui.text(_L("Zoom"));
            ImGui::SameLine(max_left_size + max_slider_txt_size);
            ImGui::PushItemWidth(imgui.scaled(6.0f));
            float zoom_scale = params_copy.zoom.scale / Params::DefaultZoomScale;
            bool  b_zoom = imgui.bbl_slider_float_style("##Zoom", &zoom_scale, 0.1f, 10.0f, "%.1f");
            ImGui::SameLine();
            ImGui::PushItemWidth(imgui.scaled(3.0f));
            bool b_zoom_dragfloat = ImGui::BBLDragFloat("##Zoom_input", &zoom_scale, 0.1f, 10.0f, 0.0, "%.1f");
            if (b_zoom || b_zoom_dragfloat) {
            	params_copy.zoom.scale = Params::DefaultZoomScale * zoom_scale;
            	params_changed = true;
            }

            ImGui::Separator();
            ImGui::AlignTextToFramePadding();
            imgui.text_colored(color,_L("Deadzone:"));
            ImGui::SameLine(max_left_size + max_slider_txt_size - trasn_zoom_size);
            imgui.text(_L("Translation/Zoom"));
            ImGui::SameLine(max_left_size + max_slider_txt_size);

            ImGui::PushItemWidth(imgui.scaled(6.0f));
            float translation_deadzone = (float) params_copy.translation.deadzone;
            bool  b_tran_zoom = imgui.bbl_slider_float_style("##Translation/Zoom", &translation_deadzone, 0.0f, (float) Params::MaxTranslationDeadzone, "%.2f");
            ImGui::SameLine();
            ImGui::PushItemWidth(imgui.scaled(3.0f));
            bool b_tran_zoom_dragfloat = ImGui::BBLDragFloat("##Translation/Zoom_input", &translation_deadzone, 0.0f, (float) Params::MaxTranslationDeadzone, 0.0, "%.1f");
            if (b_tran_zoom || b_tran_zoom_dragfloat) {
                params_copy.translation.deadzone = (double) translation_deadzone;
                params_changed = true;
            }
            ImGui::SameLine();
            ImGui::Dummy({space_size, 0.0});

            ImGui::Dummy({0.0, 0.0});
            ImGui::SameLine(max_left_size + max_slider_txt_size - rota_size);
            ImGui::AlignTextToFramePadding();
            imgui.text(_L("Rotation"));
            ImGui::SameLine(max_left_size + max_slider_txt_size);
            ImGui::PushItemWidth(imgui.scaled(6.0f));
            float rotation_deadzone = params_copy.rotation.deadzone;
            bool  b_rotation2 = imgui.bbl_slider_float_style("##Rotation2", &rotation_deadzone, 0.0f, Params::MaxRotationDeadzone, "%.2f");
            ImGui::SameLine();
            ImGui::PushItemWidth(imgui.scaled(3.0f));
            bool b_rotation2_dragfloat = ImGui::BBLDragFloat("##Rotation2_input", &rotation_deadzone, 0.0f, Params::MaxRotationDeadzone, 0.0, "%.1f");
            if (b_rotation2 || b_rotation2_dragfloat) {
                params_copy.rotation.deadzone = rotation_deadzone;
                params_changed = true;
            }
            ImGui::SameLine();
            ImGui::Dummy({space_size, 0.0});
            ImGui::PopItemWidth();

            ImGui::Separator();
            ImGui::AlignTextToFramePadding();
            imgui.text_colored(color,_L("Options:"));
            ImGui::SameLine(max_left_size + max_slider_txt_size - imgui.get_slider_icon_size().x + + space_size);

            bool swap_yz = params_copy.swap_yz;
            if (imgui.bbl_checkbox(_L("Swap Y/Z axes"), swap_yz)) {
                params_copy.swap_yz = swap_yz;
                params_changed = true;
            }
            ImGui::Dummy({0.0, 0.0});
            ImGui::SameLine(max_left_size + max_slider_txt_size - imgui.get_slider_icon_size().x + + space_size);
            bool invert_x = params_copy.invert_x;
            if (imgui.bbl_checkbox(_L("Invert X axis"), invert_x)) {
                params_copy.invert_x = invert_x;
                params_changed = true;
            }
            ImGui::Dummy({0.0, 0.0});
            ImGui::SameLine(max_left_size + max_slider_txt_size - imgui.get_slider_icon_size().x + + space_size);
            bool invert_y = params_copy.invert_y;
            if (imgui.bbl_checkbox(_L("Invert Y axis"), invert_y)) {
                params_copy.invert_y = invert_y;
                params_changed = true;
            }
            ImGui::Dummy({0.0, 0.0});
            ImGui::SameLine(max_left_size + max_slider_txt_size - imgui.get_slider_icon_size().x + + space_size);
            bool invert_z = params_copy.invert_z;
            if (imgui.bbl_checkbox(_L("Invert Z axis"), invert_z)) {
                params_copy.invert_z = invert_z;
                params_changed = true;
            }
            ImGui::Dummy({0.0, 0.0});
            ImGui::SameLine(max_left_size + max_slider_txt_size - imgui.get_slider_icon_size().x + + space_size);
            bool invert_yaw = params_copy.invert_yaw;
            if (imgui.bbl_checkbox(_L("Invert Yaw axis"), invert_yaw)) {
                params_copy.invert_yaw = invert_yaw;
                params_changed = true;
            }
            ImGui::Dummy({0.0, 0.0});
            ImGui::SameLine(max_left_size + max_slider_txt_size - imgui.get_slider_icon_size().x + + space_size);
            bool invert_pitch = params_copy.invert_pitch;
            if (imgui.bbl_checkbox(_L("Invert Pitch axis"), invert_pitch)) {
                params_copy.invert_pitch = invert_pitch;
                params_changed = true;
            }
            ImGui::Dummy({0.0, 0.0});
            ImGui::SameLine(max_left_size + max_slider_txt_size - imgui.get_slider_icon_size().x + + space_size);
            bool invert_roll = params_copy.invert_roll;
            if (imgui.bbl_checkbox(_L("Invert Roll axis"), invert_roll)) {
                params_copy.invert_roll = invert_roll;
                params_changed = true;
            }
            ImGui::Dummy({0.0, 0.0});
            ImGui::SameLine(max_left_size + max_slider_txt_size - imgui.get_slider_icon_size().x + + space_size);

#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
            ImGui::Separator();
            ImGui::Separator();
            imgui.text_colored(color, "DEBUG:");
            imgui.text_colored(color, "Vectors:");
            Vec3f translation = m_state.get_first_vector_of_type(State::QueueItem::TranslationType).cast<float>();
            Vec3f rotation = m_state.get_first_vector_of_type(State::QueueItem::RotationType).cast<float>();
            ImGui::InputFloat3("Translation##2", translation.data(), "%.3f", ImGuiInputTextFlags_ReadOnly);
            ImGui::InputFloat3("Rotation##3", rotation.data(), "%.3f", ImGuiInputTextFlags_ReadOnly);

            imgui.text_colored(color, "Queue size:");

            int input_queue_size_current[2] = { int(m_state.input_queue_size_current()), int(m_state.input_queue_max_size_achieved) };
            ImGui::InputInt2("Current", input_queue_size_current, ImGuiInputTextFlags_ReadOnly);

            int input_queue_size_param = int(params_copy.input_queue_max_size);
            if (ImGui::InputInt("Max size", &input_queue_size_param, 1, 1, ImGuiInputTextFlags_ReadOnly)) {
                if (input_queue_size_param > 0) {
	            	params_copy.input_queue_max_size = input_queue_size_param;
    	        	params_changed = true;
                }
            }

            ImGui::Separator();
            imgui.text_colored(color, "Camera:");
            Vec3f target = wxGetApp().plater()->get_camera().get_target().cast<float>();
            ImGui::InputFloat3("Target", target.data(), "%.3f", ImGuiInputTextFlags_ReadOnly);
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT

            ImGui::Separator();
            float window_width = ImGui::GetWindowWidth();
            ImGui::Dummy({0.0, 0.0});
            ImGui::SameLine((window_width - imgui.calc_text_size(_L("Close")).x)/2);
            if (imgui.button(_L("Close"))) {
                // the user clicked on the [Close] button
                m_settings_dialog_closed_by_user = true;
                canvas.set_as_dirty();
            }
        }
        else {
            // the user clicked on the [X] button
            m_settings_dialog_closed_by_user = true;
            canvas.set_as_dirty();
        }
    }

    imgui.end();
    ImGui::PopStyleVar(1);
    ImGuiWrapper::pop_toolbar_style();
    if (params_changed) {
        // Synchronize front end parameters to back end.
    	std::scoped_lock<std::mutex> lock(m_params_ui_mutex);
        auto pthis = const_cast<Mouse3DController*>(this);
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
        if (params_copy.input_queue_max_size != params_copy.input_queue_max_size)
        	// Reset the statistics counter.
            m_state.input_queue_max_size_achieved = 0;
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
        pthis->m_params_ui = params_copy;
        pthis->m_params_ui_changed = true;
    }
}

#if __APPLE__

void Mouse3DController::connected(std::string device_name)
{
    assert(! m_connected);
    assert(m_device_str.empty());
	m_device_str = device_name;
    // Copy the parameters for m_device_str into the current parameters.
    if (auto it_params = m_params_by_device.find(m_device_str); it_params != m_params_by_device.end()) {
    	std::scoped_lock<std::mutex> lock(m_params_ui_mutex);
    	m_params = m_params_ui = it_params->second;
    }
    m_connected = true;
}

void Mouse3DController::disconnected()
{
    // Copy the current parameters for m_device_str into the parameter database.
    assert(m_connected == ! m_device_str.empty());
    if (m_connected) {
        std::scoped_lock<std::mutex> lock(m_params_ui_mutex);
        m_params_by_device[m_device_str] = m_params_ui;
	    m_device_str.clear();
	    m_connected = false;
		wxGetApp().plater()->get_notification_manager()->push_notification(NotificationType::Mouse3dDisconnected);

        wxGetApp().plater()->CallAfter([]() {
        	Plater *plater = wxGetApp().plater();
        	if (plater != nullptr) {
	        	plater->get_camera().recover_from_free_camera();
    	   		plater->set_current_canvas_as_dirty();
    	   	}
    	});
    }
}

bool Mouse3DController::handle_input(const DataPacketAxis& packet)
{
    if (! wxGetApp().IsActive())
        return false;

    {
    	// Synchronize parameters between the UI thread and the background thread.
    	//FIXME is this necessary on OSX? Are these notifications triggered from the main thread or from a worker thread?
    	std::scoped_lock<std::mutex> lock(m_params_ui_mutex);
    	if (m_params_ui_changed) {
    		m_params = m_params_ui;
    		m_params_ui_changed = false;
    	}
    }
    
    bool updated = false;
    // translation
    double deadzone = m_params.translation.deadzone;
    Vec3d translation(std::abs(packet[0]) > deadzone ? -packet[0] : 0.0,
                      std::abs(packet[1]) > deadzone ?  packet[1] : 0.0,
                      std::abs(packet[2]) > deadzone ?  packet[2] : 0.0);
    if (! translation.isApprox(Vec3d::Zero())) {
        m_state.append_translation(translation, m_params.input_queue_max_size);
        updated = true;
    }
    // rotation
    deadzone = m_params.rotation.deadzone;
    Vec3f rotation(std::abs(packet[3]) > deadzone ? (float)packet[3] : 0.0,
                   std::abs(packet[4]) > deadzone ? (float)packet[4] : 0.0,
                   std::abs(packet[5]) > deadzone ? (float)packet[5] : 0.0);
    if (! rotation.isApprox(Vec3f::Zero())) {
        m_state.append_rotation(rotation, m_params.input_queue_max_size);
        updated = true;
    }

    if (updated) {
        wxGetApp().plater()->set_current_canvas_as_dirty();
        // ask for an idle event to update 3D scene
        wxWakeUpIdle();
    }
    return updated;
}

#else //__APPLE__

// Initialize the application.
void Mouse3DController::init()
{
#ifdef _WIN32
    m_device_str = detect_attached_device();
    if (!m_device_str.empty()) {
        m_connected = true;
        if (auto it_params = m_params_by_device.find(m_device_str); it_params != m_params_by_device.end())
            m_params = m_params_ui = it_params->second;
    }
#endif // _WIN32

	assert(! m_thread.joinable());
    if (! m_thread.joinable()) {
    	m_stop = false;
#ifndef _WIN32
    	// Don't start the background thread on Windows, as the HID messages are sent as Windows messages.
	    m_thread = std::thread(&Mouse3DController::run, this);
#endif // _WIN32
	}
}

// Closing the application.
void Mouse3DController::shutdown()
{
    if (m_thread.joinable()) {
    	// Stop the worker thread, if running.
    	{
    		// Notify the worker thread to cancel wait on detection polling.
			std::lock_guard<std::mutex> lock(m_stop_condition_mutex);
			m_stop = true;
		}
		m_stop_condition.notify_all();
		// Wait for the worker thread to stop.
        m_thread.join();
        m_stop = false;
	}

#ifdef _WIN32
    if (!m_device_str.empty())
        m_params_by_device[m_device_str] = m_params_ui;
#endif // _WIN32
}

// Main routine of the worker thread.
void Mouse3DController::run()
{
#ifdef HAVE_SPNAV
    if (spnav_open() == -1) {
        // Give up.
        BOOST_LOG_TRIVIAL(error) << "Unable to open connection to spacenavd";
        return;
    }
    m_connected = true;
    m_device_str = "spacenavd";
    if (auto it_params = m_params_by_device.find(m_device_str); it_params != m_params_by_device.end()) {
        std::scoped_lock<std::mutex> lock(m_params_ui_mutex);
        m_params = m_params_ui = it_params->second;
    } else {
        m_params = m_params_ui = m_params_by_device[m_device_str] = Params();
    }

    for (;;) {
        {
            std::scoped_lock lock(m_params_ui_mutex);
            if (m_stop)
                break;
            if (m_params_ui_changed) {
                m_params = m_params_by_device[m_device_str] = m_params_ui;
                m_params_ui_changed = false;
            }
        }
        this->collect_input();
    }

    m_connected = false;
    // Finalize the spnav library
    spnav_close();
#else
    // Initialize the hidapi library
    int res = hid_init();
    if (res != 0) {
    	// Give up.
#if defined(__unix__) || defined(__unix) || defined(unix)    	
    	if (res == -1)
    		// Hopefully this error code comes from our bundled patched hidapi. In that case, -1 is returned by hid_wrapper_udev_init() and it mean
			BOOST_LOG_TRIVIAL(error) << "Unable to initialize hidapi library: failed to load libudev.so.1 or libudev.so.0";
    	else if (res == -2)
    		// Hopefully this error code comes from our bundled patched hidapi. In that case, -2 is returned by hid_wrapper_udev_init() and it mean
			BOOST_LOG_TRIVIAL(error) << "Unable to initialize hidapi library: failed to resolve some function from libudev.so.1 or libudev.so.0"; 
    	else
#endif // unixes
	        BOOST_LOG_TRIVIAL(error) << "Unable to initialize hidapi library";
        return;
    }

#ifdef _WIN32
    // Enumerate once just after thread start.
	m_wakeup = true;
#endif // _WIN32

    for (;;) {
        {
        	std::scoped_lock<std::mutex> lock(m_params_ui_mutex);
        	if (m_stop)
        		break;
        	if (m_params_ui_changed) {
                m_params = m_params_ui;
        		m_params_ui_changed = false;
        	}
        }
    	if (m_device == nullptr)
    		// Polls the HID devices, blocks for maximum 2 seconds.
    		m_connected = this->connect_device();
    	else
    		// Waits for 3DConnexion mouse input for maximum 100ms, then repeats.
        	this->collect_input();
    }

    this->disconnect_device();

    // Finalize the hidapi library
    hid_exit();
#endif
}

bool Mouse3DController::connect_device()
{
    if (m_stop || wxGetApp().app_config->get("skip_3dmouse_detect") == "true")
    	return false;

    {
    	// Wait for 2 seconds, but cancellable by m_stop.
    	std::unique_lock<std::mutex> lock(m_stop_condition_mutex);
#ifdef _WIN32
    	// Wait indifinetely for the stop signal.
        m_stop_condition.wait(lock, [this]{ return m_stop || m_wakeup; });
        m_wakeup = false;
#else
        m_stop_condition.wait_for(lock, std::chrono::seconds(2), [this]{ return m_stop; });
#endif
    }

    if (m_stop)
    	return false;

    // Enumerates devices
    hid_device_info* devices = hid_enumerate(0, 0);
    if (devices == nullptr) {
        BOOST_LOG_TRIVIAL(trace) << "Mouse3DController::connect_device() - no HID device enumerated.";
        return false;
    }

#ifdef _WIN32
    BOOST_LOG_TRIVIAL(trace) << "Mouse3DController::connect_device() - enumerating HID devices.";
#endif // _WIN32

    // Searches for 1st connected 3Dconnexion device
    struct DeviceData
    {
        std::string path;
        unsigned short usage_page;
        unsigned short usage;

        DeviceData()
            : path(""), usage_page(0), usage(0)
        {}
        DeviceData(const std::string& path, unsigned short usage_page, unsigned short usage)
            : path(path), usage_page(usage_page), usage(usage)
        {}

        // https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf
        // Usage page 1 - Generic Desktop Controls
        // Usage page 1, usage 8 - Multi-axis Controller
        bool has_valid_usage() const { return usage_page == 1 && usage == 8; }
    };

#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
    hid_device_info* cur = devices;
    std::cout << std::endl << "======================================================================================================================================" << std::endl;
    std::cout << "Detected devices:" << std::endl;
    while (cur != nullptr) {
        std::cout << "\"";
        std::wcout << ((cur->manufacturer_string != nullptr) ? cur->manufacturer_string : L"Unknown");
        std::cout << "/";
        std::wcout << ((cur->product_string != nullptr) ? cur->product_string : L"Unknown");
        std::cout << "\" code: " << cur->vendor_id << "/" << cur->product_id << " (" << std::hex << cur->vendor_id << "/" << cur->product_id << std::dec << ")";
        std::cout << " serial number: '";
        std::wcout << ((cur->serial_number != nullptr) ? cur->serial_number : L"Unknown");
        std::cout << "' usage page: " << cur->usage_page << " usage: " << cur->usage << " interface number: " << cur->interface_number << std::endl;

        cur = cur->next;
    }
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT

    // When using 3Dconnexion universal receiver, multiple devices are detected sharing the same vendor_id and product_id.
    // To choose from them the right one we use:
    // On Windows and Mac: usage_page == 1 and usage == 8
    // On Linux: as usage_page and usage are not defined (see hidapi.h) we try all detected devices until one is succesfully open
    // When only a single device is detected, as for wired connections, vendor_id and product_id are enough

    // First we count all the valid devices from the enumerated list,

    hid_device_info* current = devices;
    typedef std::pair<unsigned short, unsigned short> DeviceIds;
    typedef std::vector<DeviceData> DeviceDataList;
    typedef std::map<DeviceIds, DeviceDataList> DetectedDevices;
    DetectedDevices detected_devices;
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
    std::cout << std::endl << "Detected 3D connexion devices:" << std::endl;
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
    while (current != nullptr) {
        unsigned short vendor_id = 0;
        unsigned short product_id = 0;

        for (size_t i = 0; i < _3DCONNEXION_VENDORS.size(); ++i) {
            if (_3DCONNEXION_VENDORS[i] == current->vendor_id) {
                vendor_id = current->vendor_id;
                break;
            }
        }

        if (vendor_id != 0) {
            for (size_t i = 0; i < _3DCONNEXION_DEVICES.size(); ++i) {
                if (_3DCONNEXION_DEVICES[i] == current->product_id) {
                    product_id = current->product_id;
                    DeviceIds detected_device(vendor_id, product_id);
                    DetectedDevices::iterator it = detected_devices.find(detected_device);
                    if (it == detected_devices.end())
                        it = detected_devices.insert(DetectedDevices::value_type(detected_device, DeviceDataList())).first;

                    it->second.emplace_back(current->path, current->usage_page, current->usage);

#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
                    std::wcout << "\"" << ((current->manufacturer_string != nullptr) ? current->manufacturer_string : L"Unknown");
                    std::cout << "/";
                    std::wcout << ((current->product_string != nullptr) ? current->product_string : L"Unknown");
                    std::cout << "\" code: " << current->vendor_id << "/" << current->product_id << " (" << std::hex << current->vendor_id << "/" << current->product_id << std::dec << ")";
                    std::cout << " serial number: '";
                    std::wcout << ((current->serial_number != nullptr) ? current->serial_number : L"Unknown");
                    std::cout << "' usage page: " << current->usage_page << " usage: " << current->usage << std::endl;
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
                }
            }
        }

        current = current->next;
    }

    // Free enumerated devices
    hid_free_enumeration(devices);

    if (detected_devices.empty())
        return false;

    std::string path;
    unsigned short vendor_id = 0;
    unsigned short product_id = 0;

    // Then we'll decide the choosing logic to apply in dependence of the device count and operating system

    for (const DetectedDevices::value_type& device : detected_devices) {
        if (device.second.size() == 1) {
#if defined(__linux__)
            hid_device* test_device = hid_open(device.first.first, device.first.second, nullptr);
            if (test_device == nullptr) {
                BOOST_LOG_TRIVIAL(error) << "3DConnexion device cannot be opened: " << device.second.front().path <<
                    " You may need to update /etc/udev/rules.d";
            } else {
                hid_close(test_device);
#else
            if (device.second.front().has_valid_usage()) {
#endif // __linux__ 
                vendor_id = device.first.first;
                product_id = device.first.second;
                break;
            }
        }
        else {
            bool found = false;
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
            std::cout << std::endl;
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
            for (const DeviceData& data : device.second) {
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
                std::cout << "Test device: " << std::hex << device.first.first << std::dec << "/" << std::hex << device.first.second << std::dec << " \"" << data.path << "\"";
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT

#ifdef __linux__
                hid_device* test_device = hid_open_path(data.path.c_str());
                if (test_device != nullptr) {
                    path = data.path;
                    vendor_id = device.first.first;
                    product_id = device.first.second;
                    found = true;
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
                    std::cout << "-> PASSED" << std::endl;
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
                    hid_close(test_device);
                    break;
                }
#else // !__linux__
                if (data.has_valid_usage()) {
                    path = data.path;
                    vendor_id = device.first.first;
                    product_id = device.first.second;
                    found = true;
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
                    std::cout << "-> PASSED" << std::endl;
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
                    break;
                }
#endif // __linux__
                else {
                    BOOST_LOG_TRIVIAL(error) << "3DConnexion device cannot be opened: " << data.path <<
                        " You may need to update /etc/udev/rules.d";
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
                    std::cout << "-> NOT PASSED" << std::endl;
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
                }
            }

            if (found)
                break;
        }
    }

    if (path.empty()) {
        if ((vendor_id != 0) && (product_id != 0)) {
            // Open the 3Dconnexion device using vendor_id and product_id
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
            std::cout << std::endl << "Opening device: " << std::hex << vendor_id << std::dec << "/" << std::hex << product_id << std::dec << " using hid_open()" << std::endl;
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
            m_device = hid_open(vendor_id, product_id, nullptr);
        }
        else
            return false;
    }
    else {
        // Open the 3Dconnexion device using the device path
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
        std::cout << std::endl << "Opening device: " << std::hex << vendor_id << std::dec << "/" << std::hex << product_id << std::dec << "\"" << path << "\" using hid_open_path()" << std::endl;
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
        m_device = hid_open_path(path.c_str());
    }

    if (m_device != nullptr) {
        wchar_t buffer[1024];
        hid_get_manufacturer_string(m_device, buffer, 1024);
        m_device_str = into_u8(buffer);
        // #3479 seems to show that sometimes an extra whitespace is added, so we remove it
        boost::algorithm::trim(m_device_str);

        hid_get_product_string(m_device, buffer, 1024);
        m_device_str += "/" + into_u8(buffer);
        // #3479 seems to show that sometimes an extra whitespace is added, so we remove it
        boost::algorithm::trim(m_device_str);

        BOOST_LOG_TRIVIAL(info) << "Connected 3DConnexion device:";
        BOOST_LOG_TRIVIAL(info) << "Manufacturer/product: " << m_device_str;
        BOOST_LOG_TRIVIAL(info) << "Manufacturer id.....: " << vendor_id << " (" << std::hex << vendor_id << std::dec << ")";
        BOOST_LOG_TRIVIAL(info) << "Product id..........: " << product_id << " (" << std::hex << product_id << std::dec << ")";
        if (!path.empty())
            BOOST_LOG_TRIVIAL(info) << "Path................: '" << path << "'";
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
        std::cout << "Opened device." << std::endl;
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
        // Copy the parameters for m_device_str into the current parameters.
        if (auto it_params = m_params_by_device.find(m_device_str); it_params != m_params_by_device.end()) {
	    	std::scoped_lock<std::mutex> lock(m_params_ui_mutex);
	    	m_params = m_params_ui = it_params->second;
	    }
    }
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
    else {
        std::cout << std::endl << "Unable to connect to device:" << std::endl;
        std::cout << "Manufacturer/product: " << m_device_str << std::endl;
        std::cout << "Manufacturer id.....: " << vendor_id << " (" << std::hex << vendor_id << std::dec << ")" << std::endl;
        std::cout << "Product id..........: " << product_id << " (" << std::hex << product_id << std::dec << ")" << std::endl;
        std::cout << "Path................: '" << path << "'" << std::endl;
    }
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT

    return (m_device != nullptr);
}

void Mouse3DController::disconnect_device()
{
    if (m_device) {
	    hid_close(m_device);
	    m_device = nullptr;
	    BOOST_LOG_TRIVIAL(info) << "Disconnected device: " << m_device_str;
        // Copy the current parameters for m_device_str into the parameter database.
        {
	        std::scoped_lock<std::mutex> lock(m_params_ui_mutex);
	        m_params_by_device[m_device_str] = m_params_ui;
	    }
	    m_device_str.clear();
	    m_connected = false;
#ifdef _WIN32
	    // Enumerate once immediately after disconnect.
	    m_wakeup = true;
#endif // _WIN32	    
        wxGetApp().plater()->CallAfter([]() {
        	Plater *plater = wxGetApp().plater();
        	if (plater != nullptr) {
	        	plater->get_camera().recover_from_free_camera();
    	   		plater->set_current_canvas_as_dirty();
    	   	}
    	});
    }
}

#ifdef HAVE_SPNAV
// Convert a signed 16bit word from a 3DConnexion mouse HID packet into a double coordinate, apply a dead zone.
static double convert_spnav_input(int value)
{
    return (double)value/100;
}
#endif

void Mouse3DController::collect_input()
{
#ifdef HAVE_SPNAV
    // Read packet, block maximum 100 ms. That means when closing the application, closing the application will be delayed by 100 ms.
    int fd = spnav_fd();

    if (fd != -1) {
        fd_set fds;
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};

        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        if (select(fd + 1, &fds, NULL, NULL, &tv) == 1) {
            spnav_event ev = {};
            switch (spnav_poll_event(&ev)) {
                case SPNAV_EVENT_MOTION: {
                    Vec3d translation(-convert_spnav_input(ev.motion.x), convert_spnav_input(ev.motion.y), -convert_spnav_input(ev.motion.z));
                    if (!translation.isApprox(Vec3d::Zero())) {
                        m_state.append_translation(translation, m_params.input_queue_max_size);
                    }
                    Vec3f rotation(convert_spnav_input(ev.motion.rx), convert_spnav_input(ev.motion.ry), -convert_spnav_input(ev.motion.rz));
                    if (!rotation.isApprox(Vec3f::Zero())) {
                        m_state.append_rotation(rotation, m_params.input_queue_max_size);
                    }
                    break;
                }
                case SPNAV_EVENT_BUTTON:
                    if (ev.button.press)
                        m_state.append_button((unsigned int)ev.button.bnum, m_params.input_queue_max_size);
                    break;
            }
            wxGetApp().plater()->set_current_canvas_as_dirty();
            // ask for an idle event to update 3D scene
            wxWakeUpIdle();
        }
    }
#else
    DataPacketRaw packet = { 0 };
    // Read packet, block maximum 100 ms. That means when closing the application, closing the application will be delayed by 100 ms.
    int res = hid_read_timeout(m_device, packet.data(), packet.size(), 100);
    if (res < 0) {
        // An error occourred (device detached from pc ?). Close the 3Dconnexion device.
        this->disconnect_device();
    } else
		this->handle_input(packet, res, m_params, m_state);
#endif
}

#ifdef _WIN32
bool Mouse3DController::handle_raw_input_win32(const unsigned char *data, const int packet_length)
{
    if (! wxGetApp().IsActive())
        return false;

    if (packet_length == 7 || packet_length == 13) {
        DataPacketRaw packet;
    	memcpy(packet.data(), data, packet_length);
        handle_packet(packet, packet_length, m_params, m_state);
        m_connected = true;
    }

    return true;
}
#endif /* _WIN32 */

// Unpack raw 3DConnexion HID packet of a wired 3D mouse into m_state. Called by the worker thread.
bool Mouse3DController::handle_input(const DataPacketRaw& packet, const int packet_length, const Params &params, State &state_in_out)
{
    if (! wxGetApp().IsActive())
        return false;

    int res = packet_length;
    bool updated = false;

    if (res == 7 || res == 13 ||
        // On Mac button packets can be 3 bytes long
       	((res == 3) && (packet[0] == 3)))
        updated = handle_packet(packet, res, params, state_in_out);
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
    else if (res > 0)
        std::cout << "Got unknown data packet of length: " << res << ", code:" << (int)packet[0] << std::endl;
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT

    if (updated) {
        wxGetApp().plater()->set_current_canvas_as_dirty();
        // ask for an idle event to update 3D scene
        wxWakeUpIdle();
    }
    return updated;
}

// Unpack raw 3DConnexion HID packet of a wired 3D mouse into m_state. Called by handle_input() from the worker thread.
bool Mouse3DController::handle_packet(const DataPacketRaw& packet, const int packet_length, const Params &params, State &state_in_out)
{
    switch (packet[0])
    {
    case 1: // Translation + Rotation
        {
            bool updated = handle_packet_translation(packet, params, state_in_out);
            if (packet_length == 13)
	            updated |= handle_packet_rotation(packet, 7, params, state_in_out);

            if (updated)
                return true;

            break;
        }
    case 2: // Rotation
        {
            if (handle_packet_rotation(packet, 1, params, state_in_out))
                return true;

            break;
        }
    case 3: // Button
        {
            if (params.buttons_enabled && handle_packet_button(packet, packet.size() - 1, params, state_in_out))
                return true;

            break;
        }
    case 23: // Battery charge
        {
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
            std::cout << "3DConnexion - battery level: " << (int)packet[1] << " percent" << std::endl;
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
            break;
        }
    default:
        {
#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
            std::cout << "3DConnexion - Got unknown data packet of code: " << (int)packet[0] << std::endl;
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
            break;
        }
    }

    return false;
}

// Convert a signed 16bit word from a 3DConnexion mouse HID packet into a double coordinate, apply a dead zone.
static double convert_input(int coord_byte_low, int coord_byte_high, double deadzone)
{
    int value = coord_byte_low | (coord_byte_high << 8);
    if (value >= 32768)
    	value = value - 65536;
    double ret = (double)value / 350.0;
    return (std::abs(ret) > deadzone) ? ret : 0.0;
}

// Unpack raw 3DConnexion HID packet, decode state of translation axes into state_in_out. Called by handle_input() from the worker thread.
bool Mouse3DController::handle_packet_translation(const DataPacketRaw& packet, const Params &params, State &state_in_out)
{
    double deadzone = params.translation.deadzone;
    Vec3d translation(-convert_input(packet[1], packet[2], deadzone),
        convert_input(packet[3], packet[4], deadzone),
        convert_input(packet[5], packet[6], deadzone));

    if (!translation.isApprox(Vec3d::Zero()))
    {
        state_in_out.append_translation(translation, params.input_queue_max_size);
        return true;
    }

    return false;
}

// Unpack raw 3DConnexion HID packet, decode state of rotation axes into state_in_out. Called by the handle_input() from worker thread.
bool Mouse3DController::handle_packet_rotation(const DataPacketRaw& packet, unsigned int first_byte, const Params &params, State &state_in_out)
{
    double deadzone = (double)params.rotation.deadzone;
    Vec3f rotation((float)convert_input(packet[first_byte + 0], packet[first_byte + 1], deadzone),
        (float)convert_input(packet[first_byte + 2], packet[first_byte + 3], deadzone),
        (float)convert_input(packet[first_byte + 4], packet[first_byte + 5], deadzone));

    if (!rotation.isApprox(Vec3f::Zero()))
    {
        state_in_out.append_rotation(rotation, params.input_queue_max_size);
        return true;
    }

    return false;
}

// Unpack raw 3DConnexion HID packet, decode button state into state_in_out. Called by handle_input() from the worker thread.
bool Mouse3DController::handle_packet_button(const DataPacketRaw& packet, unsigned int packet_size, const Params &params, State &state_in_out)
{
    unsigned int data = 0;
    for (unsigned int i = 1; i < packet_size; ++i)
    {
        data |= packet[i] << 8 * (i - 1);
    }

    const std::bitset<32> data_bits{ data };
    for (size_t i = 0; i < data_bits.size(); ++i)
    {
        if (data_bits.test(i))
        {
            state_in_out.append_button((unsigned int)i, params.input_queue_max_size);
            return true;
        }
    }

    return false;
}

#endif //__APPLE__

} // namespace GUI
} // namespace Slic3r
