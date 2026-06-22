#ifndef slic3r_MakerbotDiscoveryDialog_hpp_
#define slic3r_MakerbotDiscoveryDialog_hpp_

// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// =============================================================================
// Network discovery dialog for MakerBot and UltiMaker printers.
//
// Discovery strategy (in order):
//   1. mDNS / DNS-SD browse for _makerbot._tcp and _ultimaker._tcp
//   2. UDP broadcast on port 12307 (MakerBot Birdwing legacy beacon)
//
// The dialog shows a list of discovered printers with:
//   Name | Model | IP Address | Protocol
//
// Selecting a printer and clicking OK sets the IP in the Physical Printer
// dialog and auto-selects the correct Host Type.
// =============================================================================

#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/timer.h>

#include <string>
#include <vector>
#include <atomic>
#include <thread>

namespace Slic3r {
namespace GUI {

class MakerbotDiscoveryDialog : public wxDialog
{
public:
    // filter: 0 = MakerBot only, 1 = UltiMaker only, 2 = both
    explicit MakerbotDiscoveryDialog(wxWindow* parent, int filter = 2);
    ~MakerbotDiscoveryDialog() override;

    struct DiscoveredPrinter {
        std::string ip;
        std::string name;
        std::string model;
        std::string protocol;    // "MakerBot Birdwing (SSL:12309)"
                                 // "MakerBot Lava (HTTP:2222)"
                                 // "UltiMaker (REST:80)"
        int         port { 0 };
        bool        is_makerbot  { false };
        bool        is_ultimaker { false };
        bool        is_birdwing  { false };
        bool        is_method    { false };
    };

    std::string selected_ip()       const { return m_selected_ip; }
    std::string selected_model()    const { return m_selected_model; }
    bool        selected_is_birdwing() const { return m_selected_birdwing; }
    bool        selected_is_method()   const { return m_selected_method; }

private:
    void start_discovery();
    void stop_discovery();
    void add_result(const DiscoveredPrinter& p);
    void on_ok(wxCommandEvent&);
    void EndModal_if_selected();
    void on_rescan(wxCommandEvent&);
    void on_timer(wxTimerEvent&);

    void discover_mdns_makerbot(std::vector<DiscoveredPrinter>& out);
    void discover_mdns_ultimaker(std::vector<DiscoveredPrinter>& out);
    void discover_udp_broadcast(std::vector<DiscoveredPrinter>& out);
    void discover_subnet_scan(std::vector<DiscoveredPrinter>& out);

    wxListCtrl*  m_list   { nullptr };
    wxStaticText* m_status { nullptr };
    wxButton*    m_ok     { nullptr };
    wxButton*    m_rescan { nullptr };
    wxTimer      m_timer;

    int m_filter;
    std::vector<DiscoveredPrinter> m_results;
    std::thread  m_thread;
    std::atomic<bool> m_stop { false };

    std::string m_selected_ip;
    std::string m_selected_model;
    bool        m_selected_birdwing { false };
    bool        m_selected_method   { false };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_MakerbotDiscoveryDialog_hpp_
