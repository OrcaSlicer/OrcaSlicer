// Phase 4.1 — shared orca_ui_builder_t implementation.

#include "PluginUiBuilder.hpp"

#include "Widgets/WebView.hpp"

#include "nlohmann/json.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/statbox.h>
#include <wx/textctrl.h>
#include <wx/webview.h>

#include <cstring>

namespace Slic3r { namespace GUI {

namespace {

constexpr int kRowPad = 4;

wxBoxSizer* make_row(wxWindow* parent, const char* label) {
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    if (label && label[0]) {
        auto* st = new wxStaticText(parent, wxID_ANY, wxString::FromUTF8(label));
        st->SetMinSize({160, -1});
        row->Add(st, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, kRowPad);
    }
    return row;
}

} // namespace

PluginUiBuilder::PluginUiBuilder(wxWindow* parent, wxBoxSizer* host_sizer)
    : parent_(parent), host_sizer_(host_sizer)
{
    vtable_.struct_size      = sizeof(orca_ui_builder_t);
    vtable_.push_group       = &PluginUiBuilder::c_push_group;
    vtable_.pop_group        = &PluginUiBuilder::c_pop_group;
    vtable_.add_label        = &PluginUiBuilder::c_add_label;
    vtable_.add_separator    = &PluginUiBuilder::c_add_separator;
    vtable_.add_text_field   = &PluginUiBuilder::c_add_text_field;
    vtable_.add_int_field    = &PluginUiBuilder::c_add_int_field;
    vtable_.add_float_field  = &PluginUiBuilder::c_add_float_field;
    vtable_.add_bool_field   = &PluginUiBuilder::c_add_bool_field;
    vtable_.add_combo        = &PluginUiBuilder::c_add_combo;
    vtable_.add_button       = &PluginUiBuilder::c_add_button;
    vtable_.add_html         = &PluginUiBuilder::c_add_html;
    vtable_.on_value_changed = &PluginUiBuilder::c_on_value_changed;
    vtable_.get_value        = &PluginUiBuilder::c_get_value;
    vtable_.set_value        = &PluginUiBuilder::c_set_value;
}

wxBoxSizer* PluginUiBuilder::current_sizer() {
    return group_stack_.empty() ? host_sizer_ : group_stack_.back();
}

std::string PluginUiBuilder::value_of(const std::string& key) const {
    auto it = values_.find(key);
    return it == values_.end() ? std::string{} : it->second;
}

void PluginUiBuilder::store_and_notify(const std::string& key,
                                       std::string        json_value)
{
    values_[key] = json_value;
    if (value_changed_cb_) {
        value_changed_cb_(
            reinterpret_cast<orca_ui_page_t*>(this),
            key.c_str(),
            json_value.c_str(),
            value_changed_ud_);
    }
}

// ---- C ABI thunks ---------------------------------------------------------

orca_ui_group_t* PluginUiBuilder::c_push_group(orca_ui_page_t* page,
                                               const char*     label)
{
    auto* self = from(page);
    auto* sb   = new wxStaticBox(self->parent_, wxID_ANY,
                                  label ? wxString::FromUTF8(label) : "");
    auto* box  = new wxStaticBoxSizer(sb, wxVERTICAL);
    self->current_sizer()->Add(box, 0, wxEXPAND | wxALL, kRowPad);
    self->group_stack_.push_back(box);
    return reinterpret_cast<orca_ui_group_t*>(box);
}

void PluginUiBuilder::c_pop_group(orca_ui_page_t* page) {
    auto* self = from(page);
    if (!self->group_stack_.empty()) self->group_stack_.pop_back();
}

void PluginUiBuilder::c_add_label(orca_ui_page_t* page, const char* text) {
    auto* self = from(page);
    auto* st   = new wxStaticText(
        self->parent_, wxID_ANY,
        text ? wxString::FromUTF8(text) : "");
    self->current_sizer()->Add(st, 0, wxEXPAND | wxALL, kRowPad);
}

void PluginUiBuilder::c_add_separator(orca_ui_page_t* page) {
    auto* self = from(page);
    auto* line = new wxStaticLine(self->parent_);
    self->current_sizer()->Add(line, 0, wxEXPAND | wxTOP | wxBOTTOM, kRowPad);
}

void PluginUiBuilder::c_add_text_field(orca_ui_page_t* page,
                                       const char*     label,
                                       const char*     key,
                                       const char*     default_value)
{
    auto* self = from(page);
    auto* row  = make_row(self->parent_, label);
    auto* tc   = new wxTextCtrl(
        self->parent_, wxID_ANY,
        default_value ? wxString::FromUTF8(default_value) : "");
    row->Add(tc, 1, wxALIGN_CENTER_VERTICAL);
    self->current_sizer()->Add(row, 0, wxEXPAND | wxALL, kRowPad);

    std::string skey = key ? key : "";
    self->values_[skey] = nlohmann::json(
        default_value ? std::string{default_value} : std::string{}).dump();
    tc->Bind(wxEVT_TEXT, [self, skey, tc](wxCommandEvent&) {
        self->store_and_notify(skey,
            nlohmann::json(tc->GetValue().utf8_string()).dump());
    });
}

void PluginUiBuilder::c_add_int_field(orca_ui_page_t* page,
                                      const char*     label,
                                      const char*     key,
                                      int64_t         default_value)
{
    auto* self = from(page);
    auto* row  = make_row(self->parent_, label);
    auto* sc   = new wxSpinCtrl(
        self->parent_, wxID_ANY, wxString::Format("%lld",
            static_cast<long long>(default_value)),
        wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS,
        -2147483647, 2147483647, static_cast<int>(default_value));
    row->Add(sc, 1, wxALIGN_CENTER_VERTICAL);
    self->current_sizer()->Add(row, 0, wxEXPAND | wxALL, kRowPad);

    std::string skey = key ? key : "";
    self->values_[skey] = std::to_string(default_value);
    sc->Bind(wxEVT_SPINCTRL, [self, skey, sc](wxSpinEvent&) {
        self->store_and_notify(skey, std::to_string(sc->GetValue()));
    });
}

void PluginUiBuilder::c_add_float_field(orca_ui_page_t* page,
                                        const char*     label,
                                        const char*     key,
                                        double          default_value)
{
    auto* self = from(page);
    auto* row  = make_row(self->parent_, label);
    // wxSpinCtrlDouble varies across platforms; use a plain wxTextCtrl
    // with a number-format-tolerant validator at parse time.
    auto* tc = new wxTextCtrl(
        self->parent_, wxID_ANY,
        wxString::Format("%g", default_value));
    row->Add(tc, 1, wxALIGN_CENTER_VERTICAL);
    self->current_sizer()->Add(row, 0, wxEXPAND | wxALL, kRowPad);

    std::string skey = key ? key : "";
    self->values_[skey] = nlohmann::json(default_value).dump();
    tc->Bind(wxEVT_TEXT, [self, skey, tc](wxCommandEvent&) {
        double d = 0.0;
        tc->GetValue().ToDouble(&d);
        self->store_and_notify(skey, nlohmann::json(d).dump());
    });
}

void PluginUiBuilder::c_add_bool_field(orca_ui_page_t* page,
                                       const char*     label,
                                       const char*     key,
                                       int             default_value)
{
    auto* self = from(page);
    auto* cb   = new wxCheckBox(
        self->parent_, wxID_ANY,
        label ? wxString::FromUTF8(label) : "");
    cb->SetValue(default_value != 0);
    self->current_sizer()->Add(cb, 0, wxEXPAND | wxALL, kRowPad);

    std::string skey = key ? key : "";
    self->values_[skey] = default_value ? "true" : "false";
    cb->Bind(wxEVT_CHECKBOX, [self, skey, cb](wxCommandEvent&) {
        self->store_and_notify(skey, cb->GetValue() ? "true" : "false");
    });
}

void PluginUiBuilder::c_add_combo(orca_ui_page_t* page,
                                  const char*     label,
                                  const char*     key,
                                  const char* const* options,
                                  const char*     default_value)
{
    auto* self = from(page);
    auto* row  = make_row(self->parent_, label);

    wxArrayString choices;
    if (options) {
        for (auto** p = const_cast<char**>(const_cast<const char**>(options));
             p && *p; ++p)
            choices.Add(wxString::FromUTF8(*p));
    }
    auto* ch = new wxChoice(self->parent_, wxID_ANY,
                             wxDefaultPosition, wxDefaultSize, choices);
    if (default_value)
        ch->SetStringSelection(wxString::FromUTF8(default_value));
    else if (!choices.IsEmpty())
        ch->SetSelection(0);
    row->Add(ch, 1, wxALIGN_CENTER_VERTICAL);
    self->current_sizer()->Add(row, 0, wxEXPAND | wxALL, kRowPad);

    std::string skey = key ? key : "";
    self->values_[skey] = nlohmann::json(
        default_value ? std::string{default_value} : std::string{}).dump();
    ch->Bind(wxEVT_CHOICE, [self, skey, ch](wxCommandEvent&) {
        self->store_and_notify(skey,
            nlohmann::json(ch->GetStringSelection().utf8_string()).dump());
    });
}

void PluginUiBuilder::c_add_button(orca_ui_page_t*         page,
                                   const char*             label,
                                   orca_ui_button_click_fn_t on_click,
                                   void*                   user_data)
{
    auto* self = from(page);
    auto* btn  = new wxButton(
        self->parent_, wxID_ANY,
        label ? wxString::FromUTF8(label) : "");
    self->current_sizer()->Add(btn, 0, wxALL, kRowPad);
    auto page_handle = self->page();
    btn->Bind(wxEVT_BUTTON, [on_click, page_handle, user_data](wxCommandEvent&) {
        if (on_click) on_click(page_handle, user_data);
    });
}

void PluginUiBuilder::c_add_html(orca_ui_page_t* page, const char* html) {
    auto* self = from(page);
    auto* webview = WebView::CreateWebView(self->parent_, wxString{});
    if (webview) {
        webview->SetPage(html ? wxString::FromUTF8(html) : "", "about:plugin");
        webview->SetMinSize({-1, 200});
        self->current_sizer()->Add(webview, 1, wxEXPAND | wxALL, kRowPad);
    }
}

void PluginUiBuilder::c_on_value_changed(orca_ui_page_t*           page,
                                         orca_ui_value_changed_fn_t cb,
                                         void*                     user_data)
{
    auto* self = from(page);
    self->value_changed_cb_ = cb;
    self->value_changed_ud_ = user_data;
}

const char* PluginUiBuilder::c_get_value(orca_ui_page_t* page,
                                         const char*     key)
{
    auto* self = from(page);
    auto it = self->values_.find(key ? key : "");
    if (it == self->values_.end()) return nullptr;
    // Hand a heap-allocated copy back; plugin frees via host->string_free.
    char* out = static_cast<char*>(std::malloc(it->second.size() + 1));
    std::memcpy(out, it->second.data(), it->second.size());
    out[it->second.size()] = '\0';
    return out;
}

void PluginUiBuilder::c_set_value(orca_ui_page_t* page,
                                  const char*     key,
                                  const char*     json_value)
{
    auto* self = from(page);
    self->store_and_notify(key ? key : "",
                           json_value ? json_value : "null");
}

}} // namespace Slic3r::GUI
