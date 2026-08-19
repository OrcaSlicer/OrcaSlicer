#ifndef slic3r_CrealityDiscoveryDialog_hpp_
#define slic3r_CrealityDiscoveryDialog_hpp_

#include <wx/dialog.h>

#include <string>
#include <vector>

class wxListView;
class wxStaticText;

namespace Slic3r {
namespace GUI {

// Modal dialog that finds Creality K-series printers on the LAN via DNS-SD
// mDNS (vendored mjansson/mdns + cxmdns wrapper) and lets the user pick one.
//
// Discovery is synchronous (~5 second mDNS listen + ~2-4 sec /info probe per
// host) and runs during ShowModal() with a busy cursor. The user-facing busy
// time is bounded by the mDNS listener's 5-second deadline plus any in-flight
// probes; in practice 5-10 seconds total for a typical LAN with one K2.
//
// After ShowModal() returns wxID_OK, selected_ip() yields the chosen
// printer's IPv4 address. Returns the empty string on Cancel or if no match
// was found.
class CrealityDiscoveryDialog : public wxDialog
{
public:
    CrealityDiscoveryDialog(wxWindow* parent);
    ~CrealityDiscoveryDialog() override = default;

    std::string selected_ip() const { return m_selected_ip; }

private:
    void run_discovery();
    void on_ok();

    struct Row { std::string ip; std::string model; std::string hostname; };

    wxListView*   m_list = nullptr;
    wxStaticText* m_status = nullptr;
    std::vector<Row> m_rows;
    std::string   m_selected_ip;
};

} // namespace GUI
} // namespace Slic3r

#endif
