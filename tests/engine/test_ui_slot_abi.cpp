// Phase 4.0 — UI slot ABI surface check.
//
// Locks down the layout of the new UI builder + slot vtables added in
// plugin_api.h so accidental reorderings are caught early. The test is
// intentionally minimal — it does not exercise dispatch (that lands as
// each per-slot dispatcher arrives in Phase 4.1.1-4.1.4); it just
// asserts the C ABI types compile, have non-zero sizes, and expose the
// function-pointer fields documented in the header.

#include <catch2/catch_all.hpp>

#include "orca/plugin_api.h"

#include <cstddef>

TEST_CASE("Phase 4.0 — UI builder vtable surface is present", "[abi][ui]") {
    orca_ui_builder_t b{};
    b.struct_size = sizeof(orca_ui_builder_t);

    REQUIRE(b.struct_size >= sizeof(uint32_t) * 1);

    // Every documented method slot should be a valid lvalue you can
    // assign — proves the field exists with the right type.
    b.push_group       = nullptr;
    b.pop_group        = nullptr;
    b.add_label        = nullptr;
    b.add_separator    = nullptr;
    b.add_text_field   = nullptr;
    b.add_int_field    = nullptr;
    b.add_float_field  = nullptr;
    b.add_bool_field   = nullptr;
    b.add_combo        = nullptr;
    b.add_button       = nullptr;
    b.add_html         = nullptr;
    b.on_value_changed = nullptr;
    b.get_value        = nullptr;
    b.set_value        = nullptr;
}

TEST_CASE("Phase 4.0 — UI slot vtables register through the registry kinds",
          "[abi][ui]") {
    SECTION("settings_page") {
        orca_slot_settings_page_t s{};
        s.struct_size = sizeof(orca_slot_settings_page_t);
        s.tab         = ORCA_SETTINGS_TAB_PRINT;
        s.page_title  = "test";
        s.on_build    = nullptr;
        CHECK(s.struct_size > 0);
    }
    SECTION("sidebar_panel") {
        orca_slot_sidebar_panel_t s{};
        s.struct_size = sizeof(orca_slot_sidebar_panel_t);
        s.panel_title = "test";
        s.on_build    = nullptr;
        CHECK(s.struct_size > 0);
    }
    SECTION("menu_command") {
        orca_slot_menu_command_t s{};
        s.struct_size = sizeof(orca_slot_menu_command_t);
        s.menu        = ORCA_MENU_TOOLS;
        s.label       = "Test";
        s.shortcut    = nullptr;
        s.on_click    = nullptr;
        CHECK(s.struct_size > 0);
    }
    SECTION("device_tab") {
        orca_slot_device_tab_t s{};
        s.struct_size  = sizeof(orca_slot_device_tab_t);
        s.tab_title    = "Test";
        s.match_vendor = nullptr;
        s.on_build     = nullptr;
        CHECK(s.struct_size > 0);
    }
}
