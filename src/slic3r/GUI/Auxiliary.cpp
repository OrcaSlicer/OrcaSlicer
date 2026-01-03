#include "Tab.hpp"
#include "Auxiliary.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"

#include <wx/app.h>
#include <wx/button.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>

#include <wx/bmpcbox.h>
#include <wx/bmpbuttn.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>
#include <wx/settings.h>
#include <wx/filedlg.h>
#include <wx/wupdlock.h>
#include <wx/dataview.h>
#include <wx/tokenzr.h>
#include <wx/arrstr.h>
#include <wx/tglbtn.h>

#include <boost/log/trivial.hpp>

#include "wxExtensions.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "MainFrame.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r { namespace GUI {

wxDEFINE_EVENT(EVT_AUXILIARY_IMPORT, wxCommandEvent);
wxDEFINE_EVENT(EVT_AUXILIARY_UPDATE_COVER, wxCommandEvent);
wxDEFINE_EVENT(EVT_AUXILIARY_UPDATE_DELETE, wxCommandEvent);
wxDEFINE_EVENT(EVT_AUXILIARY_UPDATE_RENAME, wxCommandEvent);
wxDEFINE_EVENT(EVT_AUXILIARY_DONE, wxCommandEvent);


const std::vector<std::string> license_list = {
    "",
    "CC0",
    "BY",
    "BY-SA",
    "BY-ND",
    "BY-NC",
    "BY-NC-SA",
    "BY-NC-ND",
};

static std::shared_ptr<ModelInfo> ensure_model_info()
{
    auto& model = wxGetApp().plater()->model();
    if (model.model_info == nullptr) {
        model.model_info = std::make_shared<ModelInfo>();
    }
    return model.model_info;
}

AuFile::AuFile(wxWindow *parent, fs::path file_path, wxString file_name, AuxiliaryFolderType type, wxWindowID id, const wxPoint &pos, const wxSize &size, long style)
{
    m_type      = type;
    m_file_path = file_path;
    m_file_name = file_name;

    wxSize panel_size = m_type == MODEL_PICTURE ? AUFILE_PICTURES_PANEL_SIZE : AUFILE_PANEL_SIZE;
    wxPanel::Create(parent, id, pos, panel_size, style);
    SetBackgroundColour(StateColor::darkModeColorFor(AUFILE_GREY300));
    wxBoxSizer *sizer_body = new wxBoxSizer(wxVERTICAL);

   SetSize(panel_size);

    if (m_type == MODEL_PICTURE) {
        if (m_file_path.empty()) { return; }
        auto image = new wxImage(encode_path(m_file_path.string().c_str()));

        //constrain
        auto size = wxSize(0, 0);
        float proportion = float(image->GetSize().x) / float(image->GetSize().y);
        if (proportion >= 1) { 
            size.x = AUFILE_PICTURES_SIZE.x;
            size.y = AUFILE_PICTURES_SIZE.x / proportion;
        } else {
            size.y = AUFILE_PICTURES_SIZE.y;
            size.x = AUFILE_PICTURES_SIZE.y * proportion;
        }

        image->Rescale(size.x, size.y);
        m_file_bitmap.bmp() = wxBitmap(*image);
    } else {
        m_bitmap_excel = ScalableBitmap(this, "placeholder_excel", 168);
        m_bitmap_pdf   = ScalableBitmap(this, "placeholder_pdf", 168);
        m_bitmap_txt   = ScalableBitmap(this, "placeholder_txt", 168);

        if (m_type == OTHERS) {m_file_bitmap = m_bitmap_txt;}
        if (m_type == BILL_OF_MATERIALS) {
            if (m_file_path.extension() == ".xls" || m_file_path.extension() == ".xlsx") {
                m_file_bitmap = m_bitmap_excel;
            }

            if (m_file_path.extension() == ".pdf") { m_file_bitmap = m_bitmap_pdf; }
            
        }
        if (m_type == ASSEMBLY_GUIDE) {m_file_bitmap = m_bitmap_pdf;}
    }
    
    m_add_file = _L("Add File");
    cover_text_left  = _L("Set as cover");
    cover_text_right = _L("Rename");
    cover_text_cover = _L("Cover");

    m_file_cover     = ScalableBitmap(this, "auxiliary_cover", 40);
    m_file_edit_mask = ScalableBitmap(this, "auxiliary_edit_mask", 30);
    m_file_delete    = ScalableBitmap(this, "auxiliary_delete", 20);
    

    auto m_text_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(panel_size.x, AUFILE_TEXT_HEIGHT), wxTAB_TRAVERSAL);
    m_text_panel->SetBackgroundColour(StateColor::darkModeColorFor(AUFILE_GREY300));
    

    wxBoxSizer *m_text_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_text_name              = new wxStaticText(m_text_panel, wxID_ANY, m_file_name, wxDefaultPosition, wxSize(panel_size.x, -1), wxST_ELLIPSIZE_END);
    m_text_name->Wrap(panel_size.x - FromDIP(10));
    m_text_name->SetFont(::Label::Body_14);
    m_text_name->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));

    m_input_name = new ::TextInput(m_text_panel, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(panel_size.x - FromDIP(28), FromDIP(32)), wxTE_PROCESS_ENTER);
    m_input_name->GetTextCtrl()->SetFont(::Label::Body_13);
    m_input_name->SetFont(::Label::Body_14);
    m_input_name->Hide();

    m_file_exit_rename = new wxStaticBitmap(m_text_panel, wxID_ANY, create_scaled_bitmap("auxiliary_delete", this, 20), wxDefaultPosition, wxSize(FromDIP(20), FromDIP(20)), 0);

    m_file_exit_rename->Bind(wxEVT_LEFT_UP, [this](auto& e) {
        exit_rename_mode();
    });

    m_text_sizer->Add(0, 0, 1, wxEXPAND, 0);
    m_text_sizer->Add(m_text_name, 0, wxALIGN_CENTER, 0);
    m_text_sizer->Add(m_input_name, 0, wxALIGN_CENTER, 0);
    m_text_sizer->Add( 0, 0, 1, wxEXPAND, 0 );
    m_text_sizer->Add(m_file_exit_rename, 0, wxALIGN_CENTER, 0);
    m_file_exit_rename->Hide();

    m_text_panel->SetSizer(m_text_sizer);
    m_text_panel->Layout();
    sizer_body->Add(0, 0, 0, wxTOP, panel_size.y - AUFILE_TEXT_HEIGHT);
    sizer_body->Add(m_text_panel, 0, wxALIGN_CENTER, 0);

    SetSizer(sizer_body);
    Layout();
    Fit();

    Bind(wxEVT_PAINT, &AuFile::OnPaint, this);
    Bind(wxEVT_ERASE_BACKGROUND, &AuFile::OnEraseBackground, this);
    Bind(wxEVT_ENTER_WINDOW, &AuFile::on_mouse_enter, this);
    Bind(wxEVT_LEAVE_WINDOW, &AuFile::on_mouse_leave, this);
    Bind(wxEVT_LEFT_UP, &AuFile::on_mouse_left_up, this);
    Bind(wxEVT_LEFT_DCLICK, &AuFile::on_dclick, this);
    m_input_name->Bind(wxEVT_TEXT_ENTER, &AuFile::on_input_enter, this);
}

