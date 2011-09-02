#ifndef slic3r_MsgDialog_hpp_
#define slic3r_MsgDialog_hpp_

#include <string>
#include <unordered_map>
#include "GUI_Utils.hpp"
#include <wx/dialog.h>
#include <wx/font.h>
#include <wx/bitmap.h>
#include <wx/msgdlg.h>
#include <wx/richmsgdlg.h>
#include <wx/textctrl.h>
#include <wx/statline.h>
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/HyperLink.hpp"
#include "BBLStatusBar.hpp"
#include "BBLStatusBarSend.hpp"
#include "libslic3r/Semver.hpp"

class wxBoxSizer;
class wxCheckBox;
class wxStaticBitmap;

enum ButtonSizeType{
	ButtonSizeNormal = 0,
	ButtonSizeMiddle = 1,
	ButtonSizeLong	 = 2,
};

#define MSG_DIALOG_BUTTON_SIZE wxSize(FromDIP(58), FromDIP(24))
#define MSG_DIALOG_MIDDLE_BUTTON_SIZE wxSize(FromDIP(76), FromDIP(24))
#define MSG_DIALOG_LONG_BUTTON_SIZE wxSize(FromDIP(90), FromDIP(24))
#define MSG_DIALOG_LONGER_BUTTON_SIZE wxSize(FromDIP(120), FromDIP(24))


namespace Slic3r {
namespace GUI {

struct ButtonData
{
    ButtonSizeType type;
	Button * button;
};

class MsgButton
{
public:
    wxString id;
    ButtonData *buttondata;
};

WX_DECLARE_HASH_MAP(wxString, MsgButton *, wxStringHash, wxStringEqual, MsgButtonsHash);

// A message / query dialog with a bitmap on the left and any content on the right
// with buttons underneath.
struct MsgDialog : DPIDialog
{
	MsgDialog(MsgDialog &&) = delete;
	MsgDialog(const MsgDialog &) = delete;
	MsgDialog &operator=(MsgDialog &&) = delete;
	MsgDialog &operator=(const MsgDialog &) = delete;
	virtual ~MsgDialog();

	void show_dsa_button(wxString const & title = {});
	bool get_checkbox_state();
	virtual void on_dpi_changed(const wxRect& suggested_rect);
	void SetButtonLabel(wxWindowID btn_id, const wxString& label, bool set_focus = false);

protected:
	enum {
		BORDER = 20,
		LOGO_SPACING = 35,
		LOGO_GAP = 20,
		CONTENT_WIDTH = 242,
		CONTENT_MAX_HEIGHT = 60,//TO
		BTN_SPACING = 20,
		VERT_SPACING = 15,//TO
	};

	MsgDialog(wxWindow *parent, const wxString &title, const wxString &headline, long style = wxOK, wxBitmap bitmap = wxNullBitmap, const wxString &forward_str = "");
	// returns pointer to created button
	Button* add_button(wxWindowID btn_id, bool set_focus = false, const wxString& label = wxString());
	// returns pointer to found button or NULL
	Button* get_button(wxWindowID btn_id);
	void apply_style(long style);
	void finalize();

