#include "UnsavedChangesDialog.hpp"

#include <cstddef>
#include <string>
#include <vector>
#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>

#include <wx/tokenzr.h>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Color.hpp"
#include "format.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "ExtraRenderers.hpp"
#include "wxExtensions.hpp"
#include "SavePresetDialog.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"

#include "PresetComboBoxes.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"

using boost::optional;

#ifdef __linux__
#define wxLinux true
#else
#define wxLinux false
#endif

namespace Slic3r {

namespace GUI {

wxDEFINE_EVENT(EVT_DIFF_DIALOG_TRANSFER, SimpleEvent);


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
            m_old_value = _L("Undef");
    }
    else {
        m_old_color_bmp = get_bitmap(m_old_color);
        m_old_value.Clear();
    }

    if (m_new_color.IsEmpty()) {
        if (!m_old_color.IsEmpty())
            m_new_value = _L("Undef");
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


static std::string get_pure_opt_key(std::string opt_key)
{
    const int pos = opt_key.find("#");
    if (pos > 0)
        boost::erase_tail(opt_key, opt_key.size() - pos);
    return opt_key;
}

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
#define UNSAVE_CHANGE_DIALOG_ACTION_LINE_SIZE wxSize(FromDIP(490), FromDIP(60))
#define UNSAVE_CHANGE_DIALOG_FIRST_VALUE_WIDTH FromDIP(190)
#define UNSAVE_CHANGE_DIALOG_VALUE_WIDTH FromDIP(150)
#define UNSAVE_CHANGE_DIALOG_ITEM_HEIGHT FromDIP(24)
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
                caption + ": " + _L("Unsaved Changes"),
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

UnsavedChangesDialog::UnsavedChangesDialog(Preset::Type type, PresetCollection *dependent_presets, const std::string &new_selected_preset, bool no_transfer)
    : m_new_selected_preset_name(new_selected_preset)
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
        m_exit_action = Action(result);
        return 0;
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

    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);

    m_top_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_top_line->SetBackgroundColour(wxColour(166, 169, 170));

    m_sizer_main->Add(m_top_line, 0, wxEXPAND, 0);

    m_sizer_main->Add(0, 0, 0, wxTOP, 20);

