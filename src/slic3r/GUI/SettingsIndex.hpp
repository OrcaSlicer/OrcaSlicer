#ifndef slic3r_SettingsIndex_hpp_
#define slic3r_SettingsIndex_hpp_

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <wx/string.h>

#include <libslic3r/Config.hpp>
#include <libslic3r/Preset.hpp>

namespace Slic3r {
namespace Search {

struct InputInfo
{
    DynamicPrintConfig *config{nullptr};
    Preset::Type        type{Preset::TYPE_INVALID};
    ConfigOptionMode    mode{comSimple};
};

struct GroupAndCategory
{
    wxString group;
    wxString category;
    wxString icon;       // icon of the group's own header, or empty
    wxString line_label; // label the settings row actually draws (Line::label), or empty
    std::string path;    // wiki path (Line::label_path) of the option's line, or empty
};

// Title for a setting: the row label, qualified with the field leaf when the row packs several
// options (e.g. "Cool Plate \u2013 First layer"). Pure; inputs are already localized.
inline wxString compose_display_label(const wxString& line_label, const wxString& leaf_label, bool multi)
{
    if (line_label.empty())
        return leaf_label;
    if (!multi || leaf_label.empty() || leaf_label == line_label)
        return line_label;
    return line_label + L" \u2013 " + leaf_label; // en dash separator
}

// Title to show. A single-option row uses its live label, which can be renamed at runtime
// (brim_width -> "Brim ear radius"); otherwise fall back to the precomposed label.
inline wxString resolve_setting_title(const wxString& precomposed, const wxString& live_label, bool live_multi)
{
    if (!live_multi && !live_label.empty())
        return live_label;
    return precomposed;
}

struct Option
{
    //    bool operator<(const Option& other) const { return other.label > this->label; }
    bool operator<(const Option &other) const { return other.key > this->key; }

    // Fuzzy matching works at a character level. Thus matching with wide characters is a safer bet than with short characters,
    // though for some languages (Chinese?) it may not work correctly.
    std::wstring key;
    Preset::Type type{Preset::TYPE_INVALID};
    std::wstring label;
    std::wstring label_local;
    std::wstring group;
    std::wstring group_local;
    std::string  group_icon; // SVG base name of the group's own header icon, or empty
    std::wstring category;
    std::wstring category_local;
    bool multi_category { false };
    ConfigOptionMode mode{comSimple}; // option's visibility threshold; drives the Speed Dial's mode prompt
    std::string  tooltip;             // localized ConfigOptionDef::tooltip, or empty
    std::string  wiki_path;           // Line::label_path for the option's row, or empty
    std::string  display_label;       // label the settings row draws (localized); empty falls back to label

    std::string opt_key() const;
};

// Catalog of settings and their metadata. Owns the group/category registry populated by the
// settings pages, plus two views of the options: the mode-filtered view the sidebar search
// queries, and every option regardless of mode for the Speed Dial.
class SettingsIndex
{
    std::map<std::string, GroupAndCategory> m_groups_and_categories;

    std::vector<Option> m_options;   // mode-filtered view used by the sidebar search
    std::vector<Option> m_all_modes; // every option regardless of mode, for the Speed Dial

    void append_options(DynamicPrintConfig *config, Preset::Type type, ConfigOptionMode mode);
    void sort_options();

public:
    void init(std::vector<InputInfo> input_values);
    // Rebuild the given type's options; returns false when the index was never initialised.
    bool apply(DynamicPrintConfig *config, Preset::Type type, ConfigOptionMode mode);

    void add_key(const std::string &opt_key, Preset::Type type, const wxString &group, const wxString &category,
                 const wxString &icon = wxEmptyString);
    void set_path(const std::string &opt_key, Preset::Type type, const std::string &path);
    // Record the label the option's row draws, so the Speed Dial names a setting like the page
    // (ConfigOptionDef::label/full_label is a search name, not the row text).
    void set_line_label(const std::string &opt_key, Preset::Type type, const wxString &label);

    const std::vector<Option> &options() const { return m_options; }
    const std::vector<Option> &all_options() const { return m_all_modes; }
    const Option &             option_at(size_t pos) const { return m_options[pos]; }

    const Option &get_option(const std::string &opt_key, Preset::Type type, int &variant_index) const;
    Option        get_option(const std::string &opt_key, const wxString &label, Preset::Type type) const;

    const GroupAndCategory &get_group_and_category(const std::string &opt_key) { return m_groups_and_categories[opt_key]; }

    void sort_options_by_key()
    {
        std::sort(m_options.begin(), m_options.end(), [](const Option &o1, const Option &o2) { return o1.key < o2.key; });
    }
    void sort_options_by_label() { sort_options(); }
};

} // namespace Search
} // namespace Slic3r

#endif // slic3r_SettingsIndex_hpp_