	wxFont boldfont;
	wxBoxSizer *content_sizer;
	wxBoxSizer *btn_sizer;
	wxBoxSizer *m_dsa_sizer;
	wxStaticBitmap *logo;
    MsgButtonsHash  m_buttons;
	CheckBox* m_checkbox_dsa{nullptr};
    wxString  m_forward_str;
};


// Generic error dialog, used for displaying exceptions
class ErrorDialog : public MsgDialog
{
public:
	// If monospaced_font is true, the error message is displayed using html <code><pre></pre></code> tags,
	// so that the code formatting will be preserved. This is useful for reporting errors from the placeholder parser.
	ErrorDialog(wxWindow *parent, const wxString &temp_msg, bool courier_font);
	ErrorDialog(ErrorDialog &&) = delete;
	ErrorDialog(const ErrorDialog &) = delete;
	ErrorDialog &operator=(ErrorDialog &&) = delete;
	ErrorDialog &operator=(const ErrorDialog &) = delete;
	virtual ~ErrorDialog() = default;

private:
	wxString msg;
};


// Generic warning dialog, used for displaying exceptions
class WarningDialog : public MsgDialog
{
public:
	WarningDialog(	wxWindow *parent,
		            const wxString& message,
		            const wxString& caption = wxEmptyString,
		            long style = wxOK);
	WarningDialog(WarningDialog&&) = delete;
	WarningDialog(const WarningDialog&) = delete;
	WarningDialog &operator=(WarningDialog&&) = delete;
	WarningDialog &operator=(const WarningDialog&) = delete;
	virtual ~WarningDialog() = default;
};

wxString get_wraped_wxString(const wxString& text_in, size_t line_len = 80);

#if 1
// Generic static line, used intead of wxStaticLine
//class StaticLine: public wxTextCtrl
//{
//public:
//	StaticLine( wxWindow* parent,
//				wxWindowID id = wxID_ANY,
//				const wxPoint& pos = wxDefaultPosition,
//				const wxSize& size = wxDefaultSize,
//				long style = wxLI_HORIZONTAL,
//				const wxString& name = wxString::FromAscii(wxTextCtrlNameStr))
//	: wxTextCtrl(parent, id, wxEmptyString, pos, size!=wxDefaultSize ? size : (style == wxLI_HORIZONTAL ? wxSize(10, 1) : wxSize(1, 10)), wxSIMPLE_BORDER, wxDefaultValidator, name)
//	{
//		this->Enable(false);
//	}
//	~StaticLine() {}
//};

// Generic message dialog, used intead of wxMessageDialog
class MessageDialog : public MsgDialog
{
public:
	// NOTE! Don't change a signature of contsrucor. It have to  be tha same as for wxMessageDialog
    MessageDialog(wxWindow       *parent,
                  const wxString &message,
                  const wxString &caption     = wxEmptyString,
                  long            style       = wxOK,
                  const wxString &forward_str = "",
                  const wxString &link_text   = "",
                  std::function<void(const wxString &)> link_callback = nullptr);
	MessageDialog(MessageDialog&&) = delete;
	MessageDialog(const MessageDialog&) = delete;
	MessageDialog &operator=(MessageDialog&&) = delete;
	MessageDialog &operator=(const MessageDialog&) = delete;
	virtual ~MessageDialog() = default;
};

// Generic rich message dialog, used intead of wxRichMessageDialog
class RichMessageDialog : public MsgDialog
{
	wxCheckBox* m_checkBox{ nullptr };
	wxString	m_checkBoxText;
	bool		m_checkBoxValue{ false };

public:
	// NOTE! Don't change a signature of contsrucor. It have to  be tha same as for wxRichMessageDialog
	RichMessageDialog(	wxWindow *parent,
						const wxString& message,
						const wxString& caption = wxEmptyString,
						long style = wxOK);
	RichMessageDialog(RichMessageDialog&&) = delete;
	RichMessageDialog(const RichMessageDialog&) = delete;
	RichMessageDialog &operator=(RichMessageDialog&&) = delete;
	RichMessageDialog &operator=(const RichMessageDialog&) = delete;
	virtual ~RichMessageDialog() = default;

	int  ShowModal() override;

	void ShowCheckBox(const wxString& checkBoxText, bool checked = false)
	{
		m_checkBoxText = checkBoxText;
		m_checkBoxValue = checked;
	}

	wxString	GetCheckBoxText()	const { return m_checkBoxText; }
	bool		IsCheckBoxChecked() const;

// This part o fcode isported from the "wx\msgdlg.h"
	using wxMD = wxMessageDialogBase;
	// customization of the message box buttons
	virtual bool SetYesNoLabels(const wxMD::ButtonLabel& yes, const wxMD::ButtonLabel& no)
	{
		DoSetCustomLabel(m_yes, yes);
		DoSetCustomLabel(m_no, no);
		return true;
	}

