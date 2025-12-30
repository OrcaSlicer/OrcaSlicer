#ifndef slic3r_GUI_ObjectDataViewModel_hpp_
#define slic3r_GUI_ObjectDataViewModel_hpp_

#include <boost/log/trivial.hpp>
#include <wx/dataview.h>
#include <vector>
#include <map>

#include "ExtraRenderers.hpp"

namespace Slic3r {
class ModelObject;
class ModelVolume;
enum class ModelVolumeType : int;

namespace GUI {
class PartPlate;

typedef double                          coordf_t;
typedef std::pair<coordf_t, coordf_t>   t_layer_height_range;

// ----------------------------------------------------------------------------
// ObjectDataViewModelNode: a node inside ObjectDataViewModel
// ----------------------------------------------------------------------------
enum ItemType {
    itUndef         = 0,
    itPlate         = 1,
    itObject        = 2,
    itVolume        = 4,
    itInstanceRoot  = 8,
    itInstance      = 16,
    itSettings      = 32,
    itLayerRoot     = 64,
    itLayer         = 128,
    itInfo          = 256,
};

enum ColumnNumber
{
    colName         = 0,    // item name
    colHeight          ,    // variable height
    colPrint           ,    // printable property
    colFilament        ,    // extruder selection
    // BBS
    colSupportPaint    ,
    colColorPaint      ,
    colSinking         ,
    colEditing         ,    // item editing
    colCount           ,
};

enum PrintIndicator
{
    piUndef         = 0,    // no print indicator
    piPrintable        ,    // printable
    piUnprintable      ,    // unprintable
};

enum VaryHeightIndicator
{
    hiUnVariable,    // unvariable height
    hiVariable,    // variable height
};

enum class InfoItemType
{
    Undef,
    CustomSupports,
    //CustomSeam,
    MmSegmentation,
    FuzzySkin,
    //Sinking
    CutConnectors,
};

class ObjectDataViewModelNode;
WX_DEFINE_ARRAY_PTR(ObjectDataViewModelNode*, MyObjectTreeModelNodePtrArray);

class ObjectDataViewModelNode
{
    ObjectDataViewModelNode*	    m_parent;
    MyObjectTreeModelNodePtrArray   m_children;
    wxBitmap                        m_empty_bmp;
    size_t                          m_volumes_cnt = 0;
    std::vector< std::string >      m_opt_categories;
    t_layer_height_range            m_layer_range = { 0.0f, 0.0f };

    wxString				        m_name;
    wxBitmap&                       m_bmp = m_empty_bmp;
    ItemType				        m_type;
    int                             m_idx = -1;
    int                             m_plate_idx = -1;
    bool					        m_container = false;
    // BBS
    wxString				        m_extruder = wxEmptyString;
    wxBitmap                        m_extruder_bmp;
    wxBitmap  		                m_action_icon;
    // BBS
    wxBitmap                        m_support_icon;
    wxBitmap                        m_color_icon;
    wxBitmap                        m_sinking_icon;
    PrintIndicator                  m_printable {piUndef};
    wxBitmap                        m_printable_icon;

    VaryHeightIndicator             m_variable_height{ hiUnVariable };
    wxBitmap				        m_variable_height_icon;
    std::string                     m_warning_icon_name{ "" };
    bool                            m_has_lock{false};  // for cut object icon

    std::string                     m_action_icon_name = "";
    ModelVolumeType                 m_volume_type = ModelVolumeType(-1);
    bool                            m_is_text_volume{ false };
    bool                            m_is_svg_volume{false};
    InfoItemType                    m_info_item_type {InfoItemType::Undef};
    bool                            m_action_enable = false; // can undo all settings
    // BBS
    bool                            m_support_enable = false;
    bool                            m_color_enable = false;
    bool                            m_sink_enable = false;

public:
    PartPlate*                      m_part_plate;
    ModelObject*                    m_model_object;

public:
    ObjectDataViewModelNode(const wxString& name,
                            const wxString& extruder,
                            const int plate_idx,
                            ModelObject *model_object):
        m_parent(NULL),
        m_name(name),
        m_type(itObject),
        m_extruder(extruder),
        m_plate_idx(plate_idx),
        m_model_object(model_object)
    {
        set_icons();
        init_container();
	}