void AuFile::enter_rename_mode()
{
    m_input_name->Show();
    m_file_exit_rename->Show();
    m_text_name->Hide();
    auto name = m_file_name.SubString(0, (m_file_name.Find(".") - 1));
    m_input_name->GetTextCtrl()->SetLabelText(name);
    Layout();
}

void AuFile::exit_rename_mode()
{
    m_input_name->Hide();
    m_file_exit_rename->Hide();
    m_text_name->Show();
    Layout();
}

void AuFile::OnPaint(wxPaintEvent &event)
{
    wxPaintDC dc(this);
#ifdef __WXMSW__
    wxSize     size = GetSize();
    wxMemoryDC memdc;
    wxBitmap   bmp(size.x, size.y);
    memdc.SelectObject(bmp);
    memdc.Blit({ 0, 0 }, size, &dc, { 0, 0 });

    {
        wxGCDC dc2(memdc);
        PaintBackground(dc2);
        PaintForeground(dc2);
    }

    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    PaintBackground(dc);
    PaintForeground(dc);
#endif
}

void AuFile::PaintBackground(wxDC &dc)
{
    wxColour backgroundColour = GetBackgroundColour();
    if (!backgroundColour.Ok()) backgroundColour = AUFILE_GREY300;
    dc.SetBrush(wxBrush(backgroundColour));
    dc.SetPen(wxPen(backgroundColour, 1));
    wxRect windowRect(wxPoint(0, 0), GetClientSize());

    //CalcUnscrolledPosition(windowRect.x, windowRect.y, &windowRect.x, &windowRect.y);
    dc.DrawRectangle(windowRect);


    wxSize size = m_type == MODEL_PICTURE ? AUFILE_PICTURES_SIZE : AUFILE_SIZE;

    if (m_type == AddFileButton)
    {
        auto pen_width = FromDIP(2);
        dc.SetPen(wxPen(AUFILE_GREY500, pen_width));
        dc.SetBrush(StateColor::darkModeColorFor(AUFILE_GREY200));
        dc.DrawRoundedRectangle(pen_width / 2, pen_width / 2, size.x - pen_width / 2, size.y - pen_width / 2, AUFILE_ROUNDING);

        auto line_length = FromDIP(50);
        dc.DrawLine(wxPoint((size.x - line_length) / 2, size.y / 2), wxPoint((size.x + line_length) / 2, size.y / 2));
        dc.DrawLine(wxPoint(size.x / 2, (size.y - line_length) / 2), wxPoint(size.x / 2, (size.y + line_length) / 2));

        dc.SetFont(Label::Body_16);
        auto sizet = dc.GetTextExtent(m_add_file);
        auto pos = wxPoint(0, 0);
        pos.x = (size.x - sizet.x) / 2;
        pos.y = (size.y - 40); // to modify
        dc.SetTextForeground(AUFILE_GREY500);
        dc.DrawText(m_add_file, pos);
    }
    else {
        // ORCA match look with add button
        auto pen_width = FromDIP(2);
        dc.SetPen(wxPen(AUFILE_GREY500, pen_width));
        dc.SetBrush(StateColor::darkModeColorFor(AUFILE_GREY200));
        dc.DrawRoundedRectangle(pen_width / 2, pen_width / 2, size.x - pen_width / 2, size.y - pen_width / 2, AUFILE_ROUNDING);
        dc.DrawBitmap(m_file_bitmap.bmp(), (size.x - m_file_bitmap.GetBmpWidth()) / 2, (size.y - m_file_bitmap.GetBmpHeight()) / 2);
    }
}

void AuFile::OnEraseBackground(wxEraseEvent &evt) {}

void AuFile::PaintForeground(wxDC &dc)
{
    wxSize size = m_type == MODEL_PICTURE ? AUFILE_PICTURES_SIZE : AUFILE_SIZE;

    if (m_hover) {

        // ORCA add hover effect to borders
        if (m_type == BILL_OF_MATERIALS || m_type == ASSEMBLY_GUIDE || m_type == OTHERS || m_type == MODEL_PICTURE || m_type == AddFileButton) {
            auto pen_width = FromDIP(2);
            dc.SetPen(wxPen(AUFILE_BRAND, pen_width));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRoundedRectangle(pen_width / 2, pen_width / 2, size.x - pen_width / 2, size.y - pen_width / 2, AUFILE_ROUNDING);
        }

        if (m_type == AddFileButton) {
            auto line_length = FromDIP(50);
            dc.DrawLine(wxPoint((size.x - line_length) / 2, size.y / 2), wxPoint((size.x + line_length) / 2, size.y / 2));
            dc.DrawLine(wxPoint(size.x / 2, (size.y - line_length) / 2), wxPoint(size.x / 2, (size.y + line_length) / 2));

            auto sizet = dc.GetTextExtent(m_add_file);
            auto pos = wxPoint(0, 0);
            pos.x = (size.x - sizet.x) / 2;
            pos.y = (size.y - 40); // to modify
            dc.SetTextForeground(AUFILE_BRAND);
            dc.DrawText(m_add_file, pos);
            return;
        }

        if (m_type == MODEL_PICTURE) {
            dc.DrawBitmap(m_file_edit_mask.bmp(), 0, size.y - m_file_edit_mask.GetBmpSize().y); 
        }


        dc.SetFont(Label::Body_12);
        dc.SetTextForeground(*wxWHITE);
        if (m_type == MODEL_PICTURE) {
            // left text
            auto sizet = dc.GetTextExtent(cover_text_left);
            auto pos   = wxPoint(0, 0);
            pos.x      = (size.x / 2 - sizet.x) / 2;
            pos.y      = (size.y - (m_file_edit_mask.GetBmpSize().y + sizet.y) / 2);
            dc.DrawText(cover_text_left, pos);

            // right text
            sizet = dc.GetTextExtent(cover_text_right);
            pos   = wxPoint(0, 0);
            pos.x = size.x / 2 + (size.x / 2 - sizet.x) / 2;
            pos.y = (size.y - (m_file_edit_mask.GetBmpSize().y + sizet.y) / 2);
            dc.DrawText(cover_text_right, pos);

            // Split
            dc.SetPen(*wxWHITE);
            dc.SetBrush(*wxWHITE);
            pos   = wxPoint(0, 0);
            pos.x = size.x / 2 - 1;
            pos.y = size.y - FromDIP(24) - (m_file_edit_mask.GetBmpSize().y - FromDIP(24)) / 2;
            dc.DrawRectangle(pos.x, pos.y, 2, FromDIP(24));
        } else {
            // right text
           /* auto sizet = dc.GetTextExtent(cover_text_right);
            auto pos   = wxPoint(0, 0);
            pos.x      = (size.x - sizet.x) / 2;
            pos.y      = (size.y - (m_file_edit_mask.GetBmpSize().y + sizet.y) / 2);
            dc.DrawText(cover_text_right, pos);*/
        }       
    }

    if (m_cover) {
        dc.SetTextForeground(*wxWHITE);
        dc.DrawBitmap(m_file_cover.bmp(), size.x - m_file_cover.GetBmpSize().x, 0);
        dc.SetFont(Label::Body_12);
        auto sizet = dc.GetTextExtent(cover_text_cover);
        auto pos   = wxPoint(0, 0);
        pos.x      = size.x - sizet.x - FromDIP(3);
        pos.y      = FromDIP(3);
        dc.DrawText(cover_text_cover, pos);
    }

    if (m_hover) { dc.DrawBitmap(m_file_delete.bmp(), size.x - m_file_delete.GetBmpSize().x - FromDIP(10), FromDIP(10)); }
}

