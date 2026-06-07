#include "PrintHostDialogs.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>

#include <wx/frame.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>
#include <wx/button.h>
#include <wx/dataview.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/wrapsizer.h>
#include <wx/wupdlock.h>
#include <wx/debug.h>
#include <wx/msgdlg.h>

#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/algorithm/string.hpp>
#include <nlohmann/json.hpp>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "libslic3r/AppConfig.hpp"
#include "NotificationManager.hpp"
#include "ExtraRenderers.hpp"
#include "format.hpp"
#include "../Utils/CrealityPrint.hpp"
#include "BitmapComboBox.hpp"
#include "wxExtensions.hpp"

#include <nlohmann/json.hpp>

namespace fs = boost::filesystem;
using json = nlohmann::json;

namespace Slic3r {
namespace GUI {

namespace {

wxColour contrasting_text_color(const wxColour& background)
{
    return background.GetLuminance() < 0.60 ? *wxWHITE : wxColour("#303030");
}

long long color_distance_sq(const wxColour& lhs, const wxColour& rhs)
{
    const long long dr = static_cast<long long>(lhs.Red()) - static_cast<long long>(rhs.Red());
    const long long dg = static_cast<long long>(lhs.Green()) - static_cast<long long>(rhs.Green());
    const long long db = static_cast<long long>(lhs.Blue()) - static_cast<long long>(rhs.Blue());
    return dr * dr + dg * dg + db * db;
}

class FlashforgeSlotCard : public wxPanel
{
public:
    FlashforgeSlotCard(wxWindow* parent)
        : wxPanel(parent, wxID_ANY)
    {
        SetDoubleBuffered(true);
        SetMinSize(wxSize(FromDIP(68), FromDIP(92)));
        SetMaxSize(GetMinSize());
        Bind(wxEVT_PAINT, &FlashforgeSlotCard::on_paint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
            m_hover = true;
            SetCursor(wxCursor(m_enabled ? wxCURSOR_HAND : wxCURSOR_NO_ENTRY));
            Refresh();
            e.Skip();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
            m_hover = false;
            SetCursor(wxCursor(wxCURSOR_ARROW));
            Refresh();
            e.Skip();
        });
        Bind(wxEVT_LEFT_DOWN, &FlashforgeSlotCard::on_left_down, this);
    }

    void set_slot(const Slic3r::FlashforgeMaterialSlot& slot, bool enabled)
    {
        m_slot_id  = slot.slot_id;
        m_color    = parse_color(slot.material_color);
        m_name     = slot.material_name.empty() ? _L("Unknown") : from_u8(slot.material_name);
        m_empty    = !slot.has_filament;
        m_enabled  = enabled && !m_empty;
        Refresh();
    }

private:
    static wxColour parse_color(const std::string& raw)
    {
        wxColour color(from_u8(raw));
        if (color.IsOk())
            return color;

        std::string value = raw;
        boost::trim(value);
        if (!value.empty() && value.front() != '#')
            value.insert(value.begin(), '#');
        color = wxColour(from_u8(value));
        return color.IsOk() ? color : wxColour("#D0D0D0");
    }

    void on_left_down(wxMouseEvent& e)
    {
        if (!m_enabled)
            return;

        wxCommandEvent evt(wxEVT_BUTTON, GetId());
        evt.SetInt(m_slot_id);
        evt.SetString(m_color.GetAsString(wxC2S_HTML_SYNTAX));
        wxPostEvent(this, evt);
        e.Skip();
    }

    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc == nullptr)
            return;

        const wxSize size           = GetSize();
        const int    circle_size    = FromDIP(24);
        const int    outline_width  = (m_hover && m_enabled) ? FromDIP(2) : FromDIP(1);
        const wxRect body_rect(FromDIP(7), FromDIP(26), size.x - FromDIP(14), size.y - FromDIP(32));
        const wxColour badge_color  = m_enabled ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT)
                                                : wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
        const wxColour body_border  = m_enabled ? wxSystemSettings::GetColour(wxSYS_COLOUR_BTNSHADOW)
                                                : wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush((m_hover && m_enabled) ? badge_color.ChangeLightness(130) : badge_color));
        gc->DrawEllipse((size.x - circle_size) / 2.0, 0, circle_size, circle_size);

        dc.SetFont(::Label::Body_13);
        dc.SetTextForeground(*wxWHITE);
        const wxString slot_txt  = wxString::Format("%d", m_slot_id);
        const wxSize   slot_size = dc.GetTextExtent(slot_txt);
        dc.DrawText(slot_txt, (size.x - slot_size.x) / 2, (circle_size - slot_size.y) / 2);

        const wxColour bg_color = m_empty ? wxColour("#F6F6F6") : m_color;
        gc->SetPen(wxPen(body_border, outline_width));
        gc->SetBrush(wxBrush(bg_color));
        gc->DrawRoundedRectangle(body_rect.x, body_rect.y, body_rect.width, body_rect.height, FromDIP(8));

        dc.SetFont(::Label::Body_12);
        dc.SetTextForeground(contrasting_text_color(bg_color));

        wxString label = m_empty ? _L("Empty") : m_name;
        if (dc.GetTextExtent(label).x > body_rect.width - FromDIP(8))
            dc.SetFont(::Label::Body_10);
        if (dc.GetTextExtent(label).x > body_rect.width - FromDIP(8)) {
            while (!label.empty() && dc.GetTextExtent(label + "...").x > body_rect.width - FromDIP(8))
                label.RemoveLast();
            label += "...";
        }
        const wxSize label_size = dc.GetTextExtent(label);
        dc.DrawText(label, body_rect.x + (body_rect.width - label_size.x) / 2, body_rect.y + (body_rect.height - label_size.y) / 2);
    }

private:
    int      m_slot_id {0};
    wxColour m_color {*wxWHITE};
    wxString m_name;
    bool     m_empty {true};
    bool     m_enabled {false};
    bool     m_hover {false};
};

class FlashforgeSlotDialog : public DPIDialog
{
public:
    FlashforgeSlotDialog(wxWindow* parent, const wxString& material_name)
        : DPIDialog(parent, wxID_ANY, _L("Choose a slot for the selected color"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
        , m_material_name(material_name)
    {
        SetFont(wxGetApp().normal_font());
        SetBackgroundColour(*wxWHITE);

        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* title = new wxStaticText(this, wxID_ANY, _L("Material in the material station"));
        title->SetFont(::Label::Head_13);
        root->Add(title, 0, wxALL | wxALIGN_CENTER_HORIZONTAL, FromDIP(12));

        m_grid = new wxGridSizer(1, 4, FromDIP(10), FromDIP(12));
        auto* grid_row = new wxBoxSizer(wxHORIZONTAL);
        grid_row->AddStretchSpacer();
        grid_row->Add(m_grid, 0);
        grid_row->AddStretchSpacer();
        root->Add(grid_row, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(18));

        auto* tip = new wxStaticText(this, wxID_ANY, _L("Only materials of the same type can be selected."));
        tip->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        root->Add(tip, 0, wxALL | wxALIGN_CENTER_HORIZONTAL, FromDIP(12));

        SetSizer(root);
        SetMinSize(wxSize(FromDIP(460), FromDIP(240)));

        for (int i = 0; i < 4; ++i) {
            auto* card = new FlashforgeSlotCard(this);
            card->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
                m_selected_slot_id = e.GetInt();
                m_selected_color   = wxColour(e.GetString());
                EndModal(wxID_OK);
            });
            m_cards.push_back(card);
            m_grid->Add(card, 0);
        }

        wxGetApp().UpdateDlgDarkUI(this);
        Layout();
        Fit();
        CenterOnParent();
        Refresh();
    }

    void update_slots(const std::vector<Slic3r::FlashforgeMaterialSlot>& slots, const std::function<bool(const Slic3r::FlashforgeMaterialSlot&)>& matcher)
    {
        for (size_t i = 0; i < m_cards.size(); ++i) {
            Slic3r::FlashforgeMaterialSlot slot;
            slot.slot_id = static_cast<int>(i) + 1;
            if (const auto it = std::find_if(slots.begin(), slots.end(), [&](const Slic3r::FlashforgeMaterialSlot& item) { return item.slot_id == slot.slot_id; }); it != slots.end())
                slot = *it;
            const bool enabled = slot.has_filament && matcher(slot);
            m_cards[i]->set_slot(slot, enabled);
        }
        Layout();
        Fit();
    }

    bool has_selection() const { return m_selected_slot_id > 0; }
    int selected_slot_id() const { return m_selected_slot_id; }
    wxColour selected_color() const { return m_selected_color; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override
    {
        Fit();
        Refresh();
        if (suggested_rect.IsEmpty())
            return;
        SetSize(suggested_rect.GetSize());
    }

private:
    wxString                         m_material_name;
    wxGridSizer*                     m_grid {nullptr};
    std::vector<FlashforgeSlotCard*> m_cards;
    int                              m_selected_slot_id {0};
    wxColour                         m_selected_color;
};

class FlashforgeMaterialMapWidget : public wxPanel
{
public:
    using SelectFn = std::function<void(FlashforgeMaterialMapWidget*)>;

    FlashforgeMaterialMapWidget(wxWindow* parent, int tool_id, const wxColour& color, const wxString& material_name, SelectFn on_select)
        : wxPanel(parent, wxID_ANY)
        , m_tool_id(tool_id)
        , m_color(color)
        , m_name(material_name.Strip())
        , m_select_fn(std::move(on_select))
    {
        SetDoubleBuffered(true);
        const wxSize size(FromDIP(72), FromDIP(58));
        SetSize(size);
        SetMinSize(size);
        SetMaxSize(size);
        Bind(wxEVT_PAINT, &FlashforgeMaterialMapWidget::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &FlashforgeMaterialMapWidget::on_left_down, this);

    }

    int tool_id() const { return m_tool_id; }
    int selected_slot_id() const { return m_slot_id; }
    bool is_slot_selected() const { return m_slot_id > 0; }
    wxString material_name() const { return m_name; }

    void set_enable_mapping(bool enable)
    {
        if (m_mapping_enabled == enable)
            return;
        m_mapping_enabled = enable;
        if (!enable)
            reset_slot();
        Enable(enable);
        Refresh();
    }

    void set_slot_selection(int slot_id, const wxColour& slot_color)
    {
        m_slot_id = slot_id;
        m_slot_color = slot_color;
        Refresh();
    }

