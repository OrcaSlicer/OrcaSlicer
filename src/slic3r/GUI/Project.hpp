#ifndef slic3r_Project_hpp_
#define slic3r_Project_hpp_

#include "Tabbook.hpp"
#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include "wx/webview.h"

#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "wx/numdlg.h"
#include "wx/infobar.h"
#include "wx/filesys.h"
#include "wx/fs_arc.h"
#include "wx/fs_mem.h"
#include "wx/stdpaths.h"
#include <wx/panel.h>
#include <wx/tbarbase.h>
#include "wx/textctrl.h"
#include <wx/timer.h>

#include "nlohmann/json.hpp"
#include "slic3r/Utils/json_diff.hpp"

#include <map>
#include <vector>
#include <memory>
#include "Event.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "wxExtensions.hpp"
#include "Auxiliary.hpp"

#define AUFILE_GREY700 wxColour(107, 107, 107)
#define AUFILE_GREY500 wxColour(158, 158, 158)
#define AUFILE_GREY300 wxColour(238, 238, 238)
#define AUFILE_GREY200 wxColour(248, 248, 248)
#define AUFILE_BRAND wxColour(0, 150, 136)
#define AUFILE_BRAND_TRANSPARENT wxColour("#E5F0EE") // ORCA color with %10 opacity
//#define AUFILE_PICTURES_SIZE wxSize(FromDIP(300), FromDIP(300))
//#define AUFILE_PICTURES_PANEL_SIZE wxSize(FromDIP(300), FromDIP(340))
#define AUFILE_PICTURES_SIZE wxSize(FromDIP(168), FromDIP(168))
#define AUFILE_PICTURES_PANEL_SIZE wxSize(FromDIP(168), FromDIP(208))
#define AUFILE_SIZE wxSize(FromDIP(168), FromDIP(168))
#define AUFILE_PANEL_SIZE wxSize(FromDIP(168), FromDIP(208))
#define AUFILE_TEXT_HEIGHT FromDIP(40)
#define AUFILE_ROUNDING FromDIP(5)

namespace Slic3r { namespace GUI {

struct project_file{
    std::string filepath;
    std::string filename;
    std::string size;
};

class ProjectPanel : public wxPanel
{
private:
    bool       m_web_init_completed = {false};
    bool       m_reload_already = {false};

    wxWebView* m_browser = {nullptr};
    AuxiliaryPanel*   m_auxiliary{nullptr};
    wxString   m_project_home_url;
    wxString   m_root_dir;
    static inline int m_sequence_id = 8000;

    void show_info_editor(bool show);
    

public:
    ProjectPanel(wxWindow *parent, wxWindowID id = wxID_ANY, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize, long style = wxTAB_TRAVERSAL);
    ~ProjectPanel();

    
    void onWebNavigating(wxWebViewEvent& evt);
    void on_reload(wxCommandEvent& evt);
    void on_size(wxSizeEvent &event);
    void on_navigated(wxWebViewEvent& event);
   
    void msw_rescale();
    void update_model_data();
    void clear_model_info();
    void init_auxiliary() { m_auxiliary->init_auxiliary(); }

    bool Show(bool show);
    void OnScriptMessage(wxWebViewEvent& evt);
    void RunScript(std::string content);

    std::map<std::string, std::vector<json>> Reload(wxString aux_path);
    std::string formatBytes(unsigned long bytes);
    wxString to_base64(std::string path);
};

wxDECLARE_EVENT(EVT_PROJECT_RELOAD, wxCommandEvent);
}} // namespace Slic3r::GUI

#endif
