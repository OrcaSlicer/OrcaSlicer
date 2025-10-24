#ifndef slic3r_GUI_Factories_hpp_
#define slic3r_GUI_Factories_hpp_

#include <map>
#include <vector>
#include <array>

#include <wx/bitmap.h>

#include "libslic3r/PrintConfig.hpp"
#include "wxExtensions.hpp"

class wxMenu;
class wxMenuItem;

namespace Slic3r {

enum class ModelVolumeType : int;

namespace GUI {

struct SimpleSettingData
{
    std::string name;
    std::string label;
    int priority;
};

struct SettingsFactory
{
//				     category ->       vector ( option )
    typedef std::map<std::string, std::vector<std::string>> Bundle;
    static std::map<std::string, std::string>               CATEGORY_ICON;

    //BBS: add setting data for table
    static std::map<std::string, std::vector<SimpleSettingData>>  OBJECT_CATEGORY_SETTINGS;
    static std::map<std::string, std::vector<SimpleSettingData>>  PART_CATEGORY_SETTINGS;

    static wxBitmap                             get_category_bitmap(const std::string& category_name, bool menu_bmp = true);
    static Bundle                               get_bundle(const DynamicPrintConfig* config, bool is_object_settings, bool is_layer_settings = false);
    static std::vector<std::string>             get_options(bool is_part);
    //BBS: add api to get options for catogary
    static std::vector<SimpleSettingData> get_visible_options(const std::string& category, const bool is_part);
    static std::map<std::string, std::vector<SimpleSettingData>> get_all_visible_options(const bool is_part);
};

class MenuFactory
{
public:
	static std::vector<wxBitmap>    get_volume_bitmaps();
	static std::vector<wxBitmap> get_text_volume_bitmaps();
	static std::vector<wxBitmap> get_svg_volume_bitmaps();

    MenuFactory();
    ~MenuFactory() = default;

    void    init(wxWindow* parent);
    void    update();
    void    update_object_menu();
    void    update_default_menu();
    void    msw_rescale();
    void    sys_color_changed();

    static void sys_color_changed(wxMenuBar* menu_bar);

    wxMenu* default_menu();
    wxMenu* object_menu();
    wxMenu* sla_object_menu();
    wxMenu* part_menu();
    wxMenu* text_part_menu();
    wxMenu* svg_part_menu();
    wxMenu* instance_menu();
    wxMenu* layer_menu();
    wxMenu* multi_selection_menu();
    //BBS: add part plate related logic
    wxMenu* plate_menu();
    wxMenu* assemble_object_menu();
    wxMenu* assemble_part_menu();
    wxMenu* assemble_multi_selection_menu();

    wxMenu *filament_action_menu(int active_filament_menu_id);

private:
    enum MenuType {
        mtObjectFFF = 0,
        mtObjectSLA,
        mtCount
    };

    wxWindow* m_parent {nullptr};

    MenuWithSeparators m_object_menu;
    MenuWithSeparators m_part_menu;
    MenuWithSeparators m_text_part_menu;
    MenuWithSeparators m_svg_part_menu;
    MenuWithSeparators m_sla_object_menu;
    MenuWithSeparators m_default_menu;
    MenuWithSeparators m_instance_menu;
    //BBS: add part plate related logic
    MenuWithSeparators m_plate_menu;
    MenuWithSeparators m_assemble_object_menu;
    MenuWithSeparators m_assemble_part_menu;

    wxMenu m_filament_action_menu;
   

    // Removed/Prepended Items according to the view mode
    std::array<wxMenuItem*, mtCount> items_increase;
    std::array<wxMenuItem*, mtCount> items_decrease;
    std::array<wxMenuItem*, mtCount> items_set_number_of_copies;

    void        create_default_menu();
    void        create_common_object_menu(wxMenu *menu);
    void        create_object_menu();
    void        create_sla_object_menu();
    void        create_part_menu();
    void        create_text_part_menu();
    void        create_svg_part_menu();
    //BBS: add part plate related logic
    void        create_plate_menu();
    //BBS: add bbl object menu
    void        create_extra_object_menu();
    void        create_bbl_part_menu();
    void        create_bbl_assemble_object_menu();
    void        create_bbl_assemble_part_menu();

    void        create_filament_action_menu(bool init, int active_filament_menu_id);

    wxMenu*     append_submenu_add_generic(wxMenu* menu, ModelVolumeType type);
    // Orca: add submenu for adding handy models
    wxMenu*     append_submenu_add_handy_model(wxMenu* menu, ModelVolumeType type);
    void        append_menu_item_add_text(wxMenu* menu, ModelVolumeType type, bool is_submenu_item = true);
    void        append_menu_item_add_svg(wxMenu *menu, ModelVolumeType type, bool is_submenu_item = true);    
    void        append_menu_items_add_volume(wxMenu* menu);
    wxMenuItem* append_menu_item_layers_editing(wxMenu* menu);
    wxMenuItem* append_menu_item_settings(wxMenu* menu);
    wxMenuItem* append_menu_item_change_type(wxMenu* menu);
    wxMenuItem* append_menu_item_instance_to_object(wxMenu* menu);
    wxMenuItem* append_menu_item_printable(wxMenu* menu);
    void        append_menu_item_rename(wxMenu* menu);
    wxMenuItem* append_menu_item_fix_through_netfabb(wxMenu* menu);
    //wxMenuItem* append_menu_item_simplify(wxMenu* menu);
    void        append_menu_item_export_stl(wxMenu* menu, bool is_mulity_menu = false);
    void        append_menu_item_reload_from_disk(wxMenu* menu);
    void        append_menu_item_replace_with_stl(wxMenu* menu);
    void        append_menu_item_change_extruder(wxMenu* menu);
    void        append_menu_item_set_visible(wxMenu* menu);
    void        append_menu_item_delete(wxMenu* menu);
    void        append_menu_item_scale_selection_to_fit_print_volume(wxMenu* menu);
    void        append_menu_items_convert_unit(wxMenu* menu); // Add "Conver/Revert..." menu items (from/to inches/meters) after "Reload From Disk"
    void        append_menu_items_flush_options(wxMenu* menu);
    void        append_menu_item_merge_to_multipart_object(wxMenu *menu);
    void        append_menu_item_merge_to_single_object(wxMenu* menu);
    void        append_menu_item_merge_parts_to_single_part(wxMenu *menu);
    void        append_menu_items_mirror(wxMenu *menu);
    void        append_menu_item_invalidate_cut_info(wxMenu *menu);
    void        append_menu_item_edit_text(wxMenu *menu);
    void        append_menu_item_edit_svg(wxMenu *menu);

    //void        append_menu_items_instance_manipulation(wxMenu *menu);
    //void        update_menu_items_instance_manipulation(MenuType type);
    //BBS add bbl menu item
    void        append_menu_item_clone(wxMenu* menu);
    void        append_menu_item_simplify(wxMenu* menu);
    void        append_menu_item_center(wxMenu* menu);
    void        append_menu_item_drop(wxMenu* menu);
    void        append_menu_item_per_object_process(wxMenu* menu);
    void        append_menu_item_per_object_settings(wxMenu* menu);
    void        append_menu_item_change_filament(wxMenu* menu);
    void        append_menu_item_set_printable(wxMenu* menu);
    void        append_menu_item_locked(wxMenu* menu);
    void        append_menu_item_fill_bed(wxMenu *menu);
    void        append_menu_item_plate_name(wxMenu *menu);
};

}}

#endif //slic3r_GUI_Factories_hpp_
