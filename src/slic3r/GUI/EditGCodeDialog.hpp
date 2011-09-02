#ifndef slic3r_EditGCodeDialog_hpp_
#define slic3r_EditGCodeDialog_hpp_

#include <vector>

#include <wx/gdicmn.h>
#include <slic3r/GUI/Widgets/Button.hpp>

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <wx/srchctrl.h>

class wxListBox;
class wxTextCtrl;
class ScalableButton;

namespace Slic3r {

namespace GUI {

class ParamsViewCtrl;

//------------------------------------------
//          EditGCodeDialog
//------------------------------------------

class EditGCodeDialog : public DPIDialog
{
    ParamsViewCtrl*   m_params_list   {nullptr};
    ScalableButton*   m_add_btn       {nullptr};
    wxTextCtrl*       m_gcode_editor  {nullptr};
    wxStaticText*     m_param_label   {nullptr};
    wxStaticText*     m_param_description {nullptr};
    wxSearchCtrl*     m_search_bar   {nullptr};

    ReadOnlySlicingStatesConfigDef  cgp_ro_slicing_states_config_def;
    ReadWriteSlicingStatesConfigDef cgp_rw_slicing_states_config_def;
    OtherSlicingStatesConfigDef     cgp_other_slicing_states_config_def;
    PrintStatisticsConfigDef        cgp_print_statistics_config_def;
    ObjectsInfoConfigDef            cgp_objects_info_config_def;
    DimensionsConfigDef             cgp_dimensions_config_def;
    TemperaturesConfigDef           cgp_temperatures_config_def;
    TimestampsConfigDef             cgp_timestamps_config_def;
    OtherPresetsConfigDef           cgp_other_presets_config_def;

public:
    EditGCodeDialog(wxWindow*parent, const std::string&key, const std::string&value);
    ~EditGCodeDialog();

    std::string get_edited_gcode() const;
    void        on_search_update();

    void init_params_list(const std::string& custom_gcode_name);
    wxDataViewItem add_presets_placeholders();

    void add_selected_value_to_gcode();
    void bind_list_and_button();

protected:
    std::unordered_map<int, Button *> m_button_list;

    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_sys_color_changed() override;

    void selection_changed(wxDataViewEvent& evt);
};




// ----------------------------------------------------------------------------
//                  ParamsModelNode: a node inside ParamsModel
// ----------------------------------------------------------------------------

class ParamsNode;
using ParamsNodePtrArray = std::vector<std::unique_ptr<ParamsNode>>;

enum class ParamType {
    Undef,
    Scalar,
    Vector,
    FilamentVector,
};

// On all of 3 different platforms Bitmap+Text icon column looks different 
// because of Markup text is missed or not implemented.
// As a temporary workaround, we will use:
// MSW - DataViewBitmapText (our custom renderer wxBitmap + wxString, supported Markup text)
// OSX - -//-, but Markup text is not implemented right now
// GTK - wxDataViewIconText (wxWidgets for GTK renderer wxIcon + wxString, supported Markup text)
class ParamsNode
{
    ParamsNode*         m_parent{ nullptr };
    ParamsNodePtrArray  m_children;
    wxDataViewCtrl*     m_ctrl;

    ParamType           m_param_type{ ParamType::Undef };

    // TODO/FIXME:
    // the GTK version of wxDVC (in particular wxDataViewCtrlInternal::ItemAdded)
    // needs to know in advance if a node is or _will be_ a container.
    // Thus implementing:
    //   bool IsContainer() const
    //    { return m_children.size()>0; }
    // doesn't work with wxGTK when DiffModel::AddToClassical is called
    // AND the classical node was removed (a new node temporary without children
    // would be added to the control)
    bool                m_container{ true };
    bool                m_expanded_before_search{false};
    bool                m_enabled{true};

    bool                 m_bold{false};
    // first is pos, second is length
    std::unique_ptr<std::pair<int, int>> m_highlight_index{nullptr};

public:

#ifdef __linux__
    wxIcon      icon;
#else
    wxBitmap    icon;
#endif //__linux__
    std::string icon_name;
    std::string param_key;
    wxString    text;

    // Group params(root) node
    ParamsNode(const wxString& group_name, const std::string& icon_name, wxDataViewCtrl* ctrl);

    // sub SlicingState node
    ParamsNode(ParamsNode*          parent,
               const wxString&      sub_group_name,
               const std::string&   icon_name,
               wxDataViewCtrl* ctrl);