	virtual bool SetYesNoCancelLabels(const wxMD::ButtonLabel& yes,
		const wxMD::ButtonLabel& no,
		const wxMD::ButtonLabel& cancel)
	{
		DoSetCustomLabel(m_yes, yes);
		DoSetCustomLabel(m_no, no);
		DoSetCustomLabel(m_cancel, cancel);
		return true;
	}

	virtual bool SetOKLabel(const wxMD::ButtonLabel& ok)
	{
		DoSetCustomLabel(m_ok, ok);
		return true;
}

	virtual bool SetOKCancelLabels(const wxMD::ButtonLabel& ok,
		const wxMD::ButtonLabel& cancel)
	{
		DoSetCustomLabel(m_ok, ok);
		DoSetCustomLabel(m_cancel, cancel);
		return true;
	}

	virtual bool SetHelpLabel(const wxMD::ButtonLabel& help)
	{
		DoSetCustomLabel(m_help, help);
		return true;
	}
	// test if any custom labels were set
	bool HasCustomLabels() const
	{
		return !(m_ok.empty() && m_cancel.empty() && m_help.empty() &&
			m_yes.empty() && m_no.empty());
	}

	// these functions return the label to be used for the button which is
	// either a custom label explicitly set by the user or the default label,
	// i.e. they always return a valid string
	wxString GetYesLabel() const
	{
		return m_yes.empty() ? GetDefaultYesLabel() : m_yes;
	}
	wxString GetNoLabel() const
	{
		return m_no.empty() ? GetDefaultNoLabel() : m_no;
	}
	wxString GetOKLabel() const
	{
		return m_ok.empty() ? GetDefaultOKLabel() : m_ok;
	}
	wxString GetCancelLabel() const
	{
		return m_cancel.empty() ? GetDefaultCancelLabel() : m_cancel;
	}
	wxString GetHelpLabel() const
	{
		return m_help.empty() ? GetDefaultHelpLabel() : m_help;
	}

protected:
	// this function is called by our public SetXXXLabels() and should assign
	// the value to var with possibly some transformation (e.g. Cocoa version
	// currently uses this to remove any accelerators from the button strings
	// while GTK+ one handles stock items specifically here)
	void DoSetCustomLabel(wxString& var, const wxMD::ButtonLabel& label)
	{
		var = label.GetAsString();
	}

	// these functions return the custom label or empty string and should be
	// used only in specific circumstances such as creating the buttons with
	// these labels (in which case it makes sense to only use a custom label if
	// it was really given and fall back on stock label otherwise), use the
	// Get{Yes,No,OK,Cancel}Label() methods above otherwise
	const wxString& GetCustomYesLabel() const { return m_yes; }
	const wxString& GetCustomNoLabel() const { return m_no; }
	const wxString& GetCustomOKLabel() const { return m_ok; }
	const wxString& GetCustomHelpLabel() const { return m_help; }
	const wxString& GetCustomCancelLabel() const { return m_cancel; }

private:
	// these functions may be overridden to provide different defaults for the
	// default button labels (this is used by wxGTK)
	virtual wxString GetDefaultYesLabel() const { return wxGetTranslation("Yes"); }
	virtual wxString GetDefaultNoLabel() const { return wxGetTranslation("No"); }
	virtual wxString GetDefaultOKLabel() const { return wxGetTranslation("OK"); }
	virtual wxString GetDefaultCancelLabel() const { return wxGetTranslation("Cancel"); }
	virtual wxString GetDefaultHelpLabel() const { return wxGetTranslation("Help"); }

