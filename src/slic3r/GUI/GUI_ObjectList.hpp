#ifndef slic3r_GUI_ObjectList_hpp_
#define slic3r_GUI_ObjectList_hpp_

#include <map>
#include <vector>
#include <set>

#include <wx/bitmap.h>
#include <wx/dataview.h>
#include <wx/menu.h>

#include "Event.hpp"
#include "wxExtensions.hpp"
#include "ObjectDataViewModel.hpp"

#include "libslic3r/PrintConfig.hpp"

class wxBoxSizer;
class wxBitmapComboBox;
class wxMenuItem;
class MenuWithSeparators;

namespace Slic3r {
class ConfigOptionsGroup;
class DynamicPrintConfig;
class ModelConfig;
class ModelObject;
class ModelVolume;
class TriangleMesh;
enum class ModelVolumeType : int;

// FIXME: broken build on mac os because of this is missing:
typedef std::vector<std::string>                    t_config_option_keys;
typedef std::vector<ModelVolume*>                   ModelVolumePtrs;
typedef double                                      coordf_t;
typedef std::pair<coordf_t, coordf_t>               t_layer_height_range;
typedef std::map<t_layer_height_range, ModelConfig> t_layer_config_ranges;

// Manifold mesh may contain self-intersections, so we want to always allow fixing the mesh.
#define FIX_THROUGH_NETFABB_ALWAYS 1

namespace GUI {
struct ObjectVolumeID {
    ModelObject* object{ nullptr };
    ModelVolume* volume{ nullptr };
};

typedef Event<ObjectVolumeID> ObjectSettingEvent;

class PartPlate;

wxDECLARE_EVENT(EVT_OBJ_LIST_OBJECT_SELECT, SimpleEvent);
wxDECLARE_EVENT(EVT_PARTPLATE_LIST_PLATE_SELECT, IntEvent);
class BitmapComboBox;

struct ItemForDelete
{
    ItemType    type;
    int         obj_idx;
    int         sub_obj_idx;

    ItemForDelete(ItemType type, int obj_idx, int sub_obj_idx)
        : type(type), obj_idx(obj_idx), sub_obj_idx(sub_obj_idx)
    {}

    bool operator==(const ItemForDelete& r) const
    {
        return (type == r.type && obj_idx == r.obj_idx && sub_obj_idx == r.sub_obj_idx);
    }

    bool operator<(const ItemForDelete& r) const
    {
        if (obj_idx != r.obj_idx)
            return (obj_idx < r.obj_idx);
        return (sub_obj_idx < r.sub_obj_idx);
    }
};

struct MeshErrorsInfo
{
    wxString    tooltip;
    std::string warning_icon_name;
};

class ObjectList : public wxDataViewCtrl
{
public:
    enum SELECTION_MODE
    {
        smUndef     = 0,
        smVolume    = 1,
        smInstance  = 2,
        smLayer     = 4,
        smSettings  = 8,  // used for undo/redo
        smLayerRoot = 16, // used for undo/redo
    };

    enum OBJECT_ORGANIZE_TYPE
    {
        ortByPlate = 0,
        ortByModule = 1,
    };

    struct Clipboard
    {
        void reset() {
            m_type = itUndef;
            m_layer_config_ranges_cache .clear();
            m_config_cache.clear();
        }
        bool        empty()    const { return m_type == itUndef; }
        ItemType    get_type() const { return m_type; }
        void        set_type(ItemType type) { m_type = type; }

        t_layer_config_ranges&  get_ranges_cache() { return m_layer_config_ranges_cache; }
        DynamicPrintConfig&     get_config_cache() { return m_config_cache; }

    private:
        ItemType                m_type {itUndef};
        t_layer_config_ranges   m_layer_config_ranges_cache;
        DynamicPrintConfig      m_config_cache;
    };

private:
    SELECTION_MODE  m_selection_mode {smUndef};
    int             m_selected_layers_range_idx {-1};

    Clipboard       m_clipboard;

    struct dragged_item_data
    {
        void init(const int obj_idx, const int subobj_idx, const ItemType type) {
            m_obj_idx = obj_idx;
            m_type = type;
            if (m_type&itVolume)
                m_vol_idx = subobj_idx;
            else
                m_inst_idxs.insert(subobj_idx);
        }

