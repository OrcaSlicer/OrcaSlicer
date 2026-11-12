#include "UnsavedChangesDialog.hpp"

#include <climits>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <boost/algorithm/string/replace.hpp>
#include <cassert>
#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include "slic3r/GUI/Event.hpp"
#include <map>
#include "libslic3r/Preset.hpp"
#include "libslic3r/Config.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoBase.hpp"
#include <memory>
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/SettingsIndex.hpp"
#include <set>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
#include <boost/algorithm/string.hpp>

#include <wx/colour.h>
#include <wx/dataview.h>
#include <wx/log.h>
#include <wx/gdicmn.h>
#include <wx/dvrenderers.h>
#include <wx/panel.h>
#include <wx/string.h>
#include <wx/event.h>
#include <wx/stattext.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/settings.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>
#include <wx/variant.h>
#include <wx/window.h>
#include <wx/toplevel.h>
#include <wx/statline.h>
#include <wx/tglbtn.h>
#include <wx/types.h>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Color.hpp"
#include "format.hpp"
#include "ConfigValueFormatter.hpp"
#include "GUI_App.hpp"
#include "OptionsGroup.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "ExtraRenderers.hpp"
#include "wxExtensions.hpp"
#include "SavePresetDialog.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"

#include "PresetComboBoxes.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"

#ifdef __linux__
#define wxLinux true
#else
#define wxLinux false
#endif

namespace Slic3r {

namespace GUI {

wxDEFINE_EVENT(EVT_DIFF_DIALOG_TRANSFER, SimpleEvent);

static PresetCollection* get_preset_collection(Preset::Type type, PresetBundle* preset_bundle = nullptr);


// ----------------------------------------------------------------------------
//                  ModelNode: a node inside DiffModel
// ----------------------------------------------------------------------------

static const std::map<Preset::Type, std::string> type_icon_names = {
    {Preset::TYPE_PRINT,        "cog"           },
    {Preset::TYPE_SLA_PRINT,    "cog"           },
    {Preset::TYPE_FILAMENT,     "spool"         },
    {Preset::TYPE_SLA_MATERIAL, "blank_16"      },
    {Preset::TYPE_PRINTER,      "printer"       },
};

static std::string get_icon_name(Preset::Type type, PrinterTechnology pt) {
    return pt == ptSLA && type == Preset::TYPE_PRINTER ? "sla_printer" : type_icon_names.at(type);
}

static std::string def_text_color()
{
    wxColour def_colour = wxGetApp().get_label_clr_default();//wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    return encode_color(ColorRGB(def_colour.Red(), def_colour.Green(), def_colour.Blue()));
}
static std::string grey     = "#808080";
static std::string orange   = "#ed6b21";

static void color_string(wxString& str, const std::string& color)
{
#if defined(SUPPORTS_MARKUP) && !defined(__APPLE__)
    str = from_u8((boost::format("<span color=\"%1%\">%2%</span>") % color % into_u8(str)).str());
#endif
}

static void make_string_bold(wxString& str)
{
#if defined(SUPPORTS_MARKUP) && !defined(__APPLE__)
    str = from_u8((boost::format("<b>%1%</b>") % into_u8(str)).str());
#endif
}

// preset(root) node
ModelNode::ModelNode(Preset::Type preset_type, wxWindow* parent_win, const wxString& text, const std::string& icon_name) :
    m_parent_win(parent_win),
    m_parent(nullptr),
    m_preset_type(preset_type),
    m_icon_name(icon_name),
    m_text(text)
{
    UpdateIcons();
}

// category node
ModelNode::ModelNode(ModelNode* parent, const wxString& text, const std::string& icon_name) :
    m_parent_win(parent->m_parent_win),
    m_parent(parent),
    m_icon_name(icon_name),
    m_text(text)
{
    UpdateIcons();
}

// group node
ModelNode::ModelNode(ModelNode* parent, const wxString& text) :
    m_parent_win(parent->m_parent_win),
    m_parent(parent),
    m_icon_name("node_dot"),
    m_text(text)
{
    UpdateIcons();
}

#ifdef __linux__
wxIcon ModelNode::get_bitmap(const wxString& color)
#else
wxBitmap ModelNode::get_bitmap(const wxString& color)
#endif // __linux__
{
    /* It's supposed that standard size of an icon is 48px*16px for 100% scaled display.
     * So set sizes for solid_colored icons used for filament preset
     * and scale them in respect to em_unit value
     */
    const double em = em_unit(m_parent_win);
    const int icon_width    = lround(6.4 * em);
    const int icon_height   = lround(1.6 * em);

    BitmapCache bmp_cache;
    ColorRGB rgb;
    decode_color(into_u8(color), rgb);
    // there is no need to scale created solid bitmap
#ifndef __linux__
    return bmp_cache.mksolid(icon_width, icon_height, rgb, true);
#else
    wxIcon icon;
    icon.CopyFromBitmap(bmp_cache.mksolid(icon_width, icon_height, rgb, true));
    return icon;
#endif // __linux__
}

// option node
ModelNode::ModelNode(ModelNode* parent, const wxString& text, const wxString& old_value, const wxString& new_value) :
    m_parent(parent),
    m_old_color(old_value.StartsWith("#") ? old_value : ""),
    m_new_color(new_value.StartsWith("#") ? new_value : ""),
    m_icon_name("empty"),
    m_text(text),
    m_old_value(old_value),
    m_new_value(new_value),
    m_container(false)
{
    // check if old/new_value is color
    if (m_old_color.IsEmpty()) {
        if (!m_new_color.IsEmpty())
            m_old_value = _L("Undefined");
    }
    else {
        m_old_color_bmp = get_bitmap(m_old_color);
        m_old_value.Clear();
    }

    if (m_new_color.IsEmpty()) {
        if (!m_old_color.IsEmpty())
            m_new_value = _L("Undefined");
    }
    else {
        m_new_color_bmp = get_bitmap(m_new_color);
        m_new_value.Clear();
    }

    // "color" strings
    color_string(m_old_value, def_text_color());
    color_string(m_new_value, orange);

    UpdateIcons();
}

void ModelNode::UpdateEnabling()
{
    auto change_text_color = [](wxString& str, const std::string& clr_from, const std::string& clr_to)
    {
#if defined(SUPPORTS_MARKUP) && !defined(__APPLE__)
        std::string old_val = into_u8(str);
        boost::replace_all(old_val, clr_from, clr_to);
        str = from_u8(old_val);
#endif
    };

    if (!m_toggle) {
        change_text_color(m_text,      def_text_color(), grey);
        change_text_color(m_old_value, def_text_color(), grey);
        change_text_color(m_new_value, orange,grey);
    }
    else {
        change_text_color(m_text,      grey, def_text_color());
        change_text_color(m_old_value, grey, def_text_color());
        change_text_color(m_new_value, grey, orange);
    }
    // update icons for the colors
    UpdateIcons();
}

void ModelNode::UpdateIcons()
{
    // update icons for the colors, if any exists
    if (!m_old_color.IsEmpty())
        m_old_color_bmp = get_bitmap(m_toggle ? m_old_color : wxString::FromUTF8(grey.c_str()));
    if (!m_new_color.IsEmpty())
        m_new_color_bmp = get_bitmap(m_toggle ? m_new_color : wxString::FromUTF8(grey.c_str()));

    // update main icon, if any exists
    if (m_icon_name.empty())
        return;

#ifdef __linux__
    m_icon.CopyFromBitmap(create_scaled_bitmap(m_icon_name, m_parent_win, 16, !m_toggle));
#else
    m_icon = create_scaled_bitmap(m_icon_name, m_parent_win, 16, !m_toggle);
#endif //__linux__
}


// ----------------------------------------------------------------------------
//                          DiffModel
// ----------------------------------------------------------------------------

DiffModel::DiffModel(wxWindow* parent) :
    m_parent_win(parent)
{
}

wxDataViewItem DiffModel::AddPreset(Preset::Type type, wxString preset_name, PrinterTechnology pt)
{
    // "color" strings
    color_string(preset_name, def_text_color());
    make_string_bold(preset_name);

    auto preset = new ModelNode(type, m_parent_win, preset_name, get_icon_name(type, pt));
    m_preset_nodes.emplace_back(preset);

    wxDataViewItem child((void*)preset);
    wxDataViewItem parent(nullptr);

    ItemAdded(parent, child);
    return child;
}

ModelNode* DiffModel::AddOption(ModelNode* group_node, wxString option_name, wxString old_value, wxString new_value)
{
    group_node->Append(std::make_unique<ModelNode>(group_node, option_name, old_value, new_value));
    ModelNode* option = group_node->GetChildren().back().get();
    wxDataViewItem group_item = wxDataViewItem((void*)group_node);
    ItemAdded(group_item, wxDataViewItem((void*)option));

    m_ctrl->Expand(group_item);
    return option;
}

ModelNode* DiffModel::AddOptionWithGroup(ModelNode* category_node, wxString group_name, wxString option_name, wxString old_value, wxString new_value)
{
    category_node->Append(std::make_unique<ModelNode>(category_node, group_name));
    ModelNode* group_node = category_node->GetChildren().back().get();
    ItemAdded(wxDataViewItem((void*)category_node), wxDataViewItem((void*)group_node));

    return AddOption(group_node, option_name, old_value, new_value);
}

ModelNode* DiffModel::AddOptionWithGroupAndCategory(ModelNode* preset_node, wxString category_name, wxString group_name,
                                            wxString option_name, wxString old_value, wxString new_value, const std::string category_icon_name)
{
    preset_node->Append(std::make_unique<ModelNode>(preset_node, category_name, category_icon_name));
    ModelNode* category_node = preset_node->GetChildren().back().get();
    ItemAdded(wxDataViewItem((void*)preset_node), wxDataViewItem((void*)category_node));

    return AddOptionWithGroup(category_node, group_name, option_name, old_value, new_value);
}

wxDataViewItem DiffModel::AddOption(Preset::Type type, wxString category_name, wxString group_name, wxString option_name,
                                              wxString old_value, wxString new_value, const std::string category_icon_name)
{
    // "color" strings
    color_string(category_name, def_text_color());
    color_string(group_name,    def_text_color());
    color_string(option_name,   def_text_color());

    // "make" strings bold
    make_string_bold(category_name);
    make_string_bold(group_name);

    // add items
    for (std::unique_ptr<ModelNode>& preset : m_preset_nodes)
        if (preset->type() == type)
        {
            for (std::unique_ptr<ModelNode> &category : preset->GetChildren())
                if (category->text() == category_name)
                {
                    for (std::unique_ptr<ModelNode> &group : category->GetChildren())
                        if (group->text() == group_name)
                            return wxDataViewItem((void*)AddOption(group.get(), option_name, old_value, new_value));

                    return wxDataViewItem((void*)AddOptionWithGroup(category.get(), group_name, option_name, old_value, new_value));
                }

            return wxDataViewItem((void*)AddOptionWithGroupAndCategory(preset.get(), category_name, group_name, option_name, old_value, new_value, category_icon_name));
        }

    return wxDataViewItem(nullptr);
}

static void update_children(ModelNode* parent)
{
    if (parent->IsContainer()) {
        bool toggle = parent->IsToggled();
        for (std::unique_ptr<ModelNode> &child : parent->GetChildren()) {
            child->Toggle(toggle);
            child->UpdateEnabling();
            update_children(child.get());
        }
    }
}

static void update_parents(ModelNode* node)
{
    ModelNode* parent = node->GetParent();
    if (parent) {
        bool toggle = false;
        for (std::unique_ptr<ModelNode> &child : parent->GetChildren()) {
            if (child->IsToggled()) {
                toggle = true;
                break;
            }
        }
        parent->Toggle(toggle);
        parent->UpdateEnabling();
        update_parents(parent);
    }
}

void DiffModel::UpdateItemEnabling(wxDataViewItem item)
{
    assert(item.IsOk());
    ModelNode* node = static_cast<ModelNode*>(item.GetID());
    node->UpdateEnabling();

    update_children(node);
    update_parents(node);
}

bool DiffModel::IsEnabledItem(const wxDataViewItem& item)
{
    assert(item.IsOk());
    ModelNode* node = static_cast<ModelNode*>(item.GetID());
    return node->IsToggled();
}

void DiffModel::GetValue(wxVariant& variant, const wxDataViewItem& item, unsigned int col) const
{
    assert(item.IsOk());

    ModelNode* node = static_cast<ModelNode*>(item.GetID());
    switch (col)
    {
    case colToggle:
        variant = node->m_toggle;
        break;
#ifdef __linux__
    case colIconText:
        variant << wxDataViewIconText(node->m_text, node->m_icon);
        break;
    case colOldValue:
        variant << wxDataViewIconText(node->m_old_value, node->m_old_color_bmp);
        break;
    case colNewValue:
        variant << wxDataViewIconText(node->m_new_value, node->m_new_color_bmp);
        break;
#else
    case colIconText:
        variant << DataViewBitmapText(node->m_text, node->m_icon);
        break;
    case colOldValue:
        variant << DataViewBitmapText(node->m_old_value, node->m_old_color_bmp);
        break;
    case colNewValue:
        variant << DataViewBitmapText(node->m_new_value, node->m_new_color_bmp);
        break;
#endif //__linux__

    default:
        wxLogError("DiffModel::GetValue: wrong column %d", col);
    }
}

bool DiffModel::SetValue(const wxVariant& variant, const wxDataViewItem& item, unsigned int col)
{
    assert(item.IsOk());

    ModelNode* node = static_cast<ModelNode*>(item.GetID());
    switch (col)
    {
    case colToggle:
        node->m_toggle = variant.GetBool();
        return true;
#ifdef __linux__
    case colIconText: {
        wxDataViewIconText data;
        data << variant;
        node->m_icon = data.GetIcon();
        node->m_text = data.GetText();
        return true; }
    case colOldValue: {
        wxDataViewIconText data;
        data << variant;
        node->m_old_color_bmp   = data.GetIcon();
        node->m_old_value       = data.GetText();
        return true; }
    case colNewValue: {
        wxDataViewIconText data;
        data << variant;
        node->m_new_color_bmp   = data.GetIcon();
        node->m_new_value       = data.GetText();
        return true; }
#else
    case colIconText: {
        DataViewBitmapText data;
        data << variant;
        node->m_icon = data.GetBitmap();
        node->m_text = data.GetText();
        return true; }
    case colOldValue: {
        DataViewBitmapText data;
        data << variant;
        node->m_old_color_bmp   = data.GetBitmap();
        node->m_old_value       = data.GetText();
        return true; }
    case colNewValue: {
        DataViewBitmapText data;
        data << variant;
        node->m_new_color_bmp   = data.GetBitmap();
        node->m_new_value       = data.GetText();
        return true; }
#endif //__linux__
    default:
        wxLogError("DiffModel::SetValue: wrong column");
    }
    return false;
}

bool DiffModel::IsEnabled(const wxDataViewItem& item, unsigned int col) const
{
    assert(item.IsOk());
    if (col == colToggle)
        return true;

    // disable unchecked nodes
    return (static_cast<ModelNode*>(item.GetID()))->IsToggled();
}

wxDataViewItem DiffModel::GetParent(const wxDataViewItem& item) const
{
    // the invisible root node has no parent
    if (!item.IsOk())
        return wxDataViewItem(nullptr);

    ModelNode* node = static_cast<ModelNode*>(item.GetID());

    if (node->IsRoot())
        return wxDataViewItem(nullptr);

    return wxDataViewItem((void*)node->GetParent());
}

bool DiffModel::IsContainer(const wxDataViewItem& item) const
{
    // the invisble root node can have children
    if (!item.IsOk())
        return true;

    ModelNode* node = static_cast<ModelNode*>(item.GetID());
    return node->IsContainer();
}

unsigned int DiffModel::GetChildren(const wxDataViewItem& parent, wxDataViewItemArray& array) const
{
    ModelNode* parent_node = (ModelNode*)parent.GetID();

    const ModelNodePtrArray& children = parent_node ? parent_node->GetChildren() : m_preset_nodes;
    for (const std::unique_ptr<ModelNode>& child : children)
        array.Add(wxDataViewItem((void*)child.get()));

    return array.Count();
}


wxString DiffModel::GetColumnType(unsigned int col) const
{
    switch (col)
    {
    case colToggle:
        return "bool";
    case colIconText:
    case colOldValue:
    case colNewValue:
    default:
        return "DataViewBitmapText";//"string";
    }
}

static void rescale_children(ModelNode* parent)
{
    if (parent->IsContainer()) {
        for (std::unique_ptr<ModelNode> &child : parent->GetChildren()) {
            child->UpdateIcons();
            rescale_children(child.get());
        }
    }
}

void DiffModel::Rescale()
{
    for (std::unique_ptr<ModelNode> &node : m_preset_nodes) {
        node->UpdateIcons();
        rescale_children(node.get());
    }
}

wxDataViewItem DiffModel::Delete(const wxDataViewItem& item)
{
    auto ret_item = wxDataViewItem(nullptr);
    ModelNode* node = static_cast<ModelNode*>(item.GetID());
    if (!node)      // happens if item.IsOk()==false
        return ret_item;

    // first remove the node from the parent's array of children;
    // NOTE: m_preset_nodes is only a vector of _pointers_
    //       thus removing the node from it doesn't result in freeing it
    ModelNodePtrArray& children = node->GetChildren();
    // Delete all children
    while (!children.empty())
        Delete(wxDataViewItem(children.back().get()));

    auto node_parent = node->GetParent();
    wxDataViewItem parent(node_parent);

    ModelNodePtrArray& parents_children = node_parent ? node_parent->GetChildren() : m_preset_nodes;
    auto it = find_if(parents_children.begin(), parents_children.end(),
                      [node](std::unique_ptr<ModelNode>& child) { return child.get() == node; });
    assert(it != parents_children.end());
    it = parents_children.erase(it);

    if (it != parents_children.end())
        ret_item = wxDataViewItem(it->get());

    // set m_container to FALSE if parent has no child
    if (node_parent) {
#ifndef __WXGTK__
        if (node_parent->GetChildCount() == 0)
            node_parent->m_container = false;
#endif //__WXGTK__
        ret_item = parent;
    }

    // notify control
    ItemDeleted(parent, item);
    return ret_item;
}

void DiffModel::Clear()
{
    while (!m_preset_nodes.empty())
        Delete(wxDataViewItem(m_preset_nodes.back().get()));
}


// Top-down order of the sidebar's preset sections.
static const Preset::Type PRESET_TYPE_ORDER[] = {
    Preset::TYPE_PRINTER, Preset::TYPE_FILAMENT, Preset::TYPE_SLA_MATERIAL, Preset::TYPE_PRINT, Preset::TYPE_SLA_PRINT };

static int preset_type_rank(Preset::Type type)
{
    return int(std::find(std::begin(PRESET_TYPE_ORDER), std::end(PRESET_TYPE_ORDER), type) - std::begin(PRESET_TYPE_ORDER));
}

// opt_key -> its position on the settings pages, with the tabs taken in PRESET_TYPE_ORDER.
static std::map<std::string, int> build_tab_order_map()
{
    std::vector<Tab *> tabs = wxGetApp().tabs_list;
    std::stable_sort(tabs.begin(), tabs.end(), [](Tab *a, Tab *b) { return preset_type_rank(a->type()) < preset_type_rank(b->type()); });

    std::map<std::string, int> order;
    for (Tab *tab : tabs)
        for (const PageShp &page : tab->pages())
            for (const auto &optgroup : page->m_optgroups)
                for (const Line &line : optgroup->get_lines())
                    for (const Option &opt : line.get_options())
                        order.emplace(get_pure_opt_key(opt.opt_id), int(order.size()));
    return order;
}

static int order_of(const std::map<std::string, int> &tab_order, const std::string &opt_key)
{
    auto it = tab_order.find(get_pure_opt_key(opt_key));
    return it != tab_order.end() ? it->second : INT_MAX;
}

// Same-named categories on different tabs must stay separate, so the keys carry the preset type.
static wxString preset_item_cat_key(const PresetItem &pi) { return wxString::Format("%d:", int(pi.type)) + pi.category_name; }
static wxString preset_item_grp_key(const PresetItem &pi) { return preset_item_cat_key(pi) + ":" + pi.group_name; }

// ----------------------------------------------------------------------------
//                  DiffViewCtrl
// ----------------------------------------------------------------------------

DiffViewCtrl::DiffViewCtrl(wxWindow* parent, wxSize size)
    : wxDataViewCtrl(parent, wxID_ANY, wxDefaultPosition, size, wxDV_VARIABLE_LINE_HEIGHT | wxDV_ROW_LINES
#ifdef _WIN32
        | wxBORDER_SIMPLE
#endif
    ),
    m_em_unit(em_unit(parent))
{
    SetBackgroundColour(wxColour("#FFFFFF"));
    wxGetApp().UpdateDVCDarkUI(this);

    model = new DiffModel(parent);
    this->AssociateModel(model);
    model->SetAssociatedControl(this);

    this->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, &DiffViewCtrl::context_menu, this);
    this->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,    &DiffViewCtrl::context_menu, this);
    this->Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, &DiffViewCtrl::item_value_changed, this);
}

