#include "EditGCodeDialog.hpp"

#include <vector>
#include <string>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/wupdlock.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "Tab.hpp"
#include "wxExtensions.hpp"
#include "ExtraRenderers.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"

#include "Widgets/DialogButtons.hpp"

#include "libslic3r/Preset.hpp"

#define BTN_GAP  FromDIP(20)
#define BTN_SIZE wxSize(FromDIP(58), FromDIP(24))

namespace Slic3r {
namespace GUI {

//------------------------------------------
//          EditGCodeDialog
//------------------------------------------

EditGCodeDialog::EditGCodeDialog(wxWindow* parent, const std::string& key, const std::string& value) :
    DPIDialog(parent, wxID_ANY, format_wxstr(_L("Edit Custom G-code (%1%)"), key), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetFont(wxGetApp().normal_font());
    SetBackgroundColour(*wxWHITE);
    wxGetApp().UpdateDarkUI(this);
    wxGetApp().UpdateDlgDarkUI(this);

    int border = 10;
    int em = em_unit();

    wxStaticText* label_top = new wxStaticText(this, wxID_ANY, _L("Built-in placeholders (Double click item to add to G-code)") + ":");

    auto* grid_sizer = new wxFlexGridSizer(1, 3, 5, 15);
    grid_sizer->SetFlexibleDirection(wxBOTH);

    auto* param_sizer = new wxBoxSizer(wxVERTICAL);

    m_search_bar = new wxSearchCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_search_bar->ShowSearchButton(true);
    m_search_bar->ShowCancelButton(true);
    m_search_bar->SetDescriptiveText(_L("Search G-code placeholders"));
    m_search_bar->SetForegroundColour(*wxBLACK);
    wxGetApp().UpdateDarkUI(m_search_bar);

    m_search_bar->Bind(wxEVT_SET_FOCUS, [](wxFocusEvent&) {
//        this->on_search_update();
    });
    m_search_bar->Bind(wxEVT_COMMAND_TEXT_UPDATED, [this](wxCommandEvent&) {
        this->on_search_update();
    });

    param_sizer->Add(m_search_bar, 0, wxEXPAND | wxALL, border);

    m_params_list = new ParamsViewCtrl(this, wxDefaultSize);
    m_params_list->SetFont(wxGetApp().code_font());
    wxGetApp().UpdateDarkUI(m_params_list);
    param_sizer->Add(m_params_list, 1, wxEXPAND | wxALL, border);

    m_add_btn = new ScalableButton(this, wxID_ANY, "add_copies");
    m_add_btn->SetToolTip(_L("Add selected placeholder to G-code"));

    m_gcode_editor = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(value), wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE
    #ifdef _WIN32
        | wxBORDER_SIMPLE
    #endif
    );

    m_gcode_editor->SetFont(wxGetApp().code_font());
    m_gcode_editor->SetInsertionPointEnd();
    wxGetApp().UpdateDarkUI(m_gcode_editor);

    grid_sizer->Add(param_sizer,  1, wxEXPAND);
    grid_sizer->Add(m_add_btn,      0, wxALIGN_CENTER_VERTICAL);
    grid_sizer->Add(m_gcode_editor, 2, wxEXPAND);

    grid_sizer->AddGrowableRow(0, 1);
    grid_sizer->AddGrowableCol(0, 1);
    grid_sizer->AddGrowableCol(2, 2);

    m_param_label = new wxStaticText(this, wxID_ANY, _L("Select placeholder"));
    m_param_label->SetFont(wxGetApp().bold_font());

    m_param_description = new wxStaticText(this, wxID_ANY, wxEmptyString);

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    wxBoxSizer* topSizer = new wxBoxSizer(wxVERTICAL);

    topSizer->Add(label_top           , 0, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, border);
    topSizer->Add(grid_sizer          , 1, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, border);
    topSizer->Add(m_param_label       , 0, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, border);
    topSizer->Add(m_param_description , 0, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, border);
    topSizer->Add(dlg_btns            , 0, wxEXPAND);

    SetSizer(topSizer);
    topSizer->SetSizeHints(this);

    this->Fit();

    fit_in_display(*this, {100 * em, 70 * em});

