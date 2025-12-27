#include "SysInfoDialog.hpp"
#include "I18N.hpp"
#include "3DScene.hpp"
#include "GUI.hpp"
#include "../Utils/UndoRedo.hpp"
#include "Plater.hpp"

#include <string>

#include <Eigen/Core>

#include <wx/clipbrd.h>
#include <wx/platinfo.h>
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/BlacklistedLibraryCheck.hpp"
#include "libslic3r/Color.hpp"
#include "format.hpp"

#ifdef _WIN32
	// The standard Windows includes.
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <Windows.h>
	#include <psapi.h>
#endif /* _WIN32 */

namespace Slic3r {
namespace GUI {

std::string get_main_info(bool format_as_html)
{
    std::stringstream out;

    std::string b_start  = format_as_html ? "<b>"  : "";
    std::string b_end    = format_as_html ? "</b>" : "";
    std::string line_end = format_as_html ? "<br>" : "\n";

    if (!format_as_html)
        out << b_start << (wxGetApp().is_editor() ? SLIC3R_APP_FULL_NAME : GCODEVIEWER_APP_NAME) << b_end << line_end;
    out << b_start << "Version:   "             << b_end << SLIC3R_VERSION << line_end;
    out << b_start << "Build:     " << b_end << (wxGetApp().is_editor() ? SLIC3R_BUILD_ID : GCODEVIEWER_BUILD_ID) << line_end;
    out << line_end;
    out << b_start << "Operating System:    "   << b_end << wxPlatformInfo::Get().GetOperatingSystemFamilyName() << line_end;
    out << b_start << "System Architecture: "   << b_end << wxPlatformInfo::Get().GetArchName() << line_end;
    out << b_start <<
#if defined _WIN32
        "Windows Version:     "
#else
        // Hopefully some kind of unix / linux.
        "System Version:      "
#endif
        << b_end << wxPlatformInfo::Get().GetOperatingSystemDescription() << line_end;
    out << b_start << "Total RAM size [MB]: "  << b_end << Slic3r::format_memsize_MB(Slic3r::total_physical_memory());

    return out.str();
}

std::string get_mem_info(bool format_as_html)
{
    std::stringstream out;

    std::string b_start  = format_as_html ? "<b>"  : "";
    std::string b_end    = format_as_html ? "</b>" : "";
    std::string line_end = format_as_html ? "<br>" : "\n";

    std::string mem_info_str = log_memory_info(true);
    std::istringstream mem_info(mem_info_str);
    std::string value;
    while (std::getline(mem_info, value, ':')) {
        out << b_start << (value+": ") << b_end;
        std::getline(mem_info, value, ';');
        out << value << line_end;
    }

    const Slic3r::UndoRedo::Stack &stack = wxGetApp().plater()->undo_redo_stack_main();
    out << b_start << "RAM size reserved for the Undo / Redo stack: "  << b_end << Slic3r::format_memsize_MB(stack.get_memory_limit()) << line_end;
    out << b_start << "RAM size occupied by the Undo / Redo stack: "  << b_end << Slic3r::format_memsize_MB(stack.memsize()) << line_end << line_end;

    return out.str();
}

SysInfoDialog::SysInfoDialog()
    : DPIDialog(static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, (wxGetApp().is_editor() ? wxString(SLIC3R_APP_FULL_NAME) : wxString(GCODEVIEWER_APP_NAME)) + " - " + _L("System Information"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	wxColour bgr_clr = wxGetApp().get_window_default_clr();//wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
	SetBackgroundColour(bgr_clr);
    SetFont(wxGetApp().normal_font());

    wxBoxSizer* hsizer = new wxBoxSizer(wxHORIZONTAL);
    hsizer->SetMinSize(wxSize(50 * wxGetApp().em_unit(), -1));

	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(hsizer, 1, wxEXPAND | wxALL, 10);

    // logo
    m_logo_bmp = ScalableBitmap(this, wxGetApp().logo_name(), 192);
    m_logo = new wxStaticBitmap(this, wxID_ANY, m_logo_bmp.bmp());
	hsizer->Add(m_logo, 0, wxALIGN_CENTER_VERTICAL);

    wxBoxSizer* vsizer = new wxBoxSizer(wxVERTICAL);
    hsizer->Add(vsizer, 1, wxEXPAND|wxLEFT, 20);

    // title
    {
        wxStaticText* title = new wxStaticText(this, wxID_ANY, wxGetApp().is_editor() ? SLIC3R_APP_FULL_NAME : GCODEVIEWER_APP_NAME, wxDefaultPosition, wxDefaultSize);
        wxFont title_font = wxGetApp().bold_font();
        title_font.SetFamily(wxFONTFAMILY_ROMAN);
        title_font.SetPointSize(22);
        title->SetFont(title_font);
        vsizer->Add(title, 0, wxEXPAND | wxALIGN_LEFT | wxTOP, wxGetApp().em_unit()/*50*/);
    }

    // main_info_text
    wxFont font = get_default_font(this);
    const auto text_clr = wxGetApp().get_label_clr_default();//wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    auto text_clr_str = encode_color(ColorRGB(text_clr.Red(), text_clr.Green(), text_clr.Blue()));
    auto bgr_clr_str = encode_color(ColorRGB(bgr_clr.Red(), bgr_clr.Green(), bgr_clr.Blue()));

    const int fs = font.GetPointSize() - 1;
    int size[] = { static_cast<int>(fs*1.5), static_cast<int>(fs*1.4), static_cast<int>(fs*1.3), fs, fs, fs, fs };

    m_html = new wxHtmlWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHW_SCROLLBAR_NEVER);
    {
        m_html->SetFonts(font.GetFaceName(), font.GetFaceName(), size);
        m_html->SetBorders(2);
		const auto text = wxString::Format(
            "<html>"
            "<body bgcolor= %s link= %s>"
            "<font color=%s>"
            "%s"
            "</font>"
            "</body>"
            "</html>", bgr_clr_str, text_clr_str, text_clr_str,
            get_main_info(true));
        m_html->SetPage(text);
        vsizer->Add(m_html, 1, wxEXPAND | wxBOTTOM, wxGetApp().em_unit());
    }

    // opengl_info
    m_opengl_info_html = new wxHtmlWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHW_SCROLLBAR_AUTO);
    {
        m_opengl_info_html->SetMinSize(wxSize(-1, 16 * wxGetApp().em_unit()));
        m_opengl_info_html->SetFonts(font.GetFaceName(), font.GetFaceName(), size);
        m_opengl_info_html->SetBorders(10);
        wxString blacklisted_libraries_message;
#ifdef WIN32
        std::wstring blacklisted_libraries = BlacklistedLibraryCheck::get_instance().get_blacklisted_string().c_str();
        if (! blacklisted_libraries.empty())
            blacklisted_libraries_message = wxString("<br><b>") + _L("Blacklisted libraries loaded into OrcaSlicer process:") + "</b><br>" + blacklisted_libraries;
#endif // WIN32
       const auto text = GUI::format_wxstr(
            "<html>"
            "<body bgcolor= %s link= %s>"
            "<font color=%s>"
            "%s<br>%s<br>%s<br>%s"
            "</font>"
            "</body>"
            "</html>", bgr_clr_str, text_clr_str, text_clr_str,
            blacklisted_libraries_message,
            get_mem_info(true), wxGetApp().get_gl_info(false),
            "<b>" + _L("SIMD is supported:") + "</b> " + Eigen::SimdInstructionSetsInUse());

        m_opengl_info_html->SetPage(text);
        main_sizer->Add(m_opengl_info_html, 1, wxEXPAND | wxBOTTOM, 15);
    }

