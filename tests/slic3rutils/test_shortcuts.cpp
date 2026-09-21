#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/KeyChord.hpp"
#include "slic3r/GUI/Shortcuts.hpp"

#include <wx/event.h>

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {

wxKeyEvent key_event(wxEventType type, int key_code, int modifiers = wxMOD_NONE)
{
    wxKeyEvent evt(type);
    evt.m_keyCode = key_code;
    evt.SetControlDown(modifiers & wxMOD_CONTROL);
    evt.SetShiftDown(modifiers & wxMOD_SHIFT);
    evt.SetAltDown(modifiers & wxMOD_ALT);
    return evt;
}

} // namespace

TEST_CASE("KeyChord round-trips through its canonical text", "[Shortcuts]")
{
    const auto [chord, text] = GENERATE(table<KeyChord, std::string>({
        { { 'N', wxMOD_CONTROL },                 "Ctrl+N" },
        { { 'S', wxMOD_CONTROL | wxMOD_SHIFT },   "Ctrl+Shift+S" },
        { { WXK_RETURN, wxMOD_SHIFT | wxMOD_ALT }, "Shift+Alt+Enter" },
        { { WXK_TAB, wxMOD_SHIFT },               "Shift+Tab" },
        { { WXK_DELETE },                         "Del" },
        { { WXK_F5 },                             "F5" },
        { { WXK_F12, wxMOD_CONTROL },             "Ctrl+F12" },
        { { '+' },                                "+" },
        { { '-', wxMOD_CONTROL },                 "Ctrl+-" },
        { { '?' },                                "?" },
        { { ',', wxMOD_CONTROL },                 "Ctrl+," },
    }));
    CAPTURE(text);
    CHECK(chord.to_string() == text);
    REQUIRE(KeyChord::parse(text).has_value());
    CHECK(*KeyChord::parse(text) == chord);
}

TEST_CASE("KeyChord::parse accepts aliases and rejects malformed text", "[Shortcuts]")
{
    CHECK(KeyChord::parse("control+n") == KeyChord{ 'N', wxMOD_CONTROL });
    CHECK(KeyChord::parse("Cmd+Shift+Delete") == KeyChord{ WXK_DELETE, wxMOD_CONTROL | wxMOD_SHIFT });
    CHECK(KeyChord::parse("PageUp") == KeyChord{ WXK_PAGEUP });
    CHECK(KeyChord::parse("f3") == KeyChord{ WXK_F3 });

    CHECK_FALSE(KeyChord::parse("").has_value());
    CHECK_FALSE(KeyChord::parse("Ctrl+").has_value());
    CHECK_FALSE(KeyChord::parse("Meta+A").has_value());
    CHECK_FALSE(KeyChord::parse("F25").has_value());
    CHECK_FALSE(KeyChord::parse("Shift+/").has_value());   // Shift is part of the punctuation character
}

TEST_CASE("Key events normalize to the key-down key codes", "[Shortcuts]")
{
    CHECK(KeyChord::from_event(key_event(wxEVT_KEY_DOWN, 'A', wxMOD_CONTROL)) == KeyChord{ 'A', wxMOD_CONTROL });
    CHECK(KeyChord::from_event(key_event(wxEVT_KEY_DOWN, WXK_NUMPAD5, wxMOD_CONTROL)) == KeyChord{ '5', wxMOD_CONTROL });
    CHECK(KeyChord::from_event(key_event(wxEVT_KEY_DOWN, WXK_NUMPAD_ADD)) == KeyChord{ '+' });
    CHECK(KeyChord::from_event(key_event(wxEVT_KEY_DOWN, WXK_NUMPAD_PAGEUP)) == KeyChord{ WXK_PAGEUP });
    CHECK_FALSE(KeyChord::from_event(key_event(wxEVT_KEY_DOWN, WXK_SHIFT, wxMOD_SHIFT)).valid());
    CHECK_FALSE(KeyChord::from_event(key_event(wxEVT_KEY_DOWN, WXK_CONTROL, wxMOD_CONTROL)).valid());

    CHECK(KeyChord::from_event(key_event(wxEVT_CHAR, 'a')) == KeyChord{ 'A' });
    CHECK(KeyChord::from_event(key_event(wxEVT_CHAR, 'A', wxMOD_SHIFT)) == KeyChord{ 'A', wxMOD_SHIFT });
    CHECK(KeyChord::from_event(key_event(wxEVT_CHAR, WXK_CONTROL_C, wxMOD_CONTROL)) == KeyChord{ 'C', wxMOD_CONTROL });
    CHECK(KeyChord::from_event(key_event(wxEVT_CHAR, '+', wxMOD_SHIFT)) == KeyChord{ '+' });
    CHECK(KeyChord::from_event(key_event(wxEVT_CHAR, WXK_DELETE)) == KeyChord{ WXK_DELETE });
    CHECK_FALSE(KeyChord::from_event(key_event(wxEVT_CHAR, 0x444)).valid());   // a Cyrillic letter is not bindable
}