void AuFile::on_mouse_enter(wxMouseEvent &evt)
{
    m_hover = true;
    Refresh();
}

void AuFile::on_mouse_leave(wxMouseEvent &evt)
{
    m_hover = false;
    Refresh();
}

void AuFile::on_input_enter(wxCommandEvent &evt)
{
    auto     new_file_name = m_input_name->GetTextCtrl()->GetValue();
    auto     m_valid_type  = Valid;
    wxString info_line;

    const char *unusable_symbols = "<>[]:/\\|?*\"";

    const std::string unusable_suffix = PresetCollection::get_suffix_modified(); //"(modified)";
    for (size_t i = 0; i < std::strlen(unusable_symbols); i++) {
        if (new_file_name.find_first_of(unusable_symbols[i]) != std::string::npos) {
            info_line    = _L("Name is invalid;") + "\n" + _L("illegal characters:") + " " + unusable_symbols;
            m_valid_type = NoValid;
            break;
        }
    }

    if (m_valid_type == Valid && new_file_name.find(unusable_suffix) != std::string::npos) {
        info_line    = _L("Name is invalid;") + "\n" + _L("illegal suffix:") + "\n\t" + from_u8(PresetCollection::get_suffix_modified());
        m_valid_type = NoValid;
    }

    auto     existing  = false;
    auto     dir       = m_file_path.parent_path();
    auto     new_fullname = new_file_name + m_file_path.extension().string();

    
    wxString new_fullname_path = dir.wstring() + "/" + new_fullname;
    fs::path new_dir_path(new_fullname_path.ToStdWstring());
    

    if (fs::exists(new_dir_path)) existing = true;

    if (m_valid_type == Valid && existing) {
        info_line    = from_u8((boost::format(_u8L("The name \"%1%\" already exists.")) % new_file_name).str());
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_file_name.empty()) {
        info_line    = _L("The name is not allowed to be empty.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_file_name.find_first_of(' ') == 0) {
        info_line    = _L("The name is not allowed to start with space character.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid && new_file_name.find_last_of(' ') == new_file_name.length() - 1) {
        info_line    = _L("The name is not allowed to end with space character.");
        m_valid_type = NoValid;
    }

    if (m_valid_type == Valid) {
        fs::path oldPath = m_file_path;
        fs::path newPath(new_dir_path);
        fs::rename(oldPath, newPath);
    } else {
        /*MessageDialog msg_wingow(nullptr, info_line, "",
                                 wxICON_WARNING | wxOK);
        if (msg_wingow.ShowModal() == wxID_CANCEL) {
            m_input_name->GetTextCtrl()->SetValue(wxEmptyString);
            return;
        }*/

        return;
    }

    // post event
    auto event = wxCommandEvent(EVT_AUXILIARY_UPDATE_RENAME);
    event.SetString(wxString::Format("%s|%s|%s", s_default_folders[m_type], m_file_path.wstring(), new_dir_path.wstring()));
    event.SetEventObject(m_parent);
    wxPostEvent(m_parent, event);

    // update layout
    m_file_path = new_dir_path.string();
    m_file_name = new_fullname;
    m_text_name->SetLabel(new_fullname);
    exit_rename_mode();
    // evt.Skip();
}

void AuFile::on_dclick(wxMouseEvent &evt) 
{
    if (m_type == AddFileButton)
        return;
    else
        wxLaunchDefaultApplication(m_file_path.wstring(), 0);
}

void AuFile::on_mouse_left_up(wxMouseEvent &evt)
{
    if (m_type == AddFileButton) {
        return;
    }

    wxSize size = m_type == MODEL_PICTURE ? AUFILE_PICTURES_SIZE : AUFILE_SIZE;

    auto pos = evt.GetPosition();
    // set cover
    auto mask_size    = wxSize(GetSize().x, m_file_edit_mask.GetBmpSize().y);
    auto cover_left   = 0;
    auto cover_top    = size.y - mask_size.y;
    auto cover_right  = mask_size.x / 2;
    auto cover_bottom = size.y;

    if (pos.x > cover_left && pos.x < cover_right && pos.y > cover_top && pos.y < cover_bottom) { 
        if(m_type == MODEL_PICTURE)
            on_set_cover(); 
       /* else
             on_set_rename();*/
        return;
    }

    // rename
    auto rename_left   = mask_size.x / 2;
    auto rename_top    = size.y - mask_size.y;
    auto rename_right  = mask_size.x;
    auto rename_bottom = size.y;
    if (pos.x > rename_left && pos.x < rename_right && pos.y > rename_top && pos.y < rename_bottom) { on_set_rename(); return; }

    // close
    auto close_left   = size.x - m_file_delete.GetBmpSize().x - FromDIP(10);
    auto close_top    = FromDIP(10);
    auto close_right  = size.x - FromDIP(10);
    auto close_bottom = m_file_delete.GetBmpSize().y + FromDIP(10);
    if (pos.x > close_left && pos.x < close_right && pos.y > close_top && pos.y < close_bottom) { on_set_delete(); return; }

    exit_rename_mode();
}

void AuFile::on_set_cover()
{
    fs::path path(into_path(m_file_name));
    ensure_model_info()->cover_file = path.string();
    //wxGetApp().plater()->model().model_info->cover_file = m_file_name.ToStdString();

    auto full_path          = m_file_path.parent_path();
    auto full_root_path         = full_path.parent_path();
    auto full_root_path_str = encode_path(full_root_path.string().c_str());
    auto dir       = wxString::Format("%s/.thumbnails", full_root_path_str);

    fs::path dir_path(dir.ToStdWstring());

    if (!fs::exists(dir_path)) {
        fs::create_directory(dir_path); 
    }

    bool result = true;
    wxImage thumbnail_img;

    result = generate_image(m_file_path.string(), thumbnail_img, _3MF_COVER_SIZE);
    if (result) {
        auto cover_img_path = dir_path.string() + "/thumbnail_3mf.png";
        thumbnail_img.SaveFile(encode_path(cover_img_path.c_str()));
    }

    result = generate_image(m_file_path.string(), thumbnail_img, PRINTER_THUMBNAIL_SMALL_SIZE);
    if (result) {
        auto small_img_path = dir_path.string() + "/thumbnail_small.png";
        thumbnail_img.SaveFile(encode_path(small_img_path.c_str()));
    }

    result = generate_image(m_file_path.string(), thumbnail_img, PRINTER_THUMBNAIL_MIDDLE_SIZE);
    if (result) {
        auto middle_img_path = dir_path.string() + "/thumbnail_middle.png";
        thumbnail_img.SaveFile(encode_path(middle_img_path.c_str()));
    }

    wxGetApp().plater()->set_plater_dirty(true);
    auto evt = wxCommandEvent(EVT_AUXILIARY_UPDATE_COVER);
    evt.SetString(s_default_folders[m_type]);
    evt.SetEventObject(m_parent);
    wxPostEvent(m_parent, evt);
}

void AuFile::on_set_delete()
{
    fs::path bfs_path = m_file_path;
    auto     is_fine = fs::remove(bfs_path);

    if (m_cover) {
        auto full_path          = m_file_path.parent_path();
        auto full_root_path     = full_path.parent_path();
        auto full_root_path_str = encode_path(full_root_path.string().c_str());
        auto dir                = wxString::Format("%s/.thumbnails", full_root_path_str);
        fs::path dir_path(dir.ToStdWstring());

        auto cover_img_path = dir_path.string() + "/thumbnail_3mf.png";
        auto small_img_path = dir_path.string() + "/thumbnail_small.png";
        auto middle_img_path = dir_path.string() + "/thumbnail_middle.png";

        if (fs::exists(fs::path(cover_img_path))) { fs::remove(fs::path(cover_img_path));}
        if (fs::exists(fs::path(small_img_path))) { fs::remove(fs::path(small_img_path)); }
        if (fs::exists(fs::path(middle_img_path))) { fs::remove(fs::path(middle_img_path)); }
    }

    if (wxGetApp().plater()->model().model_info != nullptr) {
        if (wxGetApp().plater()->model().model_info->cover_file == m_file_name) {
            wxGetApp().plater()->model().model_info->cover_file = "";
        }
    }

    if (is_fine) {
        auto evt = wxCommandEvent(EVT_AUXILIARY_UPDATE_DELETE);
        evt.SetString(wxString::Format("%s|%s", s_default_folders[m_type], m_file_path.wstring()));
        evt.SetEventObject(m_parent);
        wxPostEvent(m_parent, evt);
    }
}

void AuFile::on_set_rename() { enter_rename_mode(); }

void AuFile::on_set_open() {}

void AuFile::set_cover(bool cover)
{
    m_cover = cover;
    Refresh();
}

AuFile::~AuFile() {}

void AuFile::msw_rescale() 
{ 
    m_file_cover     = ScalableBitmap(this, "auxiliary_cover", 40);
    m_file_edit_mask = ScalableBitmap(this, "auxiliary_edit_mask", FromDIP(30));
    m_file_delete    = ScalableBitmap(this, "auxiliary_delete", 20);

    if (m_type == MODEL_PICTURE) {
        if (m_file_path.empty()) { return;}
        auto image = new wxImage(encode_path(m_file_path.string().c_str()));
        // constrain
        auto  size       = wxSize(0, 0);
        float proportion = float(image->GetSize().x) / float(image->GetSize().y);
        if (proportion >= 1) {
            size.x = FromDIP(300);
            size.y = FromDIP(300) / proportion;
        } else {
            size.y = FromDIP(300);
            size.x = FromDIP(300) * proportion;
        }

        image->Rescale(size.x, size.y);
        m_file_bitmap.bmp() = wxBitmap(*image);
    } else {
        m_bitmap_excel = ScalableBitmap(this, "placeholder_excel", 168);
        m_bitmap_pdf   = ScalableBitmap(this, "placeholder_pdf", 168);
        m_bitmap_txt   = ScalableBitmap(this, "placeholder_txt", 168);

        if (m_type == OTHERS) { m_file_bitmap = m_bitmap_txt; }
        if (m_type == BILL_OF_MATERIALS) { m_file_bitmap = m_bitmap_excel; }
        if (m_type == ASSEMBLY_GUIDE) { m_file_bitmap = m_bitmap_pdf; }
    }
    Refresh();
}

AuFolderPanel::AuFolderPanel(wxWindow *parent, AuxiliaryFolderType type, wxWindowID id, const wxPoint &pos, const wxSize &size, long style)
    : wxPanel(parent, id, pos, size, style)
{
    m_type = type;
    SetBackgroundColour(AUFILE_GREY300);
    wxBoxSizer *sizer_main = new wxBoxSizer(wxVERTICAL);

    m_scrolledWindow = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxVSCROLL);
    m_scrolledWindow->SetScrollRate(5, 5);
    wxBoxSizer *sizer_body = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *sizer_top  = new wxBoxSizer(wxHORIZONTAL);

    StateColor btn_bg_white(std::pair<wxColour, int>(AMS_CONTROL_DISABLE_COLOUR, StateColor::Disabled), std::pair<wxColour, int>(AMS_CONTROL_DISABLE_COLOUR, StateColor::Pressed),
                            std::pair<wxColour, int>(AMS_CONTROL_DEF_BLOCK_BK_COLOUR, StateColor::Hovered),
                            std::pair<wxColour, int>(AMS_CONTROL_WHITE_COLOUR, StateColor::Normal));

    StateColor btn_bd_white(std::pair<wxColour, int>(AMS_CONTROL_WHITE_COLOUR, StateColor::Disabled), std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));
    //m_button_add = new AuFile(m_scrolledWindow, fs::path(), "", AddFileButton, -1);
    /*m_button_add->SetBackgroundColor(btn_bg_white);
    m_button_add->SetBorderColor(btn_bd_white);
    m_button_add->SetMinSize(wxSize(-1, FromDIP(24)));
    m_button_add->SetCornerRadius(FromDIP(12));
    m_button_add->SetFont(Label::Body_14);*/

    m_big_button_add = new AuFile(m_scrolledWindow, fs::path(), "", AddFileButton, -1);

    /*m_button_del = new Button(m_scrolledWindow, _L("Delete"), "auxiliary_delete_file", 12, 12);
    m_button_del->SetBackgroundColor(btn_bg_white);
    m_button_del->SetBorderColor(btn_bd_white);
    m_button_del->SetMinSize(wxSize(FromDIP(80), FromDIP(24)));
    m_button_del->SetCornerRadius(12);
    m_button_del->SetFont(Label::Body_14);*/
    // m_button_del->Bind(wxEVT_LEFT_UP, &AuxiliaryPanel::on_delete, this);

    sizer_top->Add(0, 0, 0, wxLEFT, FromDIP(10));
    m_gsizer_content = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
    //if (m_type == MODEL_PICTURE) {
    //    //sizer_top->Add(m_button_add, 0, wxALL, 0);
    //    //m_big_button_add->Hide();
    //}
    //else {
        m_gsizer_content->Add(m_big_button_add, 0, wxALL, FromDIP(8));
        //m_button_add->Hide();
    //}
    // sizer_top->Add(m_button_del, 0, wxALL, 0);
    sizer_body->Add(sizer_top, 0, wxEXPAND | wxTOP, FromDIP(35));
    sizer_body->AddSpacer(FromDIP(14));
    sizer_body->Add(m_gsizer_content, 0, 0, 0);
    m_scrolledWindow->SetSizer(sizer_body);
    m_scrolledWindow->Layout();
    sizer_main->Add(m_scrolledWindow, 1, wxEXPAND | wxLEFT, FromDIP(40));

    this->SetSizer(sizer_main);
    this->Layout();

    m_big_button_add->Bind(wxEVT_LEFT_DOWN, [this](auto& e)
        {
            auto evt = wxCommandEvent(EVT_AUXILIARY_IMPORT);
            evt.SetString(s_default_folders[m_type]);
            evt.SetEventObject(m_parent);
            wxPostEvent(m_parent, evt);
        });
    //m_button_add->Bind(wxEVT_LEFT_UP, &AuFolderPanel::on_add, this);
}