    void reset_slot()
    {
        m_slot_id = 0;
        m_slot_color = wxColour("#DDDDDD");
        Refresh();
    }

    void update_popup_slots(const std::vector<Slic3r::FlashforgeMaterialSlot>& slots, const std::function<bool(const Slic3r::FlashforgeMaterialSlot&)>& matcher)
    {
        m_slots_snapshot = slots;
        m_matcher        = matcher;
    }

    wxSize DoGetBestSize() const override
    {
        return wxSize(FromDIP(72), FromDIP(58));
    }

private:
    void on_left_down(wxMouseEvent& e)
    {
        if (!m_mapping_enabled)
            return;

        FlashforgeSlotDialog dlg(this, m_name);
        dlg.update_slots(m_slots_snapshot, m_matcher);
        m_selected = true;
        Refresh();
        if (dlg.ShowModal() == wxID_OK && dlg.has_selection()) {
            m_slot_id    = dlg.selected_slot_id();
            m_slot_color = dlg.selected_color();
            if (m_select_fn)
                m_select_fn(this);
        }
        m_selected = false;
        Refresh();
        e.Skip();
    }

    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc == nullptr)
            return;

        const wxSize size = GetSize();
        const int half_h = size.y / 2;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(m_color));
        gc->DrawRoundedRectangle(0, 0, size.x, half_h, FromDIP(3));
        gc->DrawRectangle(0, half_h - FromDIP(3), size.x, FromDIP(3));

        gc->SetBrush(wxBrush(m_mapping_enabled ? m_slot_color : wxColour("#DDDDDD")));
        gc->DrawRoundedRectangle(0, half_h, size.x, half_h, FromDIP(3));
        gc->DrawRectangle(0, half_h, size.x, FromDIP(3));

        if (m_selected) {
            gc->SetPen(wxPen(wxColour("#00AE42"), FromDIP(2)));
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->DrawRoundedRectangle(0, 0, size.x - FromDIP(1), size.y - FromDIP(1), FromDIP(3));
        } else if (m_color.GetLuminance() > 0.95 || m_slot_color.GetLuminance() > 0.95) {
            gc->SetPen(wxPen(wxColour("#ACACAC"), FromDIP(1)));
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->DrawRoundedRectangle(0, 0, size.x - FromDIP(1), size.y - FromDIP(1), FromDIP(3));
        }

        dc.SetFont(::Label::Body_13);
        dc.SetTextForeground(contrasting_text_color(m_color));
        wxString top_text = m_name;
        if (dc.GetTextExtent(top_text).x > size.x - FromDIP(10)) {
            dc.SetFont(::Label::Body_10);
        }
        wxSize top_size = dc.GetTextExtent(top_text);
        dc.DrawText(top_text, (size.x - top_size.x) / 2, (half_h - top_size.y) / 2);

        dc.SetFont(::Label::Body_13);
        dc.SetTextForeground(contrasting_text_color(m_slot_color));
        const wxString bottom_text = m_slot_id > 0 ? wxString::Format("%d", m_slot_id) : "-";
        const wxSize bottom_size = dc.GetTextExtent(bottom_text);
        dc.DrawText(bottom_text, (size.x - bottom_size.x - FromDIP(10)) / 2, half_h + (half_h - bottom_size.y) / 2);

        wxPoint pts[3] = {
            wxPoint(size.x - FromDIP(18), half_h + half_h / 2 - FromDIP(2)),
            wxPoint(size.x - FromDIP(10), half_h + half_h / 2 - FromDIP(2)),
            wxPoint(size.x - FromDIP(14), half_h + half_h / 2 + FromDIP(3))
        };
        dc.SetBrush(wxBrush(contrasting_text_color(m_slot_color)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawPolygon(3, pts);
    }

private:
    int                  m_tool_id {-1};
    wxColour             m_color;
    wxString             m_name;
    wxColour             m_slot_color {wxColour("#DDDDDD")};
    int                  m_slot_id {0};
    bool                 m_selected {false};
    bool                 m_mapping_enabled {true};
    SelectFn             m_select_fn;
    std::vector<Slic3r::FlashforgeMaterialSlot> m_slots_snapshot;
    std::function<bool(const Slic3r::FlashforgeMaterialSlot&)> m_matcher;
};

static FlashforgeMaterialMapWidget* as_ff_map_widget(wxWindow* window)
{
    return dynamic_cast<FlashforgeMaterialMapWidget*>(window);
}

} // namespace

static const char *CONFIG_KEY_PATH  = "printhost_path";
static const char *CONFIG_KEY_GROUP = "printhost_group";
static const char* CONFIG_KEY_STORAGE = "printhost_storage";

PrintHostSendDialog::PrintHostSendDialog(const fs::path &path, PrintHostPostUploadActions post_actions, const wxArrayString &groups, const wxArrayString& storage_paths, const wxArrayString& storage_names, bool switch_to_device_tab)
    : MsgDialog(static_cast<wxWindow*>(wxGetApp().mainframe), _L("Send G-code to printer host"), _L("Upload to Printer Host with the following filename:"), 0) // Set style = 0 to avoid default creation of the "OK" button. 
                                                                                                                                                               // All buttons will be added later in this constructor 
    , txt_filename(new wxTextCtrl(this, wxID_ANY))
    , combo_groups(!groups.IsEmpty() ? new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, groups, wxCB_READONLY) : nullptr)
    , combo_storage(storage_names.GetCount() > 1 ? new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, storage_names, wxCB_READONLY) : nullptr)
    , post_upload_action(PrintHostPostUploadAction::None)
    , m_paths(storage_paths)
    , m_switch_to_device_tab(switch_to_device_tab)
    , m_path(path)
    , m_post_actions(post_actions)
    , m_storage_names(storage_names)
{
#ifdef __APPLE__
    txt_filename->OSXDisableAllSmartSubstitutions();
#endif
}
void PrintHostSendDialog::init()
{
    const auto& path = m_path;
    const auto& storage_paths = m_paths;
    const auto& post_actions = m_post_actions;
    const auto& storage_names = m_storage_names;

    const AppConfig* app_config = wxGetApp().app_config;

    auto *label_dir_hint = new wxStaticText(this, wxID_ANY, _L("Use forward slashes ( / ) as a directory separator if needed."));
    label_dir_hint->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());

    content_sizer->Add(txt_filename, 0, wxEXPAND);
    content_sizer->Add(label_dir_hint);
    content_sizer->AddSpacer(VERT_SPACING);
    
    if (combo_groups != nullptr) {
        // Repetier specific: Show a selection of file groups.
        auto *label_group = new wxStaticText(this, wxID_ANY, _L("Group"));
        content_sizer->Add(label_group);
        content_sizer->Add(combo_groups, 0, wxBOTTOM, 2*VERT_SPACING);        
        wxString recent_group = from_u8(app_config->get("recent", CONFIG_KEY_GROUP));
        if (! recent_group.empty())
            combo_groups->SetValue(recent_group);
    }

    if (combo_storage != nullptr) {
        // PrusaLink specific: User needs to choose a storage
        auto* label_group = new wxStaticText(this, wxID_ANY, _L("Upload to storage") + ":");
        content_sizer->Add(label_group);
        content_sizer->Add(combo_storage, 0, wxBOTTOM, 2 * VERT_SPACING);
        combo_storage->SetValue(storage_names.front());
        wxString recent_storage = from_u8(app_config->get("recent", CONFIG_KEY_STORAGE));
        if (!recent_storage.empty())
            combo_storage->SetValue(recent_storage); 
    } else if (storage_names.GetCount() == 1){
        // PrusaLink specific: Show which storage has been detected.
        auto* label_group = new wxStaticText(this, wxID_ANY, _L("Upload to storage") + ": " + storage_names.front());
        content_sizer->Add(label_group);
        m_preselected_storage = storage_paths.front();
    }


    wxString recent_path = from_u8(app_config->get("recent", CONFIG_KEY_PATH));
    if (recent_path.Length() > 0 && recent_path[recent_path.Length() - 1] != '/') {
        recent_path += '/';
    }
    const auto recent_path_len = recent_path.Length();
    recent_path += path.filename().wstring();
    wxString stem(path.stem().wstring());
    const auto stem_len = stem.Length();

    txt_filename->SetValue(recent_path);

    auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto checkbox       = new ::CheckBox(this, wxID_APPLY);
    checkbox->SetValue(m_switch_to_device_tab);
    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        m_switch_to_device_tab = e.IsChecked();
        e.Skip();
    });
    checkbox_sizer->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

    auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Switch to Device tab after upload."), wxDefaultPosition, wxDefaultSize, 0);
    checkbox_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
    checkbox_text->SetFont(::Label::Body_13);
    checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
    content_sizer->Add(checkbox_sizer);
    content_sizer->AddSpacer(VERT_SPACING);

    if (size_t extension_start = recent_path.find_last_of('.'); extension_start != std::string::npos)
        m_valid_suffix = recent_path.substr(extension_start);
    // .gcode suffix control
    auto validate_path = [this](const wxString &path) -> bool {
        if (! path.Lower().EndsWith(m_valid_suffix.Lower())) {
            MessageDialog msg_wingow(this, wxString::Format(_L("Upload filename doesn't end with \"%s\". Do you wish to continue?"), m_valid_suffix), wxString(SLIC3R_APP_NAME), wxYES | wxNO);
            if (msg_wingow.ShowModal() == wxID_NO)
                return false;
        }
        return true;
    };

    auto* btn_ok = add_button(wxID_OK, true, _L("Upload"));
    btn_ok->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
        if (validate_path(txt_filename->GetValue())) {
            post_upload_action = PrintHostPostUploadAction::None;
            EndDialog(wxID_OK);
        }
    });
    txt_filename->SetFocus();
    
    // if (post_actions.has(PrintHostPostUploadAction::QueuePrint)) {
    //     auto* btn_print = add_button(wxID_ADD, false, _L("Upload to Queue"));
    //     btn_print->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
    //         if (validate_path(txt_filename->GetValue())) {
    //             post_upload_action = PrintHostPostUploadAction::QueuePrint;
    //             EndDialog(wxID_OK);
    //         }
    //         });
    // }

    if (post_actions.has(PrintHostPostUploadAction::StartPrint)) {
        auto* btn_print = add_button(wxID_YES, false, _L("Upload and Print"));
        btn_print->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
            if (validate_path(txt_filename->GetValue())) {
                post_upload_action = PrintHostPostUploadAction::StartPrint;
                EndDialog(wxID_OK);
            }
        });
    }

    // if (post_actions.has(PrintHostPostUploadAction::StartSimulation)) {
    //     // Using wxID_MORE as a button identifier to be different from the other buttons, wxID_MORE has no other meaning here.
    //     auto* btn_simulate = add_button(wxID_MORE, false, _L("Upload and Simulate"));
    //     btn_simulate->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
    //         if (validate_path(txt_filename->GetValue())) {
    //             post_upload_action = PrintHostPostUploadAction::StartSimulation;
    //             EndDialog(wxID_OK);
    //         }        
    //     });
    // }

    add_button(wxID_CANCEL,false, L("Cancel"));
    finalize();