TEST_CASE("Punctuation chords are the ones matched on char events", "[Shortcuts]")
{
    CHECK(KeyChord{ '+' }.is_punctuation());
    CHECK(KeyChord{ '?' }.is_punctuation());
    CHECK_FALSE(KeyChord{ 'A' }.is_punctuation());
    CHECK_FALSE(KeyChord{ '1' }.is_punctuation());
    CHECK_FALSE(KeyChord{ '=', wxMOD_CONTROL }.is_punctuation());
    CHECK_FALSE(KeyChord{ WXK_DELETE }.is_punctuation());
}

TEST_CASE("Chords that only the char event can resolve are recognized", "[Shortcuts]")
{
    CHECK(KeyChord{ '/', wxMOD_SHIFT }.needs_char_event());
    CHECK(KeyChord{ '-' }.needs_char_event());
    CHECK_FALSE(KeyChord{ '=', wxMOD_CONTROL }.needs_char_event());
    CHECK_FALSE(KeyChord{ 'A', wxMOD_SHIFT }.needs_char_event());
    CHECK_FALSE(KeyChord{ '1' }.needs_char_event());
    CHECK_FALSE(KeyChord{ WXK_F5 }.needs_char_event());
}

TEST_CASE("Only modified or non-printable chords qualify as menu accelerators", "[Shortcuts]")
{
    CHECK(KeyChord{ 'N', wxMOD_CONTROL }.is_menu_accelerator());
    CHECK(KeyChord{ 'N', wxMOD_ALT }.is_menu_accelerator());
    CHECK(KeyChord{ WXK_DELETE }.is_menu_accelerator());
    CHECK(KeyChord{ WXK_F5 }.is_menu_accelerator());
    CHECK_FALSE(KeyChord{ 'A' }.is_menu_accelerator());
    CHECK_FALSE(KeyChord{ 'A', wxMOD_SHIFT }.is_menu_accelerator());
    CHECK_FALSE(KeyChord{ '?' }.is_menu_accelerator());
    CHECK_FALSE(KeyChord{ WXK_SPACE }.is_menu_accelerator());
    CHECK_FALSE(KeyChord{}.is_menu_accelerator());

    ShortcutRegistry registry;
    CHECK(registry.accelerator(Shortcut::NewProject) == "Ctrl+N");
#ifdef __APPLE__
    CHECK(registry.accelerator(Shortcut::DeleteSelected) == "Backspace");
#else
    CHECK(registry.accelerator(Shortcut::DeleteSelected) == "Del");
#endif
    CHECK(registry.accelerator(Shortcut::Arrange).empty());
    CHECK(registry.accelerator(Shortcut::ArrangePlate).empty());
    CHECK(registry.accelerator(Shortcut::KeyboardShortcuts).empty());
}

TEST_CASE("Chords convert to wx accelerator entries", "[Shortcuts]")
{
    const wxAcceleratorEntry entry = KeyChord{ 'S', wxMOD_CONTROL | wxMOD_SHIFT }.to_accelerator_entry(42);
    CHECK(entry.GetFlags() == (wxACCEL_CTRL | wxACCEL_SHIFT));
    CHECK(entry.GetKeyCode() == 'S');
    CHECK(entry.GetCommand() == 42);

    const wxAcceleratorEntry bare = KeyChord{ WXK_BACK }.to_accelerator_entry(7);
    CHECK(bare.GetFlags() == wxACCEL_NORMAL);
    CHECK(bare.GetKeyCode() == WXK_BACK);
}