    ObjectDataViewModelNode(ObjectDataViewModelNode* parent,
                            const wxString& sub_obj_name,
                            Slic3r::ModelVolumeType type,
                            const bool is_text_volume,
                            const bool is_svg_volume,
                            const wxString& extruder,
                            const int idx = -1 );

    ObjectDataViewModelNode(ObjectDataViewModelNode* parent,
                            const t_layer_height_range& layer_range,
                            const int idx = -1,
                            const wxString& extruder = wxEmptyString );

    ObjectDataViewModelNode(PartPlate* part_plate, wxString name);

    //BBS: add part plate related logic
    ObjectDataViewModelNode(ObjectDataViewModelNode* parent, const ItemType type, const int plate_idx = -1);
    // BBS: to be checked. Whether need to add plate_idx for the following constructor ?
    ObjectDataViewModelNode(ObjectDataViewModelNode* parent, const InfoItemType type);

    ~ObjectDataViewModelNode()
    {
        // free all our children nodes
        size_t count = m_children.GetCount();
        for (size_t i = 0; i < count; i++)
        {
            ObjectDataViewModelNode *child = m_children[i];
            delete child;
        }
#ifndef NDEBUG
        // Indicate that the object was deleted.
        m_idx = -2;
#endif /* NDEBUG */
    }

	void init_container();
	bool IsContainer() const
	{
		return m_container;
	}

    ObjectDataViewModelNode* GetParent()
    {
        assert(m_parent == nullptr || m_parent->valid());
        return m_parent;
    }
    MyObjectTreeModelNodePtrArray& GetChildren()
    {
        return m_children;
    }
    ObjectDataViewModelNode* GetNthChild(unsigned int n)
    {
        return m_children.Item(n);
    }

    int GetChildIndex(ObjectDataViewModelNode* child) const
    {
        size_t child_count = GetChildCount();
        for (int index = 0; index < child_count; index++)
        {
            if (m_children.Item(index) == child)
                return index;
        }
        return -1;
    }

    void Insert(ObjectDataViewModelNode* child, unsigned int n)
    {
        if (!m_container)
            m_container = true;
        m_children.Insert(child, n);
    }
    void Append(ObjectDataViewModelNode* child)
    {
        if (!m_container)
            m_container = true;
        m_children.Add(child);
    }
    void RemoveAllChildren()
    {
        if (GetChildCount() == 0)
            return;
        for (int id = int(GetChildCount()) - 1; id >= 0; --id)
        {
            if (m_children.Item(id)->GetChildCount() > 0)
                m_children[id]->RemoveAllChildren();
            auto node = m_children[id];
            m_children.RemoveAt(id);
            delete node;
        }
    }

    size_t GetChildCount() const
    {
        return m_children.GetCount();
    }
    void            SetName(const wxString &);
    bool            SetValue(const wxVariant &variant, unsigned int col);
    void            SetVolumeType(ModelVolumeType type) { m_volume_type = type; }
    void            SetBitmap(const wxBitmap &icon) { m_bmp = icon; }
    void            SetExtruder(const wxString &extruder) { m_extruder = extruder; }
    void            SetWarningIconName(const std::string& warning_icon_name) { m_warning_icon_name = warning_icon_name; }
    void            SetLock(bool has_lock)                                   { m_has_lock = has_lock; }
    const wxBitmap& GetBitmap() const         { return m_bmp; }
    const wxString& GetName() const                 { return m_name; }
    ItemType        GetType() const                 { return m_type; }
    InfoItemType    GetInfoItemType() const         { return m_info_item_type; }
	void			SetIdx(const int& idx);
	int             GetIdx() const                  { return m_idx; }
    //BBS: add part plate related logic
    void            SetPlateIdx(const int& idx);
    int             GetPlateIdx() const { return m_plate_idx; }
    ModelVolumeType GetVolumeType()                 { return m_volume_type; }
	t_layer_height_range    GetLayerRange() const   { return m_layer_range; }
    wxString        GetExtruder()                   { return m_extruder; }
    PrintIndicator  IsPrintable() const             { return m_printable; }
    VaryHeightIndicator  IsVaribaleHeight() const { return m_variable_height; }
    // BBS
    bool            HasColorPainting() const        { return m_color_enable; }
    bool            HasSupportPainting() const { return m_support_enable; }
    bool            HasSinking() const { return m_sink_enable; }
    bool            IsActionEnabled() const         { return m_action_enable; }
    void            UpdateExtruderAndColorIcon(wxString extruder = "");