#ifdef __linux__
    // On Linux with GTK2 when text control lose the focus then selection (colored background) disappears but text color stay white
    // and as a result the text is invisible with light mode
    // see https://github.com/prusa3d/PrusaSlicer/issues/4532
    // Workaround: Unselect text selection explicitly on kill focus
    txt_filename->Bind(wxEVT_KILL_FOCUS, [this](wxEvent& e) {
        e.Skip();
        txt_filename->SetInsertionPoint(txt_filename->GetLastPosition());
    }, txt_filename->GetId());
#endif /* __linux__ */

    Bind(wxEVT_SHOW, [=](const wxShowEvent &) {
        // Another similar case where the function only works with EVT_SHOW + CallAfter,
        // this time on Mac.
        CallAfter([=]() {
            txt_filename->SetInsertionPoint(0);
            txt_filename->SetSelection(recent_path_len, recent_path_len + stem_len);
        });
    });
}

fs::path PrintHostSendDialog::filename() const
{
    return into_path(txt_filename->GetValue());
}

PrintHostPostUploadAction PrintHostSendDialog::post_action() const
{
    return post_upload_action;
}

std::string PrintHostSendDialog::group() const
{
     if (combo_groups == nullptr) {
         return "";
     } else {
         wxString group = combo_groups->GetValue();
         return into_u8(group);
    }
}

std::string PrintHostSendDialog::storage() const
{
    if (!combo_storage)
        return GUI::format("%1%", m_preselected_storage);
    if (combo_storage->GetSelection() < 0 || combo_storage->GetSelection() >= int(m_paths.size()))
        return {};
    return into_u8(m_paths[combo_storage->GetSelection()]);
}

void PrintHostSendDialog::EndModal(int ret)
{
    if (ret == wxID_OK) {
        // Persist path and print settings
        wxString path = txt_filename->GetValue();
        int last_slash = path.Find('/', true);
		if (last_slash == wxNOT_FOUND)
			path.clear();
		else
            path = path.SubString(0, last_slash);
                
		AppConfig *app_config = wxGetApp().app_config;
		app_config->set("recent", CONFIG_KEY_PATH, into_u8(path));

        if (combo_groups != nullptr) {
            wxString group = combo_groups->GetValue();
            app_config->set("recent", CONFIG_KEY_GROUP, into_u8(group));
        }
        if (combo_storage != nullptr) {
            wxString storage = combo_storage->GetValue();
            app_config->set("recent", CONFIG_KEY_STORAGE, into_u8(storage));
        }
    }

    MsgDialog::EndModal(ret);
}

FlashforgePrintHostSendDialog::FlashforgePrintHostSendDialog(const fs::path&             path,
                                                             PrintHostPostUploadActions  post_actions,
                                                             const wxArrayString&        groups,
                                                             const wxArrayString&        storage_paths,
                                                             const wxArrayString&        storage_names,
                                                             bool                        switch_to_device_tab,
                                                             const Slic3r::Flashforge*   host,
                                                             bool                        supports_material_station,
                                                             std::vector<Slic3r::FlashforgeMaterialSlot> slots,
                                                             const std::vector<FilamentInfo>& project_filaments)
    : PrintHostSendDialog(path, post_actions, groups, storage_paths, storage_names, switch_to_device_tab)
    , m_host(host)
    , m_slots(std::move(slots))
    , m_project_filaments(project_filaments)
{
    m_supports_material_station = supports_material_station;
    m_slots_loaded = !m_slots.empty();
}

void FlashforgePrintHostSendDialog::init()
{
    const AppConfig* app_config = wxGetApp().app_config;
    const auto&      path       = m_path;

    std::string leveling = app_config->get("recent", CONFIG_KEY_LEVELING);
    if (!leveling.empty())
        m_leveling_before_print = leveling == "1";

    std::string timelapse = app_config->get("recent", CONFIG_KEY_TIMELAPSE);
    if (!timelapse.empty())
        m_time_lapse_video = timelapse == "1";

    // Flashforge local printing should default to IFS enabled when supported.
    // We don't revive an old stale "0" here.
    m_use_material_station = m_supports_material_station;
    if (m_supports_material_station && !app_config->has("recent", CONFIG_KEY_IFS))
        const_cast<AppConfig*>(app_config)->set("recent", CONFIG_KEY_IFS, "1");

    this->SetMinSize(wxSize(560, 420));

    auto* label_dir_hint = new wxStaticText(this, wxID_ANY, _L("Use forward slashes ( / ) as a directory separator if needed."));
    label_dir_hint->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
    content_sizer->Add(txt_filename, 0, wxEXPAND);
    content_sizer->Add(label_dir_hint);
    content_sizer->AddSpacer(VERT_SPACING);

    wxString recent_path = from_u8(app_config->get("recent", CONFIG_KEY_PATH));
    if (recent_path.Length() > 0 && recent_path[recent_path.Length() - 1] != '/')
        recent_path += '/';
    const auto recent_path_len = recent_path.Length();
    recent_path += path.filename().wstring();
    wxString stem(path.stem().wstring());
    const auto stem_len = stem.Length();
    txt_filename->SetValue(recent_path);

    {
        auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto checkbox       = new ::CheckBox(this, wxID_APPLY);
        checkbox->SetValue(m_switch_to_device_tab);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
            auto* source = dynamic_cast<::CheckBox*>(e.GetEventObject());
            if (source != nullptr)
                source->SetValue(e.IsChecked());
            m_switch_to_device_tab = e.IsChecked();
            e.Skip();
        });
        checkbox_sizer->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

        auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Switch to Device tab after upload."));
        checkbox_text->SetFont(::Label::Body_13);
        checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        checkbox_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
        content_sizer->Add(checkbox_sizer);
        content_sizer->AddSpacer(VERT_SPACING);
    }

    m_flashforge_options_sizer = new wxBoxSizer(wxVERTICAL);

    auto add_option_checkbox = [this](wxBoxSizer* parent, const wxString& label, bool value, std::function<void(bool)> setter, ::CheckBox** out = nullptr) {
        auto row      = new wxBoxSizer(wxHORIZONTAL);
        auto checkbox = new ::CheckBox(this);
        checkbox->SetValue(value);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [setter](wxCommandEvent& e) {
            auto* source = dynamic_cast<::CheckBox*>(e.GetEventObject());
            if (source != nullptr)
                source->SetValue(e.IsChecked());
            setter(e.IsChecked());
            e.Skip();
        });
        row->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

        auto text = new wxStaticText(this, wxID_ANY, label);
        text->SetFont(::Label::Body_13);
        text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        row->Add(text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
        parent->Add(row);
        parent->AddSpacer(FromDIP(6));

        if (out != nullptr)
            *out = checkbox;
    };

    add_option_checkbox(m_flashforge_options_sizer, _L("Leveling before print"), m_leveling_before_print,
                        [this](bool checked) { m_leveling_before_print = checked; }, &m_checkbox_leveling);
    add_option_checkbox(m_flashforge_options_sizer, _L("Time-lapse"), m_time_lapse_video,
                        [this](bool checked) { m_time_lapse_video = checked; }, &m_checkbox_timelapse);
    add_option_checkbox(m_flashforge_options_sizer, _L("Enable IFS"), m_use_material_station,
                        [this](bool checked) {
                            m_use_material_station = checked;
                            if (checked) {
                                ensure_slots_loaded();
                                rebuild_mapping_rows();
                            }
                            sync_mapping_section_visibility();
                        }, &m_checkbox_ifs);

    if (m_checkbox_ifs != nullptr && !m_supports_material_station)
        m_checkbox_ifs->Enable(false);

    m_status_text = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_status_text->SetFont(::Label::Body_12);
    m_flashforge_options_sizer->Add(m_status_text, 0, wxTOP | wxBOTTOM, FromDIP(4));

    m_mapping_section_sizer = new wxBoxSizer(wxVERTICAL);
    m_mapping_wrap_sizer    = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
    m_mapping_section_sizer->Add(m_mapping_wrap_sizer, 0, wxTOP | wxALIGN_LEFT, FromDIP(10));
    m_flashforge_options_sizer->Add(m_mapping_section_sizer, 0, wxEXPAND);

    content_sizer->Add(m_flashforge_options_sizer, 0, wxEXPAND);

    if (m_supports_material_station)
        m_status_text->SetLabel(wxString::Format(_L("Detected %d IFS slots on printer."), static_cast<int>(m_slots.size())));
    else
        m_status_text->SetLabel(_L("This printer does not report a material station."));

    rebuild_mapping_rows();
    sync_mapping_section_visibility();

    if (size_t extension_start = recent_path.find_last_of('.'); extension_start != std::string::npos)
        m_valid_suffix = recent_path.substr(extension_start);

    auto validate_path = [this](const wxString& filename) -> bool {
        if (!filename.Lower().EndsWith(m_valid_suffix.Lower())) {
            MessageDialog msg_wingow(this, wxString::Format(_L("Upload filename doesn't end with \"%s\". Do you wish to continue?"), m_valid_suffix),
                                     wxString(SLIC3R_APP_NAME), wxYES | wxNO);
            if (msg_wingow.ShowModal() == wxID_NO)
                return false;
        }
        return validate_before_close();
    };

    auto* btn_ok = add_button(wxID_OK, true, _L("Upload"));
    btn_ok->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
        if (validate_path(txt_filename->GetValue())) {
            post_upload_action = PrintHostPostUploadAction::None;
            EndDialog(wxID_OK);
        }
    });

    if (m_post_actions.has(PrintHostPostUploadAction::StartPrint)) {
        auto* btn_print = add_button(wxID_YES, false, _L("Upload and Print"));
        btn_print->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
            if (validate_path(txt_filename->GetValue())) {
                post_upload_action = PrintHostPostUploadAction::StartPrint;
                EndDialog(wxID_OK);
            }
        });
    }

    add_button(wxID_CANCEL, false, _L("Cancel"));
    finalize();
    txt_filename->SetFocus();

