#include "RecordingDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "Mcp/McpPaths.hpp"
#include "Mcp/VideoEncode.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/SpinInput.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/StaticLine.hpp"
#include "Widgets/SwitchButton.hpp"
#include "Widgets/TextInput.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/system/error_code.hpp>

#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/dcbuffer.h>
#include <wx/dirdlg.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/graphics.h>
#include <wx/panel.h>
#include <wx/pen.h>
#include <wx/sizer.h>
#include <wx/spinbutt.h>
#include <wx/stdpaths.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;

// A stage's node on the rail and, for a stage another follows, the line down to it: teal where the stage runs,
// else a hollow grey node and a dashed line. Painted, the node level with the stage's title.
class StageRail : public wxPanel
{
public:
    StageRail(wxWindow* parent, bool line) : wxPanel(parent, wxID_ANY), m_line(line)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(FromDIP(20), -1));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    }

    void set_state(bool node_on, bool line_on)
    {
        if (node_on == m_node_on && line_on == m_line_on)
            return;
        m_node_on = node_on;
        m_line_on = line_on;
        Refresh();
    }

private:
    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        // The dialog's colour, which dark mode changes after this panel exists.
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        const std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        const wxColour teal("#009688");
        const wxColour grey = StateColor::darkModeColorFor(wxColour("#B4B4B4"));
        const wxSize   size = GetClientSize();
        const double   x = size.x / 2., y = FromDIP(11), r = FromDIP(6);
        if (m_line) {
            gc->SetPen(wxPen(m_line_on ? teal : grey, FromDIP(2), m_line_on ? wxPENSTYLE_SOLID : wxPENSTYLE_SHORT_DASH));
            gc->StrokeLine(x, y + r + FromDIP(4), x, size.y);
        }
        if (m_node_on) {
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(teal));
            gc->DrawEllipse(x - r, y - r, 2 * r, 2 * r);
        } else {
            const double inner = r - FromDIP(1); // the ring's stroke centred inside the node
            gc->SetPen(wxPen(grey, FromDIP(2)));
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->DrawEllipse(x - inner, y - inner, 2 * inner, 2 * inner);
        }
    }

    bool m_line;
    bool m_node_on = true;
    bool m_line_on = true;
};

namespace {

// The codecs as the combo lists them; the names are the formats' own, not translated.
const std::vector<std::pair<std::string, wxString>>& codecs()
{
    static const std::vector<std::pair<std::string, wxString>> list{{"h265", "H.265 (HEVC)"}, {"h264", "H.264 (AVC)"}};
    return list;
}

int index_of(const std::vector<std::string>& values, const std::string& value)
{
    const auto it = std::find(values.begin(), values.end(), value);
    return it == values.end() ? 0 : int(it - values.begin());
}

// The spinner's value with the text still being typed taken in: GetValue() is the last value committed.
int typed_value(SpinInput* spin)
{
    long typed = 0;
    if (spin->GetTextCtrl()->GetValue().ToLong(&typed))
        return std::clamp(int(typed), spin->GetMin(), spin->GetMax());
    return spin->GetValue();
}

fs::path folder_or_default(const std::string& folder)
{
    boost::system::error_code ec;
    if (!folder.empty())
        if (const fs::path chosen = Mcp::path_from_utf8(folder); fs::is_directory(chosen, ec))
            return chosen;
    return default_capture_folder();
}

wxColour secondary_text() { return StateColor::darkModeColorFor(wxColour("#6B6B6B")); }

// What a stage makes, in a rounded outline beside its title.
::Label* add_output(wxWindow* parent, wxSizer* header, const wxString& text)
{
    auto* box = new StaticBox(parent);
    box->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    box->SetBackgroundColor(StateColor(StateColor::darkModeColorFor(*wxWHITE)));
    box->SetBorderColor(StateColor(StateColor::darkModeColorFor(wxColour("#CECECE"))));
    box->SetCornerRadius(parent->FromDIP(9));
    auto* label = new ::Label(box, Label::Body_12, text);
    label->SetForegroundColour(secondary_text());
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, parent->FromDIP(3));
    sizer->InsertSpacer(0, parent->FromDIP(5));
    sizer->AddSpacer(parent->FromDIP(5));
    box->SetSizer(sizer);
    header->Add(box, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, parent->FromDIP(8));
    return label;
}