    // use this function only for childrens
    void AssignAllVal(ObjectDataViewModelNode& from_node)
    {
        // ! Don't overwrite other values because of equality of this values for all children --
        m_name = from_node.m_name;
        m_bmp = from_node.m_bmp;
        m_idx = from_node.m_idx;
        m_extruder = from_node.m_extruder;
        m_type = from_node.m_type;
    }

    bool SwapChildrens(int frst_id, int scnd_id) {
        if (GetChildCount() < 2 ||
            frst_id < 0 || (size_t)frst_id >= GetChildCount() ||
            scnd_id < 0 || (size_t)scnd_id >= GetChildCount())
            return false;

        ObjectDataViewModelNode new_scnd = *GetNthChild(frst_id);
        ObjectDataViewModelNode new_frst = *GetNthChild(scnd_id);

        new_scnd.m_idx = m_children.Item(scnd_id)->m_idx;
        new_frst.m_idx = m_children.Item(frst_id)->m_idx;

        m_children.Item(frst_id)->AssignAllVal(new_frst);
        m_children.Item(scnd_id)->AssignAllVal(new_scnd);
        return true;
    }

    // Set action and extruder(if any exist) icons for node
    void        set_icons();
    // set extruder icon for node
    void        set_extruder_icon();
	// Set printable icon for node
    void        set_printable_icon(PrintIndicator printable);
    void        set_variable_height_icon(VaryHeightIndicator vari_height);
    void        set_action_icon(bool enable);
    // BBS
    void        set_color_icon(bool enable, bool force = false);
    void        set_support_icon(bool enable,bool force = false);
    void        set_sinking_icon(bool enable, bool force = false);

    // Set warning icon for node
    void        set_warning_icon(const std::string& warning_icon);

    void        update_settings_digest_bitmaps();
    bool        update_settings_digest(const std::vector<std::string>& categories);
    int         volume_type() const { return int(m_volume_type); }
    bool        is_text_volume() const { return m_is_text_volume; }
    bool        is_svg_volume() const { return m_is_svg_volume; }
    void        sys_color_changed();
    void        msw_rescale();

#ifndef NDEBUG
    bool 		valid();
#endif /* NDEBUG */
    bool        invalid() const { return m_idx < -1; }
    bool        has_warning_icon() const { return !m_warning_icon_name.empty(); }
    std::string warning_icon_name() const { return m_warning_icon_name; }
    bool        has_lock() const { return m_has_lock; }

private:
    friend class ObjectDataViewModel;
};


// ----------------------------------------------------------------------------
// ObjectDataViewModel
// ----------------------------------------------------------------------------

// custom message the model sends to associated control to notify a last volume deleted from the object:
wxDECLARE_EVENT(wxCUSTOMEVT_LAST_VOLUME_IS_DELETED, wxCommandEvent);

class ObjectDataViewModel :public wxDataViewModel
{
    std::vector<ObjectDataViewModelNode*>       m_plates;
    std::vector<ObjectDataViewModelNode*>       m_objects;
    std::vector<wxBitmap>                m_volume_bmps;
    std::vector<wxBitmap>                m_text_volume_bmps;
    std::vector<wxBitmap>                m_svg_volume_bmps;
    std::map<InfoItemType, wxBitmap>     m_info_bmps;
    wxBitmap                              m_empty_bmp;
    wxBitmap                              m_warning_bmp;
    wxBitmap                              m_warning_manifold_bmp;
    wxBitmap                              m_lock_bmp;

