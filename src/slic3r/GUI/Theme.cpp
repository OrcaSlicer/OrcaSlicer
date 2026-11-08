#include "Theme.hpp"
#include "AccentColor.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "GLCanvas3D.hpp"
#include "PartPlate.hpp"
#include "3DBed.hpp"
#include "GLToolbar.hpp"
#include "FocusRing.hpp"
#include "ImGuiWrapper.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

// ---------------------------------------------------------------------------------------------
// Theme

const std::vector<ThemeRole>& Theme::roles()
{
    static const std::vector<ThemeRole> roles = {
        { "window_background", "#FFFFFF", L("Window background") },
        { "panel_title_strips", "#F8F8F8", L("Panel title strips") },
        { "tab_bar",            "#3B4446", L("Tab bar") },
        { "top_bar",            "#262E31", L("Top bar") },
        { "selected_tab",       "#CECECE", L("Selected tab") },
        { "buttons",            "#DFDFDF", L("Buttons") },
        { "input_borders",      "#DBDBDB", L("Input borders") },
        { "input_background",   "#FFFEFF", L("Input background") },
        { "labels",             "#363636", L("Labels") },
        { "button_input_text",  "#262E30", L("Button and input text") },
        { "disabled_text",      "#6B6B6B", L("Disabled text") },
        { "separators",         "#EEEEEE", L("Separators") },
        { "accent",             "#767676", L("Accent") },
    };
    return roles;
}

const ThemeRole* Theme::role_by_key(const std::string& key)
{
    for (const ThemeRole& role : roles())
        if (key == role.key)
            return &role;
    return nullptr;
}

std::string Theme::slug(const std::string& name)
{
    std::string out;
    for (unsigned char c : name)
        out += std::isalnum(c) ? char(std::tolower(c)) : '-';
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    if (out.empty()) { // nothing ASCII in the name: a stable file name from the name itself
        std::ostringstream h;
        h << "theme-" << std::hex << std::hash<std::string>{}(name);
        return h.str();
    }
    return out;
}

std::string Theme::to_json() const
{
    json j;
    j["version"]            = 1;
    j["name"]               = name;
    j["base"]               = dark ? "dark" : "light";
    j["colors"]             = json::object();
    for (const auto& [id, value] : colors)
        if (!value.empty())
            j["colors"][id] = value;
    if (!view_background.empty()) j["view_background"] = view_background;
    if (!plate_grid.empty())      j["plate_grid"]      = plate_grid;
    if (!plate_model.empty())     j["plate_model"]     = plate_model;
    if (!toolbar.empty())         j["toolbar"]         = toolbar;
    if (!label_sys.empty())       j["label_sys"]       = label_sys;
    if (!label_modified.empty())  j["label_modified"]  = label_modified;
    if (!focus_ring.empty())      j["focus_ring"]      = focus_ring;
    j["auto_contrast_text"] = auto_contrast_text;
    return j.dump(2);
}

std::optional<Theme> Theme::from_json(const std::string& text)
{
    try {
        const json j = json::parse(text);
        if (const int version = j.value("version", 1); version > 1) {
            BOOST_LOG_TRIVIAL(warning) << "Theme: file version " << version << " is newer than this build reads (1)";
            return std::nullopt;
        }
        // A colour field is "#RRGGBB" or absent; anything else is treated as absent.
        auto colour = [&](const char* key, const char* fallback_key = nullptr) -> std::string {
            std::string v = j.value(key, "");
            if (v.empty() && fallback_key)
                v = j.value(fallback_key, "");
            return wxColour(v).IsOk() ? v : std::string();
        };
        Theme t;
        t.name = j.value("name", "");
        t.dark = j.value("base", "light") == "dark";
        if (j.contains("colors") && j["colors"].is_object())
            for (const auto& [id, value] : j["colors"].items())
                if (value.is_string() && wxColour(value.get<std::string>()).IsOk())
                    t.colors[id] = value.get<std::string>();
        t.view_background    = colour("view_background");
        // "plate" (files before the split) coloured the grid lines in effect: the face it
        // named is hidden by the bed model on the current plate.
        t.plate_grid         = colour("plate_grid", "plate");
        t.plate_model        = colour("plate_model");
        t.toolbar            = colour("toolbar");
        t.label_sys          = colour("label_sys");
        t.label_modified     = colour("label_modified");
        t.focus_ring         = colour("focus_ring");
        t.auto_contrast_text = j.value("auto_contrast_text", false);
        if (t.name.empty())
            return std::nullopt;
        return t;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Theme: cannot parse: " << e.what();
        return std::nullopt;
    }
}