    wxStdDialogButtonSizer* buttons = this->CreateStdDialogButtonSizer(wxOK);
    m_btn_copy_to_clipboard = new wxButton(this, wxID_ANY, _L("Copy to Clipboard"), wxDefaultPosition, wxDefaultSize);

    buttons->Insert(0, m_btn_copy_to_clipboard, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 5);
    m_btn_copy_to_clipboard->Bind(wxEVT_BUTTON, &SysInfoDialog::onCopyToClipboard, this);

    this->SetEscapeId(wxID_OK);
    this->Bind(wxEVT_BUTTON, &SysInfoDialog::onCloseDialog, this, wxID_OK);
    main_sizer->Add(buttons, 0, wxEXPAND | wxRIGHT | wxBOTTOM, 3);

    wxGetApp().UpdateDlgDarkUI(this, true);

//     this->Bind(wxEVT_LEFT_DOWN, &SysInfoDialog::onCloseDialog, this);
//     logo->Bind(wxEVT_LEFT_DOWN, &SysInfoDialog::onCloseDialog, this);

	SetSizer(main_sizer);
	main_sizer->SetSizeHints(this);
}

void SysInfoDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    m_logo_bmp.msw_rescale();
    m_logo->SetBitmap(m_logo_bmp.bmp());

    wxFont font = get_default_font(this);
    const int fs = font.GetPointSize() - 1;
    int font_size[] = { static_cast<int>(fs*1.5), static_cast<int>(fs*1.4), static_cast<int>(fs*1.3), fs, fs, fs, fs };

    m_html->SetFonts(font.GetFaceName(), font.GetFaceName(), font_size);
    m_html->Refresh();

    const int& em = em_unit();

    msw_buttons_rescale(this, em, { wxID_OK, m_btn_copy_to_clipboard->GetId() });

    m_opengl_info_html->SetMinSize(wxSize(-1, 16 * em));
    m_opengl_info_html->SetFonts(font.GetFaceName(), font.GetFaceName(), font_size);
    m_opengl_info_html->Refresh();

    const wxSize& size = wxSize(65 * em, 55 * em);

    SetMinSize(size);
    Fit();

    Refresh();
}

void SysInfoDialog::onCopyToClipboard(wxEvent &)
{
    wxTheClipboard->Open();
    const auto text = get_main_info(false) + "\n" + wxGetApp().get_gl_info(true);
    wxTheClipboard->SetData(new wxTextDataObject(text));
    wxTheClipboard->Close();
}

void SysInfoDialog::onCloseDialog(wxEvent &)
{
    this->EndModal(wxID_CLOSE);
}

} // namespace GUI
} // namespace Slic3r