    ObjectDataViewModelNode*                    m_plate_outside;

    wxDataViewCtrl*                             m_ctrl { nullptr };
    std::vector<std::tuple<ObjectDataViewModelNode*, wxString, wxString>> assembly_name_list;
    std::vector<std::tuple<ObjectDataViewModelNode*, wxString, wxString>> search_found_list;
    std::map<int,std::map<int, int>>                                      m_ui_and_3d_volume_maps;

public:
    ObjectDataViewModel();
    ~ObjectDataViewModel();

    void Init();
    std::map<int, std::map<int, int>> &get_ui_and_3d_volume_map() { return m_ui_and_3d_volume_maps; }
    int   get_real_volume_index_in_3d(int ui_object_value, int ui_volume_value)
    {
        if (m_ui_and_3d_volume_maps.find(ui_object_value) != m_ui_and_3d_volume_maps.end()) {
            auto cur_map = m_ui_and_3d_volume_maps[ui_object_value];
            if (cur_map.find(ui_volume_value) != cur_map.end()) { return cur_map[ui_volume_value]; }
        }
        return ui_volume_value;
    }
    int get_real_volume_index_in_ui(int ui_object_value, int _3d_value)
    {
        if (m_ui_and_3d_volume_maps.find(ui_object_value) != m_ui_and_3d_volume_maps.end()) {
            auto cur_map = m_ui_and_3d_volume_maps[ui_object_value];
            for (auto item : cur_map) {
                if (item.second == _3d_value) {
                    return item.first;
                }
            }
        }
        return _3d_value;
    }
    wxDataViewItem AddPlate(PartPlate* part_plate, wxString name = wxEmptyString, bool refresh = true);
    wxDataViewItem AddObject(ModelObject* model_object, std::string warning_bitmap, bool has_lock = false, bool refresh = true);
    wxDataViewItem AddVolumeChild(  const wxDataViewItem &parent_item,
                                    const wxString &name,
                                    const Slic3r::ModelVolumeType volume_type,
                                    const bool is_text_volume,
                                    const bool is_svg_volume,
                                    const std::string& warning_icon_name = std::string(),
                                    const int extruder = 0,
                                    const bool create_frst_child = true);
    wxDataViewItem AddSettingsChild(const wxDataViewItem &parent_item);
    wxDataViewItem AddInfoChild(const wxDataViewItem &parent_item, InfoItemType info_type);
    wxDataViewItem AddInstanceChild(const wxDataViewItem &parent_item, size_t num);
    wxDataViewItem AddInstanceChild(const wxDataViewItem &parent_item, const std::vector<bool>& print_indicator, const std::vector<int>& plate_indicator);
    wxDataViewItem AddLayersRoot(const wxDataViewItem &parent_item);
    wxDataViewItem AddLayersChild(  const wxDataViewItem &parent_item,
                                    const t_layer_height_range& layer_range,
                                    const int extruder = 0,
                                    const int index = -1);
    size_t         GetItemIndexForFirstVolume(ObjectDataViewModelNode* node_parent);
    wxDataViewItem DeletePlate(const int plate_idx);
    wxDataViewItem Delete(const wxDataViewItem &item);
    wxDataViewItem DeleteLastInstance(const wxDataViewItem &parent_item, size_t num);
    void ResetAll();
    void DeleteChildren(wxDataViewItem& parent);
    void DeleteVolumeChildren(wxDataViewItem& parent);
    void DeleteSettings(const wxDataViewItem& parent);
    wxDataViewItem GetItemByPlateId(int plate_idx);
    void           SetCurSelectedPlateFullName(int plate_idx,const std::string &);
    wxDataViewItem GetItemById(int obj_idx);
    wxDataViewItem GetItemById(const int obj_idx, const int sub_obj_idx, const ItemType parent_type);
    wxDataViewItem GetItemByVolumeId(int obj_idx, int volume_idx);
    wxDataViewItem GetItemByInstanceId(int obj_idx, int inst_idx);
    wxDataViewItem GetItemByLayerId(int obj_idx, int layer_idx);
    wxDataViewItem GetItemByLayerRange(const int obj_idx, const t_layer_height_range& layer_range);
    int  GetItemIdByLayerRange(const int obj_idx, const t_layer_height_range& layer_range);
    int  GetIdByItem(const wxDataViewItem& item) const;
    int  GetPlateIdByItem(const wxDataViewItem& item) const;
    int  GetIdByItemAndType(const wxDataViewItem& item, const ItemType type) const;
    int  GetObjectIdByItem(const wxDataViewItem& item) const;
    int  GetVolumeIdByItem(const wxDataViewItem& item) const;
    int  GetInstanceIdByItem(const wxDataViewItem& item) const;
    int  GetLayerIdByItem(const wxDataViewItem& item) const;
    void GetItemInfo(const wxDataViewItem& item, ItemType& type, int& obj_idx, int& idx);
    int  GetRowByItem(const wxDataViewItem& item) const;
    bool IsEmpty() { return m_objects.empty(); }
    bool InvalidItem(const wxDataViewItem& item);

