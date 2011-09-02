#ifndef slic3r_GUI_Label_hpp_
#define slic3r_GUI_Label_hpp_

#include <wx/stattext.h>

#define LB_HYPERLINK 0x0020
#define LB_PROPAGATE_MOUSE_EVENT 0x0040
#define LB_AUTO_WRAP 0x0080


class Label : public wxStaticText
{
public:
    Label(wxWindow *parent, wxString const &text = {}, long style = 0, wxSize size = wxDefaultSize);

	Label(wxWindow *parent, wxFont const &font, wxString const &text = {}, long style = 0, wxSize size = wxDefaultSize);

    void SetLabel(const wxString& label) override;

    void SetWindowStyleFlag(long style) override;

	void Wrap(int width);

private:
	void OnSize(wxSizeEvent & evt);

private:
    wxFont m_font;
    wxColour m_color;
	wxString m_text;
	bool m_skip_size_evt = false;

public:
    static wxFont Head_48;
    static wxFont Head_32;
	static wxFont Head_24;
	static wxFont Head_20;
	static wxFont Head_18;
	static wxFont Head_16;
	static wxFont Head_15;
	static wxFont Head_14;
	static wxFont Head_13;
	static wxFont Head_12;
	static wxFont Head_11;
    static wxFont Head_10;

	static wxFont Body_16;
	static wxFont Body_15;
	static wxFont Body_14;
    static wxFont Body_13;
	static wxFont Body_12;
	static wxFont Body_10;
	static wxFont Body_11;
	static wxFont Body_9;
	static wxFont Body_8;

	static void initSysFont();

    static wxFont sysFont(int size, bool bold = false);

    static wxSize split_lines(wxDC &dc, int width, const wxString &text, wxString &multiline_text, int max_count = 0);
};

#endif // !slic3r_GUI_Label_hpp_