        void init(const int obj_idx, const ItemType type) {
            m_obj_idx = obj_idx;
            m_type = type;
        }

        void clear() {
            m_obj_idx = -1;
            m_vol_idx = -1;
            m_inst_idxs.clear();
            m_type = itUndef;
        }

        int obj_idx() const  { return m_obj_idx; }
        int sub_obj_idx() const  { return m_vol_idx; }
        ItemType type() const { return m_type; }
        std::set<int>& inst_idxs() { return m_inst_idxs; }

    private:
        int m_obj_idx = -1;
        int m_vol_idx = -1;
        std::set<int> m_inst_idxs{};
        ItemType m_type = itUndef;

    } m_dragged_data;

    ObjectDataViewModel         *m_objects_model{ nullptr };
    ModelConfig                 *m_config {nullptr};
    std::vector<ModelObject*>   *m_objects{ nullptr };
    size_t                      m_variable_layer_obj_num = 0;

    BitmapComboBox              *m_extruder_editor { nullptr };

    std::vector<wxBitmap*>      m_bmp_vector;

    int			m_selected_object_id = -1;
    bool		m_prevent_list_events = false;		// We use this flag to avoid circular event handling Select()
                                                    // happens to fire a wxEVT_LIST_ITEM_SELECTED on OSX, whose event handler
                                                    // calls this method again and again and again
    bool        m_prevent_list_manipulation = false;

    bool        m_prevent_update_filament_in_config = false; // We use this flag to avoid updating of the extruder value in config
                                                             // during updating of the extruder count.

    bool        m_prevent_canvas_selection_update = false; // This flag prevents changing selection on the canvas. See function
                                                           // update_settings_items - updating canvas selection is undesirable,
                                                           // because it would turn off the gizmos (mainly a problem for the SLA gizmo)

    wxDataViewItem m_last_selected_item {nullptr};

#ifdef __WXMSW__
    // Workaround for entering the column editing mode on Windows. Simulate keyboard enter when another column of the active line is selected.
    int 	    m_last_selected_column = -1;
#endif /* __MSW__ */

#if 0
    SettingsFactory::Bundle m_freq_settings_fff;
    SettingsFactory::Bundle m_freq_settings_sla;
#endif

    size_t    m_items_count { size_t(-1) };

    inline void ensure_current_item_visible()
    {
        if (const auto &item = this->GetCurrentItem())
            this->EnsureVisible(item);
    }

public:
    ObjectList(wxWindow* parent);
    ~ObjectList() override;

    void set_min_height();
    void update_min_height();

    ObjectDataViewModel*        GetModel() const    { return m_objects_model; }
    ModelConfig*                config() const      { return m_config; }
    std::vector<ModelObject*>*  objects() const     { return m_objects; }

    ModelObject*                object(const int obj_idx) const ;


    void                create_objects_ctrl();
    // BBS
    void                update_objects_list_filament_column(size_t filaments_count);
    void                update_objects_list_filament_column_when_delete_filament(size_t filament_id, size_t filaments_count, int replace_filament_id = -1);
    void                update_filament_colors();
    // show/hide "Extruder" column for Objects List
    void                set_filament_column_hidden(const bool hide) const;
    // show/hide variable height column for Objects List
    void                set_variable_height_column_hidden(const bool hide) const;
    // BBS
    void                set_color_paint_hidden(const bool hide) const;
    void                set_support_paint_hidden(const bool hide) const;
    void                set_sinking_hidden(const bool hide) const;

    // update extruder in current config
    void                update_filament_in_config(const wxDataViewItem& item);
    // update changed name in the object model
    void                update_name_in_model(const wxDataViewItem& item) const;
    void                update_name_in_list(int obj_idx, int vol_idx) const;
    void                update_filament_values_for_items(const size_t filaments_count);
    void                update_filament_values_for_items_when_delete_filament(const size_t filament_id, const int replace_id = -1);

    //BBS: update plate
    void                update_plate_values_for_items();
    void                update_name_for_items();