DiffViewCtrl::~DiffViewCtrl() { 
    this->AssociateModel(nullptr);
    delete model;
}

void DiffViewCtrl::AppendBmpTextColumn(const wxString& label, unsigned model_column, int width, bool set_expander/* = false*/)
{
    m_columns_width.emplace(this->GetColumnCount(), width);
#ifdef __linux__
    wxDataViewIconTextRenderer* rd = new wxDataViewIconTextRenderer();
#ifdef SUPPORTS_MARKUP
    rd->EnableMarkup(true);
#endif
    wxDataViewColumn* column = new wxDataViewColumn(label, rd, model_column, width * m_em_unit, wxALIGN_TOP, wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_CELL_INERT);
#else
    wxDataViewColumn* column = new wxDataViewColumn(label, new BitmapTextRenderer(true, wxDATAVIEW_CELL_INERT), model_column, width * m_em_unit, wxALIGN_TOP, wxDATAVIEW_COL_RESIZABLE);
#endif //__linux__
    this->AppendColumn(column);
    if (set_expander)
        this->SetExpanderColumn(column);

}

void DiffViewCtrl::AppendToggleColumn_(const wxString& label, unsigned model_column, int width)
{
    m_columns_width.emplace(this->GetColumnCount(), width);
    AppendToggleColumn(label, model_column, wxDATAVIEW_CELL_ACTIVATABLE, width * m_em_unit);
}

void DiffViewCtrl::Rescale(int em /*= 0*/)
{
    if (em > 0) {
        for (auto item : m_columns_width)
            GetColumn(item.first)->SetWidth(item.second * em);
        m_em_unit = em;
    }

    model->Rescale();
    Refresh();
}


void DiffViewCtrl::Append(  const std::string& opt_key, Preset::Type type,
                            wxString category_name, wxString group_name, wxString option_name,
                            wxString old_value, wxString new_value, const std::string category_icon_name)
{
    ItemData item_data = { opt_key, option_name, old_value, new_value, type };

    wxString old_val = get_short_string(item_data.old_val);
    wxString new_val = get_short_string(item_data.new_val);
    if (old_val != item_data.old_val || new_val != item_data.new_val)
        item_data.is_long = true;

    m_items_map.emplace(model->AddOption(type, category_name, group_name, option_name, old_val, new_val, category_icon_name), item_data);

}

void DiffViewCtrl::Clear()
{
    model->Clear();
    m_items_map.clear();
}

wxString DiffViewCtrl::get_short_string(wxString full_string)
{
    size_t max_len = 30;
    if (full_string.IsEmpty() || full_string.StartsWith("#") ||
        (full_string.Find("\n") == wxNOT_FOUND && full_string.Length() < max_len))
        return full_string;

    m_has_long_strings = true;

    int n_pos = full_string.Find("\n");
    if (n_pos != wxNOT_FOUND && n_pos < (int)max_len)
        max_len = n_pos;

    full_string.Truncate(max_len);
    return full_string + dots;
}

void DiffViewCtrl::context_menu(wxDataViewEvent& event)
{
    if (!m_has_long_strings)
        return;

    wxDataViewItem item = event.GetItem();
    if (!item) {
        wxPoint mouse_pos = wxGetMousePosition() - this->GetScreenPosition();
        wxDataViewColumn* col = nullptr;
        this->HitTest(mouse_pos, item, col);

        if (!item)
            item = this->GetSelection();

        if (!item)
            return;
    }

    auto it = m_items_map.find(item);
    if (it == m_items_map.end() || !it->second.is_long)
        return;

    size_t column_cnt = this->GetColumnCount();
    const wxString old_value_header = this->GetColumn(column_cnt - 2)->GetTitle();
    const wxString new_value_header = this->GetColumn(column_cnt - 1)->GetTitle();
    FullCompareDialog(it->second.opt_name, it->second.old_val, it->second.new_val,
                      old_value_header, new_value_header).ShowModal();

#ifdef __WXOSX__
    wxWindow* parent = this->GetParent();
    if (parent && parent->IsShown()) {
        // if this dialog is shown it have to be Hide and show again to be placed on the very Top of windows
        parent->Hide();
        parent->Show();
    }
#endif // __WXOSX__
}

void DiffViewCtrl::item_value_changed(wxDataViewEvent& event)
{
    if (event.GetColumn() != DiffModel::colToggle)
        return;

    wxDataViewItem item = event.GetItem();

    model->UpdateItemEnabling(item);
    Refresh();

    // update an enabling of the "save/move" buttons
    m_empty_selection = selected_options().empty();
}

bool DiffViewCtrl::has_unselected_options()
{
    for (auto item : m_items_map)
        if (!model->IsEnabledItem(item.first))
            return true;

    return false;
}

std::vector<std::string> DiffViewCtrl::options(Preset::Type type, bool selected)
{
    std::vector<std::string> ret;

    for (auto item : m_items_map) {
        if (item.second.type == type && model->IsEnabledItem(item.first) == selected)
            ret.emplace_back(get_pure_opt_key(item.second.opt_key));
    }

    return ret;
}

std::vector<std::string> DiffViewCtrl::selected_options()
{
    std::vector<std::string> ret;

    for (auto item : m_items_map)
        if (model->IsEnabledItem(item.first))
            ret.emplace_back(get_pure_opt_key(item.second.opt_key));

    return ret;
}


//------------------------------------------
//          UnsavedChangesDialog
//------------------------------------------

static std::string none{"none"};
#define UNSAVE_CHANGE_DIALOG_SCROLL_WINDOW_SIZE wxSize(FromDIP(490), FromDIP(374))
#define UNSAVE_CHANGE_DIALOG_ACTION_LINE_SIZE wxSize(FromDIP(490), -1)
#define UNSAVE_CHANGE_DIALOG_FIRST_VALUE_WIDTH FromDIP(190)
#define UNSAVE_CHANGE_DIALOG_VALUE_WIDTH FromDIP(150)
#define UNSAVE_CHANGE_DIALOG_VALUE_TEXT_WIDTH (UNSAVE_CHANGE_DIALOG_VALUE_WIDTH - FromDIP(10))
#define UNSAVE_CHANGE_DIALOG_CATEGORY_INDENT FromDIP(23)
#define UNSAVE_CHANGE_DIALOG_GROUP_INDENT FromDIP(37)
#define UNSAVE_CHANGE_DIALOG_OPTION_INDENT FromDIP(51)
#define UNSAVE_CHANGE_DIALOG_ICON_WIDTH FromDIP(24)
#define UNSAVE_CHANGE_DIALOG_ITEM_HEIGHT FromDIP(24)
#define UNSAVE_CHANGE_DIALOG_SYNC_CELL_PADDING FromDIP(8)
#define UNSAVE_CHANGE_DIALOG_TYPE_INDENT FromDIP(8)
// A row's checkbox and the gap after it
#define UNSAVE_CHANGE_DIALOG_CHECK_WIDTH FromDIP(22)
#define UNSAVE_CHANGE_DIALOG_BUTTON_SIZE wxSize(FromDIP(70), FromDIP(24))

#define THUMB_COLOR wxColor(196, 196, 196)
#define GREY900 wxColour(38, 46, 48)
#define GREY700 wxColour(107,107,107)
#define GREY400 wxColour(206,206,206)
#define GREY300 wxColour(238,238,238)
#define GREY200 wxColour(248,248,248)


