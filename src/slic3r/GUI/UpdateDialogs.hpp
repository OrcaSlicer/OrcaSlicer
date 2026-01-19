#ifndef slic3r_UpdateDialogs_hpp_
#define slic3r_UpdateDialogs_hpp_

#include <string>
#include <unordered_map>
#include <vector>
#include <wx/hyperlink.h>

#include "libslic3r/Semver.hpp"
#include "MsgDialog.hpp"

class wxBoxSizer;
class wxCheckBox;

namespace Slic3r {

namespace GUI {


// A confirmation dialog listing configuration updates
class MsgUpdateSlic3r : public MsgDialog
{
public:
	MsgUpdateSlic3r(const Semver &ver_current, const Semver &ver_online);
	MsgUpdateSlic3r(MsgUpdateSlic3r &&) = delete;
	MsgUpdateSlic3r(const MsgUpdateSlic3r &) = delete;
	MsgUpdateSlic3r &operator=(MsgUpdateSlic3r &&) = delete;
	MsgUpdateSlic3r &operator=(const MsgUpdateSlic3r &) = delete;
	virtual ~MsgUpdateSlic3r();

	// Tells whether the user checked the "don't bother me again" checkbox
	bool disable_version_check() const;

	void on_hyperlink(wxHyperlinkEvent& evt);
private:
	wxCheckBox *cbox;
};


// Confirmation dialog informing about configuration update. Lists updated bundles & their versions.
class MsgUpdateConfig : public DPIDialog
{
public:
	struct Update
	{
		std::string vendor;
		Semver version;
		std::string comment;
		//BBS: use changelog string instead of url
		std::string change_log;

        //BBS: use changelog string instead of url
		Update(std::string vendor, Semver version, std::string comment, std::string changelog)
			: vendor(std::move(vendor))
			, version(std::move(version))
			, comment(std::move(comment))
			, change_log(std::move(changelog))
		{}
	};

	// force_before_wizard - indicates that check of updated is forced before ConfigWizard opening
    MsgUpdateConfig(const std::vector<Update> &updates, bool force_before_wizard = false);
    void on_dpi_changed(const wxRect &suggested_rect);
    // MsgUpdateConfig(MsgUpdateConfig &&)      = delete;
    //MsgUpdateConfig(const MsgUpdateConfig &) = delete;
    //MsgUpdateConfig &operator=(MsgUpdateConfig &&) = delete;
    //MsgUpdateConfig &operator=(const MsgUpdateConfig &) = delete;
	~MsgUpdateConfig();
};

// Informs about currently installed bundles not being compatible with the running Slic3r. Asks about action.
class MsgUpdateForced : public MsgDialog
{
public:
	struct Update
	{
		std::string vendor;
		Semver version;
		std::string comment;
		//BBS: use changelog string instead of url
		std::string change_log;

		//BBS: use changelog string instead of url
		Update(std::string vendor, Semver version, std::string comment, std::string changelog)
			: vendor(std::move(vendor))
			, version(std::move(version))
			, comment(std::move(comment))
			, change_log(std::move(changelog))
		{}
	};

	MsgUpdateForced(const std::vector<Update>& updates);
	MsgUpdateForced(MsgUpdateForced&&) = delete;
	MsgUpdateForced(const MsgUpdateForced&) = delete;
	MsgUpdateForced& operator=(MsgUpdateForced&&) = delete;
	MsgUpdateForced& operator=(const MsgUpdateForced&) = delete;
	~MsgUpdateForced();
};

// Informs about currently installed bundles not being compatible with the running Slic3r. Asks about action.
class MsgDataIncompatible : public MsgDialog
{
public:
	// incompats is a map of "vendor name" -> "version restrictions"
	MsgDataIncompatible(const std::unordered_map<std::string, wxString> &incompats);
	MsgDataIncompatible(MsgDataIncompatible &&) = delete;
	MsgDataIncompatible(const MsgDataIncompatible &) = delete;
	MsgDataIncompatible &operator=(MsgDataIncompatible &&) = delete;
	MsgDataIncompatible &operator=(const MsgDataIncompatible &) = delete;
	~MsgDataIncompatible();
};

// Informs about a legacy data directory - an update from Slic3r PE < 1.40
/*class MsgDataLegacy : public MsgDialog
{
public:
	MsgDataLegacy();
	MsgDataLegacy(MsgDataLegacy &&) = delete;
	MsgDataLegacy(const MsgDataLegacy &) = delete;
	MsgDataLegacy &operator=(MsgDataLegacy &&) = delete;
	MsgDataLegacy &operator=(const MsgDataLegacy &) = delete;
	~MsgDataLegacy();
};*/

// Informs about absence of bundles requiring update.
class MsgNoUpdates : public MsgDialog
{
public:
	MsgNoUpdates();
	MsgNoUpdates(MsgNoUpdates&&) = delete;
	MsgNoUpdates(const MsgNoUpdates&) = delete;
	MsgNoUpdates& operator=(MsgNoUpdates&&) = delete;
	MsgNoUpdates& operator=(const MsgNoUpdates&) = delete;
	~MsgNoUpdates();
};

}
}

#endif
