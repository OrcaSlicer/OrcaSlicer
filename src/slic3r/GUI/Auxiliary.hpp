#ifndef slic3r_Auxiliary_hpp_
#define slic3r_Auxiliary_hpp_

#include "Tabbook.hpp"
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/bmpcbox.h>
#include <wx/bmpbuttn.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>
#include <wx/artprov.h>
#include <wx/xrc/xmlres.h>
#include <wx/string.h>
#include <wx/stattext.h>
#include <wx/gdicmn.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/grid.h>
#include <wx/dataview.h>
#include <wx/panel.h>
#include <wx/statline.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/icon.h>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/gbsizer.h>
#include <wx/statbox.h>
#include <wx/tglbtn.h>
#include <wx/popupwin.h>
#include <wx/spinctrl.h>
#include <wx/artprov.h>
#include <wx/webrequest.h>
#include <map>
#include <vector>
#include <memory>
#include "Event.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "wxExtensions.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/MonitorBasePanel.h"
#include "slic3r/GUI/StatusPanel.hpp"
#include "slic3r/GUI/UpgradePanel.hpp"
#include "slic3r/GUI/AmsWidgets.hpp"
#include "Widgets/SideTools.hpp"

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

enum AuxiliaryFolderType {
    MODEL_PICTURE,
    BILL_OF_MATERIALS,
    ASSEMBLY_GUIDE,
    OTHERS,
    THUMBNAILS,
    DESIGNER,
    AddFileButton,
};

const static std::array<wxString, 5> s_default_folders = {("Model Pictures"), ("Bill of Materials"), ("Assembly Guide"), ("Others"), (".thumbnails")};


enum ValidationType { Valid, NoValid, Warning };

namespace Slic3r { namespace GUI {

class AuFile : public wxPanel
{
public:
    AuxiliaryFolderType m_type;
    bool                m_hover{false};
    bool                m_cover{false};
    wxStaticText*       m_text_name {nullptr};
    ::TextInput*        m_input_name {nullptr};
    fs::path m_file_path;
    wxString m_add_file;
    wxString m_file_name;
    wxString cover_text_left;
    wxString cover_text_right;
    wxString cover_text_cover;
    ScalableBitmap m_file_bitmap;
    ScalableBitmap m_file_cover;
    ScalableBitmap m_file_edit_mask;
    ScalableBitmap m_file_delete;
    wxStaticBitmap* m_file_exit_rename;

    ScalableBitmap m_bitmap_excel;
    ScalableBitmap m_bitmap_pdf;
    ScalableBitmap m_bitmap_txt;

public:
    AuFile(wxWindow *parent, fs::path file_path, wxString file_name, AuxiliaryFolderType type, wxWindowID id = wxID_ANY, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize, long style = wxTAB_TRAVERSAL);
    void enter_rename_mode();
    void exit_rename_mode();
    void OnPaint(wxPaintEvent &evt);
    void PaintBackground(wxDC &dc);
    void OnEraseBackground(wxEraseEvent &evt);
    void PaintForeground(wxDC &dc);
    void on_mouse_enter(wxMouseEvent &evt);
    void on_mouse_leave(wxMouseEvent &evt);
    void on_input_enter(wxCommandEvent& evt);
    void on_dclick(wxMouseEvent &evt);
    void on_mouse_left_up(wxMouseEvent &evt);

    void on_set_cover();
    void on_set_delete();
    void on_set_rename();
    void on_set_open();

    void set_cover(bool cover);
    void msw_rescale();
    ~AuFile();
};

class AuFiles
{
public:
    wxString path;
    AuFile * file;
};

WX_DEFINE_ARRAY(AuFiles *, AuFilesHash);

class AuFolderPanel : public wxPanel
{
public:
    AuFolderPanel(wxWindow *          parent,
                  AuxiliaryFolderType type,
                  wxWindowID          id    = wxID_ANY,
                  const wxPoint &     pos   = wxDefaultPosition,
                  const wxSize &      size  = wxDefaultSize,
                  long                style = wxTAB_TRAVERSAL);
     ~AuFolderPanel();



    
    void clear();
    void update_cover();
    void update(std::vector<fs::path> paths);
    void msw_rescale();

public:
    AuxiliaryFolderType m_type;
    wxScrolledWindow *  m_scrolledWindow{nullptr};
    wxWrapSizer *       m_gsizer_content{nullptr};
    //AuFile *            m_button_add{nullptr};
    Button *            m_button_del{nullptr};
    AuFile *            m_big_button_add{ nullptr };
    AuFilesHash         m_aufiles_list;