std::optional<Theme> Theme::from_file(const std::string& path)
{
    boost::nowide::ifstream in(path.c_str(), std::ios::binary);
    if (!in)
        return std::nullopt;
    std::stringstream ss; ss << in.rdbuf();
    return from_json(ss.str());
}

bool Theme::same_values(const Theme& o) const
{
    return dark == o.dark && colors == o.colors && view_background == o.view_background && plate_grid == o.plate_grid &&
           plate_model == o.plate_model && toolbar == o.toolbar &&
           label_sys == o.label_sys && label_modified == o.label_modified && focus_ring == o.focus_ring &&
           auto_contrast_text == o.auto_contrast_text;
}

bool Theme::empty_overrides() const
{
    return colors.empty() && view_background.empty() && plate_grid.empty() && plate_model.empty() && toolbar.empty() && label_sys.empty() && label_modified.empty() &&
           focus_ring.empty() && !auto_contrast_text;
}

// ---------------------------------------------------------------------------------------------
// ThemeManager

static const char* DEFAULT_FOCUS_RING = "#3B8BF5";

ThemeManager& ThemeManager::get()
{
    static ThemeManager manager;
    return manager;
}

std::string ThemeManager::themes_dir() const
{
    return (boost::filesystem::path(data_dir()) / "themes").string();
}

std::string ThemeManager::file_for(const std::string& dir, const std::string& name)
{
    return (boost::filesystem::path(dir) / (Theme::slug(name) + ".json")).string();
}

std::string ThemeManager::slot_name(bool dark) const
{
    return wxGetApp().app_config->get(dark ? "theme_dark" : "theme_light");
}

const Theme* ThemeManager::find(const std::string& name) const
{
    for (const Theme& t : m_themes)
        if (t.name == name)
            return &t;
    return nullptr;
}

bool ThemeManager::write_theme(const Theme& theme, const std::string& path) const
{
    boost::system::error_code ec;
    boost::filesystem::create_directories(themes_dir(), ec);
    boost::nowide::ofstream out(path.c_str(), std::ios::binary);
    if (!out) {
        BOOST_LOG_TRIVIAL(error) << "Theme: cannot write " << path;
        return false;
    }
    out << theme.to_json();
    out.close(); // the small file may sit in the buffer until here; a full disk shows up on close
    if (out.fail()) {
        BOOST_LOG_TRIVIAL(error) << "Theme: cannot write " << path;
        return false;
    }
    return true;
}

void ThemeManager::load()
{
    m_themes.clear();
    Theme light; light.name = "Light"; light.dark = false; light.builtin = true;
    Theme dark;  dark.name  = "Dark";  dark.dark  = true;  dark.builtin  = true;
    m_themes.push_back(light);
    m_themes.push_back(dark);

    boost::system::error_code ec;
    // Presets shipped with the application, read-only like the built-ins.
    const boost::filesystem::path bundled_dir = boost::filesystem::path(resources_dir()) / "themes";
    if (boost::filesystem::is_directory(bundled_dir, ec)) {
        std::vector<Theme> bundled;
        for (const auto& entry : boost::filesystem::directory_iterator(bundled_dir, ec))
            if (entry.path().extension() == ".json")
                if (auto t = Theme::from_file(entry.path().string()); t && !find(t->name)) {
                    t->bundled = true;
                    bundled.push_back(*t);
                }
        // The bundled presets' names are shown translated (Preferences); markers for the catalog.
        static const char* const bundled_names[] = { L("High contrast light"), L("High contrast dark"), L("Neon") };
        (void) bundled_names;
        std::sort(bundled.begin(), bundled.end(), [](const Theme& a, const Theme& b) { return a.name < b.name; });
        m_themes.insert(m_themes.end(), bundled.begin(), bundled.end());
    }
    const boost::filesystem::path dir(themes_dir());
    if (boost::filesystem::is_directory(dir, ec)) {
        std::vector<Theme> user;
        for (const auto& entry : boost::filesystem::directory_iterator(dir, ec)) {
            const std::string fname = entry.path().filename().string();
            if (entry.path().extension() != ".json" || fname.rfind(".unsaved-", 0) == 0)
                continue;
            if (auto t = Theme::from_file(entry.path().string()); t && !find(t->name))
                user.push_back(*t);
        }
        std::sort(user.begin(), user.end(), [](const Theme& a, const Theme& b) { return a.name < b.name; });
        m_themes.insert(m_themes.end(), user.begin(), user.end());
    }

    migrate_old_config();
    load_working_for(wxGetApp().dark_mode());
}

