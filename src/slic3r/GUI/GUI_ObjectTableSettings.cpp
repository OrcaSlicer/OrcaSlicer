#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Model.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_Factories.hpp"
#include "GUI_ObjectTableSettings.hpp"
#include "GUI_ObjectTable.hpp"

#include "OptionsGroup.hpp"
#include "GUI_App.hpp"
#include "wxExtensions.hpp"
#include "Plater.hpp"

#include <boost/algorithm/string.hpp>

#include "I18N.hpp"
#include "ConfigManipulation.hpp"

#include <wx/wupdlock.h>

namespace Slic3r
{
namespace GUI
{

wxDEFINE_EVENT(EVT_LOCK_DISABLE, wxCommandEvent);
wxDEFINE_EVENT(EVT_LOCK_ENABLE, wxCommandEvent);


OTG_Settings::OTG_Settings(wxWindow* parent, const bool staticbox) :
    m_parent(parent)
{
    wxString title = ""; // temporary workaround - #ys_FIXME
    m_og = std::make_shared<ConfigOptionsGroup>(parent, title, (DynamicPrintConfig*)nullptr, true);
}

bool OTG_Settings::IsShown()
{
    return m_og->sizer->IsEmpty() ? false : m_og->sizer->IsShown(size_t(0));
}

void OTG_Settings::Show(const bool show)
{
    m_og->Show(show);
}

void OTG_Settings::Hide()
{
    Show(false);
}

void OTG_Settings::UpdateAndShow(const bool show)
{
    Show(show);
//    m_parent->Layout();
}

wxSizer* OTG_Settings::get_sizer()
{
    return m_og->sizer;
}



ObjectTableSettings::ObjectTableSettings(wxWindow* parent, ObjectGridTable* table) :
    OTG_Settings(parent, true), m_table(table)
{
    m_og->activate();
    //m_og->set_name(_(L("Per-Object Settings")));    

    m_settings_list_sizer = new wxBoxSizer(wxVERTICAL);
    m_og->sizer->Add(m_settings_list_sizer, 1, wxEXPAND | wxLEFT, 5);

    m_bmp_reset = ScalableBitmap(parent, "lock_normal");
    m_bmp_reset_focus = ScalableBitmap(parent, "lock_normal");
    //TODO, adjust later
    m_bmp_reset_disable = ScalableBitmap(parent, "dot");
}

bool ObjectTableSettings::update_settings_list(bool is_object, bool is_multiple_selection, ModelObject* object, ModelConfig* config, const std::string& category)
{
    std::string group_category;
    int different_count = 0;

    m_settings_list_sizer->Clear(true);
    m_og_settings.resize(0);
    Show(true);

    if (!config || is_multiple_selection || !object)
        return false;

    const auto printer_technology   = wxGetApp().plater()->printer_technology();

    // update config values according to configuration hierarchy
    m_current_config   = printer_technology == ptFFF ?
                                        wxGetApp().preset_bundle->prints.get_edited_preset().config :
                                        wxGetApp().preset_bundle->sla_prints.get_edited_preset().config;

    //ConfigManipulation config_manipulation(load_config, toggle_field, nullptr, config);

    if (!is_object)
    {
        m_current_config.apply(object->config.get(), true);
    }

    m_origin_config = m_current_config;
    m_current_config.apply(config->get(), true);

    //SettingsFactory::Bundle cat_options = SettingsFactory::get_bundle(&config->get(), is_object);
    std::map<std::string, std::vector<SimpleSettingData>> cat_options;
    std::vector<SimpleSettingData> category_settings = SettingsFactory::get_visible_options(category, !is_object);
    bool display_multiple = false;
    auto is_option_modified = [this](std::string key) {
        ConfigOption* config_option1 = m_origin_config.option(key);
        ConfigOption* config_option2 = m_current_config.option(key);
        if (!config_option1 && config_option2)
            return true;
        else if (config_option1 && config_option2 && (*config_option1 != *config_option2))
            return true;

        return false;
    };

    //get the category and settings
    if (category_settings.size() == 0) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "can not find settings for category " <<category <<", display all the modified settings instead!!!" << std::endl;
        //return false;
        cat_options = SettingsFactory::get_all_visible_options(!is_object);
        std::map<std::string, std::vector<SimpleSettingData>>::iterator it1 = cat_options.begin();

        while (it1 != cat_options.end())
        {
            std::vector<SimpleSettingData>& settings = it1->second;
            std::vector<SimpleSettingData>::iterator it2 = settings.begin();

            while ( it2 != settings.end() )
            {
                if (!is_option_modified(it2->name)) {
                    it2 = settings.erase(it2);
                }
                else {
                    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" category %1% , keep option %2%")%it1->first % it2->name;
                    it2++;
                }
            }
            if (settings.size() > 0)
                it1++;
            else
                it1 = cat_options.erase(it1);
        }
        display_multiple = true;
    }
    else {
        cat_options.emplace(category, category_settings);
    }
    std::vector<std::string> categories;
    categories.reserve(cat_options.size());