    // Get obj_idx and vol_idx values for the selected (by default) or an adjusted item
    void                get_selected_item_indexes(int& obj_idx, int& vol_idx, const wxDataViewItem& item = wxDataViewItem(0));
    void                get_selection_indexes(std::vector<int>& obj_idxs, std::vector<int>& vol_idxs);
    // Get count of errors in the mesh
    int                 get_repaired_errors_count(const int obj_idx, const int vol_idx = -1) const;
    // Get list of errors in the mesh and name of the warning icon
    // Return value is a pair <Tooltip, warning_icon_name>, used for the tooltip and related warning icon
    // Function without parameters is for a call from Manipulation panel,
    // when we don't know parameters of selected item
    MeshErrorsInfo      get_mesh_errors_info(const int obj_idx, const int vol_idx = -1, wxString* sidebar_info = nullptr, int* non_manifold_edges = nullptr) const;
    MeshErrorsInfo      get_mesh_errors_info(wxString* sidebar_info = nullptr, int* non_manifold_edges = nullptr);
    void                set_tooltip_for_item(const wxPoint& pt);

    void                selection_changed();
    void                show_context_menu(const bool evt_context_menu);
    void                extruder_editing();
#ifndef __WXOSX__
    void                key_event(wxKeyEvent& event);
#endif /* __WXOSX__ */

    void                copy();
    void                paste();
    void                cut();
    //BBS
    void                clone();
    bool                cut_to_clipboard();
    bool                copy_to_clipboard();
    bool                paste_from_clipboard();
    void                undo();
    void                redo();
    void                increase_instances();
    void                decrease_instances();

    void                add_category_to_settings_from_selection(const std::vector< std::pair<std::string, bool> >& category_options, wxDataViewItem item);
    void                add_category_to_settings_from_frequent(const std::vector<std::string>& category_options, wxDataViewItem item);
    void                show_settings(const wxDataViewItem settings_item);
    bool                is_instance_or_object_selected();

    void                load_subobject(ModelVolumeType type, bool from_galery = false);
    // ! ysFIXME - delete commented code after testing and rename "load_modifier" to something common
    //void                load_part(ModelObject& model_object, std::vector<ModelVolume*>& added_volumes, ModelVolumeType type, bool from_galery = false);
    void                load_modifier(const wxArrayString& input_files, ModelObject& model_object, std::vector<ModelVolume*>& added_volumes, ModelVolumeType type, bool from_galery = false);
    void                load_generic_subobject(const std::string& type_name, const ModelVolumeType type);
    void                load_shape_object(const std::string &type_name);
    void                load_mesh_object(const TriangleMesh &mesh, const wxString &name, bool center = true);
    // BBS
    void                switch_to_object_process();
    bool                del_object(const int obj_idx, bool refresh_immediately = true);
    void                del_subobject_item(wxDataViewItem& item);
    void                del_settings_from_config(const wxDataViewItem& parent_item);
    void                del_instances_from_object(const int obj_idx);
    void                del_layer_from_object(const int obj_idx, const t_layer_height_range& layer_range);
    void                del_layers_from_object(const int obj_idx);
    bool                del_from_cut_object(bool is_connector, bool is_model_part = false, bool is_negative_volume = false);
    bool                del_subobject_from_object(const int obj_idx, const int idx, const int type);
    void                del_info_item(const int obj_idx, InfoItemType type);
    void                split();
    void                merge(bool to_multipart_object);
    // void                merge_volumes(); // BBS: merge parts to single part
    void                layers_editing();

    void                boolean();    // BBS: Boolean Operation of parts
    wxDataViewItem      add_layer_root_item(const wxDataViewItem obj_item);
    wxDataViewItem      add_settings_item(wxDataViewItem parent_item, const DynamicPrintConfig* config);

    DynamicPrintConfig  get_default_layer_config(const int obj_idx);
    bool                get_volume_by_item(const wxDataViewItem& item, ModelVolume*& volume);
    bool                is_splittable(bool to_objects);
    bool                selected_instances_of_same_object();
    bool                can_split_instances();
    bool                can_merge_to_multipart_object() const;
    bool                can_merge_to_single_object() const;
    bool                can_mesh_boolean() const;

    bool                has_selected_cut_object() const;
    void                invalidate_cut_info_for_selection();
    void                invalidate_cut_info_for_object(int obj_idx);
    void                delete_all_connectors_for_selection();
    void                delete_all_connectors_for_object(int obj_idx);