    // helper method for wxLog

    wxString    GetName(const wxDataViewItem &item) const;
    wxBitmap&   GetBitmap(const wxDataViewItem &item) const;
    wxString    GetExtruder(const wxDataViewItem &item) const;
    int         GetExtruderNumber(const wxDataViewItem &item) const;

    // helper methods to change the model

    unsigned int    GetColumnCount() const override { return 3;}
    wxString        GetColumnType(unsigned int col) const override;

    void GetValue(  wxVariant &variant,
                    const wxDataViewItem &item,
                    unsigned int col) const override;
    bool SetValue(  const wxVariant &variant,
                    const wxDataViewItem &item,
                    unsigned int col) override;
    bool SetValue(  const wxVariant &variant,
                    const int item_idx,
                    unsigned int col);

    void SetExtruder(const wxString& extruder, wxDataViewItem item);
    void OnPlateChange(const int plate_idx, wxDataViewItem item);
    void SetPlateIdx(const int plate_idx, wxDataViewItem item);
    bool SetName    (const wxString& new_name, wxDataViewItem item);

    // For parent move child from cur_volume_id place to new_volume_id
    // Remaining items will moved up/down accordingly
    wxDataViewItem  ReorganizeChildren( const int cur_volume_id,
                                        const int new_volume_id,
                                        const wxDataViewItem &parent);
    wxDataViewItem  ReorganizeObjects( int current_id, int new_id);

    bool    IsEnabled(const wxDataViewItem &item, unsigned int col) const override;

    wxDataViewItem  GetParent(const wxDataViewItem &item) const override;
    // get object item
    wxDataViewItem          GetObject(const wxDataViewItem& item) const;
    wxDataViewItem          GetTopParent(const wxDataViewItem &item) const;
    bool            IsContainer(const wxDataViewItem &item) const override;
    unsigned int    GetChildren(const wxDataViewItem &parent, wxDataViewItemArray &array) const override;
    void GetAllChildren(const wxDataViewItem &parent,wxDataViewItemArray &array) const;
    // Is the container just a header or an item with all columns
    // In our case it is an item with all columns
    bool    HasContainerColumns(const wxDataViewItem& WXUNUSED(item)) const override {	return true; }
    bool    HasInfoItem(InfoItemType type) const;

    ItemType        GetItemType(const wxDataViewItem &item) const;
    ItemType        GetItemType(const wxDataViewItem &item,int& plate_idx) const;
    InfoItemType    GetInfoItemType(const wxDataViewItem &item) const;
    wxDataViewItem  GetItemByType(  const wxDataViewItem &parent_item,
                                    ItemType type) const;
    wxDataViewItem  GetSettingsItem(const wxDataViewItem &item) const;
    wxDataViewItem  GetInstanceRootItem(const wxDataViewItem &item) const;
    wxDataViewItem  GetLayerRootItem(const wxDataViewItem &item) const;
    wxDataViewItem  GetInfoItemByType(const wxDataViewItem &parent_item, InfoItemType type) const;
    // BBS
    wxDataViewItem  GetObjectItem(const ModelObject* mo) const;
    wxDataViewItem  GetVolumeItem(const wxDataViewItem& parent, int vol_idx) const;
    bool    IsSettingsItem(const wxDataViewItem &item) const;
    void    UpdateSettingsDigest(   const wxDataViewItem &item,
                                    const std::vector<std::string>& categories);

