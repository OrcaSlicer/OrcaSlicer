#include "SettingsIndex.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

#include <boost/nowide/convert.hpp>

#include "GUI.hpp"
#include "I18N.hpp"
#include "Tab.hpp"

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

using GUI::into_u8;

namespace Search {

static std::string get_key(const std::string &opt_key, Preset::Type type) { return std::to_string(int(type)) + ";" + opt_key; }

std::string Option::opt_key() const { return into_u8(key).substr(2); }

template<class T>
// void change_opt_key(std::string& opt_key, DynamicPrintConfig* config)
void change_opt_key(std::string &opt_key, DynamicPrintConfig *config, int &cnt)
{
    T *opt_cur = static_cast<T *>(config->option(opt_key));
    cnt        = opt_cur->values.size();
    return;

    if (opt_cur->values.size() > 0) opt_key += "#" + std::to_string(0);
}

// Single assembler for an indexed Option, shared by append_options() and create_option(), so a new
// Option field is only wired up in one place.
static Option make_option(const std::string &key, Preset::Type type, const wxString &label, const GroupAndCategory &gc,
                          ConfigOptionMode mode, const std::string &tooltip, bool rewrite_extruder_category)
{
    wxString suffix;
    wxString suffix_local;
    if (gc.category == "Machine limits") {
        //suffix       = key.back() == '1' ? L("Stealth") : L("Normal");
        suffix       = key.back() == '1' ? wxEmptyString : wxEmptyString;
        suffix_local = " " + _(suffix);
        suffix       = " " + suffix;
    }

    wxString category = gc.category;
    if (rewrite_extruder_category && type == Preset::TYPE_PRINTER && category.Contains("Extruder ")) {
        std::string opt_idx = key.substr(key.find("#") + 1);
        category            = wxString::Format("%s %d", "Extruder", atoi(opt_idx.c_str()) + 1);
    }

    return Option{boost::nowide::widen(key), type, (label + suffix).ToStdWstring(), (_(label) + suffix_local).ToStdWstring(),
                  gc.group.ToStdWstring(), _(gc.group).ToStdWstring(), into_u8(gc.icon), gc.category.ToStdWstring(),
                  GUI::Tab::translate_category(category, type).ToStdWstring(), false, mode, tooltip, gc.path};
}

void SettingsIndex::append_options(DynamicPrintConfig *config, Preset::Type type, ConfigOptionMode mode)
{
    for (std::string opt_key : config->keys()) {
        const ConfigOptionDef &opt = config->def()->options.at(opt_key);
        const bool in_filtered = opt.mode <= mode;

        int cnt = 0;

        if ((type == Preset::TYPE_SLA_MATERIAL || type == Preset::TYPE_PRINTER || type == Preset::TYPE_PRINT) && opt_key != "printable_area")
            switch (config->option(opt_key)->type()) {
            case coInts: change_opt_key<ConfigOptionInts>(opt_key, config, cnt); break;
            case coBools: change_opt_key<ConfigOptionBools>(opt_key, config, cnt); break;
            case coFloats: change_opt_key<ConfigOptionFloats>(opt_key, config, cnt); break;
            case coStrings: change_opt_key<ConfigOptionStrings>(opt_key, config, cnt); break;
            case coPercents: change_opt_key<ConfigOptionPercents>(opt_key, config, cnt); break;
            case coPoints: change_opt_key<ConfigOptionPoints>(opt_key, config, cnt); break;
            // BBS
            case coEnums: change_opt_key<ConfigOptionInts>(opt_key, config, cnt); break;
            default: break;
            }

        if (type == Preset::TYPE_FILAMENT && filament_options_with_variant.find(opt_key) != filament_options_with_variant.end())
            opt_key += "#0";

        wxString label = opt.full_label.empty() ? opt.label : opt.full_label;

        std::string key = get_key(opt_key, type);
        auto add = [&](const std::string &k) {
            const GroupAndCategory &gc = m_groups_and_categories[k];
            if (gc.group.IsEmpty() || gc.category.IsEmpty() || label.IsEmpty()) return;

            const std::string tooltip = into_u8(_(opt.tooltip));
            if (in_filtered)
                m_options.emplace_back(make_option(k, type, label, gc, opt.mode, tooltip, false));
            m_all_modes.emplace_back(make_option(k, type, label, gc, opt.mode, tooltip, false));
        };
        if (cnt == 0)
            add(key);
        else
            for (int i = 0; i < cnt; ++i)
                // ! It's very important to use "#". opt_key#n is a real option key used in GroupAndCategory
                add(key + "#" + std::to_string(i));
    }
}

void SettingsIndex::sort_options()
{
    // Both views are label-sorted and multi_category-marked. They are separate consumers (the sidebar
    // search and the Speed Dial); keeping them in sync here prevents the all-modes view from silently
    // diverging in order or flags.
    auto sort_and_mark = [](std::vector<Option> &v) {
        std::sort(v.begin(), v.end(), [](const Option &o1, const Option &o2) { return o1.label < o2.label; });
        Option *last = nullptr;
        for (auto &opt : v) {
            if (last && last->label == opt.label && last->group == opt.group && last->type == opt.type && last->category != opt.category) {
                last->multi_category = true;
                opt.multi_category = true;
            }
            last = &opt;
        }
    };
    sort_and_mark(m_options);
    sort_and_mark(m_all_modes);
}

void SettingsIndex::init(std::vector<InputInfo> input_values)
{
    m_options.clear();
    m_all_modes.clear();
    for (auto i : input_values) append_options(i.config, i.type, i.mode);
    sort_options();
}

bool SettingsIndex::apply(DynamicPrintConfig *config, Preset::Type type, ConfigOptionMode mode)
{
    // m_all_modes is a separate consumer (the Speed Dial), so "nothing initialised yet" means both
    // views are empty - the mode-filtered m_options can be empty while m_all_modes is not.
    if (m_options.empty() && m_all_modes.empty()) return false;

    m_options.erase(std::remove_if(m_options.begin(), m_options.end(), [type](Option opt) { return opt.type == type; }), m_options.end());
    m_all_modes.erase(std::remove_if(m_all_modes.begin(), m_all_modes.end(), [type](Option opt) { return opt.type == type; }), m_all_modes.end());

    append_options(config, type, mode);

    sort_options();

    return true;
}

const Option &SettingsIndex::get_option(const std::string &opt_key, Preset::Type type, int &variant_index) const
{
    std::string opt_key2 = opt_key;
    if (auto n = opt_key.find('#'); n != std::string::npos) {
        variant_index = std::atoi(opt_key.c_str() + n + 1);
        opt_key2 = opt_key.substr(0, n);
    }
    auto it = std::lower_bound(m_options.begin(), m_options.end(), Option({boost::nowide::widen(get_key(opt_key2, type))}));
    // BBS: return the 0th option when not found in searcher caused by mode difference
    // assert(it != options.end());
    if (it == m_options.end()) { variant_index = -2 ; return m_options[0]; }
    if (it->opt_key() == opt_key2) {
        variant_index = -1;
    } else {
        const std::string opt_key3 = opt_key2 + "#";
        it = std::lower_bound(it, m_options.end(), Option({boost::nowide::widen(get_key(opt_key3, type))}));
        if (it == m_options.end() || it->opt_key().compare(0, opt_key3.length(), opt_key3) != 0) {
            variant_index = -2; // Not found
            return m_options[0];
        }
        auto it2 = it;
        ++it2;
        if (it2 != m_options.end() && it2->opt_key().compare(0, opt_key3.length(), opt_key3) == 0
                && printer_options_with_variant_1.find(opt_key2) == printer_options_with_variant_1.end())
            variant_index = -2;
    }

    return m_options[it - m_options.begin()];
}

static Option create_option(const std::string &opt_key, const wxString &label, Preset::Type type, const GroupAndCategory &gc)
{
    return make_option(get_key(opt_key, type), type, label, gc, comSimple, std::string(), true);
}

Option SettingsIndex::get_option(const std::string &opt_key, const wxString &label, Preset::Type type) const
{
    std::string key = get_key(opt_key, type);
    auto        it  = std::lower_bound(m_options.begin(), m_options.end(), Option({boost::nowide::widen(key)}));
    // BBS: return the 0th option when not found in searcher caused by mode difference
    if (it == m_options.end()) return m_options[0];
    if (it->key == boost::nowide::widen(key)) return m_options[it - m_options.begin()];
    if (m_groups_and_categories.find(key) == m_groups_and_categories.end()) {
        size_t pos = key.find('#');
        if (pos == std::string::npos) return m_options[it - m_options.begin()];

        std::string zero_opt_key = key.substr(0, pos + 1) + "0";

        if (m_groups_and_categories.find(zero_opt_key) == m_groups_and_categories.end()) return m_options[it - m_options.begin()];

        return create_option(opt_key, label, type, m_groups_and_categories.at(zero_opt_key));
    }

    const GroupAndCategory &gc = m_groups_and_categories.at(key);
    if (gc.group.IsEmpty() || gc.category.IsEmpty()) return m_options[it - m_options.begin()];

    return create_option(opt_key, label, type, gc);
}

void SettingsIndex::add_key(const std::string &opt_key, Preset::Type type, const wxString &group, const wxString &category, const wxString &icon)
{
    // Update fields in place so a page rebuild (get_option after set_path) doesn't drop the
    // previously recorded wiki path.
    GroupAndCategory &gc = m_groups_and_categories[get_key(opt_key, type)];
    gc.group    = group;
    gc.category = category;
    gc.icon     = icon;
}

void SettingsIndex::set_path(const std::string &opt_key, Preset::Type type, const std::string &path)
{
    if (path.empty())
        return;
    m_groups_and_categories[get_key(opt_key, type)].path = path;
}

} // namespace Search
} // namespace Slic3r