    wxPoint             get_mouse_position_in_control() const { return wxGetMousePosition() - this->GetScreenPosition(); }
    int                 get_selected_obj_idx() const;
    ModelConfig&        get_item_config(const wxDataViewItem& item) const;

    void                changed_object(const int obj_idx = -1) const;
    void                part_selection_changed();

    // Add object to the list
    // @param do_info_update: [Arthur] this function becomes slow as more functions are added, but I only need a fast version in FillBedJob, and I don't care about any info updates, so I pass a do_info_update param to skip all the uneccessary steps.
    void add_objects_to_list(std::vector<size_t> obj_idxs, bool call_selection_changed = true, bool notify_partplate = true, bool do_info_update = true);
    void add_object_to_list(size_t obj_idx, bool call_selection_changed = true, bool notify_partplate = true, bool do_info_update = true);
    // Add object's volumes to the list
    // Return selected items, if add_to_selection is defined
    wxDataViewItemArray add_volumes_to_object_in_list(size_t obj_idx, std::function<bool(const ModelVolume *)> add_to_selection = nullptr);
    // Delete object from the list
    void delete_object_from_list();
    void delete_object_from_list(const size_t obj_idx);
    void delete_volume_from_list(const size_t obj_idx, const size_t vol_idx);
    void delete_instance_from_list(const size_t obj_idx, const size_t inst_idx);
    void delete_from_model_and_list(const ItemType type, const int obj_idx, const int sub_obj_idx);
    void delete_from_model_and_list(const std::vector<ItemForDelete>& items_for_delete);
    void update_lock_icons_for_model();
    // Delete all objects from the list
    void delete_all_objects_from_list();
    // Increase instances count
    void increase_object_instances(const size_t obj_idx, const size_t num);
    // Decrease instances count
    void decrease_object_instances(const size_t obj_idx, const size_t num);

    // #ys_FIXME_to_delete
    // Unselect all objects in the list on c++ side
    void unselect_objects();
    // Select object item in the ObjectList, when some gizmo is activated
    // "is_msr_gizmo" indicates if Move/Scale/Rotate gizmo was activated
    void select_object_item(bool is_msr_gizmo);

    // Remove objects/sub-object from the list
    void remove();
    void del_layer_range(const t_layer_height_range& range);
    // Add a new layer height after the current range if possible.
    // The current range is shortened and the new range is entered after the shortened current range if it fits.
    // If no range fits after the current range, then no range is inserted.
    // The layer range panel is updated even if this function does not change the layer ranges, as the panel update
    // may have been postponed from the "kill focus" event of a text field, if the focus was lost for the "add layer" button.
    // Rather providing the range by a value than by a reference, so that the memory referenced cannot be invalidated.
    void add_layer_range_after_current(const t_layer_height_range current_range);
    wxString can_add_new_range_after_current( t_layer_height_range current_range);
    void add_layer_item (const t_layer_height_range& range,
                         const wxDataViewItem layers_item,
                         const int layer_idx = -1);
    bool edit_layer_range(const t_layer_height_range& range, coordf_t layer_height);
    // This function may be called when a text field loses focus for a "add layer" or "remove layer" button.
    // In that case we don't want to destroy the panel with that "add layer" or "remove layer" buttons, as some messages
    // are already planned for them and destroying these widgets leads to crashes at least on OSX.
    // In that case the "add layer" or "remove layer" button handlers are responsible for always rebuilding the panel
    // even if the "add layer" or "remove layer" buttons did not update the layer spans or layer heights.
    bool edit_layer_range(const t_layer_height_range& range,
                          const t_layer_height_range& new_range,
                          // Don't destroy the panel with the "add layer" or "remove layer" buttons.
                          bool suppress_ui_update = false);

    void init();
    bool multiple_selection() const ;
    bool is_selected(const ItemType type) const;
    bool is_connectors_item_selected() const;
    bool is_connectors_item_selected(const wxDataViewItemArray &sels) const;
    int  get_selected_layers_range_idx() const;
    void set_selected_layers_range_idx(const int range_idx) { m_selected_layers_range_idx = range_idx; }
    void set_selection_mode(SELECTION_MODE mode) { m_selection_mode = mode; }
    void update_selections();
    void update_selections_on_canvas();
    void select_item(const wxDataViewItem& item);
    void select_item(std::function<wxDataViewItem()> get_item);
    void select_items(const wxDataViewItemArray& sels);
    // BBS
    void select_item(const ObjectVolumeID& ov_id);
    void select_items(const std::vector<ObjectVolumeID>& ov_ids);
    void select_all();
    void select_item_all_children();
    void update_selection_mode();
    bool check_last_selection(wxString& msg_str);
    // correct current selections to avoid of the possible conflicts
    void fix_multiselection_conflicts();
    // correct selection in respect to the cut_id if any exists
    void fix_cut_selection();
    bool fix_cut_selection(wxDataViewItemArray &sels);