::Label* stage_title(wxWindow* parent, const wxString& text) { return new ::Label(parent, Label::Head_15, text); }

::Label* stage_text(wxWindow* parent, const wxString& text)
{
    auto* label = new ::Label(parent, Label::Body_13, text);
    label->SetForegroundColour(secondary_text());
    label->Wrap(parent->FromDIP(440));
    return label;
}

} // namespace

fs::path default_capture_folder()
{
    const fs::path            downloads = into_path(wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Downloads));
    boost::system::error_code ec;
    return !downloads.empty() && fs::is_directory(downloads, ec) ? downloads : Mcp::output_dir();
}

fs::path capture_folder(const Mcp::RecordingSettings& settings) { return folder_or_default(settings.folder); }

RecordingDialog::RecordingDialog(wxWindow* parent, const Mcp::RecordingSettings& settings)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, _L("Start Recording"), wxDefaultPosition,
                wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_encode_on(settings.encode)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    std::string                    err;
    const std::optional<fs::path> ffmpeg = Mcp::find_ffmpeg(err);
    m_has_ffmpeg                         = ffmpeg.has_value();
    boost::system::error_code ec;
    if (!settings.folder.empty() && fs::is_directory(Mcp::path_from_utf8(settings.folder), ec))
        m_folder = settings.folder;

    const auto field_label = [this](const wxString& text) {
        auto* label = new ::Label(this, Label::Body_12, text);
        label->SetForegroundColour(secondary_text());
        return label;
    };
    // A field under its label; an encoding one turns off with the stage.
    const auto field = [&](const wxString& text, wxWindow* control) {
        auto* label  = field_label(text);
        auto* column = new wxBoxSizer(wxVERTICAL);
        column->Add(label, 0, wxBOTTOM, FromDIP(4));
        column->Add(control, 0, wxEXPAND);
        m_encoding.insert(m_encoding.end(), {label, control});
        return column;
    };
    const auto combo = [this](const char* name) {
        auto* c = new ::ComboBox(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(200), -1), 0, nullptr, wxCB_READONLY);
        c->SetName(name);
        return c;
    };

    // Record: the .mkv.
    m_record_rail = new StageRail(this, true);
    auto* record  = new wxBoxSizer(wxVERTICAL);
    auto* record_header = new wxBoxSizer(wxHORIZONTAL);
    record_header->Add(stage_title(this, _L("Record")), 0, wxALIGN_CENTER_VERTICAL);
    add_output(this, record_header, _L("MJPEG in .mkv"));
    record->Add(record_header, 0, wxEXPAND);
    record->Add(stage_text(this, _L("Each frame is kept as a JPEG while you work, so recording stays light on the app.")), 0,
                wxTOP | wxBOTTOM, FromDIP(4));
    m_quality = new SpinInput(this, wxEmptyString, "", wxDefaultPosition, wxSize(FromDIP(140), -1), wxSP_ARROW_KEYS, 30, 95,
                              settings.quality);
    m_quality->SetName("quality");
    auto* quality = new wxBoxSizer(wxVERTICAL);
    quality->Add(field_label(_L("JPEG quality")), 0, wxBOTTOM, FromDIP(4));
    quality->Add(m_quality, 0);
    auto* quality_row = new wxBoxSizer(wxHORIZONTAL);
    quality_row->Add(quality, 0);
    auto* quality_hint = field_label(_L("30 to 95. Higher is sharper and makes a larger file."));
    quality_row->Add(quality_hint, 0, wxALIGN_BOTTOM | wxLEFT | wxBOTTOM, FromDIP(8));
    record->Add(quality_row, 0, wxTOP | wxBOTTOM, FromDIP(8));
    // The overlays, each switched at its row's end as encoding is in its stage's header.
    const auto overlay = [&](const char* name, bool on, const wxString& title, const wxString& hint) {
        auto* toggle = new ::SwitchButton(this);
        toggle->SetName(name);
        toggle->SetValue(on);
        auto* text = new wxBoxSizer(wxVERTICAL);
        text->Add(new ::Label(this, Label::Body_14, title), 0);
        text->Add(field_label(hint), 0, wxTOP, FromDIP(2));
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(text, 1, wxALIGN_CENTER_VERTICAL);
        row->Add(toggle, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
        record->Add(row, 0, wxEXPAND | wxTOP, FromDIP(8));
        return toggle;
    };
    m_pointer  = overlay("pointer", settings.pointer, _L("Highlight the pointer"),
                         _L("A ring follows the pointer, and its clicks ripple."));
    m_captions = overlay("captions", settings.captions, _L("Caption each step"),
                         _L("Names each step an agent or script takes, at the bottom."));
    record->AddSpacer(FromDIP(14));
    auto* record_row = new wxBoxSizer(wxHORIZONTAL);
    record_row->Add(m_record_rail, 0, wxEXPAND | wxRIGHT, FromDIP(14));
    record_row->Add(record, 1, wxEXPAND);

    // Encode to MP4: the .mp4 ffmpeg makes of it.
    m_encode_rail = new StageRail(this, false);
    auto* encode  = new wxBoxSizer(wxVERTICAL);
    auto* encode_header = new wxBoxSizer(wxHORIZONTAL);
    encode_header->Add(stage_title(this, _L("Encode to MP4")), 0, wxALIGN_CENTER_VERTICAL);
    m_output = add_output(this, encode_header, wxEmptyString);
    encode_header->AddStretchSpacer();
    m_encode = new ::SwitchButton(this);
    m_encode->SetName("encode");
    encode_header->Add(m_encode, 0, wxALIGN_CENTER_VERTICAL);
    encode->Add(encode_header, 0, wxEXPAND);
    encode->Add(stage_text(this, _L("When the recording ends, ffmpeg turns it into an MP4 that is easy to share. The .mkv is kept.")), 0,
                wxTOP | wxBOTTOM, FromDIP(4));
    if (!m_has_ffmpeg) {
        const bool dark = wxGetApp().dark_mode();
        auto*      note = new StaticBox(this);
        const wxColour fill(dark ? "#4A3B1E" : "#FFF4E0");
        note->SetBackgroundColour(fill);
        note->SetBackgroundColor(StateColor(fill));
        note->SetBorderWidth(0);
        note->SetCornerRadius(FromDIP(6));
        // TRN ORCA_FFMPEG is an environment variable.
        auto* text = new ::Label(note, Label::Body_13,
                                 _L("ffmpeg wasn't found, so recordings can't be encoded. Install ffmpeg with libx265 and libx264, or "
                                    "set ORCA_FFMPEG to one, then open this dialog again."));
        text->SetForegroundColour(wxColour(dark ? "#F2C46B" : "#8A5300"));
        text->Wrap(FromDIP(412));
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(text, 0, wxALL, FromDIP(10));
        note->SetSizer(sizer);
        encode->Add(note, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));
    }

    m_preset = combo("encode_preset");
    for (const Mcp::EncodePreset& p : Mcp::encode_presets())
        m_preset->Append(_L(p.name));
    m_preset->Append(_L("Custom"));
    encode->Add(field(_L("Preset"), m_preset), 0, wxEXPAND | wxTOP, FromDIP(8));

    m_codec = combo("codec");
    for (const auto& codec : codecs())
        m_codec->Append(codec.second);
    m_profile = combo("profile");
    m_crf     = new SpinInput(this, wxEmptyString, "", wxDefaultPosition, wxSize(FromDIP(200), -1), wxSP_ARROW_KEYS, 0, 51, settings.crf);
    m_crf->SetName("crf");
    m_speed = combo("speed");
    for (const std::string& speed : Mcp::encoder_speeds())
        m_speed->Append(from_u8(speed));
    auto* grid = new wxFlexGridSizer(2, FromDIP(12), FromDIP(12));
    grid->AddGrowableCol(0, 1);
    grid->AddGrowableCol(1, 1);
    grid->Add(field(_L("Codec"), m_codec), 0, wxEXPAND);
    grid->Add(field(_L("Profile"), m_profile), 0, wxEXPAND);
    grid->Add(field(_L("Quality (CRF), lower is sharper"), m_crf), 0, wxEXPAND);
    grid->Add(field(_L("Speed, slower is smaller"), m_speed), 0, wxEXPAND);
    encode->Add(grid, 0, wxEXPAND | wxTOP, FromDIP(12));
    if (m_has_ffmpeg) {
        auto* uses = field_label(format_wxstr(_L("Uses %1%"), from_path(*ffmpeg)));
        m_encoding.push_back(uses);
        encode->Add(uses, 0, wxTOP, FromDIP(12));
    }
    auto* encode_row = new wxBoxSizer(wxHORIZONTAL);
    encode_row->Add(m_encode_rail, 0, wxEXPAND | wxRIGHT, FromDIP(14));
    encode_row->Add(encode, 1, wxEXPAND);

    // Save in: where the .mkv, its .mp4 and screenshots go.
    m_folder_text = new ::TextInput(this, wxEmptyString, "", "", wxDefaultPosition, wxSize(FromDIP(300), -1), wxTE_READONLY);
    m_folder_text->SetName("folder");
    m_change_folder = new ::Button(this, _L("Change..."));
    m_change_folder->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    auto* folder_row = new wxBoxSizer(wxHORIZONTAL);
    folder_row->Add(m_folder_text, 1, wxALIGN_CENTER_VERTICAL);
    folder_row->Add(m_change_folder, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    m_default_link = new ::Label(this, Label::Body_12, _L("Use the default folder"));
    m_default_link->SetWindowStyleFlag(m_default_link->GetWindowStyle() | LB_HYPERLINK);
    auto* folder_note = new wxBoxSizer(wxHORIZONTAL);
    folder_note->Add(field_label(_L("Recordings, their MP4s and screenshots are saved here.")), 0, wxALIGN_CENTER_VERTICAL);
    folder_note->Add(m_default_link, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    auto* save_in = new wxBoxSizer(wxVERTICAL);
    save_in->Add(field_label(_L("Save in")), 0, wxBOTTOM, FromDIP(4));
    save_in->Add(folder_row, 0, wxEXPAND);
    save_in->Add(folder_note, 0, wxTOP, FromDIP(6));

    auto* buttons = new DialogButtons(this, {L("Start recording"), "Cancel"}, _L("Start recording"));

    auto* root = new wxBoxSizer(wxVERTICAL);
    root->SetMinSize(wxSize(FromDIP(540), -1));
    root->Add(record_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(24));
    root->Add(encode_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24));
    // A line without a label has no height of its own.
    m_divider = new StaticLine(this);
    m_divider->SetLineColour(StateColor::darkModeColorFor(wxColour("#E4E4E4")));
    m_divider->SetMinSize(wxSize(-1, FromDIP(1)));
    root->Add(m_divider, 0, wxEXPAND | wxTOP, FromDIP(20));
    root->Add(save_in, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(24));
    root->AddSpacer(FromDIP(8));
    root->Add(buttons, 0, wxEXPAND);

    show_encoding(settings);
    update_encoding();
    show_folder();

    m_encode->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        e.Skip();
        m_encode_on = m_encode->GetValue();
        update_encoding();
    });
    m_preset->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
        const std::vector<Mcp::EncodePreset>& presets = Mcp::encode_presets();
        const int                             i       = m_preset->GetSelection();
        if (i < 0 || std::size_t(i) >= presets.size())
            return; // Custom keeps the fields as they are
        Mcp::RecordingSettings s = this->settings();
        Mcp::apply_encode_preset(s, presets[std::size_t(i)]);
        show_encoding(s);
    });
    // A codec's profiles differ: settings() takes the profile shown, or the new codec's default.
    m_codec->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { show_encoding(this->settings()); });
    m_speed->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { show_preset(); });
    m_profile->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { show_preset(); });
    m_crf->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) { show_preset(); });
    m_change_folder->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxDirDialog dialog(this, _L("Choose where recordings and screenshots are saved"), from_path(folder_or_default(m_folder)),
                           wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK)
            return;
        const fs::path chosen = into_path(dialog.GetPath());
        // The default stays the default, so it follows the Downloads folder if that moves.
        m_folder = chosen == default_capture_folder() ? std::string() : Mcp::path_to_utf8(chosen);
        show_folder();
    });
    // Out of the mouse handler's stack: the link hides itself and the layout changes.
    m_default_link->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
        CallAfter([this] {
            m_folder.clear();
            show_folder();
        });
    });
    buttons->GetFIRST()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    SetSizerAndFit(root);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

