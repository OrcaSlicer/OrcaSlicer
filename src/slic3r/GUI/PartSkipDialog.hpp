#include <wx/panel.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/gbsizer.h>
#include <wx/webrequest.h>
#include <wx/control.h>
#include <wx/dcclient.h>
#include <wx/display.h>
#include <wx/mstream.h>
#include <wx/sstream.h>
#include <wx/zstream.h>
#include <wx/window.h>
#include <wx/dcgraph.h>
#include <wx/simplebook.h>

#include "Widgets/Label.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/AnimaController.hpp"
#include "DeviceManager.hpp"
#include "PartSkipCommon.hpp"
#include "Printer/PrinterFileSystem.h"
#include "I18N.hpp"
#include "GUI_Utils.hpp"


namespace Slic3r { namespace GUI {

class SkipPartCanvas;

enum URL_STATE {
    URL_TCP,
    URL_TUTK,
};

class PartSkipConfirmDialog : public DPIDialog
{
private:
protected:
    Label    *m_msg_label;
    Button   *m_apply_button;

public:
    PartSkipConfirmDialog(wxWindow *parent);
    ~PartSkipConfirmDialog();

    void      on_dpi_changed(const wxRect &suggested_rect);
    Button*   GetConfirmButton();
    void      SetMsgLabel(wxString msg);
    bool      Show(bool show);
};

class PartSkipDialog : public DPIDialog
{
public:
    PartSkipDialog(wxWindow *parent);
    ~PartSkipDialog();
    void on_dpi_changed(const wxRect &suggested_rect);
    bool Show(bool show);

    void UpdatePartsStateFromPrinter(MachineObject *obj_);
    void SetSimplebookPage(int page);
    void InitSchedule(MachineObject *obj_);
    void InitDialogUI();


    MachineObject *m_obj{nullptr};

    wxSimplebook*   m_simplebook;
    wxPanel*        m_book_third_panel;
    wxPanel*        m_book_second_panel;
    wxPanel*        m_book_first_panel;

    SkipPartCanvas* m_canvas;
    Button*         m_zoom_in_btn;
    Button*         m_zoom_out_btn;
    Button*         m_switch_drag_btn;
    CheckBox*       m_all_checkbox;
    Button*         m_percent_label;
    Label*          m_all_label;
    wxPanel*        m_line;
    wxPanel*        m_line_top;
    wxScrolledWindow* m_list_view;

    wxPanel* m_dlg_placeholder;
    Label* m_cnt_label;
    Label* m_tot_label;

    Button* m_apply_btn;

    Label* m_loading_label;
    Label* m_retry_label;

    wxBoxSizer* m_sizer;
    wxBoxSizer* m_dlg_sizer;
    wxBoxSizer* m_dlg_content_sizer;
    wxBoxSizer* m_dlg_btn_sizer;
	wxBoxSizer* m_canvas_sizer;
    wxBoxSizer* m_canvas_btn_sizer;
    wxBoxSizer* m_list_sizer;
    wxBoxSizer* m_scroll_sizer;
    wxBoxSizer* m_book_first_sizer;
    wxBoxSizer* m_book_second_sizer;
    wxBoxSizer* m_book_second_btn_sizer;
    Button* m_second_retry_btn;
    AnimaIcon* m_loading_icon;

private:
    int m_zoom_percent{100};
    bool m_is_drag{false};
    bool m_print_lock{true};

    std::map<uint32_t, PartState> m_parts_state;
    std::map<uint32_t, std::string> m_parts_name;
    std::vector<int> m_partskip_ids;

    enum URL_STATE m_url_state = URL_STATE::URL_TCP;

    PartsInfo GetPartsInfo();
    bool is_drag_mode();

    boost::shared_ptr<PrinterFileSystem> m_file_sys;
    bool m_file_sys_result{false};
    std::string m_timestamp;
    std::string m_tmp_path;
    std::vector<string> m_local_paths;
    std::vector<string> m_target_paths;
    std::string create_tmp_path();

    bool is_local_file_existed(const std::vector<string> &local_paths);

    void DownloadPartsFile();
    void OnFileSystemEvent(wxCommandEvent &event);
    void OnFileSystemResult(wxCommandEvent &event);
    void fetchUrl(boost::weak_ptr<PrinterFileSystem> wfs);


    void OnZoomIn(wxCommandEvent &event);
    void OnZoomOut(wxCommandEvent &event);
    void OnSwitchDrag(wxCommandEvent &event);
    void OnZoomPercent(wxCommandEvent &event);
    void UpdatePartsStateFromCanvas(wxCommandEvent &event);

    void UpdateZoomPercent();
    void UpdateCountLabel();
    void UpdateDialogUI();
    bool IsAllChecked();

    void OnRetryButton(wxCommandEvent &event);
    void OnAllCheckbox(wxCommandEvent &event);
    void OnApplyDialog(wxCommandEvent &event);
};

}}