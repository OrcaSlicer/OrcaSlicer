#ifndef slic3r_PresetUpdate_hpp_
#define slic3r_PresetUpdate_hpp_

#include <memory>
#include <vector>

#include <wx/event.h>

namespace Slic3r {


class AppConfig;
class PresetBundle;
class Semver;

static constexpr const int SLIC3R_VERSION_BODY_MAX = 256;

class PresetUpdater
{
public:
	PresetUpdater();
	PresetUpdater(PresetUpdater &&) = delete;
	PresetUpdater(const PresetUpdater &) = delete;
	PresetUpdater &operator=(PresetUpdater &&) = delete;
	PresetUpdater &operator=(const PresetUpdater &) = delete;
	~PresetUpdater();

	// If either version check or config updating is enabled, get the appropriate data in the background and cache it.
	void sync(std::string http_url, std::string language, std::string plugin_version, PresetBundle *preset_bundle);

	// If version check is enabled, check if chaced online slic3r version is newer, notify if so.
	void slic3r_update_notify();

	enum UpdateResult {
		R_NOOP,
		R_INCOMPAT_EXIT,
		R_INCOMPAT_CONFIGURED,
		R_UPDATE_INSTALLED,
		R_UPDATE_REJECT,
		R_UPDATE_NOTIFICATION,
		R_ALL_CANCELED
	};

	enum class UpdateParams {
		SHOW_TEXT_BOX,				// force modal textbox
		SHOW_NOTIFICATION,			// only shows notification
		FORCED_BEFORE_WIZARD		// indicates that check of updated is forced before ConfigWizard opening
	};

	// If updating is enabled, check if updates are available in cache, if so, ask about installation.
	// A false return value implies Slic3r should exit due to incompatibility of configuration.
	// Providing old slic3r version upgrade profiles on upgrade of an application even in case
	// that the config index installed from the Internet is equal to the index contained in the installation package.
	UpdateResult config_update(const Semver &old_slic3r_version, UpdateParams params) const;

	// "Update" a list of bundles from resources (behaves like an online update).
	bool install_bundles_rsrc(std::vector<std::string> bundles, bool snapshot = true) const;

	void on_update_notification_confirm();
    void do_printer_config_update();

	bool version_check_enabled() const;

private:
	struct priv;
	std::unique_ptr<priv> p;
};

wxDECLARE_EVENT(EVT_SLIC3R_VERSION_ONLINE, wxCommandEvent);
wxDECLARE_EVENT(EVT_SLIC3R_EXPERIMENTAL_VERSION_ONLINE, wxCommandEvent);


}
#endif