    m_action_line = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, UNSAVE_CHANGE_DIALOG_ACTION_LINE_SIZE, 0);
    m_action_line->SetFont(::Label::Body_13);
    m_action_line->SetForegroundColour(GREY900);
    m_action_line->Wrap(-1);
    m_sizer_main->Add(m_action_line, 0, wxLEFT | wxRIGHT, 20);

    m_sizer_main->Add(0, 0, 0, wxTOP, 12);

    m_panel_tab = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(UNSAVE_CHANGE_DIALOG_SCROLL_WINDOW_SIZE.x, -1), wxTAB_TRAVERSAL);
    m_panel_tab->SetBackgroundColour(GREY200);
    wxBoxSizer *m_sizer_tab = new wxBoxSizer(wxVERTICAL);

    m_table_top = new wxPanel(m_panel_tab, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_table_top->SetBackgroundColour(wxColour(107, 107, 107));

    wxBoxSizer *m_sizer_top = new wxBoxSizer(wxHORIZONTAL);

    //m_sizer_top->Add(0, 0, 0, wxLEFT, UNSAVE_CHANGE_DIALOG_FIRST_VALUE_WIDTH);
    auto        m_panel_temp   = new wxPanel(m_table_top, wxID_ANY, wxDefaultPosition, wxSize(UNSAVE_CHANGE_DIALOG_FIRST_VALUE_WIDTH, -1), wxTAB_TRAVERSAL);
    wxBoxSizer *top_title_temp_v = new wxBoxSizer(wxVERTICAL);
    top_title_temp_v->SetMinSize(wxSize(UNSAVE_CHANGE_DIALOG_VALUE_WIDTH, -1));
    wxBoxSizer *top_title_temp_h = new wxBoxSizer(wxHORIZONTAL);
    static_temp_title            = new wxStaticText(m_panel_temp, wxID_ANY, _L("Settings"), wxDefaultPosition, wxDefaultSize, 0);
    static_temp_title->SetFont(::Label::Body_13);
    static_temp_title->Wrap(-1);
    static_temp_title->SetForegroundColour(*wxWHITE);
    top_title_temp_h->Add(static_temp_title, 0, wxALIGN_CENTER | wxBOTTOM | wxTOP, 5);
    top_title_temp_v->Add(top_title_temp_h, 1, wxALIGN_CENTER, 0);
    m_panel_temp->SetSizer(top_title_temp_v);
    m_panel_temp->Layout();
    m_sizer_top->Add(m_panel_temp, 1, wxALIGN_CENTER, 0);


    title_block_middle = new wxPanel(m_table_top, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    title_block_middle->SetBackgroundColour(wxColour(172, 172, 172));

    m_sizer_top->Add(title_block_middle, 0, wxBOTTOM | wxEXPAND | wxTOP, 2);
    auto m_panel_oldv = new wxPanel( m_table_top, wxID_ANY, wxDefaultPosition, wxSize(UNSAVE_CHANGE_DIALOG_VALUE_WIDTH,-1), wxTAB_TRAVERSAL );
    wxBoxSizer *top_title_oldv = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *top_title_oldv_h = new wxBoxSizer(wxHORIZONTAL);

    static_oldv_title = new wxStaticText(m_panel_oldv, wxID_ANY, _L("Old Value"), wxDefaultPosition, wxDefaultSize, 0);
    static_oldv_title->SetFont(::Label::Body_13);
    static_oldv_title->Wrap(-1);
    static_oldv_title->SetForegroundColour(*wxWHITE);
    top_title_oldv_h->Add(static_oldv_title, 0, wxALIGN_CENTER | wxBOTTOM | wxTOP, 5);
    top_title_oldv->Add(top_title_oldv_h, 1, wxALIGN_CENTER, 0);
    m_panel_oldv->SetSizer(top_title_oldv);
    m_panel_oldv->Layout();
    m_sizer_top->Add(m_panel_oldv, 0, wxALIGN_CENTER, 0);

    title_block_right = new wxPanel(m_table_top, wxID_ANY, wxDefaultPosition, wxSize(1, -1), wxTAB_TRAVERSAL);
    title_block_right->SetBackgroundColour(wxColour(172, 172, 172));

    m_sizer_top->Add(title_block_right, 0, wxBOTTOM | wxEXPAND | wxTOP, 2);

    auto m_panel_newv = new wxPanel( m_table_top, wxID_ANY, wxDefaultPosition, wxSize( UNSAVE_CHANGE_DIALOG_VALUE_WIDTH,-1 ), wxTAB_TRAVERSAL );
    wxBoxSizer *top_title_newv = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *top_title_newv_h = new wxBoxSizer(wxHORIZONTAL);

    static_newv_title = new wxStaticText(m_panel_newv, wxID_ANY, _L("New Value"), wxDefaultPosition, wxDefaultSize, 0);
    static_newv_title->SetFont(::Label::Body_13);
    static_newv_title->Wrap(-1);
    static_newv_title->SetForegroundColour(*wxWHITE);

    top_title_newv_h->Add(static_newv_title, 0, wxALIGN_CENTER | wxBOTTOM | wxTOP, 5);

    top_title_newv->Add(top_title_newv_h, 1, wxALIGN_CENTER, 0);

    m_panel_newv->SetSizer(top_title_newv);
    m_panel_newv->Layout();
    m_sizer_top->Add(m_panel_newv, 0, wxALIGN_CENTER, 0);
    //m_sizer_top->Add(top_title_newv, 1, wxALIGN_CENTER, 0);

    m_table_top->SetSizer(m_sizer_top);
    m_table_top->Layout();
    m_sizer_top->Fit(m_table_top);
    m_sizer_tab->Add(m_table_top, 1, 0, 0);

    m_scrolledWindow = new wxScrolledWindow(m_panel_tab, wxID_ANY, wxDefaultPosition, UNSAVE_CHANGE_DIALOG_SCROLL_WINDOW_SIZE,  wxNO_BORDER|wxVSCROLL);
    m_scrolledWindow->SetScrollRate(0, 5);
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
        if (dependent_presets && switched_presets && (type == dependent_presets->type() ?
            dependent_presets->get_edited_preset().printer_technology() == dependent_presets->find_preset(new_selected_preset)->printer_technology() :
            switched_presets->get_edited_preset().printer_technology() == switched_presets->find_preset(new_selected_preset)->printer_technology()))
            add_btn(&m_transfer_btn, m_move_btn_id, "menu_paste", Action::Transfer, switched_presets->get_edited_preset().name == new_selected_preset ? _L("Transfer") : _L("Transfer"), true);
    }
    if (!m_transfer_btn && (ActionButtons::KEEP & m_buttons))
        add_btn(&m_transfer_btn, m_move_btn_id, "menu_paste", Action::Transfer, _L("Transfer"), true);

    { // "Don't save" / "Discard" button
        std::string btn_icon    = (ActionButtons::DONT_SAVE & m_buttons) ? "" : (dependent_presets || (ActionButtons::KEEP & m_buttons)) ? "blank_16" : "exit";
        wxString    btn_label   = (ActionButtons::DONT_SAVE & m_buttons) ? _L("Don't save") : _L("Discard");
        add_btn(&m_discard_btn, m_continue_btn_id, btn_icon, Action::Discard, btn_label, false);
    }

    // "Save" button
    if (ActionButtons::SAVE & m_buttons) add_btn(&m_save_btn, m_save_btn_id, "save", Action::Save, _L("Save"), false);

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

    SetSizer(m_sizer_main);
    Layout();
    Fit();
    Centre(wxBOTH);


    update(type, dependent_presets, new_selected_preset, header);

    //SetSizer(topSizer);
    //topSizer->SetSizeHints(this);

    show_info_line(Action::Undef);
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
            text = ActionButtons::DONT_SAVE & m_buttons ? _L("All changes will not be saved") :_L("All changes will be discarded.");
        else {
            if (preset_name.empty())
                text = action == Action::Save           ? _L("Save the selected options.") :
                       ActionButtons::KEEP & m_buttons  ? _L("Keep the selected options.") :
                                                          _L("Transfer the selected options to the newly selected preset.");
            else
                text = format_wxstr(
                    action == Action::Save ?
                        _L("Save the selected options to preset \n\"%1%\".") :
                        _L("Transfer the selected options to the newly selected preset \n\"%1%\"."),
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
        if (preset.is_system || preset.is_default) {
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
                if (preset.is_system || preset.is_default)
                //if (preset.is_system || preset.is_default || preset.is_external)
                    types_for_save.emplace_back(preset.type);

                //BBS: add project embedded preset relate logic
                PresetData preset_data(preset.name, preset.type, preset.is_project_embedded);
                names_and_types.emplace_back(preset_data);
                //names_and_types.emplace_back(make_pair(preset.name, preset.type));
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

wxString get_string_from_enum(const std::string& opt_key, const DynamicPrintConfig& config, bool is_infill = false, int idx = -1)
{
    const ConfigOptionDef& def = config.def()->options.at(opt_key);
    const std::vector<std::string>& names = def.enum_labels;//ConfigOptionEnum<T>::get_enum_names();
    int val = 0;

    if (idx >= 0)
        val = dynamic_cast<const ConfigOptionInts*>(config.option(opt_key))->get_at(idx);
    else
        val = config.option(opt_key)->getInt();

    // Each infill doesn't use all list of infill declared in PrintConfig.hpp.
    // So we should "convert" val to the correct one
    if (is_infill) {
        for (auto key_val : *def.enum_keys_map)
            if (int(key_val.second) == val) {
                auto it = std::find(def.enum_values.begin(), def.enum_values.end(), key_val.first);
                if (it == def.enum_values.end())
                    return "";
                return from_u8(_utf8(names[it - def.enum_values.begin()]));
            }
        return _L("Undef");
    }
    return from_u8(_utf8(names[val]));
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

static wxString get_full_label(std::string opt_key, const DynamicPrintConfig& config)
{
    opt_key = get_pure_opt_key(opt_key);
    auto option = config.option(opt_key);

    if (!option || option->is_nil())
        return _L("N/A");

    const ConfigOptionDef* opt = config.def()->get(opt_key);
    return opt->full_label.empty() ? opt->label : opt->full_label;
}

static wxString get_string_value(std::string opt_key, const DynamicPrintConfig& config)
{
    int orig_opt_idx = -1;
    int opt_idx = -1;
    int pos = opt_key.find("#");
    std::string temp_str = opt_key;
    if (pos > 0) {
        boost::erase_head(temp_str, pos + 1);
        orig_opt_idx = static_cast<size_t>(atoi(temp_str.c_str()));
    }
    opt_idx = orig_opt_idx >= 0 ? orig_opt_idx : 0;
    opt_key = get_pure_opt_key(opt_key);
    auto option = config.option(opt_key);
    if (!option) {
        return _L("N/A");
    }

    if (option->is_scalar() && config.option(opt_key)->is_nil() ||
        option->is_vector() && dynamic_cast<const ConfigOptionVectorBase *>(config.option(opt_key))->is_nil(opt_idx))
        return _L("N/A");

    wxString out;

    const ConfigOptionDef* opt = config.def()->get(opt_key);
    bool is_nullable = opt->nullable;

    switch (opt->type) {
    case coInt:
        return from_u8((boost::format("%1%") % config.opt_int(opt_key)).str());
    case coInts: {
        if (is_nullable) {
            auto values = config.opt<ConfigOptionIntsNullable>(opt_key);
            if (opt_idx < values->size())
                return from_u8((boost::format("%1%") % values->get_at(opt_idx)).str());
        }
        else {
            auto values = config.opt<ConfigOptionInts>(opt_key);
            if (orig_opt_idx >= 0 && orig_opt_idx < values->size()) {
                return from_u8((boost::format("%1%") % values->get_at(opt_idx)).str());
            }
            else {
                std::string value_str;
                for (int i = 0; i < values->size(); i++) {
                    value_str += std::to_string(values->get_at(i));
                    if (i != values->size() - 1) {
                        value_str += ",";
                    }
                }
                return from_u8(value_str);
            }
        }
        return _L("Undef");
    }
    case coBool:
        return config.opt_bool(opt_key) ? "true" : "false";
    case coBools: {
        if (is_nullable) {
            auto values = config.opt<ConfigOptionBoolsNullable>(opt_key);
            if (opt_idx < values->size())
                return values->get_at(opt_idx) ? "true" : "false";
        }
        else {
            auto values = config.opt<ConfigOptionBools>(opt_key);
            if (opt_idx < values->size())
                return values->get_at(opt_idx) ? "true" : "false";
        }
        return _L("Undef");
    }
    case coPercent:
        return from_u8((boost::format("%1%%%") % int(config.optptr(opt_key)->getFloat())).str());
    case coPercents: {
        if (is_nullable) {
            auto values = config.opt<ConfigOptionPercentsNullable>(opt_key);
            if (opt_idx < values->size())
                return from_u8((boost::format("%1%%%") % values->get_at(opt_idx)).str());
        }
        else {
            auto values = config.opt<ConfigOptionPercents>(opt_key);
            if (opt_idx < values->size())
                return from_u8((boost::format("%1%%%") % values->get_at(opt_idx)).str());
        }
        return _L("Undef");
    }
    case coFloat:
        return double_to_string(config.opt_float(opt_key));
    case coFloats: {
        if (is_nullable) {
            auto values = config.opt<ConfigOptionFloatsNullable>(opt_key);
            if (opt_idx < values->size())
                return double_to_string(values->get_at(opt_idx));
        }
        else {
            auto values = config.opt<ConfigOptionFloats>(opt_key);
            if (values && opt_idx < values->size())
                return double_to_string(values->get_at(opt_idx));
        }
        return _L("Undef");
    }
    case coString:
        return from_u8(config.opt_string(opt_key));
    case coStrings: {
        const ConfigOptionStrings* strings = config.opt<ConfigOptionStrings>(opt_key);
        if (strings) {
            if (opt_key == "compatible_printers" || opt_key == "compatible_prints") {
                if (strings->empty())
                    return _L("All");
                for (size_t id = 0; id < strings->size(); id++)
                    out += from_u8(strings->get_at(id)) + "\n";
                out.RemoveLast(1);
                return out;
            }
            if (!strings->empty() && opt_idx < strings->values.size())
                return from_u8(strings->get_at(opt_idx));
        }
        break;
        }
    case coFloatOrPercent: {
        const ConfigOptionFloatOrPercent* opt = config.opt<ConfigOptionFloatOrPercent>(opt_key);
        if (opt)
            out = double_to_string(opt->value) + (opt->percent ? "%" : "");
        return out;
    }
    case coEnum: {
        return get_string_from_enum(opt_key, config,
            opt_key == "top_surface_pattern" ||
            opt_key == "bottom_surface_pattern" ||
            opt_key == "internal_solid_infill_pattern" ||
            opt_key == "sparse_infill_pattern" ||
            opt_key == "ironing_pattern" ||
            opt_key == "support_ironing_pattern" ||
            opt_key == "support_pattern" ||
            opt_key == "support_interface_pattern")
            ;
    }
    case coEnums: {
        return get_string_from_enum(opt_key, config,
            opt_key == "top_surface_pattern" ||
            opt_key == "bottom_surface_pattern" ||
            opt_key == "internal_solid_infill_pattern" ||
            opt_key == "sparse_infill_pattern" ||
            opt_key == "ironing_pattern" ||
            opt_key == "support_ironing_pattern" ||
            opt_key == "support_pattern" ||
            opt_key == "support_interface_pattern"
            , opt_idx);
    }
    case coPoint: {
        Vec2d val = config.opt<ConfigOptionPoint>(opt_key)->value;
        return from_u8((boost::format("[%1%]") % ConfigOptionPoint(val).serialize()).str());
    }
    case coPoints: {
        //BBS: add bed_exclude_area
        if (opt_key == "printable_area" || opt_key == "thumbnails") {
            ConfigOptionPoints points = *config.option<ConfigOptionPoints>(opt_key);
            //BuildVolume build_volume = {points.values, 0.};
            return get_thumbnails_string(points.values);
        }
        else if (opt_key == "bed_exclude_area") {
            return get_thumbnails_string(config.option<ConfigOptionPoints>(opt_key)->values);
        }
        else if (opt_key == "head_wrap_detect_zone") {
            return get_thumbnails_string(config.option<ConfigOptionPoints>(opt_key)->values);
        }
        else if (opt_key == "wrapping_exclude_area") {
            return get_thumbnails_string(config.option<ConfigOptionPoints>(opt_key)->values);
        }
        Vec2d val = config.opt<ConfigOptionPoints>(opt_key)->get_at(opt_idx);
        return from_u8((boost::format("[%1%]") % ConfigOptionPoint(val).serialize()).str());
    }
    default:
        break;
    }
    return out;
}

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

    update_tree(type, presets);
    update_list();
}

void UnsavedChangesDialog::update_list()
{
    std::map<wxString, std::vector<PresetItem>> class_g_list;
    std::map<wxString, std::vector<wxString>>   class_c_list;

    // group
    for (auto i = 0; i < m_presetitems.size(); i++) {
        if (class_g_list.count(m_presetitems[i].group_name) <= 0) {
            std::vector<PresetItem> vp;
            vp.push_back(m_presetitems[i]);
            class_g_list.emplace(m_presetitems[i].group_name, vp);
        } else {
            //for (auto iter = class_g_list.begin(); iter != class_g_list.end(); iter++) iter->second.push_back(m_presetitems[i]);
            class_g_list[m_presetitems[i].group_name].push_back(m_presetitems[i]);
        }
    }

    // category
    for (auto i = 0; i < m_presetitems.size(); i++) {
        if (class_c_list.count(m_presetitems[i].category_name) <= 0) {
            std::vector<wxString> vp;
            vp.push_back(m_presetitems[i].group_name);
            class_c_list.emplace(m_presetitems[i].category_name, vp);
        } else {
            /*for (auto iter = class_c_list.begin(); iter != class_c_list.end(); iter++)
                iter->second.push_back(m_presetitems[i].group_name);*/
            //class_c_list[m_presetitems[i].category_name].push_back(m_presetitems[i].group_name);
            std::vector<wxString>::iterator it;
            it = find(class_c_list[m_presetitems[i].category_name].begin(), class_c_list[m_presetitems[i].category_name].end(), m_presetitems[i].group_name);
            if (it == class_c_list[m_presetitems[i].category_name].end()) {
                class_c_list[m_presetitems[i].category_name].push_back(m_presetitems[i].group_name);
            }
        }
    }



    auto m_listsizer = new wxBoxSizer(wxVERTICAL);
    for (auto iter = class_c_list.begin(); iter != class_c_list.end(); iter++) {

        //category
        auto panel_category = new wxPanel(m_scrolledWindow, wxID_ANY, wxDefaultPosition, wxSize(-1, UNSAVE_CHANGE_DIALOG_ITEM_HEIGHT), wxTAB_TRAVERSAL);
        panel_category->SetBackgroundColour(GREY300);

        wxBoxSizer *sizer_category   = new wxBoxSizer(wxHORIZONTAL);
        wxBoxSizer *sizer_category_v = new wxBoxSizer(wxHORIZONTAL);

        auto text_category = new wxStaticText(panel_category, wxID_ANY, iter->first, wxDefaultPosition, wxSize(-1, -1), 0);
        text_category->SetFont(::Label::Head_13);
        text_category->SetForegroundColour(GREY900);
        text_category->Wrap(-1);

        sizer_category_v->Add(text_category, 0, wxALIGN_CENTER | wxLEFT, 23);

        sizer_category->Add(sizer_category_v, 1, wxEXPAND, 0);

        panel_category->SetSizer(sizer_category);
        panel_category->Layout();
        m_listsizer->Add(panel_category, 0, wxEXPAND, 0);

        /*auto item_line = new wxStaticLine(list, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL);
        item_line->SetForegroundColour(wxColour(206, 206, 206));
        item_line->SetBackgroundColour(wxColour(206, 206, 206));
        sizer_list->Add(item_line, 0, wxALL, 0); */

        // isset group
        for (auto i = 0; i < iter->second.size(); i++) {
            auto gname = iter->second[i];
            for (auto g = 0; g < class_g_list[gname].size(); g++) {

                 //first group
                if (g == 0) {
                     auto panel_item = new wxWindow(m_scrolledWindow, -1, wxDefaultPosition, wxSize(-1, UNSAVE_CHANGE_DIALOG_ITEM_HEIGHT));
                     panel_item->SetBackgroundColour(GREY200);

                     wxBoxSizer *sizer_item = new wxBoxSizer(wxHORIZONTAL);

                     auto panel_left = new wxPanel(panel_item, wxID_ANY, wxDefaultPosition, wxSize(UNSAVE_CHANGE_DIALOG_FIRST_VALUE_WIDTH, -1), wxTAB_TRAVERSAL);
                     panel_left->SetBackgroundColour(GREY200);

                     wxBoxSizer *sizer_left_v = new wxBoxSizer(wxVERTICAL);

                     auto text_left = new wxStaticText(panel_left, wxID_ANY, gname, wxDefaultPosition, wxSize(-1, -1), 0);
                     text_left->SetFont(::Label::Head_13);
                     text_left->Wrap(-1);
                     text_left->SetForegroundColour(GREY700);

                     sizer_left_v->Add(text_left, 0, wxLEFT, 37);

                     panel_left->SetSizer(sizer_left_v);
                     panel_left->Layout();
                     sizer_item->Add(panel_left, 0, wxALIGN_CENTER, 0);

                     panel_item->SetSizer(sizer_item);
                     panel_item->Layout();
                     m_listsizer->Add(panel_item, 0, wxEXPAND, 0);
                }

                auto data = class_g_list[gname][g];

                auto panel_item = new wxWindow(m_scrolledWindow, -1, wxDefaultPosition, wxSize(-1, UNSAVE_CHANGE_DIALOG_ITEM_HEIGHT));
                panel_item->SetBackgroundColour(GREY200);

                wxBoxSizer *sizer_item = new wxBoxSizer(wxHORIZONTAL);

                auto panel_left = new wxPanel(panel_item, wxID_ANY, wxDefaultPosition, wxSize(UNSAVE_CHANGE_DIALOG_FIRST_VALUE_WIDTH , -1), wxTAB_TRAVERSAL);
                panel_left->SetBackgroundColour(GREY200);

                wxBoxSizer *sizer_left_v = new wxBoxSizer(wxVERTICAL);

                auto text_left = new wxStaticText(panel_left, wxID_ANY, data.option_name, wxDefaultPosition, wxSize(-1, -1), 0);
                text_left->SetFont(::Label::Body_13);
                text_left->Wrap(-1);
                text_left->SetForegroundColour(GREY700);

                sizer_left_v->Add(text_left, 0, wxLEFT, 51 );

                panel_left->SetSizer(sizer_left_v);
                panel_left->Layout();
                sizer_item->Add(panel_left, 0, wxALIGN_CENTER, 0);

                auto        panel_oldv  = new wxPanel(panel_item, wxID_ANY, wxDefaultPosition, wxSize(UNSAVE_CHANGE_DIALOG_VALUE_WIDTH, -1), wxTAB_TRAVERSAL);
                wxBoxSizer *sizer_old_v = new wxBoxSizer(wxVERTICAL);


                data.old_value = subreplace(data.old_value.ToStdString(), "\n", " ");
                auto text_oldv = new wxStaticText(panel_oldv, wxID_ANY, data.old_value, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
                text_oldv->SetFont(::Label::Body_13);
                text_oldv->Wrap(-1);
                text_oldv->SetForegroundColour(GREY700);
                sizer_old_v->Add(text_oldv, 0, wxALIGN_CENTER|wxLEFT|wxRIGHT, 5);

                panel_oldv->SetSizer(sizer_old_v);
                panel_oldv->Layout();
                sizer_item->Add(panel_oldv, 0, wxALIGN_CENTER, 0);

                auto        panel_newv  = new wxPanel(panel_item, wxID_ANY, wxDefaultPosition, wxSize(UNSAVE_CHANGE_DIALOG_VALUE_WIDTH, -1), wxTAB_TRAVERSAL);
                wxBoxSizer *sizer_new_v = new wxBoxSizer(wxVERTICAL);

                data.new_value = subreplace(data.new_value.ToStdString(), "\n", " ");
                auto text_newv = new wxStaticText(panel_newv, wxID_ANY, data.new_value, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
                text_newv->SetFont(::Label::Body_13);
                text_newv->Wrap(-1);
                text_newv->SetForegroundColour(GREY700);

                sizer_new_v->Add(text_newv, 0, wxALIGN_CENTER|wxLEFT|wxRIGHT, 5);

                panel_newv->SetSizer(sizer_new_v);
                panel_newv->Layout();
                sizer_item->Add(panel_newv, 0, wxALIGN_CENTER, 0);

                panel_item->SetSizer(sizer_item);
                panel_item->Layout();
                m_listsizer->Add(panel_item, 0, wxEXPAND, 0);



                ////if (g == class_g_list[gname].size() - 1) {
                //    auto item_line = new wxStaticLine(list, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL);
                //    item_line->SetForegroundColour(wxColour(206, 206, 206));
                //    item_line->SetBackgroundColour(wxColour(206, 206, 206));
                //    sizer_list->Add(item_line, 0, wxALL, 0);
                ////}
            }
        }
    }

       m_scrolledWindow->SetSizer(m_listsizer);
    // m_scrolledWindow->Layout();
       wxSize text_size = m_action_line->GetTextExtent(m_action_line->GetLabel());
       int    width     = UNSAVE_CHANGE_DIALOG_ACTION_LINE_SIZE.GetWidth();
       // +2: Ensure that there is at least one line and that the content contains '\n'
       int    rows      = int(text_size.GetWidth() / width) + 2; 
       int    height    = rows * text_size.GetHeight();
       m_action_line->SetMinSize(wxSize(width, height));
       Layout();
       Fit();
}

std::string UnsavedChangesDialog::subreplace(std::string resource_str, std::string sub_str, std::string new_str)
{
    std::string            dst_str = resource_str;
    std::string::size_type pos     = 0;
    while ((pos = dst_str.find(sub_str)) != std::string::npos)
    {
        dst_str.replace(pos, sub_str.length(), new_str);
    }
    return dst_str;
}

void UnsavedChangesDialog::update_tree(Preset::Type type, PresetCollection* presets_)
{
    Search::OptionsSearcher& searcher = wxGetApp().sidebar().get_searcher();
    searcher.sort_options_by_key();

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
        const bool deep_compare = (type == Preset::TYPE_PRINTER || type == Preset::TYPE_SLA_MATERIAL);
        auto dirty_options = presets->current_dirty_options(deep_compare);

        // process changes of extruders count
        if (type == Preset::TYPE_PRINTER && old_pt == ptFFF &&
            old_config.opt<ConfigOptionFloats>("nozzle_diameter")->values.size() != new_config.opt<ConfigOptionFloats>("nozzle_diameter")->values.size()) {
            wxString local_label = _L("Extruders count");
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

        for (const std::string& opt_key : dirty_options) {
            const Search::Option& option = searcher.get_option(opt_key, type);
            if (option.opt_key() != opt_key) {
                // When founded option isn't the correct one.
                // It can be for dirty_options: "default_print_profile", "printer_model", "printer_settings_id",
                // because of they don't exist in searcher
                continue;
            }

            /*m_tree->Append(opt_key, type, option.category_local, option.group_local, option.label_local,
                get_string_value(opt_key, old_config), get_string_value(opt_key, new_config), category_icon_map.at(option.category));*/


            //PresetItem pi = {opt_key, type, 1983};
            //m_presetitems.push_back()
            PresetItem pi = {type, opt_key, option.category_local, option.group_local, option.label_local, get_string_value(opt_key, old_config), get_string_value(opt_key, new_config)};
            m_presetitems.push_back(pi);

        }
    }

    // Revert sort of searcher back
    searcher.sort_options_by_label();
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

    auto add_value = [grid_sizer, border, this](wxString label, const std::set<wxString>& diff_set, bool is_colored = false) {
        wxTextCtrl* text = new wxTextCtrl(this, wxID_ANY, label, wxDefaultPosition, wxSize(400, 400), wxTE_MULTILINE | wxTE_READONLY | wxBORDER_DEFAULT | wxTE_RICH);
        wxGetApp().UpdateDarkUI(text);
        text->SetStyle(0, label.Len(), wxTextAttr(is_colored ? wxColour(orange) : wxNullColour, wxNullColour, this->GetFont()));

        for (const wxString& str : diff_set) {
            int pos = label.First(str);
            if (pos == wxNOT_FOUND)
                continue;
            text->SetStyle(pos, pos + (int)str.Len(), wxTextAttr(is_colored ? wxColour(orange) : wxNullColour, wxNullColour, this->GetFont().Bold()));
        }

        grid_sizer->Add(text, 1, wxALL | wxEXPAND, border);
    };
    add_value(old_value, old_new_diff_set);
    add_value(new_value, new_old_diff_set, true);

    sizer->Add(grid_sizer, 1, wxEXPAND);

    auto dlg_btns = new DialogButtons(this, {"OK"});

    wxBoxSizer* topSizer = new wxBoxSizer(wxVERTICAL);

    topSizer->Add(sizer,   1, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, border);
    topSizer->Add(dlg_btns , 0, wxEXPAND);

    SetSizer(topSizer);
    topSizer->SetSizeHints(this);

    wxGetApp().UpdateDlgDarkUI(this);
}


static PresetCollection* get_preset_collection(Preset::Type type, PresetBundle* preset_bundle = nullptr) {
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

    for (auto new_type : { Preset::TYPE_PRINTER, Preset::TYPE_FILAMENT, Preset::TYPE_SLA_MATERIAL, Preset::TYPE_PRINT, Preset::TYPE_SLA_PRINT })
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
    m_tree->AppendBmpTextColumn("Left Preset Value", DiffModel::colOldValue, 15);
    m_tree->AppendBmpTextColumn("Right Preset Value",DiffModel::colNewValue, 15);
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

    m_preset_bundle_left  = std::make_unique<PresetBundle>(*wxGetApp().preset_bundle);
    m_preset_bundle_right = std::make_unique<PresetBundle>(*wxGetApp().preset_bundle);

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
    Search::OptionsSearcher& searcher = wxGetApp().sidebar().get_searcher();
    searcher.sort_options_by_key();

    m_tree->Clear();
    wxString bottom_info = "";
    bool show_tree = false;

    for (auto preset_combos : m_preset_combos)
    {
        if (!preset_combos.presets_left->IsShown())
            continue;
        Preset::Type type = preset_combos.presets_left->get_type();

        const PresetCollection* presets = get_preset_collection(type);
        const Preset* left_preset  = presets->find_preset(get_selection(preset_combos.presets_left));
        const Preset* right_preset = presets->find_preset(get_selection(preset_combos.presets_right));
        if (!left_preset || !right_preset) {
            bottom_info = "One of the presets does not exist";
            preset_combos.equal_bmp->SetBitmap_(ScalableBitmap(this, "question"));
            preset_combos.equal_bmp->SetToolTip(bottom_info);
            continue;
        }

        const DynamicPrintConfig& left_config   = left_preset->config;
        const PrinterTechnology&  left_pt       = left_preset->printer_technology();
        const DynamicPrintConfig& right_congig  = right_preset->config;

        if (left_pt != right_preset->printer_technology()) {
            bottom_info = "Compared presets has different printer technology";
            preset_combos.equal_bmp->SetBitmap_(ScalableBitmap(this, "question"));
            preset_combos.equal_bmp->SetToolTip(bottom_info);
            continue;
        }

        // Collect dirty options.
        const bool deep_compare = (type == Preset::TYPE_PRINTER || type == Preset::TYPE_SLA_MATERIAL);
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

        m_tree->model->AddPreset(type, "\"" + from_u8(left_preset->name) + "\" vs \"" + from_u8(right_preset->name) + "\"", left_pt);

        const std::map<wxString, std::string>& category_icon_map = wxGetApp().get_tab(type)->get_category_icon_map();

        // process changes of extruders count
        if (type == Preset::TYPE_PRINTER && left_pt == ptFFF &&
            left_config.opt<ConfigOptionStrings>("extruder_colour")->values.size() != right_congig.opt<ConfigOptionStrings>("extruder_colour")->values.size()) {
            wxString local_label = _L("Extruders count");
            wxString left_val = from_u8((boost::format("%1%") % left_config.opt<ConfigOptionStrings>("extruder_colour")->values.size()).str());
            wxString right_val = from_u8((boost::format("%1%") % right_congig.opt<ConfigOptionStrings>("extruder_colour")->values.size()).str());

            m_tree->Append("extruders_count", type, "General", "Capabilities", local_label, left_val, right_val, category_icon_map.at("Basic information"));
        }

        for (const std::string& opt_key : dirty_options) {
            wxString left_val = get_string_value(opt_key, left_config);
            wxString right_val = get_string_value(opt_key, right_congig);

            Search::Option option = searcher.get_option(opt_key, get_full_label(opt_key, left_config), type);
            if (option.opt_key() != opt_key) {
                // temporary solution, just for testing
                m_tree->Append(opt_key, type, "Undef category", "Undef group", opt_key, left_val, right_val, "undefined"); // ORCA: use low resolution compatible icon
                // When founded option isn't the correct one.
                // It can be for dirty_options: "default_print_profile", "printer_model", "printer_settings_id",
                // because of they don't exist in searcher
                continue;
            }
            m_tree->Append(opt_key, type, option.category_local, option.group_local, option.label_local,
                left_val, right_val, category_icon_map.at(option.category));
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

    // Revert sort of searcher back
    searcher.sort_options_by_label();
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