#ifdef __linux__
    txt_filename->Bind(wxEVT_KILL_FOCUS, [this](wxEvent& e) {
        e.Skip();
        txt_filename->SetInsertionPoint(txt_filename->GetLastPosition());
    }, txt_filename->GetId());
#endif /* __linux__ */

    Bind(wxEVT_SHOW, [=](const wxShowEvent&) {
        CallAfter([=]() {
            txt_filename->SetInsertionPoint(0);
            txt_filename->SetSelection(recent_path_len, recent_path_len + stem_len);
        });
    });
}

void FlashforgePrintHostSendDialog::EndModal(int ret)
{
    if (ret == wxID_OK) {
        AppConfig* app_config = wxGetApp().app_config;
        app_config->set("recent", CONFIG_KEY_LEVELING, m_leveling_before_print ? "1" : "0");
        app_config->set("recent", CONFIG_KEY_TIMELAPSE, m_time_lapse_video ? "1" : "0");
        app_config->set("recent", CONFIG_KEY_IFS, m_use_material_station ? "1" : "0");
    }

    PrintHostSendDialog::EndModal(ret);
}

std::map<std::string, std::string> FlashforgePrintHostSendDialog::extendedInfo() const
{
    json mappings = json::array();
    int  mapped_count = 0;

    if (m_use_material_station) {
        for (const auto& row : m_mapping_rows) {
            auto* card = as_ff_map_widget(row.card);
            if (card == nullptr || row.tool_id < 0)
                continue;

            const int slot_id = card->selected_slot_id();
            if (slot_id <= 0)
                continue;

            const auto filament_it = std::find_if(m_project_filaments.begin(), m_project_filaments.end(), [&](const FilamentInfo& item) { return item.id == row.tool_id; });
            const auto slot_it     = std::find_if(m_slots.begin(), m_slots.end(), [&](const FlashforgeMaterialSlot& slot) { return slot.slot_id == slot_id; });
            if (filament_it == m_project_filaments.end() || slot_it == m_slots.end())
                continue;

            mappings.push_back({
                {"toolId", filament_it->id},
                {"slotId", slot_it->slot_id},
                {"materialName", slot_it->material_name},
                {"toolMaterialColor", filament_it->color},
                {"slotMaterialColor", slot_it->material_color}
            });
            ++mapped_count;
        }
    }

    return {
        {"levelingBeforePrint", m_leveling_before_print ? "1" : "0"},
        {"timeLapseVideo", m_time_lapse_video ? "1" : "0"},
        {"useMatlStation", m_use_material_station ? "1" : "0"},
        {"gcodeToolCnt", std::to_string(mapped_count)},
        {"materialMappings", mappings.dump()}
    };
}

void FlashforgePrintHostSendDialog::load_slots()
{
    m_slots.clear();
    m_slots_loaded = false;
    m_supports_material_station = false;

    if (m_host == nullptr) {
        m_status_text->SetLabel(_L("Flashforge host is not available."));
        return;
    }

    wxString msg;
    bool     supports_material_station = false;
    if (!m_host->fetch_material_slots(m_slots, &supports_material_station, msg)) {
        m_status_text->SetLabel(msg.empty() ? _L("Unable to read IFS slots from printer.") : msg);
        return;
    }

    m_supports_material_station = supports_material_station;
    m_slots_loaded = !m_slots.empty();
    m_use_material_station = m_supports_material_station;

    if (m_supports_material_station)
        m_status_text->SetLabel(wxString::Format(_L("Detected %d IFS slots on printer."), static_cast<int>(m_slots.size())));
    else
        m_status_text->SetLabel(_L("This printer does not report a material station."));
}

bool FlashforgePrintHostSendDialog::ensure_slots_loaded(bool force_reload)
{
    if (!force_reload && (m_slots_loaded || !m_supports_material_station))
        return m_slots_loaded;

    if (m_status_text != nullptr)
        m_status_text->SetLabel(_L("Loading IFS slots from printer..."));

    wxBusyCursor wait;
    load_slots();
    return m_slots_loaded;
}

void FlashforgePrintHostSendDialog::rebuild_mapping_rows()
{
    if (m_mapping_wrap_sizer == nullptr)
        return;

    m_mapping_wrap_sizer->Clear(true);
    m_mapping_rows.clear();

    if (m_project_filaments.empty()) {
        m_mapping_wrap_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Slice the plate first to get project material information.")), 0, wxALL, FromDIP(2));
        return;
    }

    for (const auto& filament : m_project_filaments) {
        auto* card = new FlashforgeMaterialMapWidget(this, filament.id, to_wx_colour(filament.color), from_u8(filament.get_display_filament_type()),
                                                     [this](FlashforgeMaterialMapWidget* changed_card) {
                                                         if (changed_card == nullptr)
                                                             return;
                                                         for (auto& row : m_mapping_rows) {
                                                             if (row.card == changed_card) {
                                                                 refresh_mapping_card(row);
                                                                 break;
                                                             }
                                                         }
                                                     });
        m_mapping_wrap_sizer->Add(card, 0, wxRIGHT | wxBOTTOM | wxFIXED_MINSIZE, FromDIP(10));

        MappingRow row;
        row.tool_id = filament.id;
        row.card    = card;
        m_mapping_rows.push_back(row);
    }

    auto_assign_mappings();
}

void FlashforgePrintHostSendDialog::auto_assign_mappings()
{
    for (size_t idx = 0; idx < m_project_filaments.size() && idx < m_mapping_rows.size(); ++idx) {
        auto& filament = m_project_filaments[idx];
        auto* card     = as_ff_map_widget(m_mapping_rows[idx].card);
        if (card == nullptr)
            continue;

        const wxColour filament_color = to_wx_colour(filament.color);
        const Slic3r::FlashforgeMaterialSlot* best_slot = nullptr;
        long long best_distance = std::numeric_limits<long long>::max();

        for (const auto& slot : m_slots) {
            if (!slot.has_filament || !slot_matches_filament(slot, filament))
                continue;

            const long long distance = color_distance_sq(filament_color, to_wx_colour(slot.material_color));
            if (best_slot == nullptr || distance < best_distance) {
                best_slot = &slot;
                best_distance = distance;
            }
        }

        if (best_slot != nullptr)
            card->set_slot_selection(best_slot->slot_id, to_wx_colour(best_slot->material_color));
        else
            card->reset_slot();

        refresh_mapping_card(m_mapping_rows[idx]);
    }
}

void FlashforgePrintHostSendDialog::refresh_mapping_card(MappingRow& row)
{
    auto* card = as_ff_map_widget(row.card);
    if (card == nullptr)
        return;

    const auto* filament = find_filament_by_tool_id(row.tool_id);
    card->set_enable_mapping(m_use_material_station);
    card->update_popup_slots(m_slots, [this, filament](const FlashforgeMaterialSlot& slot) {
        return filament != nullptr && slot_matches_filament(slot, *filament);
    });

    if (card->selected_slot_id() <= 0) {
        card->reset_slot();
        return;
    }

    const auto* slot = find_slot_by_id(std::to_string(card->selected_slot_id()));
    if (slot == nullptr) {
        card->reset_slot();
        return;
    }

    card->set_slot_selection(slot->slot_id, to_wx_colour(slot->material_color));
}

void FlashforgePrintHostSendDialog::sync_mapping_section_visibility()
{
    if (m_mapping_section_sizer == nullptr)
        return;

    m_mapping_section_sizer->ShowItems(m_use_material_station && m_supports_material_station);
    if (wxSizer* sizer = GetSizer(); sizer != nullptr) {
        sizer->Layout();
        sizer->Fit(this);
        SetMinSize(GetBestSize());
    }
    Layout();
    Fit();
}

const Slic3r::FlashforgeMaterialSlot* FlashforgePrintHostSendDialog::find_slot_by_id(const std::string& slot_id_text) const
{
    const auto slot_it = std::find_if(m_slots.begin(), m_slots.end(), [&](const FlashforgeMaterialSlot& slot) { return std::to_string(slot.slot_id) == slot_id_text; });
    return slot_it == m_slots.end() ? nullptr : &(*slot_it);
}

const FilamentInfo* FlashforgePrintHostSendDialog::find_filament_by_tool_id(int tool_id) const
{
    const auto filament_it = std::find_if(m_project_filaments.begin(), m_project_filaments.end(), [&](const FilamentInfo& filament) { return filament.id == tool_id; });
    return filament_it == m_project_filaments.end() ? nullptr : &(*filament_it);
}

bool FlashforgePrintHostSendDialog::slot_matches_filament(const Slic3r::FlashforgeMaterialSlot& slot, const FilamentInfo& filament) const
{
    if (!slot.has_filament)
        return false;

    const std::string project_material = normalize_material(!filament.type.empty() ? filament.type : filament.get_display_filament_type());
    const std::string slot_material    = normalize_material(slot.material_name);
    return !project_material.empty() && !slot_material.empty() && project_material == slot_material;
}

bool FlashforgePrintHostSendDialog::validate_before_close()
{
    if (!m_use_material_station && m_project_filaments.size() > 1) {
        show_error(this, _L("This plate uses multiple materials. Enable IFS and assign each tool to a printer slot."));
        return false;
    }

    if (!m_use_material_station)
        return true;

    for (const auto& row : m_mapping_rows) {
        auto* card = as_ff_map_widget(row.card);
        if (card == nullptr || !card->is_slot_selected()) {
            show_error(this, _L("Each project material must be assigned to an IFS slot before printing."));
            return false;
        }

        const auto*       slot         = find_slot_by_id(std::to_string(card->selected_slot_id()));
        const auto*       filament     = find_filament_by_tool_id(row.tool_id);
        if (slot == nullptr || filament == nullptr || !slot->has_filament) {
            show_error(this, _L("Each project material must be assigned to a loaded IFS slot before printing."));
            return false;
        }

        if (!slot_matches_filament(*slot, *filament)) {
            show_error(this, _L("Each project material must match the material loaded in the selected IFS slot."));
            return false;
        }
    }

    return true;
}