    void on_add(wxMouseEvent& event);
    void on_delete(wxCommandEvent &event);
};

class DesignerPanel : public wxPanel
{
public:
    DesignerPanel(wxWindow *          parent,
                  AuxiliaryFolderType type,
                  wxWindowID          id    = wxID_ANY,
                  const wxPoint &     pos   = wxDefaultPosition,
                  const wxSize &      size  = wxDefaultSize,
                  long                style = wxTAB_TRAVERSAL);
    ~DesignerPanel();

    ::TextInput*        m_input_designer {nullptr};
    ::TextInput*        m_input_model_name {nullptr};
    wxTextCtrl*         m_input_description {nullptr};
    ComboBox*           m_combo_license {nullptr};
    bool Show(bool show) override;
    void                init_license_list();
    void                on_input_enter_designer(wxCommandEvent &evt);
    void                on_input_enter_model(wxCommandEvent &evt);
    void                on_input_enter_description(wxCommandEvent &evt);
    void                on_select_license(wxCommandEvent& evt);
    void                update_info();
    void                msw_rescale();
};


class AuxiliaryPanel : public wxPanel
{
private:
    Tabbook *m_tabpanel = {nullptr};
    wxSizer *m_main_sizer = {nullptr};

    AuFolderPanel *m_pictures_panel= {nullptr};
    AuFolderPanel *m_bill_of_materials_panel= {nullptr};
    AuFolderPanel *m_assembly_panel= {nullptr};
    AuFolderPanel *m_others_panel= {nullptr};
    DesignerPanel * m_designer_panel= {nullptr};

    /* images */
    wxBitmap  m_signal_strong_img;
    wxBitmap  m_signal_middle_img;
    wxBitmap  m_signal_weak_img;
    wxBitmap  m_signal_no_img;
    wxBitmap  m_printer_img;
    wxBitmap  m_arrow_img;
    wxWindow *create_side_tools();

public:
    AuxiliaryPanel(wxWindow *parent, wxWindowID id = wxID_ANY, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize, long style = wxTAB_TRAVERSAL);
    ~AuxiliaryPanel();
    void init_bitmap();
    void init_tabpanel();

    void Split(const std::string &src, const std::string &separator, std::vector<std::string> &dest);

    void msw_rescale();
    void on_size(wxSizeEvent &event);
    bool Show(bool show);

    // core logic
    std::map<std::string, std::vector<fs::path>>    m_paths_list;
    wxString                                        m_root_dir;
    void                                            init_auxiliary();
    void                                            create_folder(wxString name = wxEmptyString);
    std::string                                     replaceSpace(std::string s, std::string ts, std::string ns);
    void                                            on_import_file(wxCommandEvent &event);
    void                                            Reload(wxString aux_path, std::map<std::string, std::vector<json>> paths);

    void update_all_panel();
    void update_all_cover();
};

wxDECLARE_EVENT(EVT_AUXILIARY_IMPORT, wxCommandEvent);
wxDECLARE_EVENT(EVT_AUXILIARY_UPDATE_COVER, wxCommandEvent);
wxDECLARE_EVENT(EVT_AUXILIARY_UPDATE_DELETE, wxCommandEvent);
wxDECLARE_EVENT(EVT_AUXILIARY_UPDATE_RENAME, wxCommandEvent);
wxDECLARE_EVENT(EVT_AUXILIARY_DONE, wxCommandEvent);
}} // namespace Slic3r::GUI

#endif