    this->Layout();

    this->CenterOnScreen();

    init_params_list(key);
    bind_list_and_button();
}

EditGCodeDialog::~EditGCodeDialog()
{
    // To avoid redundant process of wxEVT_DATAVIEW_SELECTION_CHANGED after dialog distroing (on Linux)
    // unbind this event from params_list
    m_params_list->Unbind(wxEVT_DATAVIEW_SELECTION_CHANGED, &EditGCodeDialog::selection_changed, this);
}

std::string EditGCodeDialog::get_edited_gcode() const
{
    wxString text = m_gcode_editor->GetValue();
    return std::string(text.ToUTF8());
}

void EditGCodeDialog::on_search_update()
{
    wxString   search_text = m_search_bar->GetValue().Lower();
    if (search_text.empty())
        m_params_list->model->FinishSearch();
    else
        m_params_list->model->RefreshSearch(search_text);
}

static ParamType get_type(const std::string& opt_key, const ConfigOptionDef& opt_def)
{
    return opt_def.is_scalar() ? ParamType::Scalar : ParamType::Vector;
}

void EditGCodeDialog::init_params_list(const std::string& custom_gcode_name)
{
    const auto& custom_gcode_placeholders = custom_gcode_specific_placeholders();
    const auto& specific_params = custom_gcode_placeholders.count(custom_gcode_name) > 0 ?
                                  custom_gcode_placeholders.at(custom_gcode_name) : t_config_option_keys({});

    // Add slicing states placeholders

    wxDataViewItem slicing_state = m_params_list->AppendGroup(_L("[Global] Slicing State"), "custom-gcode_slicing-state_global");
    if (!cgp_ro_slicing_states_config_def.empty()) {
        wxDataViewItem read_only = m_params_list->AppendSubGroup(slicing_state, _L("Read Only"), "lock_closed");
        for (const auto& [opt_key, def]: cgp_ro_slicing_states_config_def.options)
            m_params_list->AppendParam(read_only, get_type(opt_key, def), opt_key);
    }

    if (!cgp_rw_slicing_states_config_def.empty()) {
        wxDataViewItem read_write = m_params_list->AppendSubGroup(slicing_state, _L("Read Write"), "lock_open");
        for (const auto& [opt_key, def] : cgp_rw_slicing_states_config_def.options)
            m_params_list->AppendParam(read_write, get_type(opt_key, def), opt_key);
    }

    // add other universal params, which are related to slicing state
    if (!cgp_other_slicing_states_config_def.empty()) {
        slicing_state = m_params_list->AppendGroup(_L("Slicing State"), "custom-gcode_slicing-state");
        for (const auto& [opt_key, def] : cgp_other_slicing_states_config_def.options)
            m_params_list->AppendParam(slicing_state, get_type(opt_key, def), opt_key);
    }

    // Add universal placeholders

    {
        // Add print statistics subgroup

        if (!cgp_print_statistics_config_def.empty()) {
            wxDataViewItem statistics = m_params_list->AppendGroup(_L("Print Statistics"), "custom-gcode_stats");
            for (const auto& [opt_key, def] : cgp_print_statistics_config_def.options)
                m_params_list->AppendParam(statistics, get_type(opt_key, def), opt_key);
        }

        // Add objects info subgroup

        if (!cgp_objects_info_config_def.empty()) {
            wxDataViewItem objects_info = m_params_list->AppendGroup(_L("Objects Info"), "custom-gcode_object-info");
            for (const auto& [opt_key, def] : cgp_objects_info_config_def.options)
                m_params_list->AppendParam(objects_info, get_type(opt_key, def), opt_key);
        }

        // Add  dimensions subgroup

        if (!cgp_dimensions_config_def.empty()) {
            wxDataViewItem dimensions = m_params_list->AppendGroup(_L("Dimensions"), "custom-gcode_measure");
            for (const auto& [opt_key, def] : cgp_dimensions_config_def.options)
                m_params_list->AppendParam(dimensions, get_type(opt_key, def), opt_key);
        }

        // Add temperature subgroup

        if (!cgp_temperatures_config_def.empty()) {
            wxDataViewItem temperatures = m_params_list->AppendGroup(_L("Temperatures"), "custom-gcode_temperature");
            for (const auto& [opt_key, def] : cgp_temperatures_config_def.options)
                m_params_list->AppendParam(temperatures, get_type(opt_key, def), opt_key);
        }

        // Add timestamp subgroup

        if (!cgp_timestamps_config_def.empty()) {
            wxDataViewItem dimensions = m_params_list->AppendGroup(_L("Timestamps"), "custom-gcode_time");
            for (const auto& [opt_key, def] : cgp_timestamps_config_def.options)
                m_params_list->AppendParam(dimensions, get_type(opt_key, def), opt_key);
        }
    }

    // Add specific placeholders

    if (!specific_params.empty()) {
        wxDataViewItem group = m_params_list->AppendGroup(format_wxstr(_L("Specific for %1%"), custom_gcode_name), "custom-gcode_gcode");
        for (const auto& opt_key : specific_params)
            if (auto def = custom_gcode_specific_config_def.get(opt_key); def && def->type != coNone) {
                m_params_list->AppendParam(group, get_type(opt_key, *def), opt_key);
            }
        m_params_list->Expand(group);
    }

    // Add placeholders from presets

    wxDataViewItem presets = add_presets_placeholders();
    // add other params which are related to presets
    if (!cgp_other_presets_config_def.empty())
        for (const auto& [opt_key, def] : cgp_other_presets_config_def.options)
            m_params_list->AppendParam(presets, get_type(opt_key, def), opt_key);
}

wxDataViewItem EditGCodeDialog::add_presets_placeholders()
{
    auto get_set_from_vec = [](const std::vector<std::string>&vec) {
        return std::set(vec.begin(), vec.end());
    };

    const bool  is_fff      = wxGetApp().plater()->printer_technology() == ptFFF;
    const std::set<std::string> print_options    = get_set_from_vec(is_fff ? Preset::print_options()    : Preset::sla_print_options());
    const std::set<std::string> material_options = get_set_from_vec(is_fff ? Preset::filament_options() : Preset::sla_material_options());
    const std::set<std::string> printer_options  = get_set_from_vec(is_fff ? Preset::printer_options()  : Preset::sla_printer_options());
    const auto& full_config = wxGetApp().preset_bundle->full_config();
    const auto& tab_list    = wxGetApp().tabs_list;

    Tab* tab_print = nullptr;
    Tab* tab_filament = nullptr;
    Tab* tab_printer = nullptr;
    for (const auto tab : tab_list) {
        if (tab->m_type == Preset::TYPE_PRINT)
            tab_print = tab;
        else if (tab->m_type == Preset::TYPE_FILAMENT)
            tab_filament = tab;
        else if (tab->m_type == Preset::TYPE_PRINTER)
            tab_printer = tab;
    }


    // Orca: create subgroups from the pages of the tabs
    auto init_from_tab = [this, full_config](wxDataViewItem parent, Tab* tab, const set<string>& preset_keys){
        set extra_keys(preset_keys);
        for (const auto& page : tab->m_pages) {
            // ORCA: Pull icons from tabs for subgroups, icons are hidden on tabs
            std::string icon_name = "empty"; // use empty icon if not defined
            for (const auto& icons_list : tab->m_icon_index) {
                if (icons_list.second == page->iconID()) {
                    icon_name = icons_list.first;
                    break;
                }
            }
            wxDataViewItem subgroup = m_params_list->AppendSubGroup(parent, page->title(), icon_name); // Use icon instead empty icon

            std::set<std::string> opt_keys;
            for (const auto& optgroup : page->m_optgroups)
                for (const auto& opt : optgroup->opt_map())
                    opt_keys.emplace(opt.first);

            for (const auto& opt_key : opt_keys)
                if (const ConfigOption* optptr = full_config.optptr(opt_key)) {
                    extra_keys.erase(opt_key);
                    m_params_list->AppendParam(subgroup, optptr->is_scalar() ? ParamType::Scalar : ParamType::Vector, opt_key);
                }
        }
        for (auto opt_key : extra_keys)
            if (const ConfigOption* optptr = full_config.optptr(opt_key))
                m_params_list->AppendParam(parent, optptr->is_scalar() ? ParamType::Scalar : ParamType::Vector, opt_key);
    };

    wxDataViewItem group = m_params_list->AppendGroup(_L("Presets"), "cog");

    wxDataViewItem print = m_params_list->AppendSubGroup(group, _L("Print settings"), "process");
    init_from_tab(print, tab_print, print_options);

    wxDataViewItem material = m_params_list->AppendSubGroup(group, _(is_fff ? L("Filament settings") : L("SLA Materials settings")), is_fff ? "filament" : "resin");
    init_from_tab(material, tab_filament, material_options);

    wxDataViewItem printer = m_params_list->AppendSubGroup(group, _L("Printer settings"), is_fff ? "printer" : "sla_printer");
    init_from_tab(printer, tab_printer, printer_options);

    return group;
}

void EditGCodeDialog::add_selected_value_to_gcode()
{
    const wxString val = m_params_list->GetSelectedValue();
    if (val.IsEmpty())
        return;

    m_gcode_editor->WriteText(m_gcode_editor->GetInsertionPoint() == m_gcode_editor->GetLastPosition() ? "\n" + val : val);

    if (val.Last() == ']') {
        const long new_pos = m_gcode_editor->GetInsertionPoint();
        if (val[val.Len() - 2] == '[')
            m_gcode_editor->SetInsertionPoint(new_pos - 1);          // set cursor into brackets
        else
            m_gcode_editor->SetSelection(new_pos - 17, new_pos - 1); // select "current_extruder"
    }

    m_gcode_editor->SetFocus();
}

void EditGCodeDialog::selection_changed(wxDataViewEvent& evt)
{
    wxString label;
    wxString description;

    const std::string opt_key = m_params_list->GetSelectedParamKey();
    if (!opt_key.empty()) {
        const ConfigOptionDef*    def     { nullptr };

        for (const ConfigDef* config: std::initializer_list<const ConfigDef*> {
                 &custom_gcode_specific_config_def,
                 &cgp_ro_slicing_states_config_def,
                 &cgp_rw_slicing_states_config_def,
                 &cgp_other_slicing_states_config_def,
                 &cgp_print_statistics_config_def,
                 &cgp_objects_info_config_def,
                 &cgp_dimensions_config_def,
                 &cgp_temperatures_config_def,
                 &cgp_timestamps_config_def,
                 &cgp_other_presets_config_def
             }) {
            if (config->has(opt_key)) {
                def = config->get(opt_key);
                break;
            }
        }
        // Orca: move below checking for def in custom defined G-code placeholders
        // This allows custom placeholders to override the default ones for this dialog
        // Override custom def if selection is within the preset category
        if (!def || m_params_list->GetSelectedTopLevelCategory() == "Presets") {
            const auto& full_config = wxGetApp().preset_bundle->full_config();
            if (const ConfigDef* config_def = full_config.def(); config_def && config_def->has(opt_key)) {
                def = config_def->get(opt_key);
            }
        }

            if (def) {
                const ConfigOptionType scalar_type = def->is_scalar() ? def->type : static_cast<ConfigOptionType>(def->type - coVectorType);
                wxString type_str = scalar_type == coNone           ? "none" :
                                                     scalar_type == coFloat          ? "float" :
                                                     scalar_type == coInt            ? "integer" :
                                                     scalar_type == coString         ? "string" :
                                                     scalar_type == coPercent        ? "percent" :
                                                     scalar_type == coFloatOrPercent ? "float or percent" :
                                                     scalar_type == coPoint          ? "point" :
                                                     scalar_type == coBool           ? "bool" :
                                                     scalar_type == coEnum           ? "enum" : "undef";
                if (!def->is_scalar())
                    type_str += "[]";

                label = (!def || (def->full_label.empty() && def->label.empty()) ) ? format_wxstr("%1%\n(%2%)", opt_key, type_str) :
                        (!def->full_label.empty() && !def->label.empty() ) ?
                                                                                    format_wxstr("%1% > %2%\n(%3%)", _(def->full_label), _(def->label), type_str) :
                                                                                    format_wxstr("%1%\n(%2%)", def->label.empty() ? _(def->full_label) : _(def->label), type_str);

                if (def)
                    description = get_wraped_wxString(_(def->tooltip), 120);
            }
            else
                label = "Undef optptr";
    }

    m_param_label->SetLabel(label);
    m_param_description->SetLabel(description);

    Layout();
}

void EditGCodeDialog::bind_list_and_button()
{
    m_params_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, &EditGCodeDialog::selection_changed, this);