UnsavedChangesDialog::UnsavedChangesDialog(const wxString &caption, const wxString &header, const std::string &app_config_key, int act_buttons)
    : DPIDialog(static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                caption + ": " + _L("unsaved changes"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_app_config_key(app_config_key)
    , m_buttons(act_buttons)
{
    build(Preset::TYPE_INVALID, nullptr, "", header);
    this->CenterOnScreen();
    wxGetApp().UpdateDlgDarkUI(this);
}

UnsavedChangesDialog::UnsavedChangesDialog(const wxString &caption, const wxString &header, Preset::Type type, SyncExtruderParams params)
    : DPIDialog(static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                caption,
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_buttons(ActionButtons::SAVE | ActionButtons::DONT_SAVE)
{
    build(type, reinterpret_cast<PresetCollection*>(&params), "SyncExtruderParams", header);
    this->CenterOnScreen();
    wxGetApp().UpdateDlgDarkUI(this);
}

UnsavedChangesDialog::UnsavedChangesDialog(Preset::Type type, PresetCollection *dependent_presets, const std::string &new_selected_preset, bool no_transfer)
    : m_dependent_presets(dependent_presets)
    , m_new_selected_preset_name(new_selected_preset)
    , DPIDialog(static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Transfer or discard changes"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
{
    if (new_selected_preset.empty() || no_transfer)
        m_buttons &= ~ActionButtons::TRANSFER;
    build(type, dependent_presets, new_selected_preset);
    this->CenterOnScreen();
    wxGetApp().UpdateDlgDarkUI(this);
}


inline int UnsavedChangesDialog::ShowModal()
{
    auto choise_key = "save_preset_choise"; 
    auto choise     = wxGetApp().app_config->get(choise_key);
    long result = 0;
    if ((m_buttons & REMEMBER_CHOISE) && !choise.empty() && wxString(choise).ToLong(&result) && (1 << result) & (m_buttons | DONT_SAVE)) {
        const Action remembered = Action(result);
        // A remembered Save would otherwise skip the unresolved-plugin warning in save(). Show the
        // dialog when the presets being saved need it; remembered Transfer/Discard keep the shortcut.
        if (remembered != Action::Save || !selected_save_needs_plugin_warning()) {
            m_exit_action = remembered;
            return 0;
        }
    }
    int r = DPIDialog::ShowModal();
    if (r != wxID_CANCEL && dynamic_cast<::CheckBox*>(FindWindowById(wxID_APPLY))->GetValue()) {
        wxGetApp().app_config->set(choise_key, std::to_string(int(m_exit_action)));
    }
    return r;
}

void UnsavedChangesDialog::build(Preset::Type type, PresetCollection *dependent_presets, const std::string &new_selected_preset, const wxString &header)
{
    SetBackgroundColour(*wxWHITE);

    SyncExtruderParams *params = nullptr;
    if (new_selected_preset == "SyncExtruderParams") {
        params = reinterpret_cast<SyncExtruderParams *>(dependent_presets);
        dependent_presets = nullptr;
    }

    // The rows size the columns, so collect them before building the header.
    if (params)
        update_tree(type, params->config);
    else
        update_tree(type, dependent_presets);

    // The sync dialog has a value column per extruder, each fitted to its content once the table is built.
    const int value_columns = params ? int(params->titles.size()) : 2;
    m_value_width           = params ? wxDefaultCoord : UNSAVE_CHANGE_DIALOG_VALUE_WIDTH;
    if (params) {
        m_sync_columns.assign(value_columns + 1, {});
        m_first_value_width = m_content_width = wxDefaultCoord;
    } else {
        auto row_width = [this](const wxString &text, const wxFont &font, int indent) {
            int w = 0, h = 0;
            GetTextExtent(text, &w, &h, nullptr, nullptr, &font);
            return indent + w + FromDIP(16);
        };
        // Category headers span the row, so they widen the dialog rather than the label column.
        m_first_value_width = UNSAVE_CHANGE_DIALOG_FIRST_VALUE_WIDTH;
        int category_width  = 0;
        for (const PresetItem &pi : m_presetitems) {
            const int option_w = row_width(pi.option_name, ::Label::Body_13, UNSAVE_CHANGE_DIALOG_OPTION_INDENT) + (pi.has_icon ? UNSAVE_CHANGE_DIALOG_ICON_WIDTH : 0);
            m_first_value_width = std::max({m_first_value_width, option_w, row_width(pi.group_name, ::Label::Head_13, UNSAVE_CHANGE_DIALOG_GROUP_INDENT)});
            category_width      = std::max(category_width, row_width(pi.category_name, ::Label::Head_13, UNSAVE_CHANGE_DIALOG_CATEGORY_INDENT));
        }
        // Every row has its box before the label.
        m_first_value_width = std::min(m_first_value_width + UNSAVE_CHANGE_DIALOG_CHECK_WIDTH, FromDIP(420));
        category_width += UNSAVE_CHANGE_DIALOG_CHECK_WIDTH;
        m_content_width     = std::min(std::max(m_first_value_width + 2 * UNSAVE_CHANGE_DIALOG_VALUE_WIDTH, category_width), FromDIP(760));
    }

    // update_list() gives every category, group and option one ITEM_HEIGHT row, and every type one outside the sync
    // dialog, so this is the exact list height.
    std::set<int>      row_types;
    std::set<wxString> row_cats, row_grps;
    for (const PresetItem &pi : m_presetitems) {
        row_types.insert(int(pi.type));
        row_cats.insert(preset_item_cat_key(pi));
        row_grps.insert(preset_item_grp_key(pi));
    }
    const int  row_count       = int((params ? 0 : row_types.size()) + row_cats.size() + row_grps.size() + m_presetitems.size());
    const bool needs_scrollbar = row_count * UNSAVE_CHANGE_DIALOG_ITEM_HEIGHT > UNSAVE_CHANGE_DIALOG_SCROLL_WINDOW_SIZE.GetHeight();

    // Overlay scrollbars report a width of 0 and take no space.
    const int scrollbar_w = needs_scrollbar ? std::max(0, wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, this)) : 0;
    if (!params)
        m_content_width += scrollbar_w;

    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);

    m_top_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_top_line->SetBackgroundColour(wxColour(166, 169, 170));

    m_sizer_main->Add(m_top_line, 0, wxEXPAND, 0);

    m_sizer_main->Add(0, 0, 0, wxTOP, 20);

    m_action_line = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(m_content_width, UNSAVE_CHANGE_DIALOG_ACTION_LINE_SIZE.y), 0);
    m_action_line->SetFont(::Label::Body_13);
    m_action_line->SetForegroundColour(wxColour("#363636"));
    m_action_line->Wrap(-1);
    m_sizer_main->Add(m_action_line, 0, wxLEFT | wxRIGHT, 20);

    m_sizer_main->Add(0, 0, 0, wxTOP, 12);

    m_panel_tab = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(m_content_width, -1), wxTAB_TRAVERSAL);
    m_panel_tab->SetBackgroundColour(wxColour("#D9D9D9"));
    wxBoxSizer *m_sizer_tab = new wxBoxSizer(wxVERTICAL);

    m_table_top = new wxPanel(m_panel_tab, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_table_top->SetBackgroundColour(wxColour("#D9D9D9"));

    wxBoxSizer *m_sizer_top = new wxBoxSizer(wxHORIZONTAL);

    // m_sizer_top->Add(0, 0, 0, wxLEFT, UNSAVE_CHANGE_DIALOG_FIRST_VALUE_WIDTH);
    auto        m_panel_temp   = new wxPanel(m_table_top, wxID_ANY, wxDefaultPosition, wxSize(m_first_value_width, -1), wxTAB_TRAVERSAL);
    wxBoxSizer *top_title_temp_v = new wxBoxSizer(wxVERTICAL);
    if (!params)
        top_title_temp_v->SetMinSize(wxSize(UNSAVE_CHANGE_DIALOG_VALUE_WIDTH, -1));
    wxBoxSizer *top_title_temp_h = new wxBoxSizer(wxHORIZONTAL);

    // Tick-all boxes of the header: every change or copy (column -1), or in the sync dialog every copy to one column.
    std::vector<std::pair<size_t, int>> header_checks;
    auto add_header_check = [this, &header_checks](wxPanel *panel, wxBoxSizer *sizer, int column) {
        panel->SetBackgroundColour(wxColour("#D9D9D9"));
        const size_t k = add_all_check(panel);
        header_checks.emplace_back(k, column);
        sizer->Add(m_all_checks[k].first, 0, wxALIGN_CENTER | wxRIGHT | wxRESERVE_SPACE_EVEN_IF_HIDDEN, FromDIP(4));
    };
    add_header_check(m_panel_temp, top_title_temp_h, -1);
    static_temp_title            = new wxStaticText(m_panel_temp, wxID_ANY, _L("Settings"), wxDefaultPosition, wxDefaultSize, 0);
    static_temp_title->SetFont(::Label::Body_13);
    static_temp_title->Wrap(-1);
    static_temp_title->SetForegroundColour(wxColour("#363636"));
    top_title_temp_h->Add(static_temp_title, 0, wxALIGN_CENTER | wxBOTTOM | wxTOP, 5);
    // The corner box sits above the boxes of the outermost rows: the categories in the sync dialog, else the preset types.
    top_title_temp_v->Add(top_title_temp_h, 1, wxLEFT, params ? UNSAVE_CHANGE_DIALOG_CATEGORY_INDENT : UNSAVE_CHANGE_DIALOG_TYPE_INDENT);
    m_panel_temp->SetSizer(top_title_temp_v);
    m_panel_temp->Layout();
    m_sizer_top->Add(m_panel_temp, 1, wxALIGN_CENTER, 0);
    if (params)
        m_sync_columns[0].push_back(m_panel_temp);

    auto add_title = [this, m_sizer_top, params, &add_header_check](const wxString &label, bool highlight, int column) {
        // The first separator keeps its default width, except in the sync dialog, which aligns its header with the rows.
        auto separator = new wxPanel(m_table_top, wxID_ANY, wxDefaultPosition, !params && m_sizer_top->GetItemCount() == 1 ? wxDefaultSize : wxSize(1, -1), wxTAB_TRAVERSAL);
        separator->SetBackgroundColour(wxColour(172, 172, 172));
        m_sizer_top->Add(separator, 0, wxBOTTOM | wxEXPAND | wxTOP, 2);

        auto panel   = new wxPanel(m_table_top, wxID_ANY, wxDefaultPosition, wxSize(m_value_width, -1), wxTAB_TRAVERSAL);
        auto sizer_v = new wxBoxSizer(wxVERTICAL);
        auto sizer_h = new wxBoxSizer(wxHORIZONTAL);
        if (column >= 0)
            add_header_check(panel, sizer_h, column);
        auto title   = new wxStaticText(panel, wxID_ANY, label, wxDefaultPosition, wxDefaultSize, 0);
        title->SetFont(::Label::Body_13);
        title->Wrap(-1);
        title->SetForegroundColour(highlight ? wxGetApp().get_label_clr_modified() : wxColour("#363636"));
        sizer_h->Add(title, 0, wxALIGN_CENTER | wxBOTTOM | wxTOP, 5);
        sizer_v->Add(sizer_h, 1, params ? wxLEFT : wxALIGN_CENTER, params ? UNSAVE_CHANGE_DIALOG_SYNC_CELL_PADDING : 0);
        panel->SetSizer(sizer_v);
        panel->Layout();
        m_sizer_top->Add(panel, 0, wxALIGN_CENTER, 0);
        return title;
    };
    if (params) {
        // With a single target its column box would repeat the one ticking every copy.
        for (int i = 0; i < value_columns; ++i)
            m_sync_columns[i + 1].push_back(
                add_title(params->titles[i], i == params->source, value_columns > 2 && i != params->source ? i : -1)->GetParent());
    } else {
        static_oldv_title = add_title(_L("Old Value"), false, -1);
        static_newv_title = add_title(_L("New Value"), false, -1);
    }

    if (scrollbar_w > 0) {
        // Extend the header over the scrollbar so New Value lines up with its rows.
        auto title_block_gutter = new wxPanel(m_table_top, wxID_ANY, wxDefaultPosition, wxSize(1, -1), wxTAB_TRAVERSAL);
        title_block_gutter->SetBackgroundColour(wxColour(172, 172, 172));
        m_sizer_top->Add(title_block_gutter, 0, wxBOTTOM | wxEXPAND | wxTOP, 2);
        m_sizer_top->AddSpacer(scrollbar_w - 1);
    }

    m_table_top->SetSizer(m_sizer_top);
    m_table_top->Layout();
    m_sizer_top->Fit(m_table_top);
    m_sizer_tab->Add(m_table_top, 1, 0, 0);

    m_scrolledWindow = new wxScrolledWindow(m_panel_tab, wxID_ANY, wxDefaultPosition, wxSize(m_content_width, UNSAVE_CHANGE_DIALOG_SCROLL_WINDOW_SIZE.y),  wxNO_BORDER|wxVSCROLL);
    m_scrolledWindow->SetScrollRate(0, FromDIP(20));
    m_scrolledWindow->SetBackgroundColour(GREY200);
    m_sizer_bottom = new wxBoxSizer(wxVERTICAL);
    m_sizer_bottom->Add(m_scrolledWindow, 1, wxEXPAND, 0);
    m_sizer_tab->Add(m_sizer_bottom, 0, wxEXPAND, 0);

    m_panel_tab->SetSizer(m_sizer_tab);
    m_panel_tab->Layout();
    m_sizer_tab->Fit(m_panel_tab);
    m_sizer_main->Add(m_panel_tab, 0, wxEXPAND | wxLEFT | wxRIGHT, 20);

    m_sizer_main->Add(0, 0, 0, wxTOP, 9);

   /* m_info_line = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, 44), 0);
     m_info_line->SetFont(::Label::Body_13);
     m_info_line->Wrap(-1);
     m_info_line->SetForegroundColour(wxColour(255, 111, 0));
     m_sizer_main->Add(m_info_line, 0, wxLEFT | wxRIGHT, 20);*/

    wxBoxSizer *m_sizer_button = new wxBoxSizer(wxHORIZONTAL);

    auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto checkbox       = new ::CheckBox(this, wxID_APPLY);
    checkbox_sizer->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

    auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Remember my choice."), wxDefaultPosition, wxDefaultSize, 0);
    checkbox_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
    checkbox_text->SetFont(::Label::Body_13);
    checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
    m_sizer_button->Add(checkbox_sizer, 0, wxLEFT, FromDIP(22));
    checkbox_sizer->Show(bool(m_buttons & REMEMBER_CHOISE));

    if (dependent_presets != nullptr) {
        auto wiki_btn = new Button(this, "", "toolbar_wiki", 0, 15);
        auto wiki_url = "https://www.orcaslicer.com/wiki/transfer_discard_changes";
        wiki_btn->SetToolTip(_L("Wiki Guide") + "\n" + wiki_url);
        wiki_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Circle);
        wiki_btn->SetCanFocus(false);
        wiki_btn->Bind(wxEVT_LEFT_DOWN, ([wiki_url](auto& e) {wxLaunchDefaultBrowser(wiki_url);}));

        m_sizer_button->Add(wiki_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(22));
    }

    m_sizer_button->Add(0, 0, 1, 0, 0);

     // Add Buttons
    auto add_btn = [this, m_sizer_button, dependent_presets](Button **btn, int &btn_id, const std::string &icon_name, Action close_act, const wxString &label,
                                                                              bool focus, bool process_enable = true) {
        *btn = new Button(this, _L(label));

        (*btn)->SetStyle(focus ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Choice);

        (*btn)->Bind(wxEVT_BUTTON, [this, close_act, dependent_presets](wxEvent &) {
            bool save_names_and_types = close_act == Action::Save || (close_act == Action::Transfer && ActionButtons::KEEP & m_buttons);
            if (save_names_and_types && !save(dependent_presets, close_act == Action::Save)) return;
            close(close_act);
        });

        // if (process_enable) (*btn)->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent &evt) { evt.Enable(m_tree->has_selection()); });
        (*btn)->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &e) {
            show_info_line(Action::Undef);
            e.Skip();
        });

        m_sizer_button->Add(*btn, 0, wxLEFT, FromDIP(ButtonProps::ChoiceButtonGap()));
    };

    // "Transfer" / "Keep" button
    if (ActionButtons::TRANSFER & m_buttons) {
        const PresetCollection* switched_presets = type == Preset::TYPE_INVALID ? nullptr : wxGetApp().get_tab(type)->get_presets();
        const PresetCollection* technology_source = (dependent_presets && type == dependent_presets->type()) ? dependent_presets : switched_presets;
        // find_preset() can return nullptr (e.g. an unresolved alias); guard against a null deref.
        const Preset* target_preset = technology_source ? technology_source->find_preset(new_selected_preset) : nullptr;
        if (dependent_presets && switched_presets && technology_source && target_preset &&
            technology_source->get_edited_preset().printer_technology() == target_preset->printer_technology())
            add_btn(&m_transfer_btn, m_move_btn_id, "menu_paste", Action::Transfer, switched_presets->get_edited_preset().name == new_selected_preset ? _L("Transfer") : _L("Transfer"), true);
    }
    if (!m_transfer_btn && (ActionButtons::KEEP & m_buttons))
        add_btn(&m_transfer_btn, m_move_btn_id, "menu_paste", Action::Transfer, _L("Transfer"), true);

    bool is_copy = new_selected_preset == "SyncExtruderParams";
    // "Save" button; the sync dialog's Yes confirms its question, so it comes first.
    auto add_save_btn = [&]() {
        if (ActionButtons::SAVE & m_buttons)
            add_btn(&m_save_btn, m_save_btn_id, "save", is_copy ? Action::Transfer : Action::Save, is_copy ? _L("Yes") : _L("Save"), is_copy);
    };
    if (is_copy)
        add_save_btn();
    { // "Don't save" / "Discard" button
        std::string btn_icon    = (ActionButtons::DONT_SAVE & m_buttons) ? "" : (dependent_presets || (ActionButtons::KEEP & m_buttons)) ? "blank_16" : "exit";
        wxString    btn_label   = (ActionButtons::DONT_SAVE & m_buttons) ? (is_copy ? _L("No") : _L("Don't save")) : _L("Discard");
        add_btn(&m_discard_btn, m_continue_btn_id, btn_icon, Action::Discard, btn_label, false);
    }
    if (!is_copy)
        add_save_btn();

    if (dependent_presets != nullptr) {
        const wxString previous_profile = from_u8(dependent_presets->get_edited_preset().name);
        const wxString new_profile      = new_selected_preset.empty() ? _L("the new profile") : from_u8(new_selected_preset);

        if (m_discard_btn) {
            m_discard_btn->SetToolTip(format_wxstr(
                _L("Switch to\n\"%1%\"\ndiscarding any changes made in\n\"%2%\"."),
                new_profile,
                previous_profile));
        }

        if (m_transfer_btn) {
            m_transfer_btn->SetToolTip(format_wxstr(
                _L("All \"New Value\" settings modified in\n\"%1%\"\nwill be transferred to\n\"%2%\"."),
                previous_profile,
                new_profile));
        }

        if (m_save_btn) {
            m_save_btn->SetToolTip(format_wxstr(
                _L("All \"New Value\" settings are saved in\n\"%1%\"\nand \"%2%\" will open without any changes."),
                previous_profile,
                new_profile));
        }
    }

    /* ScalableButton *cancel_btn = new ScalableButton(this, wxID_CANCEL, "cross", _L("Cancel"), wxDefaultSize, wxDefaultPosition, wxBORDER_DEFAULT, true, 24);
      buttons->Add(cancel_btn, 1, wxLEFT | wxRIGHT, 5);
      cancel_btn->SetFont(btn_font);*/
    /* m_cancel_btn = new Button(this, _L("Cancel"));
     m_cancel_btn->SetTextColor(wxColour(107, 107, 107));
     m_cancel_btn->Bind(wxEVT_LEFT_DOWN, [this](wxEvent &) { this->EndModal(wxID_CANCEL); });
     m_cancel_btn->SetMinSize(UNSAVE_CHANGE_DIALOG_BUTTON_SIZE);
     m_cancel_btn->SetCornerRadius(12);
     m_sizer_button->Add(m_cancel_btn, 0, wxLEFT, 5);
     m_sizer_button->Add(0,0,0,wxRIGHT,20);*/

    if (!m_app_config_key.empty()) {}

    m_sizer_button->Add(0, 0, 0, wxRIGHT, 20);
    m_sizer_main->Add(m_sizer_button, 0, wxEXPAND | wxTOP, 6);
    m_sizer_main->Add(0, 0, 1, wxTOP, 18);

    if (params) {
        m_action_line->SetLabel(header);
        update_list(params);
        // Each column is as wide as its widest cell or title, every extruder's column alike. The header
        // windows come first; each sits next to a 1 px separator.
        int widths[2] = {0, 0}; // labels, values
        for (size_t c = 0; c < m_sync_columns.size(); ++c)
            for (size_t r = 0; r < m_sync_columns[c].size(); ++r)
                widths[c > 0] = std::max(widths[c > 0], m_sync_columns[c][r]->GetBestSize().x + UNSAVE_CHANGE_DIALOG_SYNC_CELL_PADDING + (r == 0 ? 1 : 0));
        for (size_t c = 0; c < m_sync_columns.size(); ++c)
            for (size_t r = 0; r < m_sync_columns[c].size(); ++r) {
                m_sync_columns[c][r]->SetMinSize(wxSize(widths[c > 0] - (r == 0 ? 1 : 0), -1));
                m_sync_columns[c][r]->GetParent()->Layout();
            }
        m_scrolledWindow->FitInside();
        const int fitted_width = widths[0] + value_columns * widths[1];
        m_scrolledWindow->SetMinSize(wxSize(fitted_width + scrollbar_w, UNSAVE_CHANGE_DIALOG_SCROLL_WINDOW_SIZE.y));
        m_action_line->SetMinSize(wxSize(fitted_width, -1));
        m_action_line->Wrap(fitted_width);
    } else {
        update(type, dependent_presets, new_selected_preset, header);
    }
    for (const auto &[k, column] : header_checks) {
        for (const auto &[check, opt_key, index, check_column] : m_sync_checks)
            if (column < 0 || column == check_column)
                m_all_checks[k].second.push_back(check);
        if (column < 0)
            for (const auto &option : m_option_checks)
                m_all_checks[k].second.push_back(std::get<0>(option));
    }
    // A box over a single one would only repeat it.
    for (const auto &[all, cells] : m_all_checks)
        all->Show(cells.size() > 1);
    update_all_checks();
    //SetSizer(topSizer);
    //topSizer->SetSizeHints(this);

    show_info_line(Action::Undef);

    SetSizerAndFit(m_sizer_main);
    Layout();
    Fit();
    // Centre(wxBOTH);
}