    for (auto& cat : cat_options)
    {
        categories.push_back(cat.first);
        group_category = cat.first;

        auto extra_column = [this, is_object, object, config, group_category](wxWindow* parent, const Line& line)
        {
            auto opt_key = (line.get_options())[0].opt_id;  //we assume that we have one option per line

            auto btn = new ScalableButton(parent, wxID_ANY, m_bmp_reset);
            btn->SetToolTip(_(L("Reset parameter")));

            #ifdef __WINDOWS__
            btn->SetBackgroundColour(parent->GetBackgroundColour());
            #endif // DEBUG

            
            btn->SetBitmapFocus(m_bmp_reset_focus.bmp());
            btn->SetBitmapHover(m_bmp_reset_focus.bmp());

            #ifdef __WINDOWS__
            btn->SetBitmapDisabled(m_bmp_reset_disable.bmp());
            #endif
            
            #ifdef __WXOSX_MAC__
            btn->Bind(EVT_LOCK_DISABLE, [this, btn](auto &e) { btn->SetBitmap(m_bmp_reset_disable.bmp()); });
            btn->Bind(EVT_LOCK_ENABLE, [this, btn](auto &e) { btn->SetBitmap(m_bmp_reset_focus.bmp()); });
            #endif

            btn->Bind(wxEVT_BUTTON, [btn, opt_key, this, is_object, object, config, group_category](wxEvent &event) {
                //wxGetApp().plater()->take_snapshot(from_u8((boost::format(_utf8(L("Reset Option %s"))) % opt_key).str()));
                config->erase(opt_key);
                //btn->Hide();
                wxGetApp().obj_list()->changed_object();
                /*wxTheApp->CallAfter([this, is_object, object, config, category]() {
                    wxWindowUpdateLocker noUpdates(m_parent);
                    update_settings_list(is_object, false, object, config, category); 
                });*/
                this->m_parent->Freeze();
                /* Check overriden options list after deleting.
                 * Some options couldn't be deleted because of another one.
                 * Like, we couldn't delete fill pattern, if fill density is set to 100%
                 */
                m_current_config = m_origin_config;
                m_current_config.apply(config->get(), true);
                update_config_values(is_object, object, config, group_category);
                this->m_parent->Thaw();

                #ifdef __WXOSX_MAC__
                if (!btn->IsEnabled()) {
                    btn->SetBitmap(m_bmp_reset_disable.bmp());
                } else {
                    btn->SetBitmap(m_bmp_reset_focus.bmp());
                }
                #endif
            });
            (const_cast<Line&>(line)).extra_widget_win = btn;
            return btn;
        };

        auto optgroup = std::make_shared<ConfigOptionsGroup>(m_og->ctrl_parent(), _(cat.first), &m_current_config, false, extra_column);
        optgroup->label_width    = 20; // ORCA match label width with sidebar
        optgroup->sidetext_width = Field::def_width_thinner();
        optgroup->set_config_category_and_type(GUI::from_u8(group_category), Preset::TYPE_PRINT);

        std::weak_ptr<ConfigOptionsGroup> weak_optgroup(optgroup);
        optgroup->m_on_change = [this, is_object, object, config, group_category](const t_config_option_key &opt_id, const boost::any &value) {
                                    this->m_parent->Freeze();
                                    this->update_config_values(is_object, object, config, group_category);
                                    wxGetApp().obj_list()->changed_object();
                                    this->m_parent->Thaw();
                                    //update_extra_column_visible_status(optgroup.get(), cat.second, config);
                                };

        // call back for rescaling of the extracolumn control
        optgroup->rescale_extra_column_item = [this](wxWindow* win) {
            auto *ctrl = dynamic_cast<ScalableButton*>(win);
            if (ctrl == nullptr)
                return;
            ctrl->SetBitmap_(m_bmp_reset);
            ctrl->SetBitmapFocus(m_bmp_reset_focus.bmp()); 
            ctrl->SetBitmapHover(m_bmp_reset_focus.bmp());
            #ifdef __WINDOWS__  
            ctrl->SetBitmapDisabled(m_bmp_reset_disable.bmp());
            #endif
        };

        const bool is_extruders_cat = cat.first == "Extruders";
        for (auto& opt : cat.second)
        {
            Option option = optgroup->get_option(opt.name);
            option.opt.width = Field::def_width_wider(); // ORCA match parameter box width
            if (is_extruders_cat)
                option.opt.max = wxGetApp().extruders_edited_cnt();
            optgroup->append_single_option_line(option);

            if (!opt.label.empty()) {
                auto line = optgroup->get_line(opt.name);
                if (line)
                    line->label = GUI::from_u8(opt.label);
            }
        }
        optgroup->activate();
        for (auto& opt : cat.second)
            optgroup->get_field(opt.name)->m_on_change = [weak_optgroup](const std::string& opt_id, const boost::any& value) {
                // first of all take a snapshot and then change value in configuration
                wxGetApp().plater()->take_snapshot((boost::format("Change Option %s") % opt_id).str());
                weak_optgroup.lock()->on_change_OG(opt_id, value);
            };

        optgroup->reload_config();
        different_count = update_extra_column_visible_status(optgroup.get(), cat.second, config);
        m_current_different += different_count;
        if (different_count > 0)
            m_different_map[group_category] = different_count;
        /*for (auto& opt : cat.second)
        {
            auto line = optgroup->get_line(opt);
            if (line) {
                if (config->has(opt))
                    line->extra_widget_win->Show(true);
                else
                    line->extra_widget_win->Hide();
            }
        }*/

        m_settings_list_sizer->Add(optgroup->sizer, 0, wxEXPAND | wxALL, 0);
        m_og_settings.push_back(optgroup);

        auto toggle_field = [this, optgroup](const t_config_option_key & opt_key, bool toggle, int opt_index)
        {
            Field* field = optgroup->get_fieldc(opt_key, opt_index);;
            if (field)
                field->toggle(toggle);
        };
        auto toggle_line = [this, optgroup](const t_config_option_key &opt_key, bool toggle, int opt_index)
        {
            Line* line = optgroup->get_line(opt_key);
            if (line) line->toggle_visible = toggle;
        };
        ConfigManipulation config_manipulation(nullptr, toggle_field, toggle_line, nullptr, &m_current_config);

        bool is_BBL_printer = wxGetApp().preset_bundle->is_bbl_vendor();
        config_manipulation.set_is_BBL_Printer(is_BBL_printer);

        printer_technology == ptFFF  ?  config_manipulation.toggle_print_fff_options(&m_current_config) :
                                        config_manipulation.toggle_print_sla_options(&m_current_config) ;
        optgroup->update_visibility(wxGetApp().get_mode());
    }