void AuFolderPanel::clear()
{
    for (auto i = 0; i < m_aufiles_list.GetCount(); i++) {
        AuFiles *aufile = m_aufiles_list[i];
        if (aufile->file) { aufile->file->Destroy(); }
    }
    m_aufiles_list.clear();
    Layout();
    Refresh();
}

void AuFolderPanel::update(std::vector<fs::path> paths)
{
    clear();
    for (auto i = 0; i < paths.size(); i++) {
        std::string temp_name = fs::path(paths[i].c_str()).filename().string();
        auto name = encode_path(temp_name.c_str());

        auto        aufile = new AuFile(m_scrolledWindow, paths[i], name, m_type, wxID_ANY);
        m_gsizer_content->Add(aufile, 0, wxALL, FromDIP(8));
        auto af  = new AuFiles;
        af->path = paths[i].string();
        af->file = aufile;
        m_aufiles_list.push_back(af);
    }
    m_gsizer_content->Layout();
    Layout();
    Refresh();
}

void AuFolderPanel::msw_rescale() 
{
    //m_button_add->SetMinSize(wxSize(-1, FromDIP(24)));
    for (auto i = 0; i < m_aufiles_list.GetCount(); i++) {
        AuFiles *aufile = m_aufiles_list[i];
        aufile->file->msw_rescale();
    }
}