Mcp::RecordingSettings RecordingDialog::settings() const
{
    Mcp::RecordingSettings s;
    s.quality  = typed_value(m_quality);
    s.captions = m_captions->GetValue();
    s.pointer  = m_pointer->GetValue();
    s.encode   = m_encode_on;
    s.codec   = codecs()[std::size_t(std::max(0, m_codec->GetSelection()))].first;
    s.crf     = typed_value(m_crf);
    s.speed   = Mcp::encoder_speeds()[std::size_t(std::max(0, m_speed->GetSelection()))];
    // The profile shown when the codec has it, else the codec's default.
    const std::vector<std::string> profiles = Mcp::codec_profiles(s.codec);
    s.profile                               = profiles[std::size_t(index_of(profiles, into_u8(m_profile->GetValue())))];
    s.folder                                = m_folder;
    return s;
}

void RecordingDialog::show_encoding(const Mcp::RecordingSettings& settings)
{
    std::vector<std::string> codec_keys;
    for (const auto& codec : codecs())
        codec_keys.push_back(codec.first);
    m_codec->SetSelection(index_of(codec_keys, settings.codec));
    const std::vector<std::string> profiles = Mcp::codec_profiles(settings.codec);
    m_profile->Clear();
    for (const std::string& profile : profiles)
        m_profile->Append(from_u8(profile));
    m_profile->SetSelection(index_of(profiles, settings.profile));
    m_crf->SetValue(settings.crf);
    m_speed->SetSelection(index_of(Mcp::encoder_speeds(), settings.speed));
    show_preset();
}