    // parametre node
    ParamsNode( ParamsNode*         parent, 
                ParamType           param_type,
                const std::string&  param_key,
                wxDataViewCtrl* ctrl);

    wxString GetFormattedText();

    bool             IsContainer()      const { return m_container; }
    bool             IsGroupNode()      const { return m_parent == nullptr; }
    bool             IsParamNode()      const { return m_param_type != ParamType::Undef; }
    void             SetContainer(bool is_container) { m_container = is_container; }

    bool IsEnabled() { return m_enabled; }
    void Enable(bool enable = true) { m_enabled = enable; }
    void Disable() { Enable(false); }

    void StartSearch();
    void RefreshSearch(const wxString& search_text);
    void FinishSearch();

    ParamsNode* GetParent() { return m_parent; }
    ParamsNodePtrArray& GetChildren() { return m_children; }
    wxDataViewItemArray GetEnabledChildren();

    void Append(std::unique_ptr<ParamsNode> child) { m_children.emplace_back(std::move(child)); }
};


// ----------------------------------------------------------------------------
//                  ParamsModel
// ----------------------------------------------------------------------------

class ParamsModel : public wxDataViewModel
{
    ParamsNodePtrArray m_group_nodes;
    wxDataViewCtrl*    m_ctrl{ nullptr };
    bool               m_currently_searching{false};

public:

    ParamsModel();
    ~ParamsModel() override = default;

    void            SetAssociatedControl(wxDataViewCtrl* ctrl) { m_ctrl = ctrl; }

    wxDataViewItem AppendGroup(const wxString&    group_name,
                               const std::string& icon_name);

    wxDataViewItem AppendSubGroup(wxDataViewItem    parent,
                                  const wxString&   sub_group_name,
                                  const std::string&icon_name);

    wxDataViewItem AppendParam( wxDataViewItem      parent,
                                ParamType           param_type,
                                const std::string&  param_key);

    wxDataViewItem Delete(const wxDataViewItem& item);

    wxString        GetParamName(wxDataViewItem item);
    std::string     GetParamKey(wxDataViewItem item);
    std::string     GetTopLevelCategory(wxDataViewItem item);

    void RefreshSearch(const wxString& search_text);
    void FinishSearch();

    void            Clear();

    wxDataViewItem  GetParent(const wxDataViewItem& item) const override;
    unsigned int    GetChildren(const wxDataViewItem& parent, wxDataViewItemArray& array) const override;
    unsigned int    GetColumnCount() const override;
    wxString        GetColumnType(unsigned int col) const override;

    void GetValue(wxVariant& variant, const wxDataViewItem& item, unsigned int col) const override;
    bool SetValue(const wxVariant& variant, const wxDataViewItem& item, unsigned int col) override;

    bool IsContainer(const wxDataViewItem& item) const override;
    // Is the container just a header or an item with all columns
    // In our case it is an item with all columns
    bool HasContainerColumns(const wxDataViewItem& WXUNUSED(item)) const override { return true; }
};


// ----------------------------------------------------------------------------
//                  ParamsViewCtrl
// ----------------------------------------------------------------------------

class ParamsViewCtrl : public wxDataViewCtrl
{
    int                     m_em_unit;

public:
    ParamsViewCtrl(wxWindow* parent, wxSize size);
    ~ParamsViewCtrl() override {
        if (model) {
            Clear();
            model->DecRef();
        }
    }

    ParamsModel* model{ nullptr };

    wxDataViewItem AppendGroup(const wxString&    group_name,
                               const std::string& icon_name);

    wxDataViewItem AppendSubGroup(wxDataViewItem    parent,
                                  const wxString&   sub_group_name,
                                  const std::string&icon_name);

    wxDataViewItem AppendParam( wxDataViewItem      parent,
                                ParamType           param_type,
                                const std::string&  param_key);

    wxString        GetValue(wxDataViewItem item);
    wxString        GetSelectedValue();
    std::string     GetSelectedParamKey();
    std::string     GetSelectedTopLevelCategory();

    void    CheckAndDeleteIfEmpty(wxDataViewItem item);

    void    Clear();
    void    Rescale(int em = 0);

    void    set_em_unit(int em) { m_em_unit = em; }
};

} // namespace GUI
} // namespace Slic3r

#endif
