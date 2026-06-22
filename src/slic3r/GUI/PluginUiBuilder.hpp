// Phase 4.1 — shared orca_ui_builder_t implementation.
//
// Wraps a wxWindow + wxBoxSizer and exposes the C ABI vtable declared
// in plugin_api.h. The opaque orca_ui_page_t* the plugin sees is this
// object cast through reinterpret_cast.
//
// Every settings-page / sidebar-panel / device-tab slot dispatcher in
// Phase 4.1.{1,2,4} constructs one of these against the host wxWindow
// (a Page::parent(), Sidebar*, or device tab page) and calls
// vtable->on_build(builder.page(), builder.vtable(), user_data).
#pragma once

#include "orca/plugin_api.h"

#include <wx/sizer.h>
#include <wx/window.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r { namespace GUI {

class PluginUiBuilder {
public:
    PluginUiBuilder(wxWindow* parent, wxBoxSizer* host_sizer);

    PluginUiBuilder(const PluginUiBuilder&)            = delete;
    PluginUiBuilder& operator=(const PluginUiBuilder&) = delete;

    /// Hand to the plugin's on_build callback alongside vtable().
    orca_ui_page_t* page() {
        return reinterpret_cast<orca_ui_page_t*>(this);
    }
    const orca_ui_builder_t* vtable() const { return &vtable_; }

    /// Read the currently stored JSON-encoded value for a key (or "" when
    /// unset). Used by the host AFTER on_build returns to persist the
    /// initial state if it wants.
    std::string value_of(const std::string& key) const;

    static PluginUiBuilder* from(orca_ui_page_t* page) {
        return reinterpret_cast<PluginUiBuilder*>(page);
    }

private:
    // The "current" sizer — most-recently-pushed group, or host_sizer_
    // when no group is open. New controls land here.
    wxBoxSizer* current_sizer();

    // Persist + notify after a control changes value.
    void store_and_notify(const std::string& key, std::string json_value);

    // ---- C ABI thunks (all match orca_ui_builder_t signatures) ----
    static orca_ui_group_t* c_push_group(orca_ui_page_t*, const char*);
    static void c_pop_group(orca_ui_page_t*);
    static void c_add_label(orca_ui_page_t*, const char*);
    static void c_add_separator(orca_ui_page_t*);
    static void c_add_text_field(orca_ui_page_t*, const char*, const char*, const char*);
    static void c_add_int_field(orca_ui_page_t*, const char*, const char*, int64_t);
    static void c_add_float_field(orca_ui_page_t*, const char*, const char*, double);
    static void c_add_bool_field(orca_ui_page_t*, const char*, const char*, int);
    static void c_add_combo(orca_ui_page_t*, const char*, const char*,
                            const char* const*, const char*);
    static void c_add_button(orca_ui_page_t*, const char*,
                             orca_ui_button_click_fn_t, void*);
    static void c_add_html(orca_ui_page_t*, const char*);
    static void c_on_value_changed(orca_ui_page_t*,
                                   orca_ui_value_changed_fn_t, void*);
    static const char* c_get_value(orca_ui_page_t*, const char*);
    static void        c_set_value(orca_ui_page_t*, const char*, const char*);

    wxWindow*                      parent_     = nullptr;
    wxBoxSizer*                    host_sizer_ = nullptr;
    std::vector<wxBoxSizer*>       group_stack_;
    std::unordered_map<std::string, std::string> values_;
    orca_ui_value_changed_fn_t     value_changed_cb_ = nullptr;
    void*                          value_changed_ud_ = nullptr;
    // C ABI vtable filled in the constructor to point at the c_* thunks.
    orca_ui_builder_t              vtable_{};
};

}} // namespace Slic3r::GUI
