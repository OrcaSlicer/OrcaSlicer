#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <wx/colour.h>

namespace Slic3r { namespace GUI {

// One interface colour the user can change: `id` names it in theme files, `key` is the
// light-theme colour the code uses (the StateColor table key), `label` is shown in the editor.
struct ThemeRole
{
    const char* id;
    const char* key;
    const char* label;
};

// A named set of colours derived from one of the built-in themes (its base). Only the values
// that differ from the base are stored; an empty string means "the base's own colour".
struct Theme
{
    std::string name;
    bool        dark    = false; // base: light or dark
    bool        builtin = false; // Light / Dark themselves: never edited or saved in place
    bool        bundled = false; // shipped in resources/themes: read-only like the built-ins
    bool        read_only() const { return builtin || bundled; }

    std::map<std::string, std::string> colors; // role id -> #RRGGBB
    std::string view_background;               // 3D view background
    std::string plate_grid;                    // grid lines of the current build plate
    std::string plate_model;                   // the build plate itself (bed model or flat face)
    std::string toolbar;                       // the toolbars drawn inside the 3D view
    std::string label_sys;                     // parameter label: system value
    std::string label_modified;                // parameter label: modified value
    std::string focus_ring;                    // keyboard focus outline
    bool        auto_contrast_text = false;

    static const std::vector<ThemeRole>& roles();
    static const ThemeRole*              role_by_key(const std::string& key);
    static std::string                   slug(const std::string& name);

    std::string                to_json() const;
    static std::optional<Theme> from_json(const std::string& text);
    static std::optional<Theme> from_file(const std::string& path);
    bool                       same_values(const Theme& other) const; // everything but name/builtin
    bool                       empty_overrides() const;
};

// Owns the built-in and user themes, the theme in effect for each base (app-config
// `theme_light` / `theme_dark`), and the working copy the editor changes live. Applies a theme
// to the process: StateColor overrides, 3D view colours, label colours, focus outline, ImGui.
class ThemeManager
{
public:
    static ThemeManager& get();

    // Startup: built-ins, user theme files, migration of the old app-config keys, and the
    // working copy for the current base. Does not repaint anything.
    void load();

    const std::vector<Theme>& themes() const { return m_themes; }
    const Theme*              find(const std::string& name) const;
    const Theme&              active() const { return m_working; }    // the working copy in effect
    const std::string&        active_name() const { return m_saved_name; }
    bool                      modified() const { return m_modified; }
    // Counts every change of what is applied (edit, selection, base flip). The live re-theme is
    // a preview: windows coloured at construction are patched by matching colours, which can
    // misattribute when roles share a colour. Preferences rebuilds the main window on close
    // when this moved, so the state after editing is always built from the theme itself.
    unsigned                  generation() const { return m_generation; }
    // Counts only edits of colours/flags (not selecting another theme). Preferences rebuilds the
    // main window on close when this moved: an edit is where the live patching can leave a
    // window behind, while switching between whole themes is reproduced by the preview.
    unsigned                  edit_generation() const { return m_edit_generation; }
    std::string               themes_dir() const;
    std::string               slot_name(bool dark) const;           // the theme chosen for a base

    // Make `name` the theme for its base and show it: switches the colour scheme to that base
    // unless the scheme is "system", in which case the slot is set and it shows when the
    // desktop is in that mode.
    void select(const std::string& name);
    // The base flipped (desktop or colour scheme): load the slot's theme for the new base.
    void on_base_changed();
    // Push the working copy into StateColor, the 3D view, the focus outline and ImGui.
    void apply_to_state();

    // Live edits of the working copy. Editing a built-in forks it into an unsaved copy.
    void set_role_color(const std::string& key, const wxColour& color); // invalid colour = base
    void set_view_background(const wxColour& color);
    void set_plate_grid(const wxColour& color);
    void set_plate_model(const wxColour& color);
    void set_toolbar(const wxColour& color);
    void set_label_sys(const wxColour& color);
    void set_label_modified(const wxColour& color);
    void set_focus_ring(const wxColour& color);
    void set_auto_contrast_text(bool on);
    void reset_working(); // every value back to the base, selection kept

    bool save();                              // working copy -> its file (user themes only)
    bool save_as(const std::string& name);
    bool rename(const std::string& new_name);  // the saved user theme the working copy derives from
    bool remove(std::string name); // by value: the caller's reference may be into the list being edited
    void revert();                            // back to the saved theme
    // Exchange: a theme file from anywhere becomes a user theme (replacing one of the same
    // name) and is selected; the working copy is written out under its own name.
    bool install(Theme theme);
    bool export_to(const std::string& path) const;

private:
    ThemeManager() = default;
    void        fork_if_builtin();
    void        refresh_live();               // apply + repaint everything
    void        persist_unsaved();            // keep an unsaved working copy across restarts
    void        load_working_for(bool dark);
    bool        write_theme(const Theme& theme, const std::string& path) const;
    void        migrate_old_config();
    static std::string file_for(const std::string& dir, const std::string& name);
    const Theme* find_by_file(const std::string& name) const; // a saved theme whose file `name` would overwrite

    std::vector<Theme> m_themes;
    Theme              m_working;
    std::string        m_saved_name; // the theme the working copy derives from
    bool               m_modified = false;
    unsigned           m_generation = 0;
    unsigned           m_edit_generation = 0;
};

}} // namespace Slic3r::GUI