// The colours used to live as loose app-config keys (one set per base); fold them into a
// user theme per base so nothing the user set is lost, then drop the keys.
void ThemeManager::migrate_old_config()
{
    AppConfig& cfg = *wxGetApp().app_config;
    auto key_for = [](bool dark, const std::string& light_key) { return std::string(dark ? "theme_dark_" : "theme_light_") + light_key.substr(1); };
    const std::string label_sys      = cfg.get("label_clr_sys");
    const std::string label_modified = cfg.get("label_clr_modified");
    const std::string focus          = cfg.get("focus_ring_color");
    const bool        auto_contrast  = cfg.get_bool("auto_contrast_text");
    const bool shared = !label_sys.empty() || !label_modified.empty() || (!focus.empty() && focus != DEFAULT_FOCUS_RING) || auto_contrast;

    bool migrated = false, failed = false;
    for (bool dark : { false, true }) {
        Theme t;
        t.name = dark ? "Custom dark" : "Custom light";
        t.dark = dark;
        for (const ThemeRole& role : Theme::roles()) {
            const std::string v = cfg.get(key_for(dark, role.key));
            if (!v.empty())
                t.colors[role.id] = v;
        }
        t.view_background = cfg.get(std::string("view_bg_color_") + (dark ? "dark" : "light"));
        t.plate_grid      = cfg.get(std::string("plate_color_") + (dark ? "dark" : "light"));
        const bool own = !t.colors.empty() || !t.view_background.empty() || !t.plate_grid.empty();
        if (!own && !shared) // the shared values (labels, focus ring) go to both bases
            continue;
        t.label_sys          = label_sys;
        t.label_modified     = label_modified;
        t.focus_ring         = (focus.empty() || focus == DEFAULT_FOCUS_RING) ? "" : focus;
        t.auto_contrast_text = auto_contrast;
        while (find(t.name) || find_by_file(t.name))
            t.name += " (2)";
        if (write_theme(t, file_for(themes_dir(), t.name))) {
            m_themes.push_back(t);
            cfg.set(dark ? "theme_dark" : "theme_light", t.name);
            migrated = true;
        } else {
            failed = true;
            BOOST_LOG_TRIVIAL(error) << "Theme: could not write " << t.name << "; the old colour keys are kept for the next start";
        }
    }
    // All or nothing: the keys are the only copy until every theme is on disk.
    if (!migrated || failed)
        return;
    for (bool dark : { false, true }) {
        for (const ThemeRole& role : Theme::roles())
            cfg.erase("app", key_for(dark, role.key));
        cfg.erase("app", key_for(dark, "#F1F1F1"));
        cfg.erase("app", std::string("view_bg_color_") + (dark ? "dark" : "light"));
        cfg.erase("app", std::string("plate_color_") + (dark ? "dark" : "light"));
    }
    for (const char* key : { "label_clr_sys", "label_clr_modified", "focus_ring_color", "auto_contrast_text" })
        cfg.erase("app", key);
    cfg.save();
    BOOST_LOG_TRIVIAL(info) << "Theme: migrated the app-config colour keys into user themes";
}

void ThemeManager::load_working_for(bool dark)
{
    // An unsaved working copy survives a restart.
    const std::string unsaved = (boost::filesystem::path(themes_dir()) / (std::string(".unsaved-") + (dark ? "dark" : "light") + ".json")).string();
    boost::system::error_code ec;
    if (boost::filesystem::exists(unsaved, ec)) {
        boost::nowide::ifstream in(unsaved.c_str(), std::ios::binary);
        std::stringstream ss; ss << in.rdbuf();
        if (auto t = Theme::from_json(ss.str()); t && t->dark == dark) {
            try {
                m_saved_name = json::parse(ss.str()).value("derived_from", t->name);
            } catch (...) { m_saved_name = t->name; }
            m_working  = *t;
            m_working.builtin = false;
            m_modified = true;
            return;
        }
    }
    const Theme* chosen = find(slot_name(dark));
    if (chosen == nullptr || chosen->dark != dark)
        chosen = find(dark ? "Dark" : "Light");
    m_working    = *chosen;
    m_saved_name = chosen->name;
    m_modified   = false;
}