    m_params_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this](wxDataViewEvent& ) {
        add_selected_value_to_gcode();
    });

    m_add_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        add_selected_value_to_gcode();
    });
}

void EditGCodeDialog::on_dpi_changed(const wxRect&suggested_rect)
{
    const int& em = em_unit();
    const wxSize& size = wxSize(45 * em, 35 * em);
    SetMinSize(size);

    Fit();
    Refresh();
}

void EditGCodeDialog::on_sys_color_changed()
{
    m_add_btn->msw_rescale();
}

const std::map<ParamType, std::string> ParamsInfo {
//    Type                      BitmapName
    { ParamType::Scalar,        "custom-gcode_single"          },
    { ParamType::Vector,        "custom-gcode_vector"          },
    { ParamType::FilamentVector,"custom-gcode_vector-index" },
};

static void make_bold(wxString& str)
{
#if defined(SUPPORTS_MARKUP) && !defined(__APPLE__)
    str = format_wxstr("<b>%1%</b>", str);
#endif
}

static void highlight(wxString& str)
{
#if defined(SUPPORTS_MARKUP) && !defined(__APPLE__)
    str = format_wxstr("<span bgcolor=\"#009688\">%1%</span>", str);
#endif
}

// ----------------------------------------------------------------------------
//                  ParamsModelNode: a node inside ParamsModel
// ----------------------------------------------------------------------------

