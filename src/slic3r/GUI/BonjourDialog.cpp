#include "slic3r/Utils/Bonjour.hpp"   // On Windows, boost needs to be included before wxWidgets headers

#include "BonjourDialog.hpp"

#include <set>
#include <mutex>

#include <boost/nowide/convert.hpp>

#include <wx/sizer.h>
#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/wupdlock.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/Bonjour.hpp"

namespace Slic3r {


class BonjourReplyEvent : public wxEvent
{
public:
	BonjourReply reply;

	BonjourReplyEvent(wxEventType eventType, int winid, BonjourReply &&reply) :
		wxEvent(winid, eventType),
		reply(std::move(reply))
	{}

	virtual wxEvent *Clone() const
	{
		return new BonjourReplyEvent(*this);
	}
};

wxDEFINE_EVENT(EVT_BONJOUR_REPLY, BonjourReplyEvent);

wxDECLARE_EVENT(EVT_BONJOUR_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(EVT_BONJOUR_COMPLETE, wxCommandEvent);

class ReplySet: public std::set<BonjourReply> {};

struct LifetimeGuard
{
	std::mutex mutex;
	BonjourDialog *dialog;

	LifetimeGuard(BonjourDialog *dialog) : dialog(dialog) {}
};

BonjourDialog::BonjourDialog(wxWindow *parent, Slic3r::PrinterTechnology tech)
	: wxDialog(parent, wxID_ANY, _(L("Network lookup")), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER)
	, list(new wxListView(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT|wxSIMPLE_BORDER))
	, replies(new ReplySet)
	, label(new wxStaticText(this, wxID_ANY, ""))
	, timer(new wxTimer())
	, timer_state(0)
	, tech(tech)
{
	const int em = GUI::wxGetApp().em_unit();
	list->SetMinSize(wxSize(80 * em, 30 * em));

	wxBoxSizer *vsizer = new wxBoxSizer(wxVERTICAL);

	vsizer->Add(label, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, em);

	list->SetSingleStyle(wxLC_SINGLE_SEL);
	list->SetSingleStyle(wxLC_SORT_DESCENDING);
	list->AppendColumn(_(L("Address")), wxLIST_FORMAT_LEFT, 5 * em);
	list->AppendColumn(_(L("Hostname")), wxLIST_FORMAT_LEFT, 10 * em);
	list->AppendColumn(_(L("Service name")), wxLIST_FORMAT_LEFT, 20 * em);
	if (tech == ptFFF) {
		list->AppendColumn(_(L("OctoPrint version")), wxLIST_FORMAT_LEFT, 5 * em);
	}

	vsizer->Add(list, 1, wxEXPAND | wxALL, em);

	wxBoxSizer *button_sizer = new wxBoxSizer(wxHORIZONTAL);
	button_sizer->Add(new wxButton(this, wxID_OK, _L("OK")), 0, wxALL, em);
	button_sizer->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), 0, wxALL, em);
	// ^ Note: The Ok/Cancel labels are translated by wxWidgets

	vsizer->Add(button_sizer, 0, wxALIGN_CENTER);
	SetSizerAndFit(vsizer);

	Bind(EVT_BONJOUR_REPLY, &BonjourDialog::on_reply, this);

	Bind(EVT_BONJOUR_COMPLETE, [this](wxCommandEvent &) {
		this->timer_state = 0;
	});

	Bind(wxEVT_TIMER, &BonjourDialog::on_timer, this);
	GUI::wxGetApp().UpdateDlgDarkUI(this);
}

BonjourDialog::~BonjourDialog()
{
	// Needed bacuse of forward defs
}

bool BonjourDialog::show_and_lookup()
{
	Show();   // Because we need GetId() to work before ShowModal()

	timer->Stop();
	timer->SetOwner(this);
	timer_state = 1;
	timer->Start(1000);
    on_timer_process();

	// The background thread needs to queue messages for this dialog
	// and for that it needs a valid pointer to it (mandated by the wxWidgets API).
	// Here we put the pointer under a shared_ptr and protect it by a mutex,
	// so that both threads can access it safely.
	auto dguard = std::make_shared<LifetimeGuard>(this);

	// Note: More can be done here when we support discovery of hosts other than Octoprint and SL1
	Bonjour::TxtKeys txt_keys { "version", "model" };

    bonjour = Bonjour("octoprint")
		.set_txt_keys(std::move(txt_keys))
		.set_retries(3)
		.set_timeout(4)
		.on_reply([dguard](BonjourReply &&reply) {
			std::lock_guard<std::mutex> lock_guard(dguard->mutex);
			auto dialog = dguard->dialog;
			if (dialog != nullptr) {
				auto evt = new BonjourReplyEvent(EVT_BONJOUR_REPLY, dialog->GetId(), std::move(reply));
				wxQueueEvent(dialog, evt);
			}
		})
		.on_complete([dguard]() {
			std::lock_guard<std::mutex> lock_guard(dguard->mutex);
			auto dialog = dguard->dialog;
			if (dialog != nullptr) {
				auto evt = new wxCommandEvent(EVT_BONJOUR_COMPLETE, dialog->GetId());
				wxQueueEvent(dialog, evt);
			}
		})
		.lookup();

	bool res = ShowModal() == wxID_OK && list->GetFirstSelected() >= 0;
	{
		// Tell the background thread the dialog is going away...
		std::lock_guard<std::mutex> lock_guard(dguard->mutex);
		dguard->dialog = nullptr;
	}
	return res;
}