void AuFolderPanel::on_add(wxMouseEvent& event)
{
    auto evt = wxCommandEvent(EVT_AUXILIARY_IMPORT);
    evt.SetString(s_default_folders[m_type]);
    evt.SetEventObject(m_parent);
    wxPostEvent(m_parent, evt);
}

void AuFolderPanel::on_delete(wxCommandEvent &event) { clear(); }

AuFolderPanel::~AuFolderPanel()
{
}

void AuFolderPanel::update_cover()
{
    if (wxGetApp().plater()->model().model_info != nullptr) {
        for (auto i = 0; i < m_aufiles_list.GetCount(); i++) {
            AuFiles *aufile = m_aufiles_list[i];

            if (wxString::FromUTF8(wxGetApp().plater()->model().model_info->cover_file) == aufile->file->m_file_name) {
                aufile->file->set_cover(true);
            } else {
                aufile->file->set_cover(false);
            }
        }
    }
}

AuxiliaryPanel::AuxiliaryPanel(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size, long style) : wxPanel(parent, id, pos, size, style)
{
    init_tabpanel();
    // init_auxiliary();

    m_main_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_main_sizer->Add(m_tabpanel, 1, wxEXPAND | wxLEFT, 0);
    SetSizerAndFit(m_main_sizer);
    Bind(wxEVT_SIZE, &AuxiliaryPanel::on_size, this);
    Bind(EVT_AUXILIARY_IMPORT, &AuxiliaryPanel::on_import_file, this);

    // cover -event
    Bind(EVT_AUXILIARY_UPDATE_COVER, [this](wxCommandEvent &e) { update_all_cover(); });

    // delete event
    Bind(EVT_AUXILIARY_UPDATE_DELETE, [this](wxCommandEvent &e) {
        auto info_str = e.GetString();

        wxArrayString     parems;
        wxStringTokenizer tokenizer(info_str, "|");
        while (tokenizer.HasMoreTokens()) {
            wxString token = tokenizer.GetNextToken();
            parems.Add(token);
        }


        auto model = parems[0];
        auto name  = parems[1];

        auto iter = m_paths_list.find(model.ToStdString());
        if (iter != m_paths_list.end()) {
            auto list = iter->second;
            for (auto i = 0; i < list.size(); i++) {
                if (list[i].wstring() == name) {
                    list.erase(std::begin(list) + i);
                    break;
                }
            }

            m_paths_list[model.ToStdString()] = list;
            update_all_panel();
            update_all_cover();
        }
    });

    // rename event
    Bind(EVT_AUXILIARY_UPDATE_RENAME, [this](wxCommandEvent &e) {
        auto info_str = e.GetString();

        wxArrayString     parems;
        wxStringTokenizer tokenizer(info_str, "|");
        while (tokenizer.HasMoreTokens()) {
            wxString token = tokenizer.GetNextToken();
            parems.Add(token);
        }

        auto model    = parems[0];
        auto old_name = parems[1];
        auto new_name = parems[2];

        auto iter = m_paths_list.find(model.ToStdString());
        if (iter != m_paths_list.end()) {
            auto list = iter->second;
            for (auto i = 0; i < list.size(); i++) {
                if (list[i].wstring() == old_name) {
                    list[i] = fs::path(new_name.ToStdWstring());
                    break;
                }
            }

            m_paths_list[model.ToStdString()] = list;
        }
    });
}