ParamsNode::ParamsNode(const wxString& group_name, const std::string& icon_name, wxDataViewCtrl* ctrl)
: icon_name(icon_name)
, text(group_name)
, m_ctrl(ctrl)
, m_bold(true)
{
}

ParamsNode::ParamsNode( ParamsNode *        parent,
                        const wxString&     sub_group_name,
                        const std::string&  icon_name,
                        wxDataViewCtrl* ctrl)
    : m_parent(parent)
    , icon_name(icon_name)
    , text(sub_group_name)
    , m_ctrl(ctrl)
    , m_bold(true)
{
}

ParamsNode::ParamsNode( ParamsNode*         parent,
                        ParamType           param_type,
                        const std::string&  param_key,
                        wxDataViewCtrl* ctrl)
    : m_parent(parent)
    , m_param_type(param_type)
    , m_container(false)
    , param_key(param_key)
    , m_ctrl(ctrl)
{
    text = from_u8(param_key);
    if (param_type == ParamType::Vector)
        text += "[]";
    else if (param_type == ParamType::FilamentVector)
        text += "[current_extruder]";

    icon_name = ParamsInfo.at(param_type);
}

wxString ParamsNode::GetFormattedText()
{
    wxString formatted_text(text);
    if (m_highlight_index) {
        wxString substr = formatted_text.substr(m_highlight_index->first, m_highlight_index->second);
        formatted_text  = formatted_text.Remove(m_highlight_index->first, m_highlight_index->second);
        highlight(substr);
        formatted_text.insert(m_highlight_index->first, substr);
    }

    if (m_bold)
        make_bold(formatted_text);

    return formatted_text;
}

