#ifndef slic3r_GUI_RemovableDriveManager_hpp_
#define slic3r_GUI_RemovableDriveManager_hpp_

#include <vector>
#include <string>

#include <boost/thread.hpp>
#include <mutex>
#include <condition_variable>

// Custom wxWidget events
#include "Event.hpp"

namespace Slic3r {
namespace GUI {

struct DriveData
{
	std::string name;
	std::string path;

	void clear() {
		name.clear();
		path.clear();
	}
	bool empty() const {
		return path.empty();
	}
};

inline bool operator< (const DriveData &lhs, const DriveData &rhs) { return lhs.path < rhs.path; }
inline bool operator> (const DriveData &lhs, const DriveData &rhs) { return lhs.path > rhs.path; }
inline bool operator==(const DriveData &lhs, const DriveData &rhs) { return lhs.path == rhs.path; }

using RemovableDriveEjectEvent = Event<std::pair<DriveData, bool>>;
wxDECLARE_EVENT(EVT_REMOVABLE_DRIVE_EJECTED, RemovableDriveEjectEvent);

using RemovableDrivesChangedEvent = SimpleEvent;
wxDECLARE_EVENT(EVT_REMOVABLE_DRIVES_CHANGED, RemovableDrivesChangedEvent);

#if __APPLE__
	// Callbacks on device plug / unplug work reliably on OSX.
	#define REMOVABLE_DRIVE_MANAGER_OS_CALLBACKS
#endif // __APPLE__

class RemovableDriveManager
{
public:
	RemovableDriveManager() = default;
	RemovableDriveManager(RemovableDriveManager const&) = delete;
	void operator=(RemovableDriveManager const&) = delete;
	~RemovableDriveManager() { /*assert(! m_initialized);*/ }

	// Start the background thread and register this window as a target for update events.
	// Register for OSX notifications.
	void 		init(wxEvtHandler *callback_evt_handler);
	// Stop the background thread of the removable drive manager, so that no new updates will be sent out.
	// Deregister OSX notifications.
	void 		shutdown();

	// Returns path to a removable media if it exists, prefering the input path.
	std::string get_removable_drive_path(const std::string &path);
	bool        is_path_on_removable_drive(const std::string &path) { return this->get_removable_drive_path(path) == path; }

	// Verify whether the path provided is on removable media. If so, save the path for further eject and return true, otherwise return false.
	bool 		set_and_verify_last_save_path(const std::string &path);
	// Eject drive of a file set by set_and_verify_last_save_path().
	// On Unix / OSX, the function blocks and sends out the EVT_REMOVABLE_DRIVE_EJECTED event on success.
	// On Windows, the function does not block, and the eject is detected in the background thread.
	void 		eject_drive();

	// Status is used to retrieve info for showing UI buttons.
	// Status is called every time when change of UI buttons is possible therefore should not perform update.
	struct RemovableDrivesStatus {
		bool 	has_removable_drives { false };
		bool 	has_eject { false };
	};
	RemovableDrivesStatus status();

	// Enumerates current drives and sends out wxWidget events on change or eject.
	// Called by each public method, by the background thread and from RemovableDriveManagerMM::on_device_unmount OSX notification handler.
	// Not to be called manually.
	// Public to be accessible from RemovableDriveManagerMM::on_device_unmount OSX notification handler.
	// It would be better to make this method private and friend to RemovableDriveManagerMM, but RemovableDriveManagerMM is an ObjectiveC class.
	void 		update();
	void        set_exporting_finished(bool b) { m_exporting_finished = b; }
#ifdef _WIN32
    // Called by Win32 Volume arrived / detached callback.
	void 		volumes_changed();
#endif // _WIN32

private:
	bool 			 		m_initialized { false };
	wxEvtHandler*			m_callback_evt_handler { nullptr };

#ifndef REMOVABLE_DRIVE_MANAGER_OS_CALLBACKS
	// Worker thread, worker thread synchronization and callbacks to the UI thread.
	void 					thread_proc();
	boost::thread 			m_thread;
	std::condition_variable m_thread_stop_condition;
	mutable std::mutex 		m_thread_stop_mutex;
	bool 					m_stop { false };
#ifdef _WIN32
    std::atomic<bool>		m_wakeup { false };
#endif /* _WIN32 */
#endif // REMOVABLE_DRIVE_MANAGER_OS_CALLBACKS

	// Called from update() to enumerate removable drives.
	std::vector<DriveData> 	search_for_removable_drives() const;

	// m_current_drives is guarded by m_drives_mutex
	// sorted ascending by path
	std::vector<DriveData> 	m_current_drives;
	mutable std::mutex 		m_drives_mutex;
	// Locking the update() function to avoid that the function is executed multiple times.
	mutable std::mutex 		m_inside_update_mutex;

	// Returns drive path (same as path in DriveData) if exists otherwise empty string.
	std::string 			get_removable_drive_from_path(const std::string& path);
	// Returns iterator to a drive in m_current_drives with path equal to m_last_save_path or end().
	std::vector<DriveData>::const_iterator find_last_save_path_drive_data() const;
	// Set with set_and_verify_last_save_path() to a removable drive path to be ejected.
	std::string 			m_last_save_path;
	// Verifies that exporting was finished so drive can be ejected.
	// Set false by set_and_verify_last_save_path() that is called just before exporting.
	bool                    m_exporting_finished;
#if __APPLE__
    void register_window_osx();
    void unregister_window_osx();
    void list_devices(std::vector<DriveData> &out) const;
    // not used as of now
    void eject_device(const std::string &path);
    // Opaque pointer to RemovableDriveManagerMM
    void *m_impl_osx;
    boost::thread *m_eject_thread { nullptr };
    void eject_thread_finish();
#endif
};

}}

#endif // slic3r_GUI_RemovableDriveManager_hpp_