void AuxiliaryPanel::Split(const std::string &src, const std::string &separator, std::vector<std::string> &dest)
{
    std::string            str = src;
    std::string            substring;
    std::string::size_type start = 0, index;
    dest.clear();
    index = str.find_first_of(separator, start);
    do {
        if (index != string::npos) {
            substring = str.substr(start, index - start);
            dest.push_back(substring);
            start = index + separator.size();
            index = str.find(separator, start);
            if (start == string::npos) break;
        }
    } while (index != string::npos);

    // the last part
    substring = str.substr(start);
    dest.push_back(substring);
}

AuxiliaryPanel::~AuxiliaryPanel() {}

void AuxiliaryPanel::init_bitmap()
{
    /*m_signal_strong_img = create_scaled_bitmap("monitor_signal_strong", nullptr, 24);
    m_signal_middle_img = create_scaled_bitmap("monitor_signal_middle", nullptr, 24);
    m_signal_weak_img   = create_scaled_bitmap("monitor_signal_weak", nullptr, 24);
    m_signal_no_img     = create_scaled_bitmap("monitor_signal_no", nullptr, 24);
    m_printer_img       = create_scaled_bitmap("monitor_printer", nullptr, 26);
    m_arrow_img         = create_scaled_bitmap("monitor_arrow", nullptr, 14);*/
}

void AuxiliaryPanel::init_tabpanel()
{
    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Disabled),
                            std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
                            std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
                            std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
    auto back_btn = new Button(this, _L("Return"), "assemble_return", wxBORDER_NONE | wxBU_LEFT | wxBU_EXACTFIT);
    back_btn->SetSize(wxSize(FromDIP(220), FromDIP(18)));
    back_btn->SetBackgroundColor(btn_bg_green);
    back_btn->SetTextColor(StateColor (std::pair<wxColour, int>(wxColour("#FDFFFD"), StateColor::Normal))); // ORCA fixes color change on text. icon stays white color but text changes to black without this
    back_btn->SetCornerRadius(0);
    back_btn->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxEvent& e) {
        auto event = wxCommandEvent(EVT_AUXILIARY_DONE);
        event.SetEventObject(m_parent);
        wxPostEvent(m_parent, event);
    });

    wxBoxSizer *sizer_side_tools = new wxBoxSizer(wxVERTICAL);
    sizer_side_tools->Add(back_btn, 1, wxEXPAND, 0);
    m_tabpanel = new Tabbook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, sizer_side_tools, wxNB_LEFT | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME);
    m_tabpanel->SetBackgroundColour(wxColour("#FEFFFF"));
    m_tabpanel->Bind(wxEVT_BOOKCTRL_PAGE_CHANGED, [](wxBookCtrlEvent &e) { /* Event handling */ });

    m_designer_panel          = new DesignerPanel(m_tabpanel, AuxiliaryFolderType::DESIGNER);
    m_pictures_panel          = new AuFolderPanel(m_tabpanel, AuxiliaryFolderType::MODEL_PICTURE);
    m_bill_of_materials_panel = new AuFolderPanel(m_tabpanel, AuxiliaryFolderType::BILL_OF_MATERIALS);
    m_assembly_panel          = new AuFolderPanel(m_tabpanel, AuxiliaryFolderType::ASSEMBLY_GUIDE);
    m_others_panel            = new AuFolderPanel(m_tabpanel, AuxiliaryFolderType::OTHERS);

    m_tabpanel->AddPage(m_designer_panel, _L("Basic Info"), "", true);
    m_tabpanel->AddPage(m_pictures_panel, _L("Pictures"), "", false);
    m_tabpanel->AddPage(m_bill_of_materials_panel, _L("Bill of Materials"), "", false);
    m_tabpanel->AddPage(m_assembly_panel, _L("Assembly Guide"), "", false);
    m_tabpanel->AddPage(m_others_panel, _L("Others"), "", false);
}

wxWindow *AuxiliaryPanel::create_side_tools()
{
    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
    auto        panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(0, FromDIP(50)));
    panel->SetBackgroundColour(wxColour(135, 206, 250));
    panel->SetSizer(sizer);
    sizer->Layout();
    panel->Fit();
    return panel;
}

void AuxiliaryPanel::msw_rescale() { 
    m_pictures_panel->msw_rescale();
    m_bill_of_materials_panel->msw_rescale();
    m_assembly_panel->msw_rescale();
    m_others_panel->msw_rescale();
    m_designer_panel->msw_rescale();
}

void AuxiliaryPanel::on_size(wxSizeEvent &event)
{
    if (!wxGetApp().mainframe) return;
    Layout();
    Refresh();
}

bool AuxiliaryPanel::Show(bool show) { return wxPanel::Show(show); }

// core logic
void AuxiliaryPanel::init_auxiliary()
{
    Model &model = wxGetApp().plater()->model();
    Reload(encode_path(model.get_auxiliary_file_temp_path().c_str()), {});
}