std::string FlashforgePrintHostSendDialog::normalize_material(const std::string& material) const
{
    std::string normalized = boost::to_upper_copy(material);
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(), [](unsigned char ch) { return !std::isalnum(ch); }), normalized.end());

    if (normalized.empty())
        return {};

    if (normalized.find("SILK") != std::string::npos)
        return "SILK";

    if (normalized.find("PLA") != std::string::npos && normalized.find("CF") != std::string::npos)
        return "PLACF";
    if (normalized.find("PETG") != std::string::npos && normalized.find("CF") != std::string::npos)
        return "PETGCF";

    if (normalized == "PLA" || normalized == "PLA+" || normalized == "PLAPLUS")
        return "PLA";
    if (normalized.find("PLA") != std::string::npos)
        return "PLA";

    if (normalized == "ABS" || normalized.find("ABS") != std::string::npos)
        return "ABS";
    if (normalized == "ASA" || normalized.find("ASA") != std::string::npos)
        return "ABS";

    if (normalized.find("PETG") != std::string::npos)
        return "PETG";

    if (normalized.find("TPU") != std::string::npos || normalized.find("TPE") != std::string::npos || normalized.find("FLEX") != std::string::npos)
        return "TPU";

    return normalized;
}

wxColour FlashforgePrintHostSendDialog::to_wx_colour(const std::string& color) const
{
    wxColour wx_color(from_u8(color));
    if (wx_color.IsOk())
        return wx_color;

    std::string normalized = boost::trim_copy(color);
    if (boost::istarts_with(normalized, "0x"))
        normalized = normalized.substr(2);
    if (!normalized.empty() && normalized.front() == '#')
        normalized.erase(normalized.begin());

    if (normalized.size() == 8) {
        auto hex_to_byte = [](char hi, char lo) -> int {
            auto hex_val = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int h = hex_val(hi);
            const int l = hex_val(lo);
            return (h < 0 || l < 0) ? -1 : h * 16 + l;
        };

        const int r = hex_to_byte(normalized[0], normalized[1]);
        const int g = hex_to_byte(normalized[2], normalized[3]);
        const int b = hex_to_byte(normalized[4], normalized[5]);
        const int a = hex_to_byte(normalized[6], normalized[7]);
        if (r >= 0 && g >= 0 && b >= 0 && a >= 0)
            return wxColour(r, g, b, a);
    }

    if (normalized.size() == 6) {
        wx_color = wxColour("#" + from_u8(normalized));
        if (wx_color.IsOk())
            return wx_color;
    }

    return wxColour("#999999");
}

wxDEFINE_EVENT(EVT_PRINTHOST_PROGRESS, PrintHostQueueDialog::Event);
wxDEFINE_EVENT(EVT_PRINTHOST_ERROR,    PrintHostQueueDialog::Event);
wxDEFINE_EVENT(EVT_PRINTHOST_CANCEL,   PrintHostQueueDialog::Event);
wxDEFINE_EVENT(EVT_PRINTHOST_INFO,  PrintHostQueueDialog::Event);

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id)
    : wxEvent(winid, eventType)
    , job_id(job_id)
{}

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id, int progress)
    : wxEvent(winid, eventType)
    , job_id(job_id)
    , progress(progress)
{}

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id, wxString error)
    : wxEvent(winid, eventType)
    , job_id(job_id)
    , status(std::move(error))
{}

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id, wxString tag, wxString status)
    : wxEvent(winid, eventType)
    , job_id(job_id)
    , tag(std::move(tag))
    , status(std::move(status))
{}

wxEvent *PrintHostQueueDialog::Event::Clone() const
{
    return new Event(*this);
}

PrintHostQueueDialog::PrintHostQueueDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Print host upload queue"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , on_progress_evt(this, EVT_PRINTHOST_PROGRESS, &PrintHostQueueDialog::on_progress, this)
    , on_error_evt(this, EVT_PRINTHOST_ERROR, &PrintHostQueueDialog::on_error, this)
    , on_cancel_evt(this, EVT_PRINTHOST_CANCEL, &PrintHostQueueDialog::on_cancel, this)
    , on_info_evt(this, EVT_PRINTHOST_INFO, &PrintHostQueueDialog::on_info, this)
{
    const auto em = GetTextExtent("m").x;

    auto *topsizer = new wxBoxSizer(wxVERTICAL);

    std::vector<int> widths;
    widths.reserve(7);
    if (!load_user_data(UDT_COLS, widths)) {
        widths.clear();
        for (size_t i = 0; i < 7; i++)
            widths.push_back(-1);
    }

    job_list = new wxDataViewListCtrl(this, wxID_ANY);

    // MSW DarkMode: workaround for the selected item in the list
    auto append_text_column = [this](const wxString& label, int width, wxAlignment align = wxALIGN_LEFT,
                                     int flags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE) {
#ifdef _WIN32
            job_list->AppendColumn(new wxDataViewColumn(label, new TextRenderer(), job_list->GetColumnCount(), width, align, flags));
#else
            job_list->AppendTextColumn(label, wxDATAVIEW_CELL_INERT, width, align, flags);
#endif
    };

    // Note: Keep these in sync with Column
    append_text_column(_L("ID"), widths[0]);
    job_list->AppendProgressColumn(_L("Progress"),      wxDATAVIEW_CELL_INERT, widths[1], wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
    append_text_column(_L("Status"),widths[2]);
    append_text_column(_L("Host"),  widths[3]);
    append_text_column(_CTX(L_CONTEXT("Size", "OfFile"), "OfFile"), widths[4]);
    append_text_column(_L("Filename"),      widths[5]);
    append_text_column(_L("Message"), widths[6]);
    //append_text_column(_L("Error Message"), -1, wxALIGN_CENTER, wxDATAVIEW_COL_HIDDEN);
 
    auto *btnsizer = new wxBoxSizer(wxHORIZONTAL);
    btn_cancel = new wxButton(this, wxID_DELETE, _L("Cancel selected"));
    btn_cancel->Disable();
    btn_error = new wxButton(this, wxID_ANY, _L("Show error message"));
    btn_error->Disable();
    // Note: The label needs to be present, otherwise we get accelerator bugs on Mac
    auto *btn_close = new wxButton(this, wxID_CANCEL, _L("Close"));
    btnsizer->Add(btn_cancel, 0, wxRIGHT, SPACING);
    btnsizer->Add(btn_error, 0);
    btnsizer->AddStretchSpacer();
    btnsizer->Add(btn_close);

    topsizer->Add(job_list, 1, wxEXPAND | wxBOTTOM, SPACING);
    topsizer->Add(btnsizer, 0, wxEXPAND);
    SetSizer(topsizer);

    wxGetApp().UpdateDlgDarkUI(this);
    wxGetApp().UpdateDVCDarkUI(job_list);

    std::vector<int> size;
    SetSize(load_user_data(UDT_SIZE, size) ? wxSize(size[0] * em, size[1] * em) : wxSize(HEIGHT * em, WIDTH * em));

    Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
        OnSize(evt); 
        save_user_data(UDT_SIZE | UDT_POSITION | UDT_COLS);
     });
    
    std::vector<int> pos;
    if (load_user_data(UDT_POSITION, pos))
        SetPosition(wxPoint(pos[0], pos[1]));

    Bind(wxEVT_MOVE, [this](wxMoveEvent& evt) {
        save_user_data(UDT_SIZE | UDT_POSITION | UDT_COLS);
    });

    job_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent&) { on_list_select(); });

    btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        int selected = job_list->GetSelectedRow();
        if (selected == wxNOT_FOUND) { return; }

        const JobState state = get_state(selected);
        if (state < ST_ERROR) {
            GUI::wxGetApp().printhost_job_queue().cancel(selected);
        }
    });

    btn_error->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        int selected = job_list->GetSelectedRow();
        if (selected == wxNOT_FOUND) { return; }
        GUI::show_error(nullptr, job_list->GetTextValue(selected, COL_ERRORMSG));
    });
}

void PrintHostQueueDialog::append_job(const PrintHostJob &job)
{
    wxCHECK_RET(!job.empty(), "PrintHostQueueDialog: Attempt to append an empty job");

    wxVector<wxVariant> fields;
    fields.push_back(wxVariant(wxString::Format("%d", job_list->GetItemCount() + 1)));
    fields.push_back(wxVariant(0));
    fields.push_back(wxVariant(_L("Queued")));
    fields.push_back(wxVariant(job.printhost->get_host()));
    boost::system::error_code ec;
    boost::uintmax_t size_i = boost::filesystem::file_size(job.upload_data.source_path, ec);
    std::stringstream stream;
    if (ec) {
        stream << "unknown";
        size_i = 0;
        BOOST_LOG_TRIVIAL(error) << ec.message();
    } else 
        stream << std::fixed << std::setprecision(2) << ((float)size_i / 1024 / 1024) << "MB";
    fields.push_back(wxVariant(stream.str()));
    fields.push_back(wxVariant(from_path(job.upload_data.upload_path)));
    fields.push_back(wxVariant(""));
    job_list->AppendItem(fields, static_cast<wxUIntPtr>(ST_NEW));
    // Both strings are UTF-8 encoded.
    upload_names.emplace_back(job.printhost->get_host(), job.upload_data.upload_path.string());

    wxGetApp().notification_manager()->push_upload_job_notification(job_list->GetItemCount(), (float)size_i / 1024 / 1024, job.upload_data.upload_path.string(), job.printhost->get_host());
}

void PrintHostQueueDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    const int& em = em_unit();

    msw_buttons_rescale(this, em, { wxID_DELETE, wxID_CANCEL, btn_error->GetId() });

    SetMinSize(wxSize(HEIGHT * em, WIDTH * em));

    Fit();
    Refresh();

    save_user_data(UDT_SIZE | UDT_POSITION | UDT_COLS);
}

void PrintHostQueueDialog::on_sys_color_changed()
{
#ifdef _WIN32
    wxGetApp().UpdateDlgDarkUI(this);
    wxGetApp().UpdateDVCDarkUI(job_list);
#endif
}

PrintHostQueueDialog::JobState PrintHostQueueDialog::get_state(int idx)
{
    wxCHECK_MSG(idx >= 0 && idx < job_list->GetItemCount(), ST_ERROR, "Out of bounds access to job list");
    return static_cast<JobState>(job_list->GetItemData(job_list->RowToItem(idx)));
}