void ParamsNode::StartSearch()
{
    const wxDataViewItem item(this);
    m_expanded_before_search = m_ctrl->IsExpanded(item);
    if (!GetChildren().empty())
        for (const auto& child : GetChildren())
            child->StartSearch();
}

void ParamsNode::RefreshSearch(const wxString& search_text)
{
    if (!GetChildren().empty())
        for (auto& child : GetChildren())
            child->RefreshSearch(search_text);

    if (GetEnabledChildren().empty())
        if (auto pos = text.find(search_text); IsParamNode() && pos != wxString::npos) {
            m_highlight_index = make_unique<pair<int, int>>(pos, search_text.Len());
            Enable();
        } else {
            Disable();
        }
    else
        Enable();
}

void ParamsNode::FinishSearch()
{
    Enable();
    m_highlight_index.reset();
    const wxDataViewItem item(this);
    if (!GetChildren().empty())
        for (const auto& child : GetChildren())
            child->FinishSearch();
    m_expanded_before_search ? m_ctrl->Expand(item) : m_ctrl->Collapse(item);
}

wxDataViewItemArray ParamsNode::GetEnabledChildren() {
    wxDataViewItemArray array;
    for (const std::unique_ptr<ParamsNode>& child : m_children)
        if (child->IsEnabled())
            array.Add(wxDataViewItem(child.get()));
    return array;
}