#ifndef __APPLE__
TEST_CASE("Display text matches the canonical text without translations", "[Shortcuts]")
{
    CHECK(KeyChord{ WXK_DELETE, wxMOD_CONTROL | wxMOD_SHIFT }.display() == "Ctrl+Shift+Del");
    CHECK(KeyChord{ WXK_DELETE, wxMOD_CONTROL | wxMOD_SHIFT }.display_parts() == std::vector<std::string>{ "Ctrl", "Shift", "Del" });
    CHECK(KeyChord{ '+' }.display() == "+");
    CHECK(KeyChord{ WXK_UP, wxMOD_SHIFT }.display() == "Shift+Arrow Up");   // the arrows keep the old dialog's names
    CHECK(KeyChord{ WXK_UP, wxMOD_SHIFT }.to_string() == "Shift+Up");
    CHECK(KeyChord{}.display().empty());
}
#endif

TEST_CASE("Every shortcut is listed under the section of its table row", "[Shortcuts]")
{
    CHECK(shortcut_section(Shortcut::NewProject) == ShortcutSection::Project);
    CHECK(shortcut_section(Shortcut::Publish3mf) == ShortcutSection::Project);
    CHECK(shortcut_section(Shortcut::SlicePlate) == ShortcutSection::SlicingAndPrinting);
    CHECK(shortcut_section(Shortcut::GizmoBrimEars) == ShortcutSection::Gizmos);
    CHECK(shortcut_section(Shortcut::MovesSliderEnd) == ShortcutSection::Sliders);
    CHECK(shortcut_section(Shortcut::ViewDefault) == ShortcutSection::Camera);
    CHECK(shortcut_section(Shortcut::KeyboardShortcuts) == ShortcutSection::Application);
    CHECK(std::string(section_name(ShortcutSection::SlicingAndPrinting)) == "Slicing and printing");
}

TEST_CASE("Default bindings never collide inside a context", "[Shortcuts]")
{
    ShortcutRegistry registry;
    for (size_t i = 0; i < size_t(Shortcut::Count); ++i) {
        const Shortcut shortcut = Shortcut(i);
        CAPTURE(shortcut_info(shortcut).key);
        CHECK(registry.conflicts(shortcut, registry.binding(shortcut)).empty());
    }
}

TEST_CASE("Shift and Ctrl variants of stepping shortcuts are left unbound", "[Shortcuts]")
{
    ShortcutRegistry registry;
    for (size_t i = 0; i < size_t(Shortcut::Count); ++i) {
        const ShortcutInfo& info = shortcut_info(Shortcut(i));
        if (!info.modifier_variants)
            continue;
        CAPTURE(info.key);
        const KeyChord chord = registry.binding(info.id);
        for (int modifier : { int(wxMOD_SHIFT), int(wxMOD_CONTROL), int(wxMOD_SHIFT | wxMOD_CONTROL) })
            for (size_t c = 0; c < size_t(ShortcutContext::Count); ++c)
                if (info.contexts & context_bit(ShortcutContext(c)))
                    CHECK_FALSE(registry.lookup(ShortcutContext(c), KeyChord{ chord.key, chord.modifiers | modifier }).has_value());
    }
}

TEST_CASE("Lookups are scoped to the context of the key press", "[Shortcuts]")
{
    ShortcutRegistry registry;
    const KeyChord   ctrl_n{ 'N', wxMOD_CONTROL };
    const KeyChord   ctrl_c{ 'C', wxMOD_CONTROL };
    const KeyChord   a{ 'A' };
    const KeyChord   c{ 'C' };

    CHECK(registry.lookup(ShortcutContext::Global, ctrl_n) == Shortcut::NewProject);
    CHECK_FALSE(registry.lookup(ShortcutContext::Plater, ctrl_n).has_value());

    CHECK(registry.lookup(ShortcutContext::Plater, ctrl_c) == Shortcut::Copy);
    CHECK(registry.lookup(ShortcutContext::ObjectList, ctrl_c) == Shortcut::Copy);
    CHECK_FALSE(registry.lookup(ShortcutContext::Global, ctrl_c).has_value());

    CHECK(registry.lookup(ShortcutContext::Plater, a) == Shortcut::Arrange);
    CHECK_FALSE(registry.lookup(ShortcutContext::Preview, a).has_value());

    CHECK(registry.lookup(ShortcutContext::Plater, c) == Shortcut::GizmoCut);
    CHECK(registry.lookup(ShortcutContext::Preview, c) == Shortcut::ToggleGcodeWindow);
    CHECK(registry.lookup(ShortcutContext::Painting, c) == Shortcut::PaintToolCircle);
}