void UnsavedChangesDialog::show_info_line(Action action, std::string preset_name)
{
    return;
    if (action == Action::Undef && !m_has_long_strings)
        m_info_line->SetLabel(wxEmptyString);
    else {
        wxString text;
        if (action == Action::Undef)
            text = _L("Click the right mouse button to display the full text.");
        else if (action == Action::Discard)
            text = ActionButtons::DONT_SAVE & m_buttons ? _L("No changes will be saved.") :_L("All changes will be discarded.");
        else {
            if (preset_name.empty())
                text = action == Action::Save           ? _L("Save the selected options.") :
                       ActionButtons::KEEP & m_buttons  ? _L("Keep the selected options.") :
                                                          _L("Transfer the selected options to the newly selected preset.");
            else
                text = format_wxstr(
                    action == Action::Save ?
                        _L("Save the selected options to preset\n\"%1%\".") :
                        _L("Transfer the selected options to the newly selected preset\n\"%1%\"."),
                    preset_name);
            //text += "\n" + _L("Unselected options will be reverted.");
        }
        m_info_line->SetLabel(text);
        m_info_line->Show();
    }

    //Layout();
    //Refresh();
}

void UnsavedChangesDialog::close(Action action)
{
    if (action == Action::Transfer) {
        check_option_valid();
    }
    m_exit_action = action;
    this->EndModal(wxID_CLOSE);
}

DynamicPrintConfig UnsavedChangesDialog::make_save_config(PresetCollection& presets)
{
    DynamicPrintConfig             config = presets.get_edited_preset().config;
    const std::vector<std::string> unselected_options = get_unselected_options(presets.type());
    if (!unselected_options.empty())
        config.apply_only(presets.get_selected_preset().config, unselected_options);
    return config;
}

bool UnsavedChangesDialog::selected_save_needs_plugin_warning()
{
    if (m_dependent_presets != nullptr)
        return has_unresolved_plugin_capabilities(make_save_config(*m_dependent_presets));

    const PrinterTechnology printer_technology = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology();
    for (Tab* tab : wxGetApp().tabs_list)
        if (tab->supports_printer_technology(printer_technology) && tab->current_preset_is_dirty()) {
            PresetCollection* presets = tab->get_presets();
            if (presets != nullptr && has_unresolved_plugin_capabilities(make_save_config(*presets)))
                return true;
        }
    return false;
}

bool UnsavedChangesDialog::save(PresetCollection* dependent_presets, bool show_save_preset_dialog/* = true*/)
{
    names_and_types.clear();

    // save one preset
    if (dependent_presets) {
        const Preset& preset = dependent_presets->get_edited_preset();
        std::string name = preset.name;

        // for system/default/external presets we should take an edited name
        //BBS: add project embedded preset logic and refine is_external
        bool save_to_project = false;

        // Warn before the save-name dialog: accepting that dialog can delete an overwritten preset,
        // so a Cancel must be decided before it. Transfer reaches save() with
        // show_save_preset_dialog == false and must not warn.
        if (show_save_preset_dialog &&
            !confirm_save_with_unresolved_plugins(this, make_save_config(*dependent_presets), SaveSubject::Preset))
            return false;

        if (!preset.can_overwrite()) {
        //if (preset.is_system || preset.is_default || preset.is_external) {
            SavePresetDialog save_dlg(this, preset.type);
            if (save_dlg.ShowModal() != wxID_OK) {
                m_exit_action = Action::Discard;
                return false;
            }
            name = save_dlg.get_name();
            save_to_project = save_dlg.get_save_to_project_selection(preset.type);
        }

        //BBS: add project embedded preset relate logic
        PresetData preset_data(name, preset.type, save_to_project);
        names_and_types.emplace_back(preset_data);
        //names_and_types.emplace_back(make_pair(name, preset.type));
    }
    // save all presets
    else
    {
        std::vector<Preset::Type> types_for_save;

        PrinterTechnology printer_technology = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology();

        for (Tab* tab : wxGetApp().tabs_list)
            if (tab->supports_printer_technology(printer_technology) && tab->current_preset_is_dirty()) {
                const Preset& preset = tab->get_presets()->get_edited_preset();
                //BBS: add project embedded preset logic and refine is_external
                if (!preset.can_overwrite())
                //if (preset.is_system || preset.is_default || preset.is_external)
                    types_for_save.emplace_back(preset.type);

                //BBS: add project embedded preset relate logic
                PresetData preset_data(preset.name, preset.type, preset.is_project_embedded);
                names_and_types.emplace_back(preset_data);
                //names_and_types.emplace_back(make_pair(preset.name, preset.type));
            }


        // One Save can write several presets. Build each exact partial config the same way
        // PresetBundle::save_changes_for_preset will, and warn once before any SavePresetDialog can
        // delete an overwritten preset. Transfer/Discard must not warn.
        if (show_save_preset_dialog && !names_and_types.empty()) {
            std::vector<std::unique_ptr<DynamicPrintConfig>> configs;
            std::vector<SaveConfigToCheck>                    checks;
            checks.reserve(names_and_types.size());
            for (const PresetData& nt : names_and_types) {
                PresetCollection* presets = get_preset_collection(nt.type, wxGetApp().preset_bundle);
                if (!presets)
                    continue;
                auto config = std::make_unique<DynamicPrintConfig>(make_save_config(*presets));
                checks.push_back({from_u8(nt.name), config.get()});
                configs.emplace_back(std::move(config));
            }
            if (!checks.empty()) {
                const bool confirmed = checks.size() == 1
                    ? confirm_save_with_unresolved_plugins(this, *checks.front().config, SaveSubject::Preset)
                    : confirm_save_presets_with_unresolved_plugins(this, checks);
                if (!confirmed)
                    return false;
            }
        }

        if (show_save_preset_dialog && !types_for_save.empty()) {
            SavePresetDialog save_dlg(this, types_for_save);
            if (save_dlg.ShowModal() != wxID_OK) {
                m_exit_action = Action::Discard;
                return false;
            }

            //BBS: add project embedded preset relate logic
            for (PresetData& nt : names_and_types) {
                const std::string& name = save_dlg.get_name(nt.type);
                if (!name.empty())
                    nt.name = name;
                nt.save_to_project = save_dlg.get_save_to_project_selection(nt.type);
            }
            //for (std::pair<std::string, Preset::Type>& nt : names_and_types) {
            //    const std::string& name = save_dlg.get_name(nt.second);
            //    if (!name.empty())
            //        nt.first = name;
            //}
        }
    }
    return true;
}