    bool    IsPrintable(const wxDataViewItem &item) const;
    void    UpdateObjectPrintable(wxDataViewItem parent_item);
    void    UpdateInstancesPrintable(wxDataViewItem parent_item);
    bool    IsVariableHeight(const wxDataViewItem& item) const;

    void    SetVolumeType(const wxDataViewItem &item, const Slic3r::ModelVolumeType type);
    ModelVolumeType GetVolumeType(const wxDataViewItem &item);
    wxDataViewItem SetPrintableState( PrintIndicator printable, int obj_idx,
                                      int subobj_idx = -1,
                                      ItemType subobj_type = itInstance);
    wxDataViewItem SetObjectPrintableState(PrintIndicator printable, wxDataViewItem obj_item);
    wxDataViewItem SetObjectVariableHeightState(VaryHeightIndicator vari_height, wxDataViewItem obj_item);
    // BBS
    bool    IsColorPainted(wxDataViewItem& item) const;
    bool    IsSupportPainted(wxDataViewItem &item) const;
    bool    IsSinked(wxDataViewItem &item) const;
    void    SetColorPaintState(const bool painted, wxDataViewItem obj_item,bool force = false);
    void    SetSupportPaintState(const bool painted, wxDataViewItem obj_item,bool force = false);
    void    SetSinkState(const bool painted, wxDataViewItem obj_item,bool force = false);

    void    SetAssociatedControl(wxDataViewCtrl* ctrl) { m_ctrl = ctrl; }
    // Rescale bitmaps for existing Items
    void    Rescale();

    void        AddWarningIcon(const wxDataViewItem& item, const std::string& warning_name);
    void        DeleteWarningIcon(const wxDataViewItem& item, const bool unmark_object = false);
    void        UpdateWarningIcon(const wxDataViewItem& item, const std::string& warning_name);
    void        UpdateCutObjectIcon(const wxDataViewItem &item, bool has_cut_icon);
    bool        HasWarningIcon(const wxDataViewItem& item) const;
    t_layer_height_range    GetLayerRangeByItem(const wxDataViewItem& item) const;

    bool        UpdateColumValues(unsigned col);
    void        UpdateExtruderBitmap(wxDataViewItem item);
    // BBS: add use_obj_extruder
    void        UpdateVolumesExtruderBitmap(wxDataViewItem object_item, bool use_obj_extruder = false);
    int         GetDefaultExtruderIdx(wxDataViewItem item);

    // BBS
    void        UpdateItemNames();

    void        assembly_name(ObjectDataViewModelNode* item, wxString name);
    void        assembly_name();
    std::vector<std::tuple<ObjectDataViewModelNode*, wxString, wxString>> get_assembly_name_list() const { return assembly_name_list; }
    void        search_object(wxString search_text);
    std::vector<std::tuple<ObjectDataViewModelNode*, wxString, wxString>> get_found_list() const { return search_found_list; }

    void        sys_color_changed();

private:
    wxDataViewItem  AddRoot(const wxDataViewItem& parent_item, const ItemType root_type);
    wxDataViewItem  AddInstanceRoot(const wxDataViewItem& parent_item);
    void            AddAllChildren(const wxDataViewItem& parent);

    void            ReparentObject(ObjectDataViewModelNode* plate, ObjectDataViewModelNode* object);
    wxDataViewItem  AddOutsidePlate(bool refresh = true);

    void UpdateBitmapForNode(ObjectDataViewModelNode *node);
    void UpdateBitmapForNode(ObjectDataViewModelNode* node, const std::string& warning_icon_name, bool has_lock);
};


}
}


#endif // slic3r_GUI_ObjectDataViewModel_hpp_