TEST_CASE("Stepping shortcuts match with Shift or Ctrl added to their binding", "[Shortcuts]")
{
    ShortcutRegistry registry;
    using Match = ShortcutRegistry::Match;
    auto same = [](const std::optional<Match>& match, Shortcut shortcut, int step_modifiers) {
        return match.has_value() && match->shortcut == shortcut && match->step_modifiers == step_modifiers;
    };
    CHECK(same(registry.match(ShortcutContext::Preview, { WXK_UP }), Shortcut::LayerSliderUp, 0));
    CHECK(same(registry.match(ShortcutContext::Preview, { WXK_UP, wxMOD_SHIFT }), Shortcut::LayerSliderUp, wxMOD_SHIFT));
    CHECK(same(registry.match(ShortcutContext::Plater, { WXK_LEFT, wxMOD_CONTROL | wxMOD_SHIFT }), Shortcut::MoveSelectionLeft, wxMOD_CONTROL | wxMOD_SHIFT));
    CHECK(same(registry.match(ShortcutContext::Plater, { 'A', wxMOD_SHIFT }), Shortcut::ArrangePlate, 0));   // an exact binding wins
    CHECK_FALSE(registry.match(ShortcutContext::Plater, { 'Q', wxMOD_CONTROL }).has_value());              // Orient has no variants
    CHECK_FALSE(registry.match(ShortcutContext::Preview, { WXK_UP, wxMOD_ALT }).has_value());               // Alt is not a step modifier

    CHECK_FALSE(registry.match(ShortcutContext::Preview, { WXK_HOME, wxMOD_SHIFT }).has_value());            // Home has no variants

    // A binding with Shift or Ctrl of its own has no steps.
    registry.bind(Shortcut::LayerSliderUp, { WXK_UP, wxMOD_CONTROL });
    CHECK(same(registry.match(ShortcutContext::Preview, { WXK_UP, wxMOD_CONTROL }), Shortcut::LayerSliderUp, 0));
    CHECK_FALSE(registry.match(ShortcutContext::Preview, { WXK_UP, wxMOD_CONTROL | wxMOD_SHIFT }).has_value());
    CHECK_FALSE(registry.match(ShortcutContext::Preview, { WXK_UP, wxMOD_SHIFT }).has_value());

    // An exact binding on the combined step wins over it.
    registry.bind(Shortcut::Arrange, { WXK_LEFT, wxMOD_CONTROL | wxMOD_SHIFT });
    CHECK(same(registry.match(ShortcutContext::Plater, { WXK_LEFT, wxMOD_CONTROL | wxMOD_SHIFT }), Shortcut::Arrange, 0));
    CHECK(same(registry.match(ShortcutContext::Plater, { WXK_LEFT, wxMOD_SHIFT }), Shortcut::MoveSelectionLeft, wxMOD_SHIFT));
}

TEST_CASE("Conflicts cover shared contexts and every Global shortcut", "[Shortcuts]")
{
    ShortcutRegistry registry;
    CHECK(registry.conflicts(Shortcut::Arrange, { 'N', wxMOD_CONTROL }) == std::vector<Shortcut>{ Shortcut::NewProject });
    CHECK(registry.conflicts(Shortcut::NewProject, { 'A' }) == std::vector<Shortcut>{ Shortcut::Arrange });
    CHECK(registry.conflicts(Shortcut::ToggleGcodeWindow, { 'C' }).empty());
    CHECK(registry.conflicts(Shortcut::ZoomIn, { 'C' }) == std::vector<Shortcut>{ Shortcut::GizmoCut, Shortcut::ToggleGcodeWindow });
    CHECK(registry.conflicts(Shortcut::Arrange, { 'A' }).empty());   // a shortcut never conflicts with itself

    // Only the exact chord conflicts; the steps of a stepping shortcut are reserved instead.
    CHECK(registry.conflicts(Shortcut::GoToLayer, { WXK_UP, wxMOD_SHIFT }).empty());
    CHECK(registry.conflicts(Shortcut::LayerSliderUp, { 'G', wxMOD_SHIFT }) == std::vector<Shortcut>{ Shortcut::GoToLayer });
}

