#pragma once

#include "GUI_Utils.hpp"
#include "Mcp/RecordingSettings.hpp"

#include <boost/filesystem/path.hpp>

#include <string>
#include <vector>

class Button;
class ComboBox;
class Label;
class SpinInput;
class StaticBox;
class StaticLine;
class SwitchButton;
class TextInput;
class wxRect;
class wxWindow;

namespace Slic3r { namespace GUI {

class StageRail;

// The person's Downloads folder, where their screenshots and recordings go unless they chose another; the MCP
// output folder when there is none.
boost::filesystem::path default_capture_folder();
// The folder `settings` names while it exists, else the default.
boost::filesystem::path capture_folder(const Mcp::RecordingSettings& settings);

// Start Recording's dialog. Its two stages say which settings make which file: Record, the JPEG quality of
// the .mkv the frames go into and whether they show the pointer and captions, and Encode to MP4, which ffmpeg
// runs once the recording ends, from a built-in preset or field by field. A rail joins them, showing whether
// the second runs; it can't without an ffmpeg, and the stage then says how to get one. Below them is the
// folder both files go in. Each control is named after the key of the command's parameter it sets, so an
// agent can set it with ui_input.
class RecordingDialog : public DPIDialog
{
public:
    RecordingDialog(wxWindow* parent, const Mcp::RecordingSettings& settings);

    // What the dialog shows, the text still being typed in a number field included. `encode` is the switch the
    // person left, kept while no ffmpeg can encode.
    Mcp::RecordingSettings settings() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    // The encoding fields of `settings`, the codec's profiles, and the preset they match.
    void show_encoding(const Mcp::RecordingSettings& settings);
    // The preset the fields match, after one of them changed, and the codec in the stage's output.
    void show_preset();
    // Encoding runs when the switch is on and an ffmpeg was found: the switch, the fields and the rail follow.
    void update_encoding();
    void show_folder();

    bool        m_has_ffmpeg = false;
    bool        m_encode_on  = false; // the switch as the person set it
    std::string m_folder;             // "" for the default folder

    SpinInput*             m_quality       = nullptr;
    ::SwitchButton*        m_pointer       = nullptr;
    ::SwitchButton*        m_captions      = nullptr;
    StageRail*             m_record_rail   = nullptr;
    StageRail*             m_encode_rail   = nullptr;
    ::SwitchButton*        m_encode        = nullptr;
    ::Label*               m_output        = nullptr; // "H.265 in .mp4"
    ::ComboBox*            m_preset        = nullptr;
    ::ComboBox*            m_codec         = nullptr;
    SpinInput*             m_crf           = nullptr;
    ::ComboBox*            m_speed         = nullptr;
    ::ComboBox*            m_profile       = nullptr;
    ::TextInput*           m_folder_text   = nullptr;
    ::Button*              m_change_folder = nullptr;
    ::Label*               m_default_link  = nullptr;
    ::StaticLine*          m_divider       = nullptr;
    std::vector<wxWindow*> m_encoding; // the encoding fields and their labels
};

}} // namespace Slic3r::GUI