wxString BonjourDialog::get_selected() const
{
	auto sel = list->GetFirstSelected();
	return sel >= 0 ? list->GetItemText(sel) : wxString();
}


// Private

void BonjourDialog::on_reply(BonjourReplyEvent &e)
{
	if (replies->find(e.reply) != replies->end()) {
		// We already have this reply
		return;
	}

	// Filter replies based on selected technology
	const auto model = e.reply.txt_data.find("model");
	const bool sl1 = model != e.reply.txt_data.end() && model->second == "SL1";
	if ((tech == ptFFF && sl1) || (tech == ptSLA && !sl1)) {
		return;
	}

	replies->insert(std::move(e.reply));

	auto selected = get_selected();

	wxWindowUpdateLocker freeze_guard(this);
	(void)freeze_guard;

	list->DeleteAllItems();

	// The whole list is recreated so that we benefit from it already being sorted in the set.
	// (And also because wxListView's sorting API is bananas.)
	for (const auto &reply : *replies) {
		auto item = list->InsertItem(0, reply.full_address);
		list->SetItem(item, 1, reply.hostname);
		list->SetItem(item, 2, reply.service_name);

		if (tech == ptFFF) {
			const auto it = reply.txt_data.find("version");
			if (it != reply.txt_data.end()) {
				list->SetItem(item, 3, GUI::from_u8(it->second));
			}
		}
	}

	const int em = GUI::wxGetApp().em_unit();

	for (int i = 0; i < list->GetColumnCount(); i++) {
		list->SetColumnWidth(i, wxLIST_AUTOSIZE);
		if (list->GetColumnWidth(i) < 10 * em) { list->SetColumnWidth(i, 10 * em); }
	}

	if (!selected.IsEmpty()) {
		// Attempt to preserve selection
		auto hit = list->FindItem(-1, selected);
		if (hit >= 0) { list->SetItemState(hit, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED); }
	}
}

void BonjourDialog::on_timer(wxTimerEvent &)
{
    on_timer_process();
}

// This is here so the function can be bound to wxEVT_TIMER and also called
// explicitly (wxTimerEvent should not be created by user code).
void BonjourDialog::on_timer_process()
{
    const auto search_str = _L("Searching for devices");

    if (timer_state > 0) {
        const std::string dots(timer_state, '.');
        label->SetLabel(search_str + dots);
        timer_state = (timer_state) % 3 + 1;
    } else {
        label->SetLabel(search_str + ": " + _L("Finished") + ".");
        timer->Stop();
    }
}

IPListDialog::IPListDialog(wxWindow* parent, const wxString& hostname, const std::vector<boost::asio::ip::address>& ips, size_t& selected_index)
	: wxDialog(parent, wxID_ANY, _(L("Multiple resolved IP addresses")), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, m_list(new wxListView(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxSIMPLE_BORDER))
	, m_selected_index (selected_index)
{
	const int em = GUI::wxGetApp().em_unit();
	m_list->SetMinSize(wxSize(40 * em, 30 * em));

	wxBoxSizer* vsizer = new wxBoxSizer(wxVERTICAL);

	auto* label = new wxStaticText(this, wxID_ANY, GUI::format_wxstr(_L("There are several IP addresses resolving to hostname %1%.\nPlease select one that should be used."), hostname));
	vsizer->Add(label, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, em);

	m_list->SetSingleStyle(wxLC_SINGLE_SEL);
	m_list->AppendColumn(_(L("Address")), wxLIST_FORMAT_LEFT, 40 * em);

	for (size_t i = 0; i < ips.size(); i++) 
		m_list->InsertItem(i, boost::nowide::widen(ips[i].to_string()));

	m_list->Select(0);

	vsizer->Add(m_list, 1, wxEXPAND | wxALL, em);

	wxBoxSizer* button_sizer = new wxBoxSizer(wxHORIZONTAL);
	button_sizer->Add(new wxButton(this, wxID_OK, _L("OK")), 0, wxALL, em);
	button_sizer->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), 0, wxALL, em);

	vsizer->Add(button_sizer, 0, wxALIGN_CENTER);
	SetSizerAndFit(vsizer);

	GUI::wxGetApp().UpdateDlgDarkUI(this);
}

IPListDialog::~IPListDialog()
{
}

void IPListDialog::EndModal(int retCode)
{
	if (retCode == wxID_OK) {
		m_selected_index = (size_t)m_list->GetFirstSelected();
	}
	wxDialog::EndModal(retCode);
}

}