void AuxiliaryPanel::on_import_file(wxCommandEvent &event)
{
    auto file_model = event.GetString();

    wxString     src_path;
    wxString     dst_path;

     wxString wildcard = wxFileSelectorDefaultWildcardStr;

    if (file_model == s_default_folders[MODEL_PICTURE]) {
        //wildcard = wxT("JPEG files (*.jpeg)|*.jpeg|BMP files (*.bmp)|*.bmp|GIF files (*.gif)|*.gif|PNG files (*.png)|*.png|JPG files (*.jpg)|*.jpg");
        wildcard = wxT("files (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp");
    } 

    if (file_model == s_default_folders[OTHERS]) {  wildcard = wxT("TXT files (*.txt)|*.txt"); }
    if (file_model == s_default_folders[BILL_OF_MATERIALS]){ wildcard = wxT("EXCEL files (*.xls)|*.xls|EXCEL files (*.xlsx)|*.xlsx|PDF files (*.pdf)|*.pdf"); }
    if (file_model == s_default_folders[ASSEMBLY_GUIDE]) { wildcard = wxT("PDF files (*.pdf)|*.pdf"); }

    wxFileDialog dialog(this, _L("Choose files"), wxEmptyString, wxEmptyString, wildcard, wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (dialog.ShowModal() == wxID_OK) {
        wxArrayString file_paths;
        dialog.GetPaths(file_paths);

        for (wxString file_path : file_paths) {
            // Copy imported file to project temp directory
            fs::path src_bfs_path(file_path.ToStdWstring());
            wxString dir_path = m_root_dir;
            dir_path += "/" + file_model;
            


            auto is_exist = false;
            auto iter = m_paths_list.find(file_model.ToStdString());
            if (iter != m_paths_list.end()) {
                std::vector<fs::path> list = iter->second;
                for (auto i = 0; i < list.size(); i++) {
                    if (src_bfs_path.filename() == list[i].filename()) { 
                        is_exist = true;
                        break;
                    }
                }
            }
            
            if (!is_exist) {
                dir_path += "/" + src_bfs_path.filename().generic_wstring();
            } else {
                time_t t1 = time(0);
                char   ch1[64];
                strftime(ch1, sizeof(ch1), "%T", localtime(&t1));
                std::string time_text = ch1;

                wxString name = src_bfs_path.filename().generic_wstring();
                auto before_name = replaceSpace(name.ToStdString(), src_bfs_path.extension().string(), "");
                time_text = replaceSpace(time_text, ":", "_");
                dir_path += "/" + before_name + "_" + time_text + src_bfs_path.extension().wstring();
            }
           

            boost::system::error_code ec;
            if (!fs::copy_file(src_bfs_path, fs::path(dir_path.ToStdWstring()), fs::copy_options::overwrite_existing, ec)) continue;
            Slic3r::put_other_changes();

            // add in file list
            iter = m_paths_list.find(file_model.ToStdString());
            auto file_fs_path = fs::path(dir_path.ToStdWstring());
            if (iter != m_paths_list.end()) {
                m_paths_list[file_model.ToStdString()].push_back(file_fs_path);
            } else {
                m_paths_list[file_model.ToStdString()] = std::vector<fs::path>{file_fs_path};
            }
        }
        update_all_panel();
        update_all_cover();
    }
}

void AuxiliaryPanel::create_folder(wxString name)
{
    wxString folder_name = name;

    // Create folder in file system
    fs::path bfs_path((m_root_dir + "/" + folder_name).ToStdWstring());
    if (fs::exists(bfs_path)) {
        try {
            bool is_done = fs::remove_all(bfs_path);
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Failed  removing the auxiliary directory " << m_root_dir.c_str();
        }
    }
    fs::create_directory(bfs_path);
}

std::string AuxiliaryPanel::replaceSpace(std::string s, std::string ts, std::string ns)
{
    int    index   = -1;
    while ((index = s.find(ts.c_str())) >= 0) { s = s.replace(index, ts.length(), ns.c_str()); }
    return s;
}

void AuxiliaryPanel::Reload(wxString aux_path, std::map<std::string, std::vector<json>> paths)
{
    m_root_dir = aux_path;
    m_paths_list.clear();

    for (const auto & path : paths) {
        m_paths_list[path.first] = std::vector<fs::path>{};
        for (const auto & j : path.second) {
            m_paths_list[path.first].push_back(j["_filepath"]);
        }
    }

    update_all_panel();
    update_all_cover();
    m_designer_panel->update_info();
    m_tabpanel->SetSelection(0);
}

void AuxiliaryPanel::update_all_panel()
{
    std::map<std::string, std::vector<fs::path>>::iterator mit;

    Freeze();
    m_pictures_panel->clear();
    m_bill_of_materials_panel->clear();
    m_assembly_panel->clear();
    m_others_panel->clear();

    for (mit = m_paths_list.begin(); mit != m_paths_list.end(); mit++) {
        if (mit->first == "Model Pictures") { m_pictures_panel->update(mit->second); }
        if (mit->first == "Bill of Materials") { m_bill_of_materials_panel->update(mit->second); }
        if (mit->first == "Assembly Guide") { m_assembly_panel->update(mit->second); }
        if (mit->first == "Others") { m_others_panel->update(mit->second); }
    }
    Thaw();
}

void AuxiliaryPanel::update_all_cover()
{
    std::map<std::string, std::vector<fs::path>>::iterator mit;
    for (mit = m_paths_list.begin(); mit != m_paths_list.end(); mit++) {
        if (mit->first == "Model Pictures") { m_pictures_panel->update_cover(); }
    }
}


 DesignerPanel::DesignerPanel(wxWindow *          parent,
                             AuxiliaryFolderType type,
                             wxWindowID          id /*= wxID_ANY*/,
                             const wxPoint &     pos /*= wxDefaultPosition*/,
                             const wxSize &      size /*= wxDefaultSize*/,
                             long                style /*= wxTAB_TRAVERSAL*/)
     : wxPanel(parent, id, pos, size, style)
{
     SetBackgroundColour(AUFILE_GREY300);
     wxBoxSizer *m_sizer_body = new wxBoxSizer(wxVERTICAL);

     wxBoxSizer *m_sizer_designer = new wxBoxSizer(wxHORIZONTAL);
     auto m_text_designer = new wxStaticText(this, wxID_ANY, _L("Author"), wxDefaultPosition, wxSize(180, -1), 0);
     m_text_designer->Wrap(-1);
     m_text_designer->SetForegroundColour(*wxBLACK);
     m_sizer_designer->Add(m_text_designer, 0, wxALIGN_CENTER, 0);
     m_input_designer =  new ::TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(450), FromDIP(30)), wxTE_PROCESS_ENTER);
     m_input_designer->GetTextCtrl()->SetFont(::Label::Body_14);
     m_input_designer->GetTextCtrl()->SetSize(wxSize(FromDIP(450), -1));
     m_sizer_designer->Add(m_input_designer, 0, wxALIGN_CENTER, 0);

     wxBoxSizer *m_sizer_model_name = new wxBoxSizer(wxHORIZONTAL);
     auto m_text_model_name = new wxStaticText(this, wxID_ANY, _L("Model Name"), wxDefaultPosition, wxSize(180, -1), 0);
     m_text_model_name->SetForegroundColour(*wxBLACK);
     m_text_model_name->Wrap(-1);
     m_sizer_model_name->Add(m_text_model_name, 0, wxALIGN_CENTER, 0);
     m_input_model_name =  new ::TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition,wxSize(FromDIP(450),FromDIP(30)), wxTE_PROCESS_ENTER);
     m_input_model_name->GetTextCtrl()->SetFont(::Label::Body_14);
     m_input_model_name->GetTextCtrl()->SetSize(wxSize(FromDIP(450), -1));
     m_sizer_model_name->Add(m_input_model_name, 0, wxALIGN_CENTER, 0);

     wxBoxSizer *m_sizer_license = new wxBoxSizer(wxHORIZONTAL);
     auto m_text_license = new wxStaticText(this, wxID_ANY, _L("License"), wxDefaultPosition, wxSize(180, -1), 0);
     m_text_license->SetForegroundColour(*wxBLACK);
     m_text_license->Wrap(-1);
     m_sizer_license->Add(m_text_license, 0, wxALIGN_CENTER, 0);
     m_combo_license = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(450), -1), 0, NULL, wxCB_READONLY);
     m_sizer_license->Add(m_combo_license, 0, wxALIGN_CENTER, 0);

     wxBoxSizer *m_sizer_description = new wxBoxSizer(wxHORIZONTAL);
     auto m_text_description = new wxStaticText(this, wxID_ANY, _L("Description:"), wxDefaultPosition, wxSize(170, -1), 0); // Using "Description:" with the : because that already exists in the Localizations files
     m_text_description->SetForegroundColour(*wxBLACK);
     m_text_description->Wrap(-1);
     m_sizer_description->Add(m_text_description, 0, wxALIGN_TOP | wxRIGHT, FromDIP(10));
     m_input_description = new TextInput(this, "", "", "", wxDefaultPosition, wxSize(FromDIP(450), FromDIP(300)), wxTE_MULTILINE | wxTE_PROCESS_ENTER);
     m_input_description->SetFont(::Label::Body_14);
     m_sizer_description->Add(m_input_description, 0, wxALIGN_CENTER, 0);

     m_sizer_body->Add(0, 0, 0, wxTOP, FromDIP(50));
     m_sizer_body->Add(m_sizer_designer, 0, wxLEFT | wxALIGN_LEFT, FromDIP(50));
     m_sizer_body->Add(0, 0, 0, wxTOP, FromDIP(20));
     m_sizer_body->Add(m_sizer_model_name, 0, wxLEFT | wxALIGN_LEFT, FromDIP(50));
     m_sizer_body->Add(0, 0, 0, wxTOP, FromDIP(20));
     m_sizer_body->Add(m_sizer_license, 0, wxLEFT | wxALIGN_LEFT, FromDIP(50));
     init_license_list();
     m_sizer_body->Add(0, 0, 0, wxTOP, FromDIP(20));
     m_sizer_body->Add(m_sizer_description, 0, wxLEFT | wxALIGN_LEFT, FromDIP(50));

     SetSizer(m_sizer_body);
     Layout();
     Fit();

     m_input_designer->Bind(wxEVT_TEXT, &DesignerPanel::on_input_enter_designer, this);
     m_input_model_name->Bind(wxEVT_TEXT, &DesignerPanel::on_input_enter_model, this);
     m_input_description->Bind(wxEVT_TEXT, &DesignerPanel::on_input_enter_description, this);
     m_combo_license->Bind(wxEVT_COMMAND_COMBOBOX_SELECTED, &DesignerPanel::on_select_license, this);
}

 DesignerPanel::~DesignerPanel()
 {
 }

 void DesignerPanel::init_license_list()
 {
     wxArrayString text_licese;
     for (int i = 0; i < license_list.size(); i++) {
         text_licese.Add(license_list[i]);
     }
     m_combo_license->Set(text_licese);
 }

 void DesignerPanel::on_select_license(wxCommandEvent&evt)
 {
     int selected = evt.GetInt();
     if (selected >= 0 && selected < license_list.size()) {
         ensure_model_info()->license = license_list[selected];
     }
 }