void ThemeManager::on_base_changed()
{
    const bool dark = wxGetApp().dark_mode();
    if (m_working.dark != dark) {
        load_working_for(dark);
        ++m_generation;
    }
    apply_to_state();
}

// Table colours that are a role's colour on another surface: the second row of the title
// strip gradient, the settings tab strip and the Device page (window background), the checked
// row of a dropdown and the selected settings tab (selected tab). They take the role's value.
static const std::vector<std::pair<const char*, const char*>>& role_aliases()
{
    static const std::vector<std::pair<const char*, const char*>> aliases = {
        { "#F8F8F8", "#F1F1F1" },
        { "#FFFFFF", "#FEFFFF" },
        { "#CECECE", "#D8D8D8" },
    };
    return aliases;
}

void ThemeManager::apply_to_state()
{
    const bool dark = m_working.dark;
    StateColor::ClearThemeOverrides();
    // Every role must end with a colour of its own: the live preview tells roles apart by
    // colour, so a window colour equal to the label colour would recolour the labels with it.
    // A value equal to what another role resolves to (its override, else its base colour) is
    // nudged by one unit per channel; the base colours are the keys on the light base and the
    // table values on the dark base.
    const auto& table = StateColor::GetDarkMap();
    auto base_colour = [&](const ThemeRole& r) {
        const wxColour key(r.key);
        if (!dark) return key;
        auto it = table.find(key);
        return it != table.end() ? it->second : key;
    };
    std::map<std::string, wxColour> applied; // role id -> value applied so far
    for (const ThemeRole& role : Theme::roles()) {
        auto it = m_working.colors.find(role.id);
        if (it == m_working.colors.end() || it->second.empty())
            continue;
        const wxColour chosen(it->second);
        wxColour value = StateColor::AvoidTableCollision(chosen, dark);
        // On the light base the keys are what the override lookup matches: a label painted in a
        // colour equal to another role's key would be re-resolved through that role.
        // Taken: another role's applied value, its base colour (the construction-time pass
        // matches on those), and on the light base its key (the override lookup matches on
        // those), aliases included.
        auto taken = [&](const wxColour& c) {
            for (const ThemeRole& other : Theme::roles()) {
                if (other.id == role.id) continue;
                if (auto ap = applied.find(other.id); ap != applied.end() && ap->second == c)
                    return true;
                if (base_colour(other) == c)
                    return true;
                if (!dark && wxColour(other.key) == c)
                    return true;
            }
            for (const auto& [owner_key, alias_key] : role_aliases()) { // other roles' aliases: base colour and applied value
                if (std::string(role.key) == owner_key) continue;
                const wxColour alias(alias_key);
                auto it = table.find(alias);
                if (c == (dark && it != table.end() ? it->second : alias))
                    return true;
                if (const ThemeRole* owner = Theme::role_by_key(owner_key))
                    if (auto ap = applied.find(owner->id); ap != applied.end() && ap->second == c)
                        return true;
                if (auto ap = applied.find(std::string("alias:") + alias_key); ap != applied.end() && ap->second == c)
                    return true;
            }
            return false;
        };
        for (int i = 1; i <= 16 && taken(value); ++i) {
            // i units from the chosen colour, then clear of the table: a fixed one-unit step
            // could be undone by the table check and cycle
            auto step = [i](unsigned char v) { return (unsigned char) (v < 128 ? std::min(255, v + i) : std::max(0, v - i)); };
            value = StateColor::AvoidTableCollision(wxColour(step(chosen.Red()), step(chosen.Green()), step(chosen.Blue())), dark);
        }
        applied[role.id] = value;
        StateColor::SetThemeOverride(dark, wxColour(role.key), value);
        // An alias takes the role's colour one unit away from it: the live preview matches
        // windows by colour, and a shared value would send an alias surface back to the role's
        // original instead of its own when the theme is switched back.
        for (const auto& [owner_key, alias_key] : role_aliases())
            if (std::string(role.key) == owner_key) {
                wxColour alias_value = value;
                for (int i = 1; i <= 16; ++i) {
                    auto step = [i](unsigned char v) { return (unsigned char) (v < 128 ? std::min(255, v + i) : std::max(0, v - i)); };
                    alias_value = StateColor::AvoidTableCollision(wxColour(step(value.Red()), step(value.Green()), step(value.Blue())), dark);
                    if (alias_value != value && !taken(alias_value))
                        break;
                }
                applied[std::string("alias:") + alias_key] = alias_value;
                StateColor::SetThemeOverride(dark, wxColour(alias_key), alias_value);
            }
    }
    // Accent: the interaction ramp (hover lighter, pressed darker; the dark base lifts hover
    // further, as the built-in ramp does) is derived from the chosen colour, so a themed accent
    // keeps its states. The ImGui twins follow it; with no accent set they keep the shipped values.
    if (auto ap = applied.find("accent"); ap != applied.end()) {
        const wxColour a = ap->second;
        auto shifted = [&](int d) {
            auto s = [d](unsigned char v) { return (unsigned char) std::clamp(int(v) + d, 0, 255); };
            return StateColor::AvoidTableCollision(wxColour(s(a.Red()), s(a.Green()), s(a.Blue())), dark);
        };
        const bool     light_accent = (a.Red() + a.Green() + a.Blue()) / 3 > 160;
        const wxColour hover        = shifted(light_accent ? -18 : (dark ? 50 : 18));
        const wxColour pressed      = shifted(light_accent ? -40 : -26);
        StateColor::SetThemeOverride(dark, wxColour(Accent::Hover), hover);
        StateColor::SetThemeOverride(dark, wxColour(Accent::HoverDark), hover);
        StateColor::SetThemeOverride(dark, wxColour(Accent::Pressed), pressed);
        auto im = [](const wxColour& c) { return ImVec4(c.Red() / 255.f, c.Green() / 255.f, c.Blue() / 255.f, 1.f); };
        ImGuiWrapper::COL_ORCA = ImGuiWrapper::COL_ORCA_DARK = im(a);
        ImGuiWrapper::COL_ORCA_HOVER = ImGuiWrapper::COL_ORCA_HOVER_DARK = im(hover);
    } else {
        ImGuiWrapper::COL_ORCA            = ImGuiWrapper::COL_ORCA_DEFAULT;
        ImGuiWrapper::COL_ORCA_DARK       = ImGuiWrapper::COL_ORCA_DARK_DEFAULT;
        ImGuiWrapper::COL_ORCA_HOVER      = ImGuiWrapper::COL_ORCA_HOVER_DEFAULT;
        ImGuiWrapper::COL_ORCA_HOVER_DARK = ImGuiWrapper::COL_ORCA_HOVER_DARK_DEFAULT;
    }
    if (m_working.auto_contrast_text) {
        // Text roles and the backgrounds (theme keys) they sit on. A text colour the theme did
        // not set is pushed light or dark until it reads at 4.5:1 on each of them.
        static const std::vector<std::pair<const char*, std::vector<const char*>>> pairs = {
            { "#363636", { "#FFFFFF", "#CECECE", "#F8F8F8" } },
            { "#323A3D", { "#FFFFFF" } },
            { "#303A3C", { "#FFFFFF" } },
            { "#262E30", { "#DFDFDF", "#FFFEFF" } },
            { "#6B6A6A", { "#DFDFDF" } },
            { "#6B6B6A", { "#FFFFFF" } },
            { "#6B6B6B", { "#FFFFFF" } },
            { "#FEFEFE", { "#767676" } }, // Slice / Print and the selected main tab: white on the accent
        };
        for (const auto& [text_key, bg_keys] : pairs) {
            if (const ThemeRole* role = Theme::role_by_key(text_key); role && m_working.colors.count(role->id))
                continue;
            const wxColour text = StateColor::darkModeColorFor(wxColour(text_key));
            wxColour adjusted = text;
            for (int pass = 0; pass < 2; ++pass)
                for (const char* bg_key : bg_keys)
                    adjusted = StateColor::EnsureContrast(adjusted, StateColor::darkModeColorFor(wxColour(bg_key)), 4.5);
            if (adjusted != text)
                StateColor::SetThemeOverride(dark, wxColour(text_key), StateColor::AvoidTableCollision(adjusted, dark));
        }
    }
    auto to_rgba = [](const std::string& hex) -> std::optional<ColorRGBA> {
        if (hex.empty()) return std::nullopt;
        const wxColour c(hex);
        return ColorRGBA(c.Red() / 255.f, c.Green() / 255.f, c.Blue() / 255.f, 1.f);
    };
    GLCanvas3D::set_background_color_override(dark, to_rgba(m_working.view_background));
    PartPlate::set_grid_color_override(dark, to_rgba(m_working.plate_grid));
    Bed3D::set_model_color_override(dark, to_rgba(m_working.plate_model));
    GLToolbar::set_background_color_override(dark, to_rgba(m_working.toolbar));
    // The ImGui toolbar windows (the band across the top of the view) do not consult the
    // GLToolbar override: mirror the same colour for the base in use.
    {
        const auto   toolbar = to_rgba(m_working.toolbar);
        const ImVec4 value   = toolbar ? ImVec4(toolbar->r(), toolbar->g(), toolbar->b(), 1.f)
                                       : (dark ? ImGuiWrapper::COL_TOOLBAR_BG_DARK_DEFAULT : ImGuiWrapper::COL_TOOLBAR_BG_DEFAULT);
        (dark ? ImGuiWrapper::COL_TOOLBAR_BG_DARK : ImGuiWrapper::COL_TOOLBAR_BG) = value;
    }
    FocusRing::get().set_color(wxColour(m_working.focus_ring.empty() ? DEFAULT_FOCUS_RING : m_working.focus_ring.c_str()));
    // ImGui does not consult StateColor: mirror the window background for its dark style.
    const wxColour dark_window = dark ? StateColor::darkModeColorFor(wxColour("#FFFFFF")) : wxColour("#232327");
    ImGuiWrapper::COL_WINDOW_BG_DARK = ImVec4(dark_window.Red() / 255.f, dark_window.Green() / 255.f, dark_window.Blue() / 255.f, 1.f);
    // The translucent overlay windows (legend, tool position, plate labels, tooltips) follow the
    // window colour on the dark base when the theme sets one; the light base keeps the dark
    // overlay it always had. init_style() bakes the value into the ImGui style.
    {
        const bool themed = dark && m_working.colors.count("window_background") && !m_working.colors.at("window_background").empty();
        ImGuiWrapper::COL_WINDOW_BACKGROUND_THEMED = themed;
        ImGuiWrapper::COL_WINDOW_BACKGROUND = themed
            ? ImVec4(dark_window.Red() / 255.f, dark_window.Green() / 255.f, dark_window.Blue() / 255.f, ImGuiWrapper::COL_WINDOW_BACKGROUND_DEFAULT.w)
            : ImGuiWrapper::COL_WINDOW_BACKGROUND_DEFAULT;
        if (ImGuiWrapper* imgui = wxGetApp().imgui())
            imgui->request_style_refresh();
    }
}

