#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Mcp/UiRefs.hpp"
#include "slic3r/GUI/SyntheticInput.hpp"

#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Mcp;
using nlohmann::json;

namespace {

struct FakeTarget
{
    std::string name;
    json        locator;
};

UiNode node(const std::string& ref, const std::string& kind, const std::string& label, int depth)
{
    UiNode n;
    n.ref   = ref;
    n.kind  = kind;
    n.label = label;
    n.depth = depth;
    return n;
}

} // namespace

TEST_CASE("clean_imgui_label keeps the shown part of an ImGui label", "[Mcp]")
{
    CHECK(clean_imgui_label("Move##gizmo") == "Move");
    CHECK(clean_imgui_label("###hidden") == "");
    CHECK(clean_imgui_label("Plain") == "Plain");
    CHECK(clean_imgui_label("\x01" "Colored\x02##x") == "Colored");
}

TEST_CASE("snapshot text shows depth, ref, kind, label, class and state", "[Mcp]")
{
    UiNode button = node("w2", "button", "Slice plate", 1);
    button.cls    = "Button";
    UiNode check  = node("w3", "checkbox", "Auto arrange", 1);
    check.cls     = "wxCheckBox";
    check.checked = 1;
    check.enabled = false;
    UiNode text   = node("w4", "text", "", 2);
    text.cls      = "TextInput";
    text.value    = "0.2";
    text.focused  = true;
    text.x = 10, text.y = 20, text.w = 80, text.h = 24;
    const std::string out = format_snapshot({node("w1", "window", "OrcaSlicer", 0), button, check, text}, 0);
    CHECK(out ==
          "[w1] window \"OrcaSlicer\"\n"
          "  [w2] button \"Slice plate\" (Button)\n"
          "  [w3] checkbox \"Auto arrange\" (wxCheckBox) [checked] [disabled]\n"
          "    [w4] text = \"0.2\" (TextInput) [focused] @10,20 80x24\n");
}

TEST_CASE("a truncated snapshot ends with a marker naming how much was left out", "[Mcp]")
{
    const std::string out = format_snapshot({node("w1", "window", "Main", 0)}, 37);
    CHECK(out.find("... 37 more nodes") != std::string::npos);
}

TEST_CASE("refs are never reused and resolve to their payload while alive", "[Mcp]")
{
    RefTable<FakeTarget> refs;
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i)
        CHECK(seen.insert(refs.add('w', 1, FakeTarget{"t" + std::to_string(i), {}}, [] { return true; })).second);
    std::string err;
    const std::string ref = refs.add('m', 1, FakeTarget{"menu", {{"menu", "File>Open"}}}, [] { return true; });
    CHECK(ref.front() == 'm');
    const FakeTarget* t = refs.resolve(ref, 1, err);
    REQUIRE(t != nullptr);
    CHECK(t->name == "menu");
    CHECK(t->locator == json{{"menu", "File>Open"}});
    CHECK(refs.resolve("w999999", 1, err) == nullptr);
    CHECK(err.find("Unknown ref") != std::string::npos);
}

TEST_CASE("a ref whose element is gone is stale", "[Mcp]")
{
    RefTable<FakeTarget> refs;
    bool              alive = true;
    const std::string ref   = refs.add('w', 1, FakeTarget{"button", {}}, [&] { return alive; });
    std::string       err;
    CHECK(refs.resolve(ref, 1, err) != nullptr);
    alive = false;
    CHECK(refs.resolve(ref, 1, err) == nullptr);
    CHECK(err.find("Stale ref") != std::string::npos);
}

TEST_CASE("a ref from before a UI rebuild is stale", "[Mcp]")
{
    RefTable<FakeTarget> refs;
    const std::string ref = refs.add('w', 3, FakeTarget{"button", {}}, [] { return true; });
    std::string       err;
    CHECK(refs.resolve(ref, 3, err) != nullptr);
    CHECK(refs.resolve(ref, 4, err) == nullptr);
    CHECK(err.find("Stale ref") != std::string::npos);
}

TEST_CASE("a menu ref is stale when its label path changed", "[Mcp]")
{
    // Menu refs remember the label path they were listed under; a rebuilt menu (another language,
    // another layout) with the same id under a different path no longer matches.
    RefTable<FakeTarget> refs;
    std::string current_path = "File>Export>Export G-code";
    const std::string taken = current_path;
    const std::string ref   = refs.add('m', 1, FakeTarget{"export", {{"menu", taken}}}, [&] { return current_path == taken; });
    std::string err;
    CHECK(refs.resolve(ref, 1, err) != nullptr);
    current_path = "Datei>Exportieren>G-Code exportieren";
    CHECK(refs.resolve(ref, 1, err) == nullptr);
}

TEST_CASE("the ref table forgets the oldest refs beyond its cap", "[Mcp]")
{
    RefTable<FakeTarget> refs;
    const std::string first = refs.add('w', 1, FakeTarget{}, [] { return true; });
    for (std::size_t i = 0; i < RefTable<FakeTarget>::kMaxRefs; ++i)
        refs.add('w', 1, FakeTarget{}, [] { return true; });
    std::string err;
    CHECK(refs.resolve(first, 1, err) == nullptr);
    CHECK(err.find("Unknown ref") != std::string::npos);
}

TEST_CASE("synthetic key state is visible through get_key_state and cleared after a gesture", "[Mcp]")
{
    SyntheticInput::press_key(WXK_SHIFT);
    SyntheticInput::press_key(WXK_CONTROL);
    CHECK(SyntheticInput::is_key_down(WXK_SHIFT));
    CHECK(get_key_state(WXK_SHIFT));
#ifndef __APPLE__
    // Elsewhere Control and raw control are the same key.
    CHECK(SyntheticInput::is_key_down(WXK_RAW_CONTROL));
#endif
    SyntheticInput::release_key(WXK_SHIFT);
    CHECK_FALSE(SyntheticInput::is_key_down(WXK_SHIFT));
    CHECK(SyntheticInput::is_key_down(WXK_CONTROL));
    SyntheticInput::clear();
    CHECK_FALSE(SyntheticInput::is_key_down(WXK_CONTROL));
}
