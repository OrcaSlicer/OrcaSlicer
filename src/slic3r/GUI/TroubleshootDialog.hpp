#ifndef slic3r_GUI_TroublesootDialog_hpp_
#define slic3r_GUI_TroublesootDialog_hpp_

#include <wx/wx.h>
#include <wx/zipstrm.h>
#include <wx/dir.h>
#include <wx/dcbuffer.h>

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"

#include "Widgets/Label.hpp"
#include "Widgets/ComboBox.hpp"

#include <vector>
#include <list>
#include <map>

namespace Slic3r {
namespace GUI {

class TroubleshootDialog : public DPIDialog
{

#define DESIGN_COMBOBOX_SIZE wxSize(FromDIP(120), -1)

public:
    TroubleshootDialog();
     //~TroubleshootDialog();

private:
    ScalableBitmap   m_logo;
    wxStaticBitmap*  m_header_logo;
    Label*           m_logs_storage;
    int              m_printers__act;
    int              m_printers__usr;
    int              m_filaments_act;
    int              m_filaments_usr;
    int              m_processes_act;
    int              m_processes_usr;
    bool             m_sys_panel_mode = true;

protected:
    wxFlexGridSizer* create_item_loaded_profiles();
    ComboBox*        create_item_log_level_combo();

    wxString GetTimestamp();

    wxString GetSysInfoAll();
    //wxString GetConfigStr();
    wxString GetProfilesOverview();
    wxString GetOStype();
    wxString GetOSinfo();
#ifdef __WINDOWS__
    wxString GetWinVersion();
    wxString GetWinDisplayVersion();
#elif defined(__LINUX__)
    wxString GetLinuxDistroName();
    wxString GetLinuxDisplayServer();
#endif
    wxString GetPackageType();
    wxString GetCPUinfo();
    wxString GetGPUinfo();
    wxString GetRAMinfo();
    wxString GetMONinfo();

    void     PackAll();
    void     RebuildSystemProfiles();
    bool     RestartApplication();
    void     ClearLogs();
    void     UpdateLogsStorage();

    void     BrowseFolder(std::string path);

#ifdef __WINDOWS__
    static wxString get_cpu_info_from_registry();
#else
    static std::map<std::string, std::string> parse_lscpu_etc(const std::string& name, char delimiter);
#endif

    bool     ExportAsJson(const wxString& json_data, const wxString& export_name = wxEmptyString);
    bool     ExportAsZip(const std::vector<wxString>& sources, const wxString& export_name);
    bool     AddToZip(wxZipOutputStream& zip, const wxString& fullPathOrTextData, const wxString& rootDir);
    bool     SaveAsZip(const std::vector<wxString>& sourcePaths, const wxString& zipFullPath);

    void     on_dpi_changed(const wxRect &suggested_rect) override;
};

class CenteredMultiLinePanel : public wxPanel
{
    static constexpr double m_block_gap   = 0.55;
    static constexpr double m_line_height = 1.15;
    std::vector<wxString>   m_lines;

public:
    CenteredMultiLinePanel(wxWindow* parent, const std::vector<wxString>& lines = {})
      : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE)
    {
        SetFont(Label::Body_14);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &CenteredMultiLinePanel::OnPaint, this);
        Bind(wxEVT_SIZE,  &CenteredMultiLinePanel::OnSize,  this);

        SetText(lines);
    }
    
    void SetText(const std::vector<wxString>& lines)
    {
        if(!lines.empty()){
            m_lines = lines;
            Refresh();
            UpdateMinSize();
        }
    }
private:
    std::vector<wxString> Wrap(wxDC& dc, const wxString& text, int maxW)
    {
        std::vector<wxString> out;
        wxString cur;
        for (const auto& word : wxSplit(text, ' ', true)) {
            wxString trial = cur.empty() ? word : cur + " " + word;
            wxCoord w, h;
            dc.GetTextExtent(trial, &w, &h);
            if (w <= maxW)
                cur = trial;
            else if (!cur.empty()){
                out.push_back(cur);
                cur = word;
            }
            else 
                out.push_back(word);
        }
        if (!cur.empty())
            out.push_back(cur);
        return out;
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.Clear();

        int cWidth = GetClientSize().GetWidth();
        if (m_lines.empty() || cWidth < 50) return;

        dc.SetTextForeground(GetForegroundColour());

        int y = 0;
        for (size_t i = 0; i < m_lines.size(); ++i) {
            wxCoord tw, th;
            for (auto& line : Wrap(dc, m_lines[i], cWidth)) {
                line.Trim();
                line.Trim(false);
                dc.GetTextExtent(line, &tw, &th);
                dc.DrawText(line, (cWidth - tw) / 2, y);
                y += static_cast<int>(th * m_line_height);
            }
            if (i < m_lines.size() - 1)
                y += static_cast<int>(th * m_block_gap);
        }
    }

    void UpdateMinSize()
    {
        if (m_lines.empty() || !GetParent()) {
            SetMinSize(wxDefaultSize);
            return;
        }
        wxClientDC dc(this);
        int cWidth = GetClientSize().GetWidth();

        int y = 0;
        for (size_t i = 0; i < m_lines.size(); ++i) {
            wxCoord th;
            for (auto& line : Wrap(dc, m_lines[i], cWidth)) {
                dc.GetTextExtent(line, nullptr, &th);
                y += static_cast<int>(th * m_line_height);
            }
            if (i < m_lines.size() - 1)
                y += static_cast<int>(th * m_block_gap);
        }

        if(GetMinSize().GetHeight() != y){
            SetMinSize(wxSize(-1, y));
            if(GetParent())
                GetParent()->Layout();
        }
    }

    void OnSize(wxSizeEvent& e)
    {
        Refresh();
        UpdateMinSize();
        e.Skip();
    }
};

} // namespace GUI
} // namespace Slic3r

#endif