	// labels for the buttons, initially empty meaning that the defaults should
	// be used, use GetYes/No/OK/CancelLabel() to access them
	wxString m_yes,
		m_no,
		m_ok,
		m_cancel,
		m_help;
};
#else
// just a wrapper for wxStaticLine to use the same code on all platforms
class StaticLine : public wxStaticLine
{
public:
	StaticLine(wxWindow* parent,
		wxWindowID id = wxID_ANY,
		const wxPoint& pos = wxDefaultPosition,
		const wxSize& size = wxDefaultSize,
		long style = wxLI_HORIZONTAL,
		const wxString& name = wxString::FromAscii(wxStaticLineNameStr))
		: wxStaticLine(parent, id, pos, size, style, name) {}
	~StaticLine() {}
};
// just a wrapper to wxMessageBox to use the same code on all platforms
class MessageDialog : public wxMessageDialog
{
public:
	MessageDialog(wxWindow* parent,
		const wxString& message,
		const wxString& caption = wxEmptyString,
		long style = wxOK)
    : wxMessageDialog(parent, message, caption, style) {}
	~MessageDialog() {}
};

// just a wrapper to wxRichMessageBox to use the same code on all platforms
class RichMessageDialog : public wxRichMessageDialog
{
public:
	RichMessageDialog(wxWindow* parent,
		const wxString& message,
		const wxString& caption = wxEmptyString,
		long style = wxOK)
    : wxRichMessageDialog(parent, message, caption, style) {
		this->SetEscapeId(wxID_CANCEL);
	}
	~RichMessageDialog() {}
};
#endif

// Generic info dialog, used for displaying exceptions
class InfoDialog : public MsgDialog
{
public:
	InfoDialog(wxWindow *parent, const wxString &title, const wxString &msg, bool is_marked = false, long style = wxOK| wxICON_INFORMATION);
	InfoDialog(InfoDialog&&) = delete;
	InfoDialog(const InfoDialog&) = delete;
	InfoDialog&operator=(InfoDialog&&) = delete;
	InfoDialog&operator=(const InfoDialog&) = delete;
	virtual ~InfoDialog() = default;

private:
	wxString msg;
};

class DownloadDialog : public MsgDialog
{
public:
    DownloadDialog(wxWindow *parent, const wxString &title, const wxString &msg, bool is_marked = false, long style = wxOK | wxCANCEL);
    DownloadDialog(InfoDialog &&)      = delete;
    DownloadDialog(const InfoDialog &) = delete;
    DownloadDialog &operator=(InfoDialog &&) = delete;
    DownloadDialog &operator=(const InfoDialog &) = delete;
    virtual ~DownloadDialog()                     = default;

	void SetExtendedMessage(const wxString &extendedMessage);

private:
    wxString msg;
};

class DeleteConfirmDialog : public DPIDialog
{
public:
    DeleteConfirmDialog(wxWindow *parent, const wxString &title, const wxString &msg);
    ~DeleteConfirmDialog();
    virtual void on_dpi_changed(const wxRect &suggested_rect);

private:
    wxString      msg;
    Button *      m_del_btn    = nullptr;
    Button *      m_cancel_btn = nullptr;
    wxStaticText *m_msg_text   = nullptr;
};

class Newer3mfVersionDialog : public DPIDialog
{
public:
    Newer3mfVersionDialog(wxWindow *parent, const Semver* file_version, const Semver* cloud_version, wxString new_keys);
    ~Newer3mfVersionDialog(){};
    virtual void on_dpi_changed(const wxRect &suggested_rect){};

private:
    wxBoxSizer *get_msg_sizer();
    wxBoxSizer *get_btn_sizer();


private:
    const Semver *m_file_version;
    const Semver *m_cloud_version;
    wxString      m_new_keys;
    Button *      m_update_btn = nullptr;
    Button *      m_later_btn  = nullptr;
    wxStaticText *m_msg_text   = nullptr;
};


class NetworkErrorDialog : public DPIDialog
{
public:
    NetworkErrorDialog(wxWindow* parent);
    ~NetworkErrorDialog() {};
    virtual void on_dpi_changed(const wxRect& suggested_rect) {};

private:
    Label* m_text_basic;
    HyperLink* m_link_server_state; // ORCA
    Label* m_text_proposal;
    HyperLink* m_text_wiki; // ORCA
    Button *         m_button_confirm;

public:
    bool m_show_again{false};
};

}
}

#endif