void ThemeManager::select(const std::string& name)
{
    const Theme* theme = find(name);
    if (theme == nullptr)
        return;
    AppConfig& cfg = *wxGetApp().app_config;
    cfg.set(theme->dark ? "theme_dark" : "theme_light", theme->builtin ? "" : theme->name);
    // Drop an unsaved copy of that base: the user chose something else.
    boost::system::error_code ec;
    boost::filesystem::remove(boost::filesystem::path(themes_dir()) / (std::string(".unsaved-") + (theme->dark ? "dark" : "light") + ".json"), ec);
    cfg.save();
    if (theme->dark != wxGetApp().dark_mode()) {
        // Picking a theme is an explicit choice, so it switches the colour scheme to that theme's base,
        // also from System. It used to wait for the desktop to change mode when System was selected,
        // which on every new install (System is the default) looked like the Theme dropdown did nothing.
        wxGetApp().apply_color_scheme(theme->dark ? "dark" : "light"); // re-themes through on_base_changed()
        return;
    }
    m_working    = *theme;
    m_saved_name = theme->name;
    m_modified   = false;
    refresh_live();
}

void ThemeManager::fork_if_builtin()
{
    if (!m_working.read_only())
        return;
    m_working.builtin = false;
    m_working.bundled = false;
    m_working.name    = m_working.name + " (modified)";
}