    //if (!categories.empty()) {
    //    update_config_values(is_object, object, config, category);
    //}
    if (m_current_different > 0)
        m_table->enable_reset_all_button(true);
    else
        m_table->enable_reset_all_button(false);

    return true;
}

bool ObjectTableSettings::add_missed_options(ModelConfig* config_to, const DynamicPrintConfig& config_from)
{
    bool is_added = false;
    if (wxGetApp().plater()->printer_technology() == ptFFF)
    {
        if (config_to->has("sparse_infill_density") && !config_to->has("sparse_infill_pattern"))
        {
            if (config_from.option<ConfigOptionPercent>("sparse_infill_density")->value == 100) {
                config_to->set_key_value("sparse_infill_pattern", config_from.option("sparse_infill_pattern")->clone());
                is_added = true;
            }
        }
    }

    return is_added;
}

int ObjectTableSettings::update_extra_column_visible_status(ConfigOptionsGroup* option_group, const std::vector<SimpleSettingData>& option_keys, ModelConfig* config)
{
    int count = 0;

    for (auto& opt : option_keys)
    {
        auto line = option_group->get_line(opt.name);
        Field* field = option_group->get_fieldc(opt.name, -1);
        wxWindow *reset_window = field?field->getWindow():nullptr;
        if (line) {
            if ((config->has(opt.name)) && reset_window&&reset_window->IsEnabled()) {
                line->extra_widget_win->Enable();

                #ifdef __WXOSX_MAC__
                wxCommandEvent event(EVT_LOCK_ENABLE);
                event.SetEventObject(line->extra_widget_win);
                wxPostEvent(line->extra_widget_win, event);
                #endif

                count++;
            } else {
                line->extra_widget_win->Disable();
                #ifdef __WXOSX_MAC__
                wxCommandEvent event(EVT_LOCK_DISABLE);
                event.SetEventObject(line->extra_widget_win);
                wxPostEvent(line->extra_widget_win, event);
                #endif
            }
        }
    }
    wxGridSizer* grid_sizer = option_group->get_grid_sizer();
    grid_sizer->Layout();

    return count;
}