// ----------------------------------------------------------------------------
//                  ParamsModel
// ----------------------------------------------------------------------------

ParamsModel::ParamsModel()
{
}

wxDataViewItem ParamsModel::AppendGroup(const wxString&    group_name,
                                        const std::string& icon_name)
{
    m_group_nodes.emplace_back(std::make_unique<ParamsNode>(group_name, icon_name, m_ctrl));

    wxDataViewItem parent(nullptr);
    wxDataViewItem child((void*)m_group_nodes.back().get());

    ItemAdded(parent, child);
    m_ctrl->Expand(parent);
    return child;
}

wxDataViewItem ParamsModel::AppendSubGroup(wxDataViewItem       parent,
                                           const wxString&      sub_group_name,
                                           const std::string&   icon_name)
{
    ParamsNode* parent_node = static_cast<ParamsNode*>(parent.GetID());
    if (!parent_node)
        return wxDataViewItem(0);

    parent_node->Append(std::make_unique<ParamsNode>(parent_node, sub_group_name, icon_name, m_ctrl));
    const wxDataViewItem  sub_group_item((void*)parent_node->GetChildren().back().get());

    ItemAdded(parent, sub_group_item);
    return sub_group_item;
}

wxDataViewItem ParamsModel::AppendParam(wxDataViewItem      parent,
                                        ParamType           param_type,
                                        const std::string&  param_key)
{
    ParamsNode* parent_node = static_cast<ParamsNode*>(parent.GetID());
    if (!parent_node)
        return wxDataViewItem(0);

    parent_node->Append(std::make_unique<ParamsNode>(parent_node, param_type, param_key, m_ctrl));

    const wxDataViewItem  child_item((void*)parent_node->GetChildren().back().get());

    ItemAdded(parent, child_item);
    return child_item;
}

wxString ParamsModel::GetParamName(wxDataViewItem item)
{
    if (item.IsOk()) {
        ParamsNode* node = static_cast<ParamsNode*>(item.GetID());
        if (node->IsParamNode())
            return node->text;
    }
    return wxEmptyString;
}

std::string ParamsModel::GetParamKey(wxDataViewItem item)
{
    if (item.IsOk()) {
        ParamsNode* node = static_cast<ParamsNode*>(item.GetID());
        return node->param_key;
    }
    return std::string();
}

std::string ParamsModel::GetTopLevelCategory(wxDataViewItem item)
{
    if (item.IsOk()) {
        ParamsNode* node = static_cast<ParamsNode*>(item.GetID());
        while (!node->IsGroupNode())
            node = node->GetParent();
        return node->text.ToStdString();
    }
    return std::string();
}

void ParamsModel::RefreshSearch(const wxString& search_text)
{
    if (!m_currently_searching) { // if not currently searching, save expansion state for all items
        for (const auto& node : m_group_nodes)
            node->StartSearch();
        m_currently_searching = true;
    }

    for (const auto& node : m_group_nodes)
        node->RefreshSearch(search_text); //Enable/Disable node based on search

    Cleared(); //Reload the model into the control

    for (const auto& node : m_group_nodes) // (re)expand all
        m_ctrl->ExpandChildren(wxDataViewItem(node.get()));
}

void ParamsModel::FinishSearch()
{
    RefreshSearch("");
    Cleared();
    if (m_currently_searching) {
        for (const auto& node : m_group_nodes)
            node->FinishSearch();
        m_currently_searching = false;
    }
}