// BBS
#if 0
static size_t get_id_from_opt_key(std::string opt_key)
{
    int pos = opt_key.find("#");
    if (pos > 0) {
        boost::erase_head(opt_key, pos + 1);
        return static_cast<size_t>(atoi(opt_key.c_str()));
    }
    return 0;
}
#endif

void UnsavedChangesDialog::update(Preset::Type type, PresetCollection* dependent_presets, const std::string& new_selected_preset, const wxString& header)
{
    PresetCollection* presets = dependent_presets;

    // activate buttons and labels
    if (m_save_btn)
        m_save_btn->Bind(wxEVT_ENTER_WINDOW, [this, presets](wxMouseEvent &e) {
            show_info_line(Action::Save, presets ? presets->get_selected_preset().name : "");
            e.Skip();
        });


    if (m_transfer_btn) {
        bool is_empty_name = dependent_presets && type != dependent_presets->type();
        m_transfer_btn->Bind(wxEVT_ENTER_WINDOW, [this, new_selected_preset, is_empty_name](wxMouseEvent& e) { show_info_line(Action::Transfer, is_empty_name ? "" : new_selected_preset); e.Skip(); });
    }
    if (m_discard_btn)
        m_discard_btn ->Bind(wxEVT_ENTER_WINDOW, [this]                                    (wxMouseEvent& e) { show_info_line(Action::Discard); e.Skip(); });

    if (type == Preset::TYPE_INVALID) {
        PrinterTechnology printer_technology = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology();
        int presets_cnt = 0;
        for (Tab* tab : wxGetApp().tabs_list)
            if (tab->supports_printer_technology(printer_technology) && tab->current_preset_is_dirty())
                presets_cnt++;
        /*m_action_line->SetLabel((header.IsEmpty() ? "" : header + "\n\n") +
                                _L_PLURAL("The following preset was modified",
                                          "The following presets were modified", presets_cnt));*/
    }
    else {
        wxString action_msg;
        if (type == dependent_presets->type()) {
            action_msg = format_wxstr(_L("Preset \"%1%\" contains the following unsaved changes:"), presets->get_edited_preset().name);
        }
        else {
            action_msg = format_wxstr(type == Preset::TYPE_PRINTER ?
                _L("Preset \"%1%\" is not compatible with the new printer profile and it contains the following unsaved changes:") :
                _L("Preset \"%1%\" is not compatible with the new process profile and it contains the following unsaved changes:"),
                presets->get_edited_preset().name);
        }
        //m_action_line->SetLabel(action_msg);
    }

    wxString action_msg;
    if (dependent_presets) {
        action_msg = format_wxstr(_L("You have changed some settings of preset \"%1%\"."), dependent_presets->get_edited_preset().name);
        if (!m_transfer_btn) {
            action_msg += _L("\nYou can save or discard the preset values you have modified.");
        } else {
            action_msg += _L("\nYou can save or discard the preset values you have modified, or choose to transfer the values you have modified to the new preset.");
        }
    } else {
        action_msg = _L("You have previously modified your settings.");
        if (m_transfer_btn)
            action_msg += _L("\nYou can discard the preset values you have modified, or choose to transfer the modified values to the new project");
        else
            action_msg += _L("\nYou can save or discard the preset values you have modified.");
    }

    m_action_line->SetLabel(action_msg);
    m_action_line->Wrap(m_content_width);

    update_list();
}

bool UnsavedChangesDialog::value_clips(const wxString &value) const
{
    if (value.find('\n') != wxString::npos)
        return true;
    int w = 0, h = 0;
    GetTextExtent(value, &w, &h, nullptr, nullptr, &::Label::Body_13);
    return w > UNSAVE_CHANGE_DIALOG_VALUE_TEXT_WIDTH;
}

void UnsavedChangesDialog::update_list(SyncExtruderParams *params)
{
    if (!m_scrolledWindow) {
        Layout();
        Fit();
        return;
    }

    const std::map<std::string, int> tab_order = build_tab_order_map();
    for (PresetItem &pi : m_presetitems)
        pi.order = order_of(tab_order, pi.opt_key);

    // Each option's per-extruder rows share one tab-order slot, so ordering by slot alone would interleave the
    // extruders. Anchor each type's extruder block at its first slot, per type so no type moves above its own rows.
    std::map<Preset::Type, int> ext_anchor;
    for (const PresetItem &pi : m_presetitems)
        if (pi.extruder_id > 0) {
            auto it = ext_anchor.find(pi.type);
            if (it == ext_anchor.end())
                ext_anchor.emplace(pi.type, pi.order);
            else
                it->second = std::min(it->second, pi.order);
        }

    std::stable_sort(m_presetitems.begin(), m_presetitems.end(),
                     [&ext_anchor](const PresetItem &a, const PresetItem &b) {
                         const int ra = preset_type_rank(a.type), rb = preset_type_rank(b.type);
                         if (ra != rb) return ra < rb;
                         const int sa = a.extruder_id > 0 ? ext_anchor.at(a.type) : a.order;
                         const int sb = b.extruder_id > 0 ? ext_anchor.at(b.type) : b.order;
                         if (sa != sb) return sa < sb;
                         if (a.extruder_id != b.extruder_id) return a.extruder_id < b.extruder_id;
                         return a.order < b.order;
                     });

    auto value_tooltip = [](const wxString &full) {
        const size_t cap = 500;  // a whole g-code block is unreadable in a tooltip
        return full.length() <= cap ? full : full.Left(cap) + "...\n\n" + _L("Click the compare button to see the full value.");
    };

    // A width of wxDefaultCoord keeps the label's natural width, which the sync dialog's fitted columns use.
    auto make_label = [](wxWindow *parent, const wxString &text, const wxFont &font, int width, const wxColour &colour,
                         const wxString &tooltip = wxString(), long style = wxST_ELLIPSIZE_END) {
        const bool natural = width == wxDefaultCoord;
        auto t = new wxStaticText(parent, wxID_ANY, text, wxDefaultPosition, natural ? wxDefaultSize : wxSize(std::max(0, width), -1), style);
        t->SetFont(font);
        t->SetForegroundColour(colour);
        t->SetToolTip(tooltip.IsEmpty() && !natural && t->GetTextExtent(text).GetWidth() > width ? text : tooltip);
        return t;
    };

    // A box before a line's label: one change's own, or one ticking every change (or, in the sync dialog, copy) under it.
    auto add_check_and_label = [this](wxSizer *sizer, ::CheckBox *check, wxStaticText *text, int indent) {
        sizer->Add(check, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRESERVE_SPACE_EVEN_IF_HIDDEN, indent);
        sizer->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    };
    auto add_label = [this, &add_check_and_label](wxWindow *parent, wxSizer *sizer, wxStaticText *text, int indent) -> size_t {
        const size_t k = add_all_check(parent);
        add_check_and_label(sizer, m_all_checks[k].first, text, indent);
        return k;
    };
    // The label width of a line, or the natural one in the sync dialog.
    auto label_width = [params](int width) { return params ? wxDefaultCoord : width; };

    auto make_value_col = [&](wxWindow *parent, wxString value, bool modified, bool may_clip) {
        auto panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(UNSAVE_CHANGE_DIALOG_VALUE_WIDTH, -1), wxTAB_TRAVERSAL);
        auto sizer = new wxBoxSizer(wxVERTICAL);
        // Before flattening, so g-code newlines survive in the tooltip.
        const wxString tooltip = (may_clip && value_clips(value)) ? value_tooltip(value) : wxString();
        value.Replace("\n", " ");
        value.Replace("\r", " ");
        value.Trim(false);
        auto text = make_label(panel, value, ::Label::Body_13, UNSAVE_CHANGE_DIALOG_VALUE_TEXT_WIDTH,
                               modified ? wxGetApp().get_label_clr_modified() : GREY700, tooltip,
                               wxST_ELLIPSIZE_END | wxALIGN_CENTRE_HORIZONTAL);
        sizer->Add(text, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, 5);
        panel->SetSizer(sizer);
        panel->Layout();
        return panel;
    };

    auto m_listsizer = new wxBoxSizer(wxVERTICAL);

    auto finish_row = [&](wxWindow *panel, wxSizer *sizer) {
        panel->SetSizer(sizer);
        panel->Layout();
        m_listsizer->Add(panel, 0, wxEXPAND, 0);
    };

    // Other preset types get no header, and so no box.
    static constexpr size_t no_box = size_t(-1);
    auto emit_type_header = [&](Preset::Type type) -> size_t {
        wxString label;
        switch (type) {
        case Preset::TYPE_PRINT:    label = _L("Process settings"); break;
        case Preset::TYPE_FILAMENT: label = _L("Material settings"); break;
        case Preset::TYPE_PRINTER:  label = _L("Printer settings"); break;
        default: return no_box;
        }
        auto panel = new wxPanel(m_scrolledWindow, wxID_ANY, wxDefaultPosition, wxSize(-1, UNSAVE_CHANGE_DIALOG_ITEM_HEIGHT), wxTAB_TRAVERSAL);
        panel->SetBackgroundColour(GREY400);
        auto sizer = new wxBoxSizer(wxHORIZONTAL);
        const size_t all = add_label(panel, sizer, make_label(panel, label, ::Label::Head_16,
                                     m_content_width - UNSAVE_CHANGE_DIALOG_TYPE_INDENT - FromDIP(8) - UNSAVE_CHANGE_DIALOG_CHECK_WIDTH, GREY900),
                                     UNSAVE_CHANGE_DIALOG_TYPE_INDENT);
        finish_row(panel, sizer);
        return all;
    };

    auto emit_category = [&](const wxString &category_name) {
        auto panel = new wxPanel(m_scrolledWindow, wxID_ANY, wxDefaultPosition, wxSize(-1, UNSAVE_CHANGE_DIALOG_ITEM_HEIGHT), wxTAB_TRAVERSAL);
        panel->SetBackgroundColour(GREY300);
        auto sizer = new wxBoxSizer(wxHORIZONTAL);
        const size_t all = add_label(panel, sizer, make_label(panel, category_name, ::Label::Head_13,
                                     label_width(m_content_width - UNSAVE_CHANGE_DIALOG_CATEGORY_INDENT - FromDIP(8) - UNSAVE_CHANGE_DIALOG_CHECK_WIDTH), GREY900),
                                     UNSAVE_CHANGE_DIALOG_CATEGORY_INDENT);
        finish_row(panel, sizer);
        return all;
    };

    auto emit_group = [&](const wxString &group_name) {
        auto panel = new wxWindow(m_scrolledWindow, -1, wxDefaultPosition, wxSize(-1, UNSAVE_CHANGE_DIALOG_ITEM_HEIGHT));
        panel->SetBackgroundColour(GREY200);
        auto sizer = new wxBoxSizer(wxHORIZONTAL);
        auto panel_left = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(m_first_value_width, -1), wxTAB_TRAVERSAL);
        panel_left->SetBackgroundColour(GREY200);
        auto left_sizer = new wxBoxSizer(wxHORIZONTAL);
        const size_t all = add_label(panel_left, left_sizer, make_label(panel_left, group_name, ::Label::Head_13,
                                     label_width(m_first_value_width - UNSAVE_CHANGE_DIALOG_GROUP_INDENT - FromDIP(8) - UNSAVE_CHANGE_DIALOG_CHECK_WIDTH), GREY700),
                                     UNSAVE_CHANGE_DIALOG_GROUP_INDENT);
        panel_left->SetSizer(left_sizer);
        panel_left->Layout();
        sizer->Add(panel_left, 0, wxALIGN_CENTER, 0);
        if (params)
            m_sync_columns[0].push_back(panel_left);
        finish_row(panel, sizer);
        return all;
    };

    auto emit_option = [&](const PresetItem &data, size_t type_all, size_t category_all, size_t group_all) {
        auto panel = new wxWindow(m_scrolledWindow, -1, wxDefaultPosition, wxSize(-1, UNSAVE_CHANGE_DIALOG_ITEM_HEIGHT));
        panel->SetBackgroundColour(GREY200);
        auto sizer = new wxBoxSizer(wxHORIZONTAL);

        auto panel_left = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(m_first_value_width, -1), wxTAB_TRAVERSAL);
        panel_left->SetBackgroundColour(GREY200);
        auto left_sizer = new wxBoxSizer(wxHORIZONTAL);
        const int text_w = m_first_value_width - UNSAVE_CHANGE_DIALOG_OPTION_INDENT - FromDIP(8) - UNSAVE_CHANGE_DIALOG_CHECK_WIDTH -
                           (data.has_icon ? UNSAVE_CHANGE_DIALOG_ICON_WIDTH : 0);
        auto label = make_label(panel_left, data.option_name, ::Label::Body_13, label_width(text_w), GREY700);
        size_t row_all = 0;
        if (params)
            row_all = add_label(panel_left, left_sizer, label, UNSAVE_CHANGE_DIALOG_OPTION_INDENT);
        else {
            // The change's own box. Extruder count has none: the per-extruder values follow it.
            auto check = new ::CheckBox(panel_left);
            add_check_and_label(left_sizer, check, label, UNSAVE_CHANGE_DIALOG_OPTION_INDENT);
            if (data.opt_key == "extruders_count")
                check->Hide();
            else {
                check->SetValue(true);
                check->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) { update_all_checks(); e.Skip(); });
                m_option_checks.emplace_back(check, data.type, data.opt_key);
                for (size_t k : {type_all, category_all, group_all})
                    if (k != no_box)
                        m_all_checks[k].second.push_back(check);
            }
        }
        if (data.has_icon) {
            auto compare_btn = new Button(panel_left, "", "compare", 0, 16);
            compare_btn->SetStyle(ButtonStyle::Regular, ButtonType::Icon);
            compare_btn->SetPaddingSize(FromDIP(wxSize(1, 1)));
            compare_btn->SetMinSize(FromDIP(wxSize(20, 20)));
            compare_btn->SetSize(FromDIP(wxSize(20, 20)));
            compare_btn->SetCanFocus(false);
            compare_btn->SetToolTip(_L("Compare the full values"));
            compare_btn->Bind(wxEVT_BUTTON, [this, data](wxCommandEvent &) {
                FullCompareDialog(data.option_name, data.old_value, data.new_value,
                                  static_oldv_title->GetLabel(), static_newv_title->GetLabel()).ShowModal();
            });
            left_sizer->Add(compare_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(4));
        }
        panel_left->SetSizer(left_sizer);
        panel_left->Layout();
        sizer->Add(panel_left, 0, wxALIGN_CENTER, 0);

        if (params) {
            m_sync_columns[0].push_back(panel_left);
            // A target whose value differs from the source gets a checkbox for copying it.
            DynamicPrintConfig value_config;
            value_config.set_key_value(data.opt_key, params->config->option(data.opt_key)->clone());
            const auto  values    = dynamic_cast<const ConfigOptionVectorBase *>(params->config->option(data.opt_key))->vserialize();
            const auto &positions = params->positions.at(data.opt_key);
            const int   from      = positions.index[params->source];
            for (int i = 0; i < int(params->titles.size()); ++i) {
                // A column without a value of this option, e.g. an extruder without its nozzle variant, stays empty.
                const int index   = positions.index[i];
                bool      differs = false;
                for (int s = 0; index >= 0 && s < positions.stride; ++s)
                    differs |= values[from + s] != values[index + s];

                auto cell = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
                cell->SetBackgroundColour(GREY200);
                m_sync_columns[i + 1].push_back(cell);
                auto cell_sizer = new wxBoxSizer(wxHORIZONTAL);
                cell_sizer->AddSpacer(UNSAVE_CHANGE_DIALOG_SYNC_CELL_PADDING);
                if (i != params->source) {
                    // A target that already has the value keeps the box's space, so its column stays aligned.
                    auto check = new ::CheckBox(cell);
                    cell_sizer->Add(check, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxRESERVE_SPACE_EVEN_IF_HIDDEN, FromDIP(4));
                    if (differs) {
                        check->SetValue(true);
                        check->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) { update_all_checks(); e.Skip(); });
                        m_sync_checks.emplace_back(check, data.opt_key, index, i);
                        for (size_t k : {category_all, group_all, row_all})
                            m_all_checks[k].second.push_back(check);
                    } else
                        check->Hide();
                }
                if (index >= 0)
                    cell_sizer->Add(make_label(cell, get_string_value(data.opt_key + "#" + std::to_string(index), value_config), ::Label::Body_13,
                                               wxDefaultCoord, i == params->source ? wxGetApp().get_label_clr_modified() : GREY700),
                                    0, wxALIGN_CENTER_VERTICAL);
                cell->SetSizer(cell_sizer);
                cell->Layout();
                sizer->Add(cell, 0, wxALIGN_CENTER, 0);
            }
        } else {
            sizer->Add(make_value_col(panel, data.old_value, false, data.has_icon), 0, wxALIGN_CENTER, 0);
            sizer->Add(make_value_col(panel, data.new_value, false, data.has_icon), 0, wxALIGN_CENTER, 0);
        }

        finish_row(panel, sizer);
    };

    // Group by key, not adjacency: per-variant tags make one category recur at non-adjacent tab slots, so a walk that
    // starts a section whenever the name changes would duplicate it.
    struct Section { Preset::Type type; wxString name; std::vector<wxString> group_keys; };
    std::map<wxString, Section>                 sections;       // cat_key -> section
    std::vector<wxString>                       section_order;  // cat_keys in first-encounter order
    std::map<wxString, std::vector<PresetItem>> rows_by_group;  // grp_key -> its rows, in tab order
    for (const PresetItem &pi : m_presetitems) {
        const wxString ckey = preset_item_cat_key(pi);
        const wxString gkey = preset_item_grp_key(pi);
        auto section = sections.try_emplace(ckey);
        if (section.second) {
            section.first->second = {pi.type, pi.category_name, {}};
            section_order.push_back(ckey);
        }
        auto group = rows_by_group.try_emplace(gkey);
        if (group.second)
            section.first->second.group_keys.push_back(gkey);
        group.first->second.push_back(pi);
    }

    Preset::Type shown_type = Preset::TYPE_INVALID;
    size_t       type_all   = 0;
    for (const wxString &ckey : section_order) {
        const Section &section = sections.at(ckey);
        // The sync dialog copies within one tab, so it needs no type header.
        if (section.type != shown_type && !params) {
            shown_type = section.type;
            type_all   = emit_type_header(section.type);
        }
        const size_t category_all = emit_category(section.name);
        for (const wxString &gkey : section.group_keys) {
            const std::vector<PresetItem> &rows = rows_by_group.at(gkey);
            const size_t group_all = emit_group(rows.front().group_name);
            for (const PresetItem &pi : rows)
                emit_option(pi, type_all, category_all, group_all);
        }
    }

    m_scrolledWindow->SetSizer(m_listsizer);
    m_scrolledWindow->Layout();
    Layout();
    Fit();
}