    ModelVolume* get_selected_model_volume();
    void change_part_type();

    void last_volume_is_deleted(const int obj_idx);
    void update_and_show_object_settings_item();
    void update_settings_item_and_selection(wxDataViewItem item, wxDataViewItemArray& selections);
    void update_object_list_by_printer_technology();
    void update_info_items(size_t obj_idx, wxDataViewItemArray *selections = nullptr, bool added_object = false, bool color_mode_changed = false);
    void update_variable_layer_obj_num(ObjectDataViewModelNode* obj_node, size_t layer_data_count);

    void instances_to_separated_object(const int obj_idx, const std::set<int>& inst_idx);
    void instances_to_separated_objects(const int obj_idx);
    void split_instances();
    void rename_item();
    void fix_through_netfabb();
    void simplify();
    void update_item_error_icon(const int obj_idx, int vol_idx) const ;

    void copy_layers_to_clipboard();
    void paste_layers_into_list();
    void copy_settings_to_clipboard();
    void paste_settings_into_list();
    bool can_paste_settings_into_list();
    bool clipboard_is_empty() const { return m_clipboard.empty(); }
    void paste_volumes_into_list(int obj_idx, const ModelVolumePtrs& volumes);
    void paste_objects_into_list(const std::vector<size_t>& object_idxs);

    void msw_rescale();
    void sys_color_changed();

    void update_after_undo_redo();
    //update printable state for item from objects model
    void update_printable_state(int obj_idx, int instance_idx);
    void toggle_printable_state();
    void enable_layers_editing();

    //BBS: remove const qualifier
    void set_extruder_for_selected_items(const int extruder);
    wxDataViewItemArray reorder_volumes_and_get_selection(int obj_idx, std::function<bool(const ModelVolume*)> add_to_selection = nullptr);
    void apply_volumes_order();

    // BBS
    void on_plate_added(PartPlate* part_plate);
    void on_plate_deleted(int plate_index);
    void reload_all_plates(bool notify_partplate = false);
    void on_plate_selected(int plate_index);
    void notify_instance_updated(int obj_idx);
    void object_config_options_changed(const ObjectVolumeID& ov_id);
    void printable_state_changed(const std::vector<ObjectVolumeID>& ov_ids);

    // search objectlist
    void assembly_plate_object_name();
    void selected_object(ObjectDataViewModelNode* item);

private:
#ifdef __WXOSX__
//    void OnChar(wxKeyEvent& event);
    wxAcceleratorTable m_accel;
#endif /* __WXOSX__ */
    void OnContextMenu(wxDataViewEvent &event);
    void list_manipulation(const wxPoint& mouse_pos, bool evt_context_menu = false);

    // BBS
    void update_name_column_width() const;

    void OnBeginDrag(wxDataViewEvent &event);
    void OnDropPossible(wxDataViewEvent &event);
    void OnDrop(wxDataViewEvent &event);
    bool can_drop(const wxDataViewItem& item, int& src_obj_id, int& src_plate, int& dest_obj_id, int& dest_plate) const ;

    void ItemValueChanged(wxDataViewEvent &event);
    // Workaround for entering the column editing mode on Windows. Simulate keyboard enter when another column of the active line is selected.
    void OnStartEditing(wxDataViewEvent &event);
	void OnEditingStarted(wxDataViewEvent &event);
    void OnEditingDone(wxDataViewEvent &event);

    // apply the instance transform to all volumes and reset instance transform except the offset
    void apply_object_instance_transfrom_to_all_volumes(ModelObject *model_object, bool need_update_assemble_matrix = true);

    std::vector<int> m_columns_width;
    wxSize           m_last_size;
};


}}

#endif //slic3r_GUI_ObjectList_hpp_