void PrintHostQueueDialog::set_state(int idx, JobState state)
{
    wxCHECK_RET(idx >= 0 && idx < job_list->GetItemCount(), "Out of bounds access to job list");
    job_list->SetItemData(job_list->RowToItem(idx), static_cast<wxUIntPtr>(state));

    switch (state) {
        case ST_NEW:        job_list->SetValue(_L("Queued"), idx, COL_STATUS); break;
        case ST_PROGRESS:   job_list->SetValue(_L("Uploading"), idx, COL_STATUS); break;
        case ST_ERROR:      job_list->SetValue(_L("Error"), idx, COL_STATUS); break;
        case ST_CANCELLING: job_list->SetValue(_L("Canceling"), idx, COL_STATUS); break;
        case ST_CANCELLED:  job_list->SetValue(_L("Canceled"), idx, COL_STATUS); break;
        case ST_COMPLETED:  job_list->SetValue(_L("Completed"), idx, COL_STATUS); break;
    }
    // This might be ambigous call, but user data needs to be saved time to time
    save_user_data(UDT_SIZE | UDT_POSITION | UDT_COLS);
}

void PrintHostQueueDialog::on_list_select()
{
    int selected = job_list->GetSelectedRow();
    if (selected != wxNOT_FOUND) {
        const JobState state = get_state(selected);
        btn_cancel->Enable(state < ST_ERROR);
        btn_error->Enable(state == ST_ERROR);
        Layout();
    } else {
        btn_cancel->Disable();
    }
}

void PrintHostQueueDialog::on_progress(Event &evt)
{
    wxCHECK_RET(evt.job_id < (size_t)job_list->GetItemCount(), "Out of bounds access to job list");

    if (evt.progress < 100) {
        set_state(evt.job_id, ST_PROGRESS);
        job_list->SetValue(wxVariant(evt.progress), evt.job_id, COL_PROGRESS);
    } else {
        set_state(evt.job_id, ST_COMPLETED);
        job_list->SetValue(wxVariant(100), evt.job_id, COL_PROGRESS);
    }

    on_list_select();

    if (evt.progress > 0)
    {
        wxVariant nm, hst;
        job_list->GetValue(nm, evt.job_id, COL_FILENAME);
        job_list->GetValue(hst, evt.job_id, COL_HOST);
        wxGetApp().notification_manager()->set_upload_job_notification_percentage(evt.job_id + 1, into_u8(nm.GetString()), into_u8(hst.GetString()), evt.progress / 100.f);
    }
}

void PrintHostQueueDialog::on_error(Event &evt)
{
    wxCHECK_RET(evt.job_id < (size_t)job_list->GetItemCount(), "Out of bounds access to job list");

    set_state(evt.job_id, ST_ERROR);

    auto errormsg = format_wxstr("%1%\n%2%", _L("Error uploading to print host") + ":", evt.status);
    job_list->SetValue(wxVariant(0), evt.job_id, COL_PROGRESS);
    job_list->SetValue(wxVariant(errormsg), evt.job_id, COL_ERRORMSG);    // Stashes the error message into a hidden column for later

    on_list_select();

    GUI::show_error(nullptr, errormsg);

    wxVariant nm, hst;
    job_list->GetValue(nm, evt.job_id, COL_FILENAME);
    job_list->GetValue(hst, evt.job_id, COL_HOST);
    wxGetApp().notification_manager()->upload_job_notification_show_error(evt.job_id + 1, into_u8(nm.GetString()), into_u8(hst.GetString()));
}

void PrintHostQueueDialog::on_cancel(Event &evt)
{
    wxCHECK_RET(evt.job_id < (size_t)job_list->GetItemCount(), "Out of bounds access to job list");

    set_state(evt.job_id, ST_CANCELLED);
    job_list->SetValue(wxVariant(0), evt.job_id, COL_PROGRESS);

    on_list_select();

    wxVariant nm, hst;
    job_list->GetValue(nm, evt.job_id, COL_FILENAME);
    job_list->GetValue(hst, evt.job_id, COL_HOST);
    wxGetApp().notification_manager()->upload_job_notification_show_canceled(evt.job_id + 1, into_u8(nm.GetString()), into_u8(hst.GetString()));
}

void PrintHostQueueDialog::on_info(Event& evt)
{
    /*
    wxCHECK_RET(evt.job_id < (size_t)job_list->GetItemCount(), "Out of bounds access to job list");
    
    if (evt.tag == L"resolve") {
        wxVariant hst(evt.status);
        job_list->SetValue(hst, evt.job_id, COL_HOST);
        wxGetApp().notification_manager()->set_upload_job_notification_host(evt.job_id + 1, into_u8(evt.status));
    } else if (evt.tag == L"complete") {
        wxVariant hst(evt.status);
        job_list->SetValue(hst, evt.job_id, COL_ERRORMSG);
        wxGetApp().notification_manager()->set_upload_job_notification_completed(evt.job_id + 1);
        wxGetApp().notification_manager()->set_upload_job_notification_status(evt.job_id + 1, into_u8(evt.status));
    } else if(evt.tag == L"complete_with_warning"){
        wxVariant hst(evt.status);
        job_list->SetValue(hst, evt.job_id, COL_ERRORMSG);
        wxGetApp().notification_manager()->set_upload_job_notification_completed_with_warning(evt.job_id + 1);
        wxGetApp().notification_manager()->set_upload_job_notification_status(evt.job_id + 1, into_u8(evt.status));
    } else if (evt.tag == L"set_complete_off") {
        wxGetApp().notification_manager()->set_upload_job_notification_comp_on_100(evt.job_id + 1, false);
    }
    */
}

void PrintHostQueueDialog::get_active_jobs(std::vector<std::pair<std::string, std::string>>& ret)
{
    int ic = job_list->GetItemCount();
    for (int i = 0; i < ic; i++)
    {
        auto item = job_list->RowToItem(i);
        auto data = job_list->GetItemData(item);
        JobState st = static_cast<JobState>(data);
        if(st == JobState::ST_NEW || st == JobState::ST_PROGRESS)
            ret.emplace_back(upload_names[i]);       
    }
}
void PrintHostQueueDialog::save_user_data(int udt)
{
    const auto em = GetTextExtent("m").x;
    auto *app_config = wxGetApp().app_config;
    if (udt & UserDataType::UDT_SIZE) {
        
        app_config->set("print_host_queue_dialog_height", std::to_string(this->GetSize().x / em));
        app_config->set("print_host_queue_dialog_width", std::to_string(this->GetSize().y / em));
    }
    if (udt & UserDataType::UDT_POSITION)
    {
        app_config->set("print_host_queue_dialog_x", std::to_string(this->GetPosition().x));
        app_config->set("print_host_queue_dialog_y", std::to_string(this->GetPosition().y));
    }
    if (udt & UserDataType::UDT_COLS)
    {
        for (size_t i = 0; i < job_list->GetColumnCount() - 1; i++)
        {
            app_config->set("print_host_queue_dialog_column_" + std::to_string(i), std::to_string(job_list->GetColumn(i)->GetWidth()));
        }
    }    
}
bool PrintHostQueueDialog::load_user_data(int udt, std::vector<int>& vector)
{
    auto* app_config = wxGetApp().app_config;
    auto hasget = [app_config](const std::string& name, std::vector<int>& vector)->bool {
        if (app_config->has(name)) {
            std::string val = app_config->get(name);
            if (!val.empty() || val[0]!='\0') {
                vector.push_back(std::stoi(val));
                return true;
            }
        }
        return false;
    };
    if (udt & UserDataType::UDT_SIZE) {
        if (!hasget("print_host_queue_dialog_height",vector))
            return false;
        if (!hasget("print_host_queue_dialog_width", vector))
            return false;
    }
    if (udt & UserDataType::UDT_POSITION)
    {
        if (!hasget("print_host_queue_dialog_x", vector))
            return false;
        if (!hasget("print_host_queue_dialog_y", vector))
            return false;
    }
    if (udt & UserDataType::UDT_COLS)
    {
        for (size_t i = 0; i < 7; i++)
        {
            if (!hasget("print_host_queue_dialog_column_" + std::to_string(i), vector))
                return false;
        }
    }
    return true;
}

ElegooPrintHostSendDialog::ElegooPrintHostSendDialog(const fs::path&            path,
                                                     PrintHostPostUploadActions post_actions,
                                                     const wxArrayString&       groups,
                                                     const wxArrayString&       storage_paths,
                                                     const wxArrayString&       storage_names,
                                                     bool                       switch_to_device_tab)
    : PrintHostSendDialog(path, post_actions, groups, storage_paths, storage_names, switch_to_device_tab)
    , m_timeLapse(0)
    , m_heatedBedLeveling(0)
    , m_BedType(BedType::btPTE)
{}