wxDataViewItem ParamsModel::Delete(const wxDataViewItem& item)
{
    auto ret_item = wxDataViewItem(nullptr);
    ParamsNode* node = static_cast<ParamsNode*>(item.GetID());
    if (!node)      // happens if item.IsOk()==false
        return ret_item;
    const bool is_item_enabled = node->IsEnabled();

    // first remove the node from the parent's array of children;
    // NOTE: m_group_nodes is only a vector of _pointers_
    //       thus removing the node from it doesn't result in freeing it
    ParamsNodePtrArray& children = node->GetChildren();
    // Delete all children
    while (!children.empty())
        Delete(wxDataViewItem(children.back().get()));

    auto node_parent = node->GetParent();

    ParamsNodePtrArray& parents_children = node_parent ? node_parent->GetChildren() : m_group_nodes;
    auto it = find_if(parents_children.begin(), parents_children.end(),
                                                   [node](std::unique_ptr<ParamsNode>& child) { return child.get() == node; });
    assert(it != parents_children.end());
    it = parents_children.erase(it);

    if (it != parents_children.end())
        ret_item = wxDataViewItem(it->get());

    wxDataViewItem parent(node_parent);
    // set m_container to FALSE if parent has no child
    if (node_parent) {
#ifndef __WXGTK__
        if (node_parent->GetChildren().empty())
            node_parent->SetContainer(false);
#endif //__WXGTK__
        ret_item = parent;
    }

    // Orca: notify enabled item only, because disabled items have already been removed from UI,
    // so attempt to notify it cases a crash.
    if (is_item_enabled) {
        // notify control
        ItemDeleted(parent, item);
    }
    return ret_item;
}

void ParamsModel::Clear()
{
    while (!m_group_nodes.empty())
        Delete(wxDataViewItem(m_group_nodes.back().get()));
}

void ParamsModel::GetValue(wxVariant& variant, const wxDataViewItem& item, unsigned int col) const
{
    assert(item.IsOk());

    ParamsNode* node = static_cast<ParamsNode*>(item.GetID());
    if (col == (unsigned int)0)
#ifdef __linux__
//        variant << wxDataViewIconText(node->GetFormattedText(), get_bmp_bundle(node->icon_name)->GetIconFor(m_ctrl->GetParent())); //TODO: update to bundle with wx update
    {
        wxIcon icon;
        icon.CopyFromBitmap(create_scaled_bitmap(node->icon_name, m_ctrl->GetParent()));
        variant << wxDataViewIconText(node->GetFormattedText(), icon);
    }
#else
//        variant << DataViewBitmapText(node->GetFormattedText(), get_bmp_bundle(node->icon_name)->GetBitmapFor(m_ctrl->GetParent())); //TODO: update to bundle with wx update
        variant << DataViewBitmapText(node->GetFormattedText(), create_scaled_bitmap(node->icon_name, m_ctrl->GetParent()));
#endif //__linux__
    else
        wxLogError("DiffModel::GetValue: wrong column %d", col);
}

bool ParamsModel::SetValue(const wxVariant& variant, const wxDataViewItem& item, unsigned int col)
{
    assert(item.IsOk());

    ParamsNode* node = static_cast<ParamsNode*>(item.GetID());
    if (col == (unsigned int)0) {
#ifdef __linux__
        wxDataViewIconText data;
        data << variant;
        node->icon = data.GetIcon();
#else
        DataViewBitmapText data;
        data << variant;
        node->icon = data.GetBitmap();
#endif
        node->text = data.GetText();
        return true;
    }

    wxLogError("DiffModel::SetValue: wrong column");
    return false;
}

wxDataViewItem ParamsModel::GetParent(const wxDataViewItem&item) const
{
    // the invisible root node has no parent
    if (!item.IsOk())
        return wxDataViewItem(nullptr);

    ParamsNode* node = static_cast<ParamsNode*>(item.GetID());

    if (node->IsGroupNode())
        return wxDataViewItem(nullptr);

    return wxDataViewItem((void*)node->GetParent());
}