void ThemeManager::refresh_live()
{
    ++m_generation;
    GUI_App& app = wxGetApp();
    const auto before = app.resolved_theme_colors();
    apply_to_state();
    app.on_theme_changed(before);
}

void ThemeManager::persist_unsaved()
{
    m_modified = true;
    ++m_edit_generation;
    json j = json::parse(m_working.to_json());
    j["derived_from"] = m_saved_name;
    boost::system::error_code ec;
    boost::filesystem::create_directories(themes_dir(), ec);
    const std::string path = (boost::filesystem::path(themes_dir()) / (std::string(".unsaved-") + (m_working.dark ? "dark" : "light") + ".json")).string();
    boost::nowide::ofstream out(path.c_str(), std::ios::binary);
    out << j.dump(2);
}

void ThemeManager::set_role_color(const std::string& key, const wxColour& color)
{
    const ThemeRole* role = Theme::role_by_key(key);
    if (role == nullptr)
        return;
    fork_if_builtin();
    if (color.IsOk())
        m_working.colors[role->id] = color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    else
        m_working.colors.erase(role->id);
    persist_unsaved();
    refresh_live();
}

static std::string hex_or_empty(const wxColour& c) { return c.IsOk() ? c.GetAsString(wxC2S_HTML_SYNTAX).ToStdString() : std::string(); }