void ObjectTableSettings::update_config_values(bool is_object, ModelObject* object, ModelConfig* config, const std::string& category)
{
    int different_count = 0;
    const auto printer_technology   = wxGetApp().plater()->printer_technology();

    if (!object || !config)
        return;

    // update config values according to configuration hierarchy
    DynamicPrintConfig  &main_config   = m_current_config;


    auto toggle_field = [this](const t_config_option_key & opt_key, bool toggle, int opt_index)
    {
        Field* field = nullptr;
        for (auto og : m_og_settings) {
            field = og->get_fieldc(opt_key, opt_index);
            if (field != nullptr)
                break;
        }
        if (field)
            field->toggle(toggle);
    };
    auto toggle_line = [this](const t_config_option_key &opt_key, bool toggle, int opt_index) {
        for (auto og : m_og_settings) {
            Line *line = og->get_line(opt_key);
            if (line) { line->toggle_visible = toggle; break; }
        }
    };

    ConfigManipulation config_manipulation(nullptr, toggle_field, toggle_line, nullptr, &m_current_config);

    config_manipulation.set_is_BBL_Printer(wxGetApp().preset_bundle->is_bbl_vendor());

    printer_technology == ptFFF  ?  config_manipulation.update_print_fff_config(&main_config) :
                                    config_manipulation.update_print_sla_config(&main_config) ;

    printer_technology == ptFFF  ?  config_manipulation.toggle_print_fff_options(&main_config) :
                                    config_manipulation.toggle_print_sla_options(&main_config) ;
    for (auto og : m_og_settings) {
        og->update_visibility(wxGetApp().get_mode());
    }
    m_parent->Layout();
    m_parent->Fit();
    m_parent->GetParent()->Layout();
    t_config_option_keys diff_keys;
    for (const t_config_option_key &opt_key : main_config.keys()) {
        const ConfigOption *this_opt  = main_config.option(opt_key);
        const ConfigOption *other_opt = m_origin_config.option(opt_key);
        if (this_opt != nullptr && (other_opt == nullptr || *this_opt != *other_opt))
            diff_keys.emplace_back(opt_key);
    }

    // load checked values from main_config to config
    config->reset();
    config->apply_only(main_config, diff_keys, true);
    // Initialize UI components with the config values.
    std::vector<SimpleSettingData> category_settings = SettingsFactory::get_visible_options(category, !is_object);
    std::string current_category;
    for (auto og : m_og_settings)
    {
        current_category = GUI::into_u8(og->config_category());
        og->reload_config();
        if (category == ObjectGridTable::category_all) {
            category_settings = SettingsFactory::get_visible_options(current_category, !is_object);
            different_count = update_extra_column_visible_status(og.get(), category_settings, config);
            if (different_count > 0)
                m_different_map[current_category] = different_count;
            else
                m_different_map.erase(current_category);
        }
        else if (category == current_category){
            different_count = update_extra_column_visible_status(og.get(), category_settings, config);
            if (different_count > 0)
                m_different_map[current_category] = different_count;
            else
                m_different_map.erase(current_category);
        }
    }
    if (m_different_map.size() > 0)
        m_table->enable_reset_all_button(true);
    else
        m_table->enable_reset_all_button(false);

    //update the table and volume settings
    m_table->reload_cell_data(m_current_row, category);
}