void ElegooPrintHostSendDialog::init() {

    auto preset_bundle = wxGetApp().preset_bundle;
    auto model_id = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);

    if (model_id != "Elegoo-CC" && model_id != "Elegoo-C") {
        PrintHostSendDialog::init();
        return;
    }
           
    const auto& path = m_path;
    const auto& storage_paths = m_paths;
    const auto& post_actions  = m_post_actions;
    const auto& storage_names = m_storage_names;

    this->SetMinSize(wxSize(500, 300));
    const AppConfig* app_config = wxGetApp().app_config;

    std::string uploadAndPrint = app_config->get("recent", CONFIG_KEY_UPLOADANDPRINT);
    if (!uploadAndPrint.empty())
        post_upload_action = static_cast<PrintHostPostUploadAction>(std::stoi(uploadAndPrint));

    std::string timeLapse = app_config->get("recent", CONFIG_KEY_TIMELAPSE);
    if (!timeLapse.empty())
        m_timeLapse = std::stoi(timeLapse);
    std::string heatedBedLeveling = app_config->get("recent", CONFIG_KEY_HEATEDBEDLEVELING);
    if (!heatedBedLeveling.empty())
        m_heatedBedLeveling = std::stoi(heatedBedLeveling);
    std::string bedType = app_config->get("recent", CONFIG_KEY_BEDTYPE);
    if (!bedType.empty())
        m_BedType = static_cast<BedType>(std::stoi(bedType));

    auto* label_dir_hint = new wxStaticText(this, wxID_ANY, _L("Use forward slashes ( / ) as a directory separator if needed."));
    label_dir_hint->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());

    wxSizerFlags flags = wxSizerFlags().Border(wxRIGHT, 16).Expand();

    content_sizer->Add(txt_filename, flags);
    content_sizer->AddSpacer(4);
    content_sizer->Add(label_dir_hint);
    content_sizer->AddSpacer(VERT_SPACING);

    if (combo_groups != nullptr) {
        // Repetier specific: Show a selection of file groups.
        auto* label_group = new wxStaticText(this, wxID_ANY, _L("Group"));
        content_sizer->Add(label_group);
        content_sizer->Add(combo_groups, 0, wxBOTTOM, 2 * VERT_SPACING);
        wxString recent_group = from_u8(app_config->get("recent", CONFIG_KEY_GROUP));
        if (!recent_group.empty())
            combo_groups->SetValue(recent_group);
    }

    if (combo_storage != nullptr) {
        // PrusaLink specific: User needs to choose a storage
        auto* label_group = new wxStaticText(this, wxID_ANY, _L("Upload to storage") + ":");
        content_sizer->Add(label_group);
        content_sizer->Add(combo_storage, 0, wxBOTTOM, 2 * VERT_SPACING);
        combo_storage->SetValue(storage_names.front());
        wxString recent_storage = from_u8(app_config->get("recent", CONFIG_KEY_STORAGE));
        if (!recent_storage.empty())
            combo_storage->SetValue(recent_storage);
    } else if (storage_names.GetCount() == 1) {
        // PrusaLink specific: Show which storage has been detected.
        auto* label_group = new wxStaticText(this, wxID_ANY, _L("Upload to storage") + ": " + storage_names.front());
        content_sizer->Add(label_group);
        m_preselected_storage = m_paths.front();
    }

    wxString recent_path = from_u8(app_config->get("recent", CONFIG_KEY_PATH));
    if (recent_path.Length() > 0 && recent_path[recent_path.Length() - 1] != '/') {
        recent_path += '/';
    }
    const auto recent_path_len = recent_path.Length();
    recent_path += path.filename().wstring();
    wxString   stem(path.stem().wstring());
    const auto stem_len = stem.Length();

    txt_filename->SetValue(recent_path);

    {
        auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto checkbox       = new ::CheckBox(this, wxID_APPLY);
        checkbox->SetValue(m_switch_to_device_tab);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
            m_switch_to_device_tab = e.IsChecked();
            e.Skip();
        });
        checkbox_sizer->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

        auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Switch to Device tab after upload."), wxDefaultPosition, wxDefaultSize, 0);
        checkbox_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
        checkbox_text->SetFont(::Label::Body_13);
        checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        content_sizer->Add(checkbox_sizer);
        content_sizer->AddSpacer(VERT_SPACING);
    }
    warning_text = new wxStaticText(this, wxID_ANY,
                                    _L("The selected bed type does not match the file. Please confirm before starting the print."),
                                    wxDefaultPosition, wxDefaultSize, 0);
    uploadandprint_sizer = new wxBoxSizer(wxVERTICAL);
    {
        auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto checkbox       = new ::CheckBox(this);
        checkbox->SetValue(post_upload_action == PrintHostPostUploadAction::StartPrint);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
            if (e.IsChecked()) {
                post_upload_action = PrintHostPostUploadAction::StartPrint;
            } else {
                post_upload_action = PrintHostPostUploadAction::None;
            }
            refresh();
            e.Skip();
        });
        checkbox_sizer->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

        auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Upload and Print"), wxDefaultPosition, wxDefaultSize, 0);
        checkbox_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
        checkbox_text->SetFont(::Label::Body_13);
        checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        content_sizer->Add(checkbox_sizer);
        content_sizer->AddSpacer(VERT_SPACING);
    }

    {
        auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto checkbox       = new ::CheckBox(this);
        checkbox->SetValue(m_timeLapse == 1);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
            m_timeLapse = e.IsChecked() ? 1 : 0;
            e.Skip();
        });
        checkbox_sizer->AddSpacer(16);
        checkbox_sizer->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

        auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Time-lapse"), wxDefaultPosition, wxDefaultSize, 0);
        checkbox_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
        checkbox_text->SetFont(::Label::Body_13);
        checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        uploadandprint_sizer->Add(checkbox_sizer);
        uploadandprint_sizer->AddSpacer(VERT_SPACING);
    }

    {
        auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto checkbox       = new ::CheckBox(this);
        checkbox->SetValue(m_heatedBedLeveling == 1);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
            m_heatedBedLeveling = e.IsChecked() ? 1 : 0;
            e.Skip();
        });
        checkbox_sizer->AddSpacer(16);
        checkbox_sizer->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

        auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Heated Bed Leveling"), wxDefaultPosition, wxDefaultSize, 0);
        checkbox_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
        checkbox_text->SetFont(::Label::Body_13);
        checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        uploadandprint_sizer->Add(checkbox_sizer);
        uploadandprint_sizer->AddSpacer(VERT_SPACING);
    }

    {
        auto radioBoxA = new ::RadioBox(this);
        auto radioBoxB = new ::RadioBox(this);
        if (m_BedType == BedType::btPC)
            radioBoxB->SetValue(true);
        else
            radioBoxA->SetValue(true);

        radioBoxA->Bind(wxEVT_LEFT_DOWN, [this, radioBoxA, radioBoxB](wxMouseEvent& e) {
            radioBoxA->SetValue(true);
            radioBoxB->SetValue(false);
            m_BedType = BedType::btPTE;
            refresh();
        });
        radioBoxB->Bind(wxEVT_LEFT_DOWN, [this, radioBoxA, radioBoxB](wxMouseEvent& e) {
            radioBoxA->SetValue(false);
            radioBoxB->SetValue(true);
            m_BedType = BedType::btPC;
            refresh();
        });

        {
            auto radio_sizer = new wxBoxSizer(wxHORIZONTAL);
            radio_sizer->AddSpacer(16);
            radio_sizer->Add(radioBoxA, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

            auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Textured Build Plate (Side A)"), wxDefaultPosition, wxDefaultSize, 0);
            radio_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
            checkbox_text->SetFont(::Label::Body_13);
            checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
            uploadandprint_sizer->Add(radio_sizer);
            uploadandprint_sizer->AddSpacer(VERT_SPACING);
        }
        {
            auto radio_sizer = new wxBoxSizer(wxHORIZONTAL);
            radio_sizer->AddSpacer(16);
            radio_sizer->Add(radioBoxB, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

            auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Smooth Build Plate (Side B)"), wxDefaultPosition, wxDefaultSize, 0);
            radio_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
            checkbox_text->SetFont(::Label::Body_13);
            checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
            uploadandprint_sizer->Add(radio_sizer);
            uploadandprint_sizer->AddSpacer(VERT_SPACING);
        }
    }
    {
        auto h_sizer = new wxBoxSizer(wxHORIZONTAL);
        warning_text->SetFont(::Label::Body_13);
        warning_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF1001")));
        // wrapping the text
        warning_text->Wrap(350);
        h_sizer->AddSpacer(16);
        h_sizer->Add(warning_text);

        uploadandprint_sizer->Add(h_sizer);
        uploadandprint_sizer->AddSpacer(VERT_SPACING);
    }

    content_sizer->Add(uploadandprint_sizer);
    uploadandprint_sizer->Show(post_upload_action == PrintHostPostUploadAction::StartPrint);
    warning_text->Show(post_upload_action == PrintHostPostUploadAction::StartPrint && appBedType() != m_BedType);

    uploadandprint_sizer->Layout();

    if (size_t extension_start = recent_path.find_last_of('.'); extension_start != std::string::npos)
        m_valid_suffix = recent_path.substr(extension_start);
    // .gcode suffix control
    auto validate_path = [this](const wxString& path) -> bool {
        if (!path.Lower().EndsWith(m_valid_suffix.Lower())) {
            MessageDialog msg_wingow(this,
                                     wxString::Format(_L("Upload filename doesn't end with \"%s\". Do you wish to continue?"),
                                                      m_valid_suffix),
                                     wxString(SLIC3R_APP_NAME), wxYES | wxNO);
            if (msg_wingow.ShowModal() == wxID_NO)
                return false;
        }
        return true;
    };

    auto* btn_ok = add_button(wxID_OK, true, _L("Upload"));
    btn_ok->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
        if (validate_path(txt_filename->GetValue())) {
            // post_upload_action = PrintHostPostUploadAction::None;
            EndDialog(wxID_OK);
        }
    });
    txt_filename->SetFocus();

    add_button(wxID_CANCEL, false, _L("Cancel"));
    finalize();

#ifdef __linux__
    // On Linux with GTK2 when text control lose the focus then selection (colored background) disappears but text color stay white
    // and as a result the text is invisible with light mode
    // see https://github.com/prusa3d/PrusaSlicer/issues/4532
    // Workaround: Unselect text selection explicitly on kill focus
    txt_filename->Bind(
        wxEVT_KILL_FOCUS,
        [this](wxEvent& e) {
            e.Skip();
            txt_filename->SetInsertionPoint(txt_filename->GetLastPosition());
        },
        txt_filename->GetId());
#endif /* __linux__ */

    Bind(wxEVT_SHOW, [=](const wxShowEvent&) {
        // Another similar case where the function only works with EVT_SHOW + CallAfter,
        // this time on Mac.
        CallAfter([=]() {
            txt_filename->SetInsertionPoint(0);
            txt_filename->SetSelection(recent_path_len, recent_path_len + stem_len);
        });
    });
}

void ElegooPrintHostSendDialog::EndModal(int ret)
{
    if (ret == wxID_OK) {

        AppConfig* app_config = wxGetApp().app_config;
        app_config->set("recent", CONFIG_KEY_UPLOADANDPRINT, std::to_string(static_cast<int>(post_upload_action)));
        app_config->set("recent", CONFIG_KEY_TIMELAPSE, std::to_string(m_timeLapse));
        app_config->set("recent", CONFIG_KEY_HEATEDBEDLEVELING, std::to_string(m_heatedBedLeveling));
        app_config->set("recent", CONFIG_KEY_BEDTYPE, std::to_string(static_cast<int>(m_BedType)));
    }

    PrintHostSendDialog::EndModal(ret);
}

BedType ElegooPrintHostSendDialog::appBedType() const
{
    std::string str_bed_type = wxGetApp().app_config->get("curr_bed_type");
    int bed_type_value = atoi(str_bed_type.c_str());
    return static_cast<BedType>(bed_type_value);
}