void ThemeManager::set_view_background(const wxColour& color) { fork_if_builtin(); m_working.view_background = hex_or_empty(color); persist_unsaved(); refresh_live(); }
void ThemeManager::set_plate_grid(const wxColour& color)      { fork_if_builtin(); m_working.plate_grid      = hex_or_empty(color); persist_unsaved(); refresh_live(); }
void ThemeManager::set_plate_model(const wxColour& color)     { fork_if_builtin(); m_working.plate_model     = hex_or_empty(color); persist_unsaved(); refresh_live(); }
void ThemeManager::set_toolbar(const wxColour& color)         { fork_if_builtin(); m_working.toolbar         = hex_or_empty(color); persist_unsaved(); refresh_live(); }
void ThemeManager::set_label_sys(const wxColour& color)       { fork_if_builtin(); m_working.label_sys       = hex_or_empty(color); persist_unsaved(); refresh_live(); }
void ThemeManager::set_label_modified(const wxColour& color)  { fork_if_builtin(); m_working.label_modified  = hex_or_empty(color); persist_unsaved(); refresh_live(); }
void ThemeManager::set_focus_ring(const wxColour& color)      { fork_if_builtin(); m_working.focus_ring      = hex_or_empty(color); persist_unsaved(); refresh_live(); }
void ThemeManager::set_auto_contrast_text(bool on)            { fork_if_builtin(); m_working.auto_contrast_text = on;               persist_unsaved(); refresh_live(); }

void ThemeManager::reset_working()
{
    if (m_working.builtin)
        return;
    const bool  dark = m_working.dark;
    const std::string name = m_working.name;
    m_working = *find(dark ? "Dark" : "Light");
    m_working.builtin = false;
    m_working.name    = name;
    persist_unsaved();
    refresh_live();
}

bool ThemeManager::save()
{
    if (m_working.read_only())
        return false;
    const Theme* saved = find(m_saved_name);
    if (saved == nullptr || saved->read_only())
        return false; // a forked built-in or preset needs a name: save_as()
    m_working.name = saved->name;
    if (!write_theme(m_working, file_for(themes_dir(), m_working.name)))
        return false;
    for (Theme& t : m_themes)
        if (t.name == m_working.name)
            t = m_working;
    m_modified = false;
    boost::system::error_code ec;
    boost::filesystem::remove(boost::filesystem::path(themes_dir()) / (std::string(".unsaved-") + (m_working.dark ? "dark" : "light") + ".json"), ec);
    return true;
}

const Theme* ThemeManager::find_by_file(const std::string& name) const
{
    const std::string file = Theme::slug(name);
    for (const Theme& t : m_themes)
        if (!t.builtin && Theme::slug(t.name) == file)
            return &t;
    return nullptr;
}

bool ThemeManager::save_as(const std::string& name)
{
    if (name.empty())
        return false;
    if (const Theme* existing = find(name); existing && existing->read_only())
        return false;
    // Two names can share a file ("Blue" / "blue"); writing one would silently drop the other.
    if (const Theme* clash = find_by_file(name); clash && clash->name != name)
        return false;
    Theme t = m_working;
    t.name    = name;
    t.builtin = false;
    t.bundled = false;
    if (!write_theme(t, file_for(themes_dir(), name)))
        return false;
    bool replaced = false;
    for (Theme& existing : m_themes)
        if (existing.name == name) { existing = t; replaced = true; }
    if (!replaced) {
        m_themes.push_back(t);
        std::sort(m_themes.begin() + 2, m_themes.end(), [](const Theme& a, const Theme& b) { return a.name < b.name; });
    }
    m_working    = t;
    m_saved_name = name;
    m_modified   = false;
    wxGetApp().app_config->set(t.dark ? "theme_dark" : "theme_light", name);
    wxGetApp().app_config->save();
    boost::system::error_code ec;
    boost::filesystem::remove(boost::filesystem::path(themes_dir()) / (std::string(".unsaved-") + (t.dark ? "dark" : "light") + ".json"), ec);
    return true;
}