void RecordingDialog::show_preset()
{
    const Mcp::RecordingSettings s = settings();
    std::vector<std::string>     keys;
    for (const Mcp::EncodePreset& p : Mcp::encode_presets())
        keys.emplace_back(p.key);
    keys.emplace_back("custom");
    m_preset->SetSelection(index_of(keys, Mcp::matching_encode_preset(s)));
    // TRN %1% is a codec's name, such as H.265.
    m_output->SetLabel(format_wxstr(_L("%1% in .mp4"), s.codec == "h265" ? "H.265" : "H.264"));
    if (wxSizer* sizer = m_output->GetParent()->GetSizer())
        sizer->Layout();
    Layout();
}

void RecordingDialog::update_encoding()
{
    const bool runs = m_has_ffmpeg && m_encode_on;
    m_encode->SetValue(runs);
    m_encode->Enable(m_has_ffmpeg);
    // Orca widgets keep their enabled look when only their parent is disabled.
    for (wxWindow* w : m_encoding)
        w->Enable(runs);
    m_output->Enable(runs);
    m_record_rail->set_state(true, runs);
    m_encode_rail->set_state(runs, false);
}

void RecordingDialog::show_folder()
{
    m_folder_text->GetTextCtrl()->SetValue(from_path(folder_or_default(m_folder)));
    m_default_link->Show(!m_folder.empty());
    Layout();
}

void RecordingDialog::on_dpi_changed(const wxRect&)
{
    m_quality->Rescale();
    m_quality->SetMinSize(wxSize(FromDIP(140), -1));
    m_crf->Rescale();
    m_crf->SetMinSize(wxSize(FromDIP(200), -1));
    for (::ComboBox* c : {m_preset, m_codec, m_speed, m_profile}) {
        c->Rescale();
        c->SetMinSize(wxSize(FromDIP(200), -1));
    }
    for (::SwitchButton* toggle : {m_pointer, m_captions, m_encode})
        toggle->Rescale();
    m_folder_text->Rescale();
    m_change_folder->Rescale();
    for (StageRail* rail : {m_record_rail, m_encode_rail})
        rail->SetMinSize(wxSize(FromDIP(20), -1));
    m_divider->SetMinSize(wxSize(-1, FromDIP(1)));
    GetSizer()->SetMinSize(wxSize(FromDIP(540), -1));
    GetSizer()->SetSizeHints(this);
    Refresh();
}

}} // namespace Slic3r::GUI