TEST_CASE("Shift and Ctrl with a stepping shortcut's key are reserved for its steps", "[Shortcuts]")
{
    ShortcutRegistry registry;
    CHECK(registry.step_owner(Shortcut::GoToLayer, { WXK_UP, wxMOD_SHIFT }) == Shortcut::LayerSliderUp);
    CHECK(registry.step_owner(Shortcut::NewProject, { WXK_LEFT, wxMOD_CONTROL }) == Shortcut::MoveSelectionLeft);   // Global shares every context
    CHECK_FALSE(registry.step_owner(Shortcut::GoToLayer, { WXK_UP, wxMOD_CONTROL | wxMOD_SHIFT }).has_value());     // the combined step is free
    CHECK_FALSE(registry.step_owner(Shortcut::MoveSelectionLeft, { WXK_LEFT, wxMOD_SHIFT }).has_value());          // its own step
    CHECK_FALSE(registry.step_owner(Shortcut::PaintToolCircle, { WXK_UP, wxMOD_SHIFT }).has_value());              // Painting shares no context
    registry.bind(Shortcut::LayerSliderUp, { WXK_UP, wxMOD_CONTROL });
    CHECK_FALSE(registry.step_owner(Shortcut::GoToLayer, { WXK_UP, wxMOD_CONTROL | wxMOD_SHIFT }).has_value());    // a modified binding has no steps
}

TEST_CASE("Custom bindings replace the default and survive a config round trip", "[Shortcuts]")
{
    ShortcutRegistry registry;
    const KeyChord   w{ 'W' };
    registry.bind(Shortcut::Arrange, w);

    CHECK(registry.is_customized(Shortcut::Arrange));
    CHECK(registry.lookup(ShortcutContext::Plater, w) == Shortcut::Arrange);
    CHECK_FALSE(registry.lookup(ShortcutContext::Plater, { 'A' }).has_value());

    AppConfig config;
    registry.save(config);
    CHECK(config.get("shortcuts", "arrange") == "W");
    CHECK_FALSE(config.has("shortcuts", "orient"));

    ShortcutRegistry loaded;
    loaded.load(config);
    CHECK(loaded.lookup(ShortcutContext::Plater, w) == Shortcut::Arrange);
    CHECK(loaded.binding(Shortcut::Orient) == KeyChord{ 'Q' });

    SECTION("rebinding to the default clears the override")
    {
        registry.bind(Shortcut::Arrange, { 'A' });
        CHECK_FALSE(registry.is_customized(Shortcut::Arrange));
        registry.save(config);
        CHECK_FALSE(config.has("shortcuts", "arrange"));
    }
    SECTION("an invalid chord unbinds and persists as none")
    {
        registry.bind(Shortcut::Arrange, KeyChord{});
        CHECK_FALSE(registry.binding(Shortcut::Arrange).valid());
        registry.save(config);
        CHECK(config.get("shortcuts", "arrange") == "none");
        loaded.load(config);
        CHECK_FALSE(loaded.lookup(ShortcutContext::Plater, { 'A' }).has_value());
        CHECK_FALSE(loaded.lookup(ShortcutContext::Plater, w).has_value());
    }
    SECTION("reset_all restores every default")
    {
        registry.reset_all();
        CHECK(registry.lookup(ShortcutContext::Plater, { 'A' }) == Shortcut::Arrange);
        CHECK_FALSE(registry.is_customized(Shortcut::Arrange));
    }
}

TEST_CASE("A Global shortcut refuses a config binding that would swallow typing", "[Shortcuts]")
{
    AppConfig config;
    // A string literal would pick AppConfig::set's bool overload.
    config.set("shortcuts", "save_project", std::string("S"));
    config.set("shortcuts", "new_project", std::string("F9"));
    ShortcutRegistry registry;
    registry.load(config);
    CHECK(registry.binding(Shortcut::SaveProject) == KeyChord{ 'S', wxMOD_CONTROL });
    CHECK(registry.binding(Shortcut::NewProject) == KeyChord{ WXK_F9 });
}

TEST_CASE("Unreadable config entries fall back to the default binding", "[Shortcuts]")
{
    AppConfig config;
    config.set("shortcuts", "arrange", std::string("Hyper+Q"));
    config.set("shortcuts", "no_such_shortcut", std::string("Ctrl+Q"));

    ShortcutRegistry registry;
    registry.load(config);
    CHECK_FALSE(registry.is_customized(Shortcut::Arrange));
    CHECK(registry.lookup(ShortcutContext::Plater, { 'A' }) == Shortcut::Arrange);
}