bool ThemeManager::rename(const std::string& new_name)
{
    const Theme* saved = find(m_saved_name);
    if (saved == nullptr || saved->read_only() || new_name.empty() || find(new_name))
        return false;
    const std::string old_name = saved->name;
    if (const Theme* clash = find_by_file(new_name); clash && clash->name != old_name)
        return false;
    Theme renamed = *saved;
    renamed.name  = new_name;
    if (!write_theme(renamed, file_for(themes_dir(), new_name)))
        return false;
    boost::system::error_code ec;
    if (file_for(themes_dir(), old_name) != file_for(themes_dir(), new_name)) // "Blue" -> "blue" rewrote the same file
        boost::filesystem::remove(file_for(themes_dir(), old_name), ec);
    for (Theme& t : m_themes)
        if (t.name == old_name)
            t = renamed;
    std::sort(m_themes.begin() + 2, m_themes.end(), [](const Theme& a, const Theme& b) { return a.name < b.name; });
    AppConfig& cfg = *wxGetApp().app_config;
    if (cfg.get(renamed.dark ? "theme_dark" : "theme_light") == old_name)
        cfg.set(renamed.dark ? "theme_dark" : "theme_light", new_name);
    cfg.save();
    m_saved_name = new_name;
    if (m_working.name == old_name)
        m_working.name = new_name;
    if (m_modified)
        persist_unsaved(); // keep derived_from in step
    return true;
}

bool ThemeManager::remove(std::string name)
{
    const Theme* theme = find(name);
    if (theme == nullptr || theme->read_only())
        return false;
    const bool dark = theme->dark;
    boost::system::error_code ec;
    boost::filesystem::remove(file_for(themes_dir(), name), ec);
    m_themes.erase(std::remove_if(m_themes.begin(), m_themes.end(), [&](const Theme& t) { return t.name == name; }), m_themes.end());
    AppConfig& cfg = *wxGetApp().app_config;
    if (cfg.get(dark ? "theme_dark" : "theme_light") == name)
        cfg.set(dark ? "theme_dark" : "theme_light", "");
    cfg.save();
    if (m_saved_name == name) {
        boost::filesystem::remove(boost::filesystem::path(themes_dir()) / (std::string(".unsaved-") + (dark ? "dark" : "light") + ".json"), ec);
        load_working_for(m_working.dark);
        refresh_live();
    }
    return true;
}

bool ThemeManager::install(Theme theme)
{
    if (theme.name.empty())
        return false;
    theme.builtin = false;
    theme.bundled = false;
    bool renamed = false;
    if (const Theme* existing = find(theme.name); existing && existing->read_only()) {
        theme.name += " (imported)";
        renamed = true;
    }
    if (const Theme* clash = find_by_file(theme.name); renamed || (clash && clash->name != theme.name)) {
        // Replacing a theme was only agreed for the original name: a renamed import, or a name
        // that would share another theme's file, gets a name nothing else uses.
        while (find(theme.name) || find_by_file(theme.name))
            theme.name += " (2)";
    }
    if (!write_theme(theme, file_for(themes_dir(), theme.name)))
        return false;
    bool replaced = false;
    for (Theme& t : m_themes)
        if (t.name == theme.name) { t = theme; replaced = true; }
    if (!replaced) {
        m_themes.push_back(theme);
        std::sort(m_themes.begin() + 2, m_themes.end(), [](const Theme& a, const Theme& b) { return a.name < b.name; });
    }
    select(theme.name);
    return true;
}

bool ThemeManager::export_to(const std::string& path) const
{
    Theme t = m_working;
    t.builtin = false;
    t.bundled = false;
    boost::nowide::ofstream out(path.c_str(), std::ios::binary);
    if (!out)
        return false;
    out << t.to_json();
    out.close();
    return !out.fail();
}

void ThemeManager::revert()
{
    boost::system::error_code ec;
    boost::filesystem::remove(boost::filesystem::path(themes_dir()) / (std::string(".unsaved-") + (m_working.dark ? "dark" : "light") + ".json"), ec);
    load_working_for(m_working.dark);
    refresh_live();
}

}} // namespace Slic3r::GUI