void ObjectTableSettings::UpdateAndShow(int row, const bool show, bool is_object, bool is_multiple_selection, ModelObject* object, ModelConfig* config, const std::string& category)
{
    m_current_row = row;
    m_current_category = category;
    m_current_different = 0;
    m_different_map.clear();
    //OTG_Settings::UpdateAndShow(show ? update_settings_list(is_object, is_multiple_selection, object, config, category) : false);
    if (show) {
        update_settings_list(is_object, is_multiple_selection, object, config, category);
    }
    else
        OTG_Settings::UpdateAndShow(false);
}

void ObjectTableSettings::ValueChanged(int row, bool is_object,  ModelObject* object, ModelConfig* config, const std::string& category, const std::string& key)
{
    if ((row != m_current_row)
        || ((category != m_current_category) && (m_current_category != ObjectGridTable::category_all)))
        return;

    ConfigOption *my_opt = m_current_config.option(key, true);
    if (config->has(key)) {
        my_opt->set(config->option(key));
    }
    else {
        ConfigOption* config_option = m_origin_config.option(key);
        if (config_option)
            my_opt->set(config_option);
        else
            m_current_config.erase(key);
    }
    update_config_values(is_object, object, config, category);
}

void  ObjectTableSettings::resetAllValues(int row, bool is_object, ModelObject* object, ModelConfig* config, const std::string& category)
{
    if ((row != m_current_row) || (category != m_current_category))
        return;

    if (category == ObjectGridTable::category_all) {
        std::map<std::string, std::vector<SimpleSettingData>> cat_options;

        //get the category and settings
        cat_options = SettingsFactory::get_all_visible_options(!is_object);
        std::map<std::string, std::vector<SimpleSettingData>>::iterator it1 = cat_options.begin();

        while (it1 != cat_options.end())
        {
            std::vector<SimpleSettingData>& settings = it1->second;
            std::vector<SimpleSettingData>::iterator it2 = settings.begin();

            while ( it2 != settings.end() )
            {
                config->erase(it2->name);
                it2++;
            }
            it1++;
        }
    }
    else {
        // Initialize UI components with the config values.
        std::vector<SimpleSettingData> category_settings = SettingsFactory::get_visible_options(category, !is_object);
        for (auto& opt : category_settings)
        {
            config->erase(opt.name);
        }
    }
    m_current_config = m_origin_config;
    m_current_config.apply(config->get(), true);
    update_config_values(is_object, object, config, category);
}

void ObjectTableSettings::msw_rescale()
{
    for (auto group : m_og_settings)
        group->msw_rescale();
}

} //namespace GUI
} //namespace Slic3r 

