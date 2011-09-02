#ifndef slic3r_Mouse3DController_hpp_
#define slic3r_Mouse3DController_hpp_

// Enabled debug output to console and extended imgui dialog
#define ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT 0

#include "libslic3r/Point.hpp"

#include "hidapi.h"

#include <queue>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>
#include <condition_variable>


namespace Slic3r {

class AppConfig;

namespace GUI {

struct Camera;
class GLCanvas3D;

class Mouse3DController
{
	// Parameters, which are configured by the ImGUI dialog when pressing Ctrl+M.
	// The UI thread modifies a copy of the parameters and indicates to the background thread that there was a change
	// to copy the parameters.
	struct Params
	{
        static constexpr double MinTranslationScale = 0.1;
        static constexpr double MaxTranslationScale = 30.;
		static constexpr double DefaultTranslationScale = 2.5;
        static constexpr double MaxTranslationDeadzone = 0.2;
        static constexpr double DefaultTranslationDeadzone = 0.0;
        static constexpr float  DefaultRotationScale = 1.0f;
        static constexpr float  MaxRotationDeadzone = 0.2f;
        static constexpr float  DefaultRotationDeadzone = 0.0f;
        static constexpr double DefaultZoomScale = 0.1;

        template <typename Number>
        struct CustomParameters
        {
            Number scale;
            Number deadzone;
        };

        CustomParameters<double> translation { DefaultTranslationScale, DefaultTranslationDeadzone };
        CustomParameters<float>  rotation { DefaultRotationScale, DefaultRotationDeadzone };
        CustomParameters<double> zoom { DefaultZoomScale, 0.0 };
        // Do not process button presses from 3DConnexion device, let the user map the 3DConnexion keys in 3DConnexion driver.
        bool 					 buttons_enabled { false };
        // The default value of 15 for max_size seems to work fine on all platforms
        // The effects of changing this value can be tested by setting ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT to 1
        // and playing with the imgui dialog which shows by pressing CTRL+M
        size_t 					 input_queue_max_size { 15 };
        // Whether to swap Y/Z axes or not.
        bool 					 swap_yz{ false };
        // Invert varius axes...
        bool                                     invert_x{ false };
        bool                                     invert_y{ false };
        bool                                     invert_z{ false };
        bool                                     invert_yaw{ false };
        bool                                     invert_pitch{ false };
        bool                                     invert_roll{ false };
    };

	// Queue of the 3DConnexion input events (translations, rotations, button presses).
    class State
    {
    public:
        struct QueueItem {
            static QueueItem translation(const Vec3d &translation) { QueueItem out; out.vector = translation; out.type_or_buttons = TranslationType; return out; }
            static QueueItem rotation(const Vec3d &rotation) { QueueItem out; out.vector = rotation; out.type_or_buttons = RotationType; return out; }
            static QueueItem buttons(unsigned int buttons) { QueueItem out; out.type_or_buttons = buttons; return out; }

            bool 			 is_translation() const { return this->type_or_buttons == TranslationType; }
            bool 			 is_rotation() const { return this->type_or_buttons == RotationType; }
            bool 			 is_buttons() const { return ! this->is_translation() && ! this->is_rotation(); }

            Vec3d        	 vector;
            unsigned int 	 type_or_buttons;

            static constexpr unsigned int TranslationType = std::numeric_limits<unsigned int>::max();
            static constexpr unsigned int RotationType    = TranslationType - 1;
        };

    private:
    	// m_input_queue is accessed by the background thread and by the UI thread. Access to m_input_queue
    	// is guarded with m_input_queue_mutex.
        std::deque<QueueItem> m_input_queue;
        mutable std::mutex	  m_input_queue_mutex;

#ifdef WIN32
        // When the 3Dconnexion driver is running the system gets, by default, mouse wheel events when rotations around the X axis are detected.
        // We want to filter these out because we are getting the data directly from the device, bypassing the driver, and those mouse wheel events interfere
        // by triggering unwanted zoom in/out of the scene
        // The following variable is used to count the potential mouse wheel events triggered and is updated by: 
        // Mouse3DController::collect_input() through the call to the append_rotation() method
        // GLCanvas3D::on_mouse_wheel() through the call to the process_mouse_wheel() method
        // GLCanvas3D::on_idle() through the call to the apply() method
        unsigned int 		  m_mouse_wheel_counter { 0 };
#endif /* WIN32 */

    public:
        // Called by the background thread or by by Mouse3DHandlerMac.mm when a new event is received from 3DConnexion device.
        void append_translation(const Vec3d& translation, size_t input_queue_max_size);
        void append_rotation(const Vec3f& rotation, size_t input_queue_max_size);
        void append_button(unsigned int id, size_t input_queue_max_size);

#ifdef WIN32
        // Called by GLCanvas3D::on_mouse_wheel(wxMouseEvent& evt)
        // to filter out spurious mouse scroll events produced by the 3DConnexion driver on Windows.
        bool process_mouse_wheel();
#endif /* WIN32 */

#if ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT
        Vec3d               get_first_vector_of_type(unsigned int type) const {
            std::scoped_lock<std::mutex> lock(m_input_queue_mutex);
            auto it = std::find_if(m_input_queue.begin(), m_input_queue.end(), [type](const QueueItem& item) { return item.type_or_buttons == type; });
            return (it == m_input_queue.end()) ? Vec3d::Zero() : it->vector;
        }
        size_t              input_queue_size_current() const { 
        	std::scoped_lock<std::mutex> lock(m_input_queue_mutex); 
        	return m_input_queue.size(); 
        }
        std::atomic<size_t> input_queue_max_size_achieved;
#endif // ENABLE_3DCONNEXION_DEVICES_DEBUG_OUTPUT