void UnsavedChangesDialog::update_tree(Preset::Type type, DynamicConfig *config)
{
    Search::SettingsIndex &index = wxGetApp().sidebar().settings_index();
    index.sort_options_by_key();

    for (const std::string &opt_key : config->keys()) {
        int                   variant_index = -2;
        Search::Option        option        = index.get_option(opt_key, type, variant_index);
        if (variant_index == -2) {
            // Orca: Every transferred setting must remain visible even when it is absent from the search index.
            const ConfigOptionDef* def = print_config_def.get(opt_key);
            const std::string label = def ? (def->full_label.empty() ? def->label : def->full_label) : std::string();
            option.label_local = (label.empty() ? from_u8(opt_key) : _L(label)).ToStdWstring();
            option.category_local = (def && !def->category.empty() ?
                Tab::translate_category(from_u8(def->category), type) : _L("Others")).ToStdWstring();
        }
        // update_list() reads the values of every extruder from the config.
        m_presetitems.push_back({type, opt_key, option.category_local, option.group_local, option.label_local, {}, {}});
    }

    index.sort_options_by_label();
}

std::vector<std::pair<std::string, int>> UnsavedChangesDialog::get_sync_selection() const
{
    std::vector<std::pair<std::string, int>> selection;
    for (const auto &[check, opt_key, index, column] : m_sync_checks)
        if (check->GetValue())
            selection.emplace_back(opt_key, index);
    return selection;
}

size_t UnsavedChangesDialog::add_all_check(wxWindow *parent)
{
    const size_t k = m_all_checks.size();
    m_all_checks.push_back({new ::CheckBox(parent), {}});
    m_all_checks[k].first->Bind(wxEVT_TOGGLEBUTTON, [this, k](wxCommandEvent &e) {
        for (::CheckBox *cell : m_all_checks[k].second)
            cell->SetValue(m_all_checks[k].first->GetValue());
        update_all_checks();
        e.Skip();
    });
    return k;
}

std::vector<std::string> UnsavedChangesDialog::options_by_selection(const Preset::Type *type, bool selected) const
{
    std::vector<std::string> options;
    for (const PresetItem &item : m_presetitems) {
        if (type != nullptr && item.type != *type)
            continue;
        auto check = std::find_if(m_option_checks.begin(), m_option_checks.end(), [&item](const auto &option) {
            return std::get<1>(option) == item.type && std::get<2>(option) == item.opt_key;
        });
        if ((check == m_option_checks.end() || std::get<0>(*check)->GetValue()) == selected)
            options.push_back(item.opt_key);
    }
    return options;
}

void UnsavedChangesDialog::update_all_checks()
{
    for (const auto &[all, cells] : m_all_checks) {
        const auto checked = size_t(std::count_if(cells.begin(), cells.end(), [](const ::CheckBox *cell) { return cell->GetValue(); }));
        // A partly ticked box holds false, so clicking it ticks everything under it.
        all->SetValue(!cells.empty() && checked == cells.size());
        all->SetHalfChecked(checked > 0 && checked < cells.size());
    }
}

void UnsavedChangesDialog::update_tree(Preset::Type type, PresetCollection* presets_)
{
    Search::SettingsIndex& index = wxGetApp().sidebar().settings_index();
    index.sort_options_by_key();

    // list of the presets with unsaved changes
    std::vector<PresetCollection*> presets_list;
    if (type == Preset::TYPE_INVALID)
    {
        PrinterTechnology printer_technology = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology();

        for (Tab* tab : wxGetApp().tabs_list)
            if (tab->supports_printer_technology(printer_technology) && tab->current_preset_is_dirty())
                presets_list.emplace_back(tab->get_presets());
    }
    else
        presets_list.emplace_back(presets_);

    const bool multiple_extruders = wxGetApp().preset_bundle->get_printer_extruder_count() > 1;

    // Display a dialog showing the dirty options in a human readable form.
    for (PresetCollection* presets : presets_list)
    {
        const DynamicPrintConfig& old_config = presets->get_selected_preset().config;
        const PrinterTechnology&  old_pt     = presets->get_selected_preset().printer_technology();
        const DynamicPrintConfig& new_config = presets->get_edited_preset().config;
        type = presets->type();

        const std::map<wxString, std::string>& category_icon_map = wxGetApp().get_tab(type)->get_category_icon_map();

        //m_tree->model->AddPreset(type, from_u8(presets->get_edited_preset().name), old_pt);

        // Collect dirty options.
        const bool deep_compare = (type == Preset::TYPE_PRINTER || type == Preset::TYPE_PRINT || type == Preset::TYPE_FILAMENT || type == Preset::TYPE_SLA_MATERIAL);
        auto dirty_options = presets->current_dirty_options(deep_compare);

        // process changes of extruders count
        if (type == Preset::TYPE_PRINTER && old_pt == ptFFF &&
            old_config.opt<ConfigOptionFloats>("nozzle_diameter")->values.size() != new_config.opt<ConfigOptionFloats>("nozzle_diameter")->values.size()) {
            wxString local_label = _L("Extruder count");
            wxString old_val = from_u8((boost::format("%1%") % old_config.opt<ConfigOptionFloats>("nozzle_diameter")->values.size()).str());
            wxString new_val = from_u8((boost::format("%1%") % new_config.opt<ConfigOptionFloats>("nozzle_diameter")->values.size()).str());

            //BBS: the page "General" changed to "Basic information" instead
            //m_tree->Append("extruders_count", type, _L("General"), _L("Capabilities"), local_label, old_val, new_val, category_icon_map.at("Basic information"));
            //m_tree->Append("extruders_count", type, _L("General"), _L("Capabilities"), local_label, old_val, new_val, category_icon_map.at("General"));

            if (old_val != new_val) {
                PresetItem pi = {type, "extruders_count", _L("General"), _L("Capabilities"), local_label, old_val, new_val};
                m_presetitems.push_back(pi);
            }
        }

        auto variant_key      = Preset::get_iot_type_string(type) + "_extruder_variant";
        auto id_key           = Preset::get_iot_type_string(type) + "_extruder_id";
        // Orca: Dirty indices belong to the edited config, which may contain newly added variants.
        auto extruder_variant = dynamic_cast<ConfigOptionStrings const *>(new_config.option(variant_key));
        auto extruder_id      = dynamic_cast<ConfigOptionInts const *>(new_config.option(id_key));

        auto extruder_variant_of = [type](const std::string& pure_key, int index) {
            return type == Preset::TYPE_PRINTER && printer_options_with_variant_2.count(pure_key) > 0 ? index / 2 : index;
        };

        std::map<std::string, int> dirty_variants_per_option;
        std::map<std::pair<std::string, int>, int> dirty_variants_per_extruder;
        for (const std::string& opt_key : dirty_options) {
            const std::string pure_key = get_pure_opt_key(opt_key);
            ++dirty_variants_per_option[pure_key];
            const size_t hash_pos = opt_key.find('#');
            const int variant = hash_pos == std::string::npos ? -1 : extruder_variant_of(pure_key, std::atoi(opt_key.c_str() + hash_pos + 1));
            if (extruder_id && variant >= 0 && variant < extruder_id->size())
                ++dirty_variants_per_extruder[{pure_key, extruder_id->values[variant]}];
        }

        for (const std::string& opt_key : dirty_options) {
            int variant_index = -2;
            const Search::Option &option = index.get_option(opt_key, type, variant_index);
            if (variant_index == -2) {
                // When founded option isn't the correct one.
                // It can be for dirty_options: "default_print_profile", "printer_model", "printer_settings_id",
                // because of they don't exist in the index
                continue;
            }
            wxString category = option.category_local;
            wxString label = option.label_local;
            // A multi-extruder printer registers its per-extruder options under "Extruder N".
            const bool on_extruder_page = option.category.compare(0, 9, L"Extruder ") == 0;
            int row_extruder_id = on_extruder_page ? int(std::wcstol(option.category.c_str() + 9, nullptr, 10)) : 0;
            if (variant_index >= 0)
                variant_index = extruder_variant_of(get_pure_opt_key(opt_key), variant_index);
            if (variant_index >= 0 && extruder_variant && variant_index < extruder_variant->size()) {
                // Orca: Match the untranslated category and use the same extruder names as the printer tabs.
                if (on_extruder_page)
                    category = _L("Extruder");
                const bool ambiguous = dirty_variants_per_option[get_pure_opt_key(opt_key)] > 1;
                wxString variant_label = from_u8(extruder_variant->values[variant_index]);
                // Orca: An extruder name only disambiguates variants on printers with multiple extruders.
                const bool named_extruder = multiple_extruders && extruder_id && variant_index < extruder_id->size() && extruder_id->values[variant_index] > 0;
                if (named_extruder) {
                    const int id = extruder_id->values[variant_index];
                    const wxString extruder_name = Tab::translate_category(wxString::Format("Extruder %d", id), Preset::TYPE_PRINTER);
                    const bool several_dirty_on_extruder = dirty_variants_per_extruder[{get_pure_opt_key(opt_key), id}] > 1;
                    variant_label = several_dirty_on_extruder ? extruder_name + " (" + variant_label + ")" : extruder_name;
                    if (on_extruder_page)
                        row_extruder_id = id;
                }
                if (on_extruder_page && named_extruder)
                    category = variant_label;
                else if (ambiguous)
                    category = variant_label + ": " + category;
            }

            /*m_tree->Append(opt_key, type, option.category_local, option.group_local, option.label_local,
                get_string_value(opt_key, old_config), get_string_value(opt_key, new_config), category_icon_map.at(option.category));*/


            //PresetItem pi = {opt_key, type, 1983};
            //m_presetitems.push_back()
            PresetItem pi = {type, opt_key, category, option.group_local, label, get_string_value(opt_key, old_config), get_string_value(opt_key, new_config)};
            pi.has_icon = value_clips(pi.old_value) || value_clips(pi.new_value);
            pi.extruder_id = row_extruder_id;
            m_presetitems.push_back(pi);

        }
    }

    // Revert sort of index back
    index.sort_options_by_label();
}

void UnsavedChangesDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    int em = em_unit();

    //msw_buttons_rescale(this, em, { wxID_CANCEL, m_move_btn_id, m_continue_btn_id });
    for (auto btn : {m_transfer_btn, m_discard_btn, m_save_btn})
        if (btn) btn->Rescale();

    //m_cancel_btn->SetMinSize(UNSAVE_CHANGE_DIALOG_BUTTON_SIZE);
    const wxSize& size = wxSize(70 * em, 30 * em);
    SetMinSize(size);
    //m_tree->Rescale(em);

    Fit();
    Refresh();
}

void UnsavedChangesDialog::on_sys_color_changed()
{
    //for (auto btn : { m_save_btn, m_transfer_btn, m_discard_btn } )
        //btn->msw_rescale();
    // msw_rescale updates just icons, so use it
    //m_tree->Rescale();

    Refresh();
}

bool UnsavedChangesDialog::check_option_valid()
{
    return true;
}


//------------------------------------------
//          FullCompareDialog
//------------------------------------------

FullCompareDialog::FullCompareDialog(const wxString& option_name, const wxString& old_value, const wxString& new_value,
                                     const wxString& old_value_header, const wxString& new_value_header)
    : wxDialog(nullptr, wxID_ANY, option_name, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);

    int border = 10;

    wxStaticBoxSizer* sizer = new wxStaticBoxSizer(wxVERTICAL, this);

    wxFlexGridSizer* grid_sizer = new wxFlexGridSizer(2, 2, 1, 0);
    grid_sizer->SetFlexibleDirection(wxBOTH);
    grid_sizer->AddGrowableCol(0,1);
    grid_sizer->AddGrowableCol(1,1);
    grid_sizer->AddGrowableRow(1,1);

    auto add_header = [grid_sizer, border, this](wxString label) {
        wxStaticText* text = new wxStaticText(this, wxID_ANY, label);
        text->SetFont(this->GetFont().Bold());
        grid_sizer->Add(text, 0, wxALL, border);
    };

    add_header(old_value_header);
    add_header(new_value_header);

    auto get_set_from_val = [](wxString str) {
        if (str.Find("\n") == wxNOT_FOUND)
            str.Replace(" ", "\n");

        std::set<wxString> str_set;

        wxStringTokenizer strings(str, "\n");
        while (strings.HasMoreTokens())
            str_set.emplace(strings.GetNextToken());

        return str_set;
    };

    std::set<wxString> old_set = get_set_from_val(old_value);
    std::set<wxString> new_set = get_set_from_val(new_value);
    std::set<wxString> old_new_diff_set;
    std::set<wxString> new_old_diff_set;

    std::set_difference(old_set.begin(), old_set.end(), new_set.begin(), new_set.end(), std::inserter(old_new_diff_set, old_new_diff_set.begin()));
    std::set_difference(new_set.begin(), new_set.end(), old_set.begin(), old_set.end(), std::inserter(new_old_diff_set, new_old_diff_set.begin()));

    auto add_value = [grid_sizer, border, this](wxString label, const std::set<wxString>& diff_set) {
        wxTextCtrl* text = new wxTextCtrl(this, wxID_ANY, label, wxDefaultPosition, wxSize(400, 400), wxTE_MULTILINE | wxTE_READONLY | wxBORDER_DEFAULT | wxTE_RICH);
        wxGetApp().UpdateDarkUI(text);
        text->SetStyle(0, label.Len(), wxTextAttr(wxNullColour, wxNullColour, this->GetFont()));

        for (const wxString& str : diff_set) {
            int pos = label.First(str);
            if (pos == wxNOT_FOUND)
                continue;
            text->SetStyle(pos, pos + (int)str.Len(), wxTextAttr(wxGetApp().get_label_clr_modified(), wxNullColour, this->GetFont().Bold()));
        }

        grid_sizer->Add(text, 1, wxALL | wxEXPAND, border);
    };
    add_value(old_value, old_new_diff_set);
    add_value(new_value, new_old_diff_set);

    sizer->Add(grid_sizer, 1, wxEXPAND);

    auto dlg_btns = new DialogButtons(this, {"OK"});

    wxBoxSizer* topSizer = new wxBoxSizer(wxVERTICAL);

    topSizer->Add(sizer,   1, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, border);
    topSizer->Add(dlg_btns , 0, wxEXPAND);

    SetSizer(topSizer);
    topSizer->SetSizeHints(this);

    wxGetApp().UpdateDlgDarkUI(this);
}


static PresetCollection* get_preset_collection(Preset::Type type, PresetBundle* preset_bundle) {
    if (!preset_bundle)
        preset_bundle = wxGetApp().preset_bundle;
    return  type == Preset::Type::TYPE_PRINTER      ? &preset_bundle->printers :
            type == Preset::Type::TYPE_FILAMENT     ? &preset_bundle->filaments :
            type == Preset::Type::TYPE_SLA_MATERIAL ? &preset_bundle->sla_materials :
            type == Preset::Type::TYPE_PRINT        ? &preset_bundle->prints :
            type == Preset::Type::TYPE_SLA_PRINT    ? &preset_bundle->sla_prints :
            nullptr;
}

//------------------------------------------
//          DiffPresetDialog
//------------------------------------------
static std::string get_selection(PresetComboBox* preset_combo)
{
    return into_u8(preset_combo->GetString(preset_combo->GetSelection()));
}

void DiffPresetDialog::create_presets_sizer()
{
    m_presets_sizer = new wxBoxSizer(wxVERTICAL);

    for (auto new_type : PRESET_TYPE_ORDER)
    {
        const PresetCollection* collection = get_preset_collection(new_type);
        wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);
        PresetComboBox* presets_left;
        PresetComboBox* presets_right;
        ScalableButton* equal_bmp = new ScalableButton(this, wxID_ANY, "equal");

        auto add_preset_combobox = [collection, sizer, new_type, this](PresetComboBox** cb_, PresetBundle* preset_bundle) {
            *cb_ = new PresetComboBox(this, new_type, wxSize(em_unit() * 35, -1), preset_bundle);
            PresetComboBox* cb = (*cb_);
            cb->set_selection_changed_function([this, new_type, preset_bundle, cb](int selection) {
                if (m_view_type == Preset::TYPE_INVALID) {
                    std::string preset_name = Preset::remove_suffix_modified(cb->GetString(selection).ToUTF8().data());
                    update_compatibility(preset_name, new_type, preset_bundle);
                }
                update_tree();
            });
            if (collection->get_selected_idx() != (size_t)-1)
                cb->update(collection->get_selected_preset().name);

            sizer->Add(cb, 1);
            cb->Show(new_type == Preset::TYPE_PRINTER);
        };
        add_preset_combobox(&presets_left, m_preset_bundle_left.get());
        sizer->Add(equal_bmp, 0, wxRIGHT | wxLEFT | wxALIGN_CENTER_VERTICAL, 5);
        add_preset_combobox(&presets_right, m_preset_bundle_right.get());
        m_presets_sizer->Add(sizer, 1, wxTOP, 5);
        equal_bmp->Show(new_type == Preset::TYPE_PRINTER);

        m_preset_combos.push_back({ presets_left, equal_bmp, presets_right });

        equal_bmp->Bind(wxEVT_BUTTON, [presets_left, presets_right, this](wxEvent&) {
            std::string preset_name = get_selection(presets_left);
            presets_right->update(preset_name);
            if (m_view_type == Preset::TYPE_INVALID)
                update_compatibility(Preset::remove_suffix_modified(preset_name), presets_right->get_type(), m_preset_bundle_right.get());
            update_tree();
        });
    }
}

void DiffPresetDialog::create_show_all_presets_chb()
{
    m_show_all_presets = new wxCheckBox(this, wxID_ANY, _L("Show all presets (including incompatible)"));
    m_show_all_presets->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        bool show_all = m_show_all_presets->GetValue();
        for (auto preset_combos : m_preset_combos) {
            if (preset_combos.presets_left->get_type() == Preset::TYPE_PRINTER)
                continue;
            preset_combos.presets_left->show_all(show_all);
            preset_combos.presets_right->show_all(show_all);
        }
        if (m_view_type == Preset::TYPE_INVALID)
            update_tree();
    });
}

void DiffPresetDialog::create_info_lines()
{
    const wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT).Bold();

    m_top_info_line = new wxStaticText(this, wxID_ANY, _L("Select presets to compare"));
    m_top_info_line->SetFont(font);

    m_bottom_info_line = new wxStaticText(this, wxID_ANY, "");
    m_bottom_info_line->SetFont(font);
}

void DiffPresetDialog::create_tree()
{
    m_tree = new DiffViewCtrl(this, wxSize(em_unit() * 65, em_unit() * 40));
    m_tree->AppendToggleColumn_(L"\u2714", DiffModel::colToggle, wxLinux ? 9 : 6);
    m_tree->AppendBmpTextColumn("",                      DiffModel::colIconText, 35);
    m_tree->AppendBmpTextColumn(_L("Left Preset Value"), DiffModel::colOldValue, 15);
    m_tree->AppendBmpTextColumn(_L("Right Preset Value"),DiffModel::colNewValue, 15);
    m_tree->Hide();
    m_tree->GetColumn(DiffModel::colToggle)->SetHidden(true);
}

std::array<Preset::Type, 3> DiffPresetDialog::types_list() const
{
    return PresetBundle::types_list(m_pr_technology);
}

void DiffPresetDialog::create_buttons()
{
    m_buttons   = new wxBoxSizer(wxHORIZONTAL);

    auto show_in_bottom_info = [this](const wxString& ext_line, wxEvent* e = nullptr) {
        m_bottom_info_line->SetLabel(ext_line);
        m_bottom_info_line->Show(true);
        Layout();
        if (e) e->Skip();
    };

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    // Transfer 
    auto transfer_btn = dlg_btns->GetOK();
    transfer_btn->SetLabel(L("Transfer"));
    transfer_btn->Bind(wxEVT_BUTTON, [this](wxEvent&) { button_event(Action::Transfer);});


    auto enable_transfer = [this](const Preset::Type& type) {
        const Preset& main_edited_preset = get_preset_collection(type, wxGetApp().preset_bundle)->get_edited_preset();
        if (main_edited_preset.is_dirty)
            return main_edited_preset.name == get_right_preset_name(type);
        return true;
    };
    transfer_btn->Bind(wxEVT_UPDATE_UI, [this, enable_transfer, show_in_bottom_info, transfer_btn](wxUpdateUIEvent& evt) {
        bool enable = m_tree->has_selection();
        if (enable) {
            if (m_view_type == Preset::TYPE_INVALID) {
                for (const Preset::Type& type : types_list())
                    if (!enable_transfer(type)) {
                        enable = false;
                        break;
                    }
            }
            else
                enable = enable_transfer(m_view_type);

            if (!enable && transfer_btn->IsShown()) {
                show_in_bottom_info(_L("You can only transfer to current active profile because it has been modified."));
            }
        }
        evt.Enable(enable);
    });
    transfer_btn->Bind(wxEVT_ENTER_WINDOW, [show_in_bottom_info](wxMouseEvent& e) {
        show_in_bottom_info(_L("Transfer the selected options from left preset to the right.\n"
                            "Note: New modified presets will be selected in settings tabs after close this dialog."), &e); });

    // Cancel
    auto cancel_btn = dlg_btns->GetCANCEL();
    cancel_btn->Bind(wxEVT_BUTTON, [this](wxEvent&) { button_event(Action::Discard);});

    for (Button* btn : { transfer_btn, cancel_btn }) {
        btn->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { update_bottom_info(); Layout(); e.Skip(); });
    }

    m_buttons->Add(dlg_btns, 1, wxEXPAND);

    m_buttons->Show(false);
}

void DiffPresetDialog::create_edit_sizer()
{
    // Add check box for the edit mode
    m_use_for_transfer = new wxCheckBox(this, wxID_ANY, _L("Transfer values from left to right"));
    m_use_for_transfer->SetToolTip(_L("If enabled, this dialog can be used for transfer selected values from left to right preset."));
    m_use_for_transfer->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        bool use = m_use_for_transfer->GetValue();
        m_tree->GetColumn(DiffModel::colToggle)->SetHidden(!use);
        if (m_tree->IsShown()) {
            m_buttons->Show(use);
            Fit();
            Refresh();
        }
        else
            this->Layout();
    });

    // Add Buttons 
    create_buttons();

    // Create and fill edit sizer
    m_edit_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_edit_sizer->Add(m_use_for_transfer, 0, wxALIGN_CENTER_VERTICAL | wxALL, 10);
    m_edit_sizer->AddSpacer(em_unit() * 10);
    m_edit_sizer->Add(m_buttons, 1, wxLEFT, 5);
    m_edit_sizer->Show(false);
}

void DiffPresetDialog::complete_dialog_creation()
{
    wxBoxSizer* topSizer = new wxBoxSizer(wxVERTICAL);

    int border = 10;
    topSizer->Add(m_top_info_line,      0, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, 2 * border);
    topSizer->Add(m_presets_sizer,      0, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, border);
    topSizer->Add(m_show_all_presets,   0, wxEXPAND | wxALL, border);
    topSizer->Add(m_tree,               1, wxEXPAND | wxALL, border);
    topSizer->Add(m_bottom_info_line,   0, wxEXPAND | wxALL, 2 * border);
    topSizer->Add(m_edit_sizer,         0, wxEXPAND);

    this->SetMinSize(wxSize(80 * em_unit(), 30 * em_unit()));
    this->SetSizer(topSizer);
    topSizer->SetSizeHints(this);
    wxGetApp().UpdateDlgDarkUI(this);
}