void ElegooPrintHostSendDialog::refresh()
{
    if (uploadandprint_sizer) {
        if (post_upload_action == PrintHostPostUploadAction::StartPrint) {
            uploadandprint_sizer->Show(true);
        } else {
            uploadandprint_sizer->Show(false);
        }
    }
    if (warning_text) {
        warning_text->Show(post_upload_action == PrintHostPostUploadAction::StartPrint && appBedType() != m_BedType);
    }
    this->Layout();
    this->Fit();
}

CrealityPrintHostSendDialog::CrealityPrintHostSendDialog(const fs::path&            path,
                                                         PrintHostPostUploadActions post_actions,
                                                         const wxArrayString&       groups,
                                                         const wxArrayString&       storage_paths,
                                                         const wxArrayString&       storage_names,
                                                         bool                       switch_to_device_tab,
                                                         PrintHost*                 printhost)
    : PrintHostSendDialog(path, post_actions, groups, storage_paths, storage_names, switch_to_device_tab)
    , m_enableSelfTest(false)
    , m_printhost(printhost)
{}

void CrealityPrintHostSendDialog::init()
{
    PrintHostSendDialog::init();

    auto* creality_host = static_cast<CrealityPrint*>(m_printhost);
    bool multi_color;
    std::string printer_name;
    {
        wxBusyCursor wait;
        multi_color = creality_host->supports_multi_color_print();
        if (multi_color)
            printer_name = creality_host->model_name();
    }
    if (!multi_color)
        return;

    auto* group_box = new wxStaticBox(this, wxID_ANY,
        wxString::Format(_L("Printer: %s"), printer_name));
    auto* group_sizer = new wxStaticBoxSizer(group_box, wxVERTICAL);
    content_sizer->Add(group_sizer, 0, wxEXPAND);

    const AppConfig* app_config = wxGetApp().app_config;
    std::string saved = app_config->get("recent", CONFIG_KEY_ENABLESELFTEST);
    if (!saved.empty()) {
        try { m_enableSelfTest = std::stoi(saved) != 0; } catch (...) {}
    }

    // Calibration checkbox
    {
        auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto checkbox       = new ::CheckBox(this);
        checkbox->SetValue(m_enableSelfTest);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
            m_enableSelfTest = e.IsChecked();
            AppConfig* ac = wxGetApp().app_config;
            ac->set("recent", CONFIG_KEY_ENABLESELFTEST, m_enableSelfTest ? "1" : "0");
            e.Skip();
        });
        checkbox_sizer->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
        auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Calibrate before printing"), wxDefaultPosition, wxDefaultSize, 0);
        checkbox_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
        checkbox_text->SetFont(::Label::Body_13);
        checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        group_sizer->Add(checkbox_sizer);
        group_sizer->AddSpacer(VERT_SPACING);
    }

    // --- Color mapping UI ---
    // Get gcode filament info from slicer
    auto  preset_bundle    = wxGetApp().preset_bundle;
    auto  full_config      = preset_bundle->full_config();
    auto* filament_colors  = full_config.option<ConfigOptionStrings>("filament_colour");
    auto* filament_types   = full_config.option<ConfigOptionStrings>("filament_type");
    int   gcode_filament_count = filament_colors ? (int)filament_colors->values.size() : 0;

    // Query printer for loaded materials
    {
        wxBusyCursor wait;
        std::string boxes_json = creality_host->query_boxes_info();
        if (!boxes_json.empty()) {
            try {
                auto resp = nlohmann::json::parse(boxes_json);
                if (resp.contains("boxsInfo") && resp["boxsInfo"].contains("materialBoxs")) {
                    for (auto& box : resp["boxsInfo"]["materialBoxs"]) {
                        int box_id = box["id"].get<int>();
                        int box_type = box.value("type", 0);
                        // Skip inactive CFS boxes (type 0 with state != 1)
                        // Spool holder (type 1) is always available
                        if (box_type == 0 && box.value("state", 0) != 1)
                            continue;
                        for (auto& mat : box["materials"]) {
                            int slot_id = mat["id"].get<int>();
                            std::string tool_id = "T" + std::to_string(box_id) + std::string(1, 'A' + slot_id);
                            // Creality uses "#0RRGGBB" (7 hex digits), normalize to "#RRGGBB"
                            std::string color = mat.value("color", "#FFFFFF");
                            if (color.size() == 8 && color[0] == '#')
                                color = "#" + color.substr(2);
                            m_printer_slots.push_back({
                                tool_id,
                                mat.value("type", ""),
                                color,
                                box_id,
                                slot_id
                            });
                        }
                    }
                }
            } catch (const nlohmann::json::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "CrealityPrint dialog: Failed to parse boxsInfo: " << e.what();
            }
        }
    }

    if (gcode_filament_count > 0 && !m_printer_slots.empty()) {
        auto* label = new wxStaticText(this, wxID_ANY, _L("Filament Mapping:"));
        label->SetFont(::Label::Body_13);
        label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        group_sizer->Add(label);
        group_sizer->AddSpacer(4);

        for (int i = 0; i < gcode_filament_count; i++) {
            auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);

            // Left side: gcode filament color swatch + type
            std::string gc_color = (filament_colors && i < (int)filament_colors->values.size())
                                   ? filament_colors->values[i] : "#FFFFFF";
            std::string gc_type  = (filament_types && i < (int)filament_types->values.size())
                                   ? filament_types->values[i] : "?";

            // Color indicator panel
            auto* color_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(16), FromDIP(16)));
            color_panel->SetBackgroundColour(wxColour(gc_color));
            color_panel->SetMinSize(wxSize(FromDIP(16), FromDIP(16)));
            row_sizer->Add(color_panel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

            auto* type_label = new wxStaticText(this, wxID_ANY,
                wxString::Format("%d (%s)", i + 1, gc_type.c_str()));
            type_label->SetFont(::Label::Body_13);
            type_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
            type_label->SetMinSize(wxSize(FromDIP(80), -1));
            row_sizer->Add(type_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

            // Arrow
            auto* arrow_label = new wxStaticText(this, wxID_ANY, wxString::FromUTF8("\xe2\x86\x92"));
            arrow_label->SetFont(::Label::Body_13);
            row_sizer->Add(arrow_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

            // Right side: dropdown with color icons per slot
            int icon_sz = FromDIP(16);
            auto* combo = new BitmapComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
            for (auto& slot : m_printer_slots) {
                wxBitmap* bmp = get_extruder_color_icon(slot.color, "", icon_sz, icon_sz);
                wxString label_str;
                if (slot.box_id == 0)
                    label_str = wxString::Format("Ext - %s", slot.type.c_str());
                else
                    label_str = wxString::Format("%s - %s", slot.tool_id.substr(1).c_str(), slot.type.c_str());
                combo->Append(label_str, bmp ? *bmp : wxNullBitmap);
            }
            // Find best default: CFS exact color+type, CFS type-only,
            // Ext exact, Ext type-only, else positional
            int default_sel = (i < (int)m_printer_slots.size()) ? i : 0;
            bool matched = false;
            for (int pass = 0; pass < 4 && !matched; pass++) {
                for (int s = 0; s < (int)m_printer_slots.size(); s++) {
                    bool is_ext = (m_printer_slots[s].box_id == 0);
                    bool type_match = (m_printer_slots[s].type == gc_type);
                    bool color_match = (wxColour(m_printer_slots[s].color) == wxColour(gc_color));
                    bool hit = false;
                    switch (pass) {
                    case 0: hit = !is_ext && type_match && color_match; break;
                    case 1: hit = !is_ext && type_match; break;
                    case 2: hit = is_ext && type_match && color_match; break;
                    case 3: hit = is_ext && type_match; break;
                    }
                    if (hit) {
                        default_sel = s;
                        matched = true;
                        break;
                    }
                }
            }
            combo->SetSelection(default_sel);
            row_sizer->Add(combo, 0, wxALIGN_CENTER_VERTICAL);

            group_sizer->Add(row_sizer);
            group_sizer->AddSpacer(4);
            m_slot_combos.push_back(combo);
        }

        int ext_slot_idx = -1;
        for (int s = 0; s < (int)m_printer_slots.size(); s++) {
            if (m_printer_slots[s].box_id == 0) {
                ext_slot_idx = s;
                break;
            }
        }
        if (ext_slot_idx >= 0) {
            for (int ci = 0; ci < (int)m_slot_combos.size(); ci++) {
                int sel = m_slot_combos[ci]->GetSelection();
                if (sel >= 0 && sel < (int)m_printer_slots.size() &&
                    m_printer_slots[sel].box_id == 0) {
                    for (int cj = 0; cj < (int)m_slot_combos.size(); cj++) {
                        if (cj != ci)
                            m_slot_combos[cj]->Enable(false);
                    }
                    break;
                }
            }

            for (auto* c : m_slot_combos) {
                c->Bind(wxEVT_COMBOBOX, [this, ext_slot_idx](wxCommandEvent& e) {
                    int sel = e.GetSelection();
                    if (sel >= 0 && sel < (int)m_printer_slots.size() &&
                        m_printer_slots[sel].box_id == 0) {
                        for (auto* c2 : m_slot_combos) {
                            if (c2 != e.GetEventObject())
                                c2->Enable(false);
                        }
                    } else {
                        for (auto* c2 : m_slot_combos)
                            c2->Enable(true);
                    }
                    e.Skip();
                });
            }
        }
    }

    this->Layout();
    this->Fit();
}

std::map<std::string, std::string> CrealityPrintHostSendDialog::extendedInfo() const
{
    std::map<std::string, std::string> info;
    info["enableSelfTest"] = m_enableSelfTest ? "1" : "0";

    // Color mapping: colorMatch_0, colorMatch_1, ... tab-delimited
    for (int i = 0; i < (int)m_slot_combos.size(); i++) {
        int sel = m_slot_combos[i]->GetSelection();
        if (sel >= 0 && sel < (int)m_printer_slots.size()) {
            auto& slot = m_printer_slots[sel];
            // id = gcode tool index (T1A for first filament, T1B for second, ...),
            // not the destination CFS slot — firmware matches by gcode tool.
            std::string gcode_tool = "T1" + std::string(1, 'A' + i);
            info["colorMatch_" + std::to_string(i)] =
                gcode_tool + "\t" + slot.type + "\t" + slot.color + "\t" +
                std::to_string(slot.box_id) + "\t" + std::to_string(slot.material_id);
        }
    }

    return info;
}

}}