        // Apply the 3DConnexion events stored in the input queue, reset the input queue.
        // Returns true if any change to the camera took place.
        bool apply(const Params &params, Camera& camera);
    };

    // Background thread works with this copy.
    Params 				m_params;
    // UI thread will read / write this copy.
    Params 				m_params_ui;
    bool 	            m_params_ui_changed { false };
    mutable std::mutex	m_params_ui_mutex;

    // This is a database of parametes of all 3DConnexion devices ever connected.
    // This database is loaded from AppConfig on application start and it is stored to AppConfig on application exit.
    // We need to do that as the AppConfig is not thread safe and we need read the parameters on device connect / disconnect,
    // which is now done by a background thread.
    std::map<std::string, Params> m_params_by_device;

    mutable State 		m_state;
    std::atomic<bool> 	m_connected { false };
    std::string 		m_device_str;

#if ! __APPLE__
    // Worker thread for enumerating devices, connecting, reading data from the device and closing the device.
    std::thread 		m_thread;
    hid_device* 		m_device { nullptr };
    // Using m_stop_condition_mutex to synchronize m_stop.
    bool				m_stop { false };
#ifdef _WIN32
    std::atomic<bool>	m_wakeup { false };
#endif /* _WIN32 */
    // Mutex and condition variable for sleeping during the detection of 3DConnexion devices by polling while allowing
    // cancellation before the end of the polling interval.
	std::mutex 			m_stop_condition_mutex;
   	std::condition_variable m_stop_condition;
#endif

   	// Is the ImGUI dialog shown? Accessed from UI thread only.
    mutable bool 		m_show_settings_dialog { false };
    // Set to true when ther user closes the dialog by clicking on [X] or [Close] buttons. Accessed from UI thread only.
    mutable bool 		m_settings_dialog_closed_by_user { false };

public:
	// Load the device parameter database from appconfig. To be called on application startup.
	void load_config(const AppConfig &appconfig);
	// Store the device parameter database back to appconfig. To be called on application closeup.
	void save_config(AppConfig &appconfig) const;
	// Start the background thread to detect and connect to a HID device (Windows and Linux).
	// Connect to a 3DConnextion driver (OSX).
	// Call load_config() before init().
    void init();
	// Stop the background thread (Windows and Linux).
	// Disconnect from a 3DConnextion driver (OSX).
	// Call save_config() after shutdown().
    void shutdown();

    bool connected() const { return m_connected; }

#if __APPLE__
    // Interfacing with the Objective C code (MouseHandlerMac.mm)
    void connected(std::string device_name);
    void disconnected();
    typedef std::array<double, 6> DataPacketAxis;
	// Unpack a 3DConnexion packet provided by the 3DConnexion driver into m_state. Called by Mouse3DHandlerMac.mm
    bool handle_input(const DataPacketAxis& packet);
#endif // __APPLE__

#ifdef _WIN32
	bool handle_raw_input_win32(const unsigned char *data, const int packet_lenght);

    // Called by Win32 HID enumeration callback.
    void device_attached(const std::string &device);
    void device_detached(const std::string& device);

    // On Windows, the 3DConnexion driver sends out mouse wheel rotation events to an active application
    // if the application does not register at the driver. This is a workaround to ignore these superfluous
    // mouse wheel events.
    bool process_mouse_wheel() { return m_state.process_mouse_wheel(); }
#endif // _WIN32

    // Apply the received 3DConnexion mouse events to the camera. Called from the UI rendering thread.
    bool apply(Camera& camera);

    bool is_settings_dialog_shown() const { return m_show_settings_dialog; }
    void show_settings_dialog(bool show) { m_show_settings_dialog = show && this->connected(); }
    void render_settings_dialog(GLCanvas3D& canvas) const;

#if ! __APPLE__
private:
    bool connect_device();
    void disconnect_device();

    // secondary thread methods
    void run();
    void collect_input();

    typedef std::array<unsigned char, 13> DataPacketRaw;

	// Unpack raw 3DConnexion HID packet of a wired 3D mouse into m_state. Called by the worker thread.
    static bool handle_input(const DataPacketRaw& packet, const int packet_length, const Params &params, State &state_in_out);
    // The following is called by handle_input() from the worker thread.
    static bool handle_packet(const DataPacketRaw& packet, const int packet_length, const Params &params, State &state_in_out);
    static bool handle_packet_translation(const DataPacketRaw& packet, const Params &params, State &state_in_out);
    static bool handle_packet_rotation(const DataPacketRaw& packet, unsigned int first_byte, const Params &params, State &state_in_out);
    static bool handle_packet_button(const DataPacketRaw& packet, unsigned int packet_size, const Params &params, State &state_in_out);
#endif /* __APPLE__ */
};

} // namespace GUI
} // namespace Slic3r


#endif // slic3r_Mouse3DController_hpp_