DiffPresetDialog::DiffPresetDialog(MainFrame* mainframe)
    : DPIDialog(mainframe, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
    m_pr_technology(wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology())
{
#if defined(__WXMSW__)
    // ys_FIXME! temporary workaround for correct font scaling
    // Because of from wxWidgets 3.1.3 auto rescaling is implemented for the Fonts,
    // From the very beginning set dialog font to the wxSYS_DEFAULT_GUI_FONT
    this->SetFont(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT));
#endif // __WXMSW__

    // Init bundles

    assert(wxGetApp().preset_bundle);

    // show() copies the app's bundle into both before anything is displayed.
    m_preset_bundle_left  = std::make_unique<PresetBundle>();
    m_preset_bundle_right = std::make_unique<PresetBundle>();

    // Create UI items

    SetBackgroundColour(*wxWHITE);

    create_info_lines();

    create_presets_sizer();

    create_show_all_presets_chb();

    create_tree();

    create_edit_sizer();

    complete_dialog_creation();
}

void DiffPresetDialog::update_controls_visibility(Preset::Type type /* = Preset::TYPE_INVALID*/)
{
    for (auto preset_combos : m_preset_combos) {
        Preset::Type cb_type = preset_combos.presets_left->get_type();
        bool show = type != Preset::TYPE_INVALID    ? type == cb_type :
                    cb_type == Preset::TYPE_PRINTER ? true :
                    m_pr_technology == ptFFF        ? cb_type == Preset::TYPE_PRINT || cb_type == Preset::TYPE_FILAMENT :
                                                      cb_type == Preset::TYPE_SLA_PRINT || cb_type == Preset::TYPE_SLA_MATERIAL;
        preset_combos.presets_left->Show(show);
        preset_combos.equal_bmp->Show(show);
        preset_combos.presets_right->Show(show);

        if (show) {
            preset_combos.presets_left->update_from_bundle();
            preset_combos.presets_right->update_from_bundle();
        }
    }

    m_show_all_presets->Show(type != Preset::TYPE_PRINTER);
}

void DiffPresetDialog::update_bundles_from_app()
{
    *m_preset_bundle_left  = *wxGetApp().preset_bundle;
    *m_preset_bundle_right = *wxGetApp().preset_bundle;

    m_pr_technology = m_preset_bundle_left.get()->printers.get_edited_preset().printer_technology();
}

void DiffPresetDialog::show(Preset::Type type /* = Preset::TYPE_INVALID*/)
{
    this->SetTitle(_L("Compare presets"));
    m_view_type = type;

    update_bundles_from_app();
    update_controls_visibility(type);
    if (type == Preset::TYPE_INVALID)
        Fit();

    update_tree();
    wxGetApp().UpdateDlgDarkUI(this);

    // if this dialog is shown it have to be Hide and show again to be placed on the very Top of windows
    if (IsShown())
        Hide();
    Show();
}

void DiffPresetDialog::update_presets(Preset::Type type)
{
    update_bundles_from_app();
    update_controls_visibility(type);

    if (type == Preset::TYPE_INVALID)
        for (auto preset_combos : m_preset_combos) {
            if (preset_combos.presets_left->get_type() == Preset::TYPE_PRINTER) {
                preset_combos.presets_left->update_from_bundle ();
                preset_combos.presets_right->update_from_bundle();
                break;
            }
        }
    else
        for (auto preset_combos : m_preset_combos) {
            if (preset_combos.presets_left->get_type() == type) {
                preset_combos.presets_left->update();
                preset_combos.presets_right->update();
                break;
            }
        }

    update_tree();
}

void DiffPresetDialog::update_bottom_info(wxString bottom_info)
{
    const bool show_bottom_info = !m_tree->IsShown();
    if (show_bottom_info)
        m_bottom_info_line->SetLabel(bottom_info);
    m_bottom_info_line->Show(show_bottom_info);
}

void DiffPresetDialog::update_tree()
{
    Search::SettingsIndex& index = wxGetApp().sidebar().settings_index();
    index.sort_options_by_key();

    m_tree->Clear();
    wxString bottom_info = "";
    bool show_tree = false;
    const std::map<std::string, int> tab_order = build_tab_order_map();

    for (auto preset_combos : m_preset_combos)
    {
        if (!preset_combos.presets_left->IsShown())
            continue;
        Preset::Type type = preset_combos.presets_left->get_type();

        const PresetCollection* presets = get_preset_collection(type);
        const Preset* left_preset  = presets->find_preset(get_selection(preset_combos.presets_left));
        const Preset* right_preset = presets->find_preset(get_selection(preset_combos.presets_right));
        if (!left_preset || !right_preset) {
            bottom_info = _L("One of the presets does not exist");
            preset_combos.equal_bmp->SetBitmap_(ScalableBitmap(this, "question"));
            preset_combos.equal_bmp->SetToolTip(bottom_info);
            continue;
        }

        const DynamicPrintConfig& left_config   = left_preset->config;
        const PrinterTechnology&  left_pt       = left_preset->printer_technology();
        const DynamicPrintConfig& right_congig  = right_preset->config;

        if (left_pt != right_preset->printer_technology()) {
            bottom_info = _L("Compared presets has different printer technology");
            preset_combos.equal_bmp->SetBitmap_(ScalableBitmap(this, "question"));
            preset_combos.equal_bmp->SetToolTip(bottom_info);
            continue;
        }

        // Collect dirty options.
        const bool deep_compare = (type == Preset::TYPE_PRINTER ||
                                   type == Preset::TYPE_FILAMENT || type == Preset::TYPE_SLA_MATERIAL);
        auto dirty_options = type == Preset::TYPE_PRINTER && left_pt == ptFFF &&
                             left_config.opt<ConfigOptionStrings>("extruder_colour")->values.size() < right_congig.opt<ConfigOptionStrings>("extruder_colour")->values.size() ?
                             presets->dirty_options(right_preset, left_preset, deep_compare) :
                             presets->dirty_options(left_preset, right_preset, deep_compare);

        if (dirty_options.empty()) {
            //bottom_info = _L("Presets are the same");
            bottom_info = wxEmptyString;
            preset_combos.equal_bmp->SetBitmap_(ScalableBitmap(this, "equal"));
            preset_combos.equal_bmp->SetToolTip(bottom_info);
            continue;
        }

        show_tree = true;
        preset_combos.equal_bmp->SetBitmap_(ScalableBitmap(this, "not_equal"));
        /*preset_combos.equal_bmp->SetToolTip(_L("Presets are different.\n"
                                               "Click this button to select the same preset for the right and left preset."));*/

        preset_combos.equal_bmp->SetToolTip(wxEmptyString);

        // TRN Header of a preset comparison: the two preset names.
        m_tree->model->AddPreset(type, format_wxstr(_L("\"%1%\" vs \"%2%\""), left_preset->name, right_preset->name), left_pt);

        const std::map<wxString, std::string>& category_icon_map = wxGetApp().get_tab(type)->get_category_icon_map();
        auto get_category_icon = [&category_icon_map](const wxString& key) {
            auto it = category_icon_map.find(key);
            return it != category_icon_map.end() ? it->second : std::string();
        };

        // process changes of extruders count
        if (type == Preset::TYPE_PRINTER && left_pt == ptFFF &&
            left_config.opt<ConfigOptionStrings>("extruder_colour")->values.size() != right_congig.opt<ConfigOptionStrings>("extruder_colour")->values.size()) {
            wxString local_label = _L("Extruder count");
            wxString left_val = from_u8((boost::format("%1%") % left_config.opt<ConfigOptionStrings>("extruder_colour")->values.size()).str());
            wxString right_val = from_u8((boost::format("%1%") % right_congig.opt<ConfigOptionStrings>("extruder_colour")->values.size()).str());

            m_tree->Append("extruders_count", type, _L("General"), _L("Capabilities"), local_label, left_val, right_val,
                get_category_icon("Basic information"));
        }

        std::stable_sort(dirty_options.begin(), dirty_options.end(), [&tab_order](const std::string& a, const std::string& b) {
            return order_of(tab_order, a) < order_of(tab_order, b);
        });

        for (const std::string& opt_key : dirty_options) {
            wxString left_val = get_string_value(opt_key, left_config);
            wxString right_val = get_string_value(opt_key, right_congig);

            const std::string lookup_key = get_pure_opt_key(opt_key);
            Search::Option option = index.get_option(lookup_key, get_full_label(lookup_key, left_config), type);
            if (get_pure_opt_key(option.opt_key()) != lookup_key)
                option = index.get_option(opt_key, get_full_label(opt_key, left_config), type);
            if (get_pure_opt_key(option.opt_key()) != lookup_key) {
                // When the found option is not the requested one.
                // This can happen for dirty_options such as:
                // "default_print_profile", "printer_model", "printer_settings_id",
                // because they do not exist in the index.
                continue;
            }
            m_tree->Append(opt_key, type, option.category_local, option.group_local, option.label_local,
                left_val, right_val, get_category_icon(option.category));
        }
    }

    bool tree_was_shown = m_tree->IsShown();
    m_tree->Show(show_tree);

    bool can_transfer_options = m_view_type == Preset::TYPE_INVALID || get_left_preset_name(m_view_type) != get_right_preset_name(m_view_type);
    m_edit_sizer->Show(show_tree && can_transfer_options);
    m_buttons->Show(m_edit_sizer->IsShown(size_t(0)) && m_use_for_transfer->GetValue());
   
    update_bottom_info(bottom_info);

    if (tree_was_shown == m_tree->IsShown())
        Layout();
    else {
        Fit();
        Refresh();
    }

    // Revert sort of index back
    index.sort_options_by_label();
}

void DiffPresetDialog::on_dpi_changed(const wxRect&)
{
    int em = em_unit();

    const wxSize& size = wxSize(80 * em, 30 * em);
    SetMinSize(size);

    for (auto preset_combos : m_preset_combos) {
        preset_combos.presets_left->msw_rescale();
        preset_combos.equal_bmp->msw_rescale();
        preset_combos.presets_right->msw_rescale();
    }

    m_tree->Rescale(em);

    Fit();
    Refresh();
}

void DiffPresetDialog::on_sys_color_changed()
{
#ifdef _WIN32
    wxGetApp().UpdateAllStaticTextDarkUI(this);
    wxGetApp().UpdateDarkUI(m_show_all_presets);
    wxGetApp().UpdateDVCDarkUI(m_tree);
#endif

    for (auto preset_combos : m_preset_combos) {
        preset_combos.presets_left->msw_rescale();
        preset_combos.equal_bmp->msw_rescale();
        preset_combos.presets_right->msw_rescale();
    }

    // msw_rescale updates just icons, so use it
    m_tree->Rescale();
    Refresh();
}

void DiffPresetDialog::update_compatibility(const std::string& preset_name, Preset::Type type, PresetBundle* preset_bundle)
{
    PresetCollection* presets = get_preset_collection(type, preset_bundle);

    bool print_tab = type == Preset::TYPE_PRINT || type == Preset::TYPE_SLA_PRINT;
    bool printer_tab = type == Preset::TYPE_PRINTER;
    bool technology_changed = false;

    if (printer_tab) {
        const Preset& new_printer_preset = *presets->find_preset(preset_name, true);
        PrinterTechnology    old_printer_technology = presets->get_selected_preset().printer_technology();
        PrinterTechnology    new_printer_technology = new_printer_preset.printer_technology();

        technology_changed = old_printer_technology != new_printer_technology;
    }

    // select preset
    presets->select_preset_by_name(preset_name, false);

    // Mark the print & filament enabled if they are compatible with the currently selected preset.
    // The following method should not discard changes of current print or filament presets on change of a printer profile,
    // if they are compatible with the current printer.
    auto update_compatible_type = [](bool technology_changed, bool on_page, bool show_incompatible_presets) {
        return  technology_changed ? PresetSelectCompatibleType::Always :
            on_page ? PresetSelectCompatibleType::Never :
            show_incompatible_presets ? PresetSelectCompatibleType::OnlyIfWasCompatible : PresetSelectCompatibleType::Always;
    };
    if (print_tab || printer_tab)
        preset_bundle->update_compatible(
            update_compatible_type(technology_changed, print_tab, true),
            update_compatible_type(technology_changed, false, true));

    bool is_left_presets = preset_bundle == m_preset_bundle_left.get();
    PrinterTechnology pr_tech = preset_bundle->printers.get_selected_preset().printer_technology();

    // update preset comboboxes
    for (auto preset_combos : m_preset_combos)
    {
        PresetComboBox* cb = is_left_presets ? preset_combos.presets_left : preset_combos.presets_right;
        Preset::Type presets_type = cb->get_type();
        if ((print_tab && (
                (pr_tech == ptFFF && presets_type == Preset::TYPE_FILAMENT) ||
                (pr_tech == ptSLA && presets_type == Preset::TYPE_SLA_MATERIAL) )) ||
            (printer_tab && (
                (pr_tech == ptFFF && (presets_type == Preset::TYPE_PRINT || presets_type == Preset::TYPE_FILAMENT) ) ||
                (pr_tech == ptSLA && (presets_type == Preset::TYPE_SLA_PRINT || presets_type == Preset::TYPE_SLA_MATERIAL) )) ))
            cb->update();
    }

    if (technology_changed &&
        m_preset_bundle_left.get()->printers.get_selected_preset().printer_technology() ==
        m_preset_bundle_right.get()->printers.get_selected_preset().printer_technology())
    {
        m_pr_technology = m_preset_bundle_left.get()->printers.get_edited_preset().printer_technology();
        update_controls_visibility();
    }
}

void DiffPresetDialog::button_event(Action act)
{
    Hide();
    if (act == Action::Transfer)
        wxPostEvent(this, SimpleEvent(EVT_DIFF_DIALOG_TRANSFER));
}

std::string DiffPresetDialog::get_left_preset_name(Preset::Type type)
{
    PresetComboBox* cb = std::find_if(m_preset_combos.begin(), m_preset_combos.end(), [type](const DiffPresets& p) {
                             return p.presets_left->get_type() == type;
                         })->presets_left;
    return Preset::remove_suffix_modified(get_selection(cb));
}

std::string DiffPresetDialog::get_right_preset_name(Preset::Type type)
{
    PresetComboBox* cb = std::find_if(m_preset_combos.begin(), m_preset_combos.end(), [type](const DiffPresets& p) {
                             return p.presets_right->get_type() == type;
                         })->presets_right;
    return Preset::remove_suffix_modified(get_selection(cb));
}

}

}    // namespace Slic3r::GUI