bool DesignerPanel::Show(bool show)
 {
     if (show) update_info();
     return wxPanel::Show(show);
 }

void DesignerPanel::on_input_enter_designer(wxCommandEvent &evt) 
{ 
    auto text  = evt.GetString();
    wxGetApp().plater()->model().SetDesigner(std::string(text.ToUTF8().data()), "");
}

void DesignerPanel::on_input_enter_model(wxCommandEvent &evt) 
{
    auto text   = evt.GetString();
    ensure_model_info()->model_name = std::string(text.ToUTF8().data());
}

void DesignerPanel::on_input_enter_description(wxCommandEvent &evt) 
{
    auto text   = evt.GetString();
    ensure_model_info()->description = std::string(text.ToUTF8().data());
}

void DesignerPanel::update_info() 
{
    if (wxGetApp().plater()->model().design_info != nullptr) {
        wxString text = wxString::FromUTF8(wxGetApp().plater()->model().design_info->Designer);
        m_input_designer->GetTextCtrl()->SetValue(text);
    } else {
        m_input_designer->GetTextCtrl()->SetValue(wxEmptyString);
    }

    if (wxGetApp().plater()->model().model_info != nullptr) {
        m_input_model_name->GetTextCtrl()->SetValue(wxString::FromUTF8(wxGetApp().plater()->model().model_info->model_name));
        m_input_description->GetTextCtrl()->ChangeValue(wxString::FromUTF8(wxGetApp().plater()->model().model_info->description));
        if (!m_combo_license->SetStringSelection(wxString::FromUTF8(wxGetApp().plater()->model().model_info->license))) {
            m_combo_license->SetSelection(0);
        }
    } else {
        m_input_model_name->GetTextCtrl()->SetValue(wxEmptyString);
        m_input_description->GetTextCtrl()->ChangeValue(wxEmptyString);
        m_combo_license->SetSelection(0);
    }
}

void DesignerPanel::msw_rescale()
{
    m_input_designer->GetTextCtrl()->SetSize(wxSize(FromDIP(450), -1));
    m_input_model_name->GetTextCtrl()->SetSize(wxSize(FromDIP(450), -1));
    m_combo_license->SetSize(wxSize(FromDIP(450), -1));
    m_input_description->SetSize(wxSize(FromDIP(450), -1));}

}} // namespace Slic3r::GUI