bool ParamsModel::IsContainer(const wxDataViewItem& item) const
{
    // the invisble root node can have children
    if (!item.IsOk())
        return true;

    ParamsNode* node = static_cast<ParamsNode*>(item.GetID());
    return node->IsContainer();
}
unsigned int ParamsModel::GetChildren(const wxDataViewItem& parent, wxDataViewItemArray& array) const
{
    ParamsNode* parent_node = (ParamsNode*)parent.GetID();

    if (parent_node == nullptr) {
        for (const auto& group : m_group_nodes)
            if (group->IsEnabled())
                array.Add(wxDataViewItem((void*)group.get()));
    }
    else  {
        const ParamsNodePtrArray& children = parent_node->GetChildren();
        for (const std::unique_ptr<ParamsNode>& child : children)
            if (child->IsEnabled())
                array.Add(wxDataViewItem((void*)child.get()));
    }

    return array.Count();
}
unsigned int ParamsModel::GetColumnCount() const { return 1; }
wxString     ParamsModel::GetColumnType(unsigned int col) const {
#ifdef __linux__
    return wxT("wxDataViewIconText");
#else
    return wxT("DataViewBitmapText");
#endif
}

// ----------------------------------------------------------------------------
//                  ParamsViewCtrl
// ----------------------------------------------------------------------------

ParamsViewCtrl::ParamsViewCtrl(wxWindow *parent, wxSize size)
    : wxDataViewCtrl(parent, wxID_ANY, wxDefaultPosition, size, wxDV_SINGLE | wxDV_NO_HEADER// | wxDV_ROW_LINES
#ifdef _WIN32
        | wxBORDER_SIMPLE
#endif
    ),
    m_em_unit(em_unit(parent))
{
    wxGetApp().UpdateDVCDarkUI(this);

    model = new ParamsModel();
    this->AssociateModel(model);
    model->SetAssociatedControl(this);

#ifdef __linux__
    wxDataViewIconTextRenderer* rd = new wxDataViewIconTextRenderer();
#ifdef SUPPORTS_MARKUP
    rd->EnableMarkup(true);
#endif
    wxDataViewColumn* column = new wxDataViewColumn("", rd, 0, 20 * m_em_unit, wxALIGN_TOP, wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_CELL_INERT);
#else
    wxDataViewColumn* column = new wxDataViewColumn("", new BitmapTextRenderer(true, wxDATAVIEW_CELL_INERT), 0, 20 * m_em_unit, wxALIGN_TOP, wxDATAVIEW_COL_RESIZABLE);
#endif //__linux__
    this->AppendColumn(column);
    this->SetExpanderColumn(column);
}

wxDataViewItem ParamsViewCtrl::AppendGroup(const wxString& group_name, const std::string& icon_name)
{
    return model->AppendGroup(group_name, icon_name);
}

wxDataViewItem ParamsViewCtrl::AppendSubGroup(  wxDataViewItem      parent,
                                                const wxString&     sub_group_name,
                                                const std::string&  icon_name)
{
    return model->AppendSubGroup(parent, sub_group_name, icon_name);
}

wxDataViewItem ParamsViewCtrl::AppendParam( wxDataViewItem      parent,
                                            ParamType           param_type,
                                            const std::string&  param_key)
{
    return model->AppendParam(parent, param_type, param_key);
}

wxString ParamsViewCtrl::GetValue(wxDataViewItem item)
{
    return model->GetParamName(item);
}

wxString ParamsViewCtrl::GetSelectedValue()
{
    return model->GetParamName(this->GetSelection());
}

std::string ParamsViewCtrl::GetSelectedParamKey()
{
    return model->GetParamKey(this->GetSelection());
}

std::string ParamsViewCtrl::GetSelectedTopLevelCategory()
{
    return model->GetTopLevelCategory(this->GetSelection());
}

void ParamsViewCtrl::CheckAndDeleteIfEmpty(wxDataViewItem item)
{
    wxDataViewItemArray children;
    model->GetChildren(item, children);
    if (children.IsEmpty())
        model->Delete(item);
}

void ParamsViewCtrl::Clear()
{
    model->Clear();
}

void ParamsViewCtrl::Rescale(int em/* = 0*/)
{
//    model->Rescale();
    Refresh();
}
}}    // namespace Slic3r::GUI
