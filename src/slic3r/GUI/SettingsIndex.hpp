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
    wxString icon;    // icon of the group's own header, or empty
    std::string path; // wiki path (Line::label_path) of the option's line, or empty
};

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
