#ifndef slic3r_PresetComboBoxes_hpp_
#define slic3r_PresetComboBoxes_hpp_

//#include <wx/bmpcbox.h>
#include <wx/colourdata.h>
#include <wx/gdicmn.h>
#include <wx/clrpicker.h>

#include "libslic3r/Preset.hpp"
#include "wxExtensions.hpp"
#include "BitmapComboBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "GUI_Utils.hpp"
#include "EncodedFilament.hpp"

class wxString;
class wxTextCtrl;
class wxStaticText;
class ScalableButton;
class wxBoxSizer;
class wxComboBox;
class wxStaticBitmap;

namespace Slic3r {

namespace GUI {

class BitmapCache;

// ---------------------------------
// ***  PresetComboBox  ***
// ---------------------------------

// BitmapComboBox used to presets list on Sidebar and Tabs
class PresetComboBox : public ::ComboBox // BBS
{
    bool m_show_all { false };
public:
    PresetComboBox(wxWindow* parent, Preset::Type preset_type, const wxSize& size = wxDefaultSize, PresetBundle* preset_bundle = nullptr);
    ~PresetComboBox();

	enum LabelItemType {
		LABEL_ITEM_PHYSICAL_PRINTER = 0xffffff01,
        LABEL_ITEM_PRINTER_MODELS,
		LABEL_ITEM_DISABLED,
		LABEL_ITEM_MARKER,
		LABEL_ITEM_PHYSICAL_PRINTERS,
		LABEL_ITEM_WIZARD_PRINTERS,
        LABEL_ITEM_WIZARD_FILAMENTS,
        LABEL_ITEM_WIZARD_MATERIALS,
        LABEL_ITEM_WIZARD_ADD_PRINTERS,
        LABEL_ITEM_IMPORT_SPOOLMAN,

        LABEL_ITEM_MAX,
	};

    enum FilamentAMSType :unsigned int {
        ORIGINAL ,
        FROM_AMS,
    };

    void set_label_marker(int item, LabelItemType label_item_type = LABEL_ITEM_MARKER);
    bool set_printer_technology(PrinterTechnology pt);

    void set_selection_changed_function(std::function<void(int)> sel_changed) { on_selection_changed = sel_changed; }

    bool is_selected_physical_printer();

    bool is_selected_printer_model();

    // Return true, if physical printer was selected
    // and next internal selection was accomplished
    bool selection_is_changed_according_to_physical_printers();

    void update(std::string select_preset);
    // select preset which is selected in PreseBundle
    void update_from_bundle();

    // BBS: printer
    void add_connected_printers(std::string selected, bool alias_name = false);
    int  selected_connected_printer() const;

    // BBS: ams
    bool add_ams_filaments(std::string selected, bool alias_name = false);
    int  selected_ams_filament() const;

    void set_filament_idx(const int extr_idx) { m_filament_idx = extr_idx; }
    int  get_filament_idx() const { return m_filament_idx; }

    std::string get_selected_dev_id() const { return m_selected_dev_id; }
    void clear_selected_dev_id() { m_selected_dev_id.clear(); }

    // BBS
    wxString get_tooltip(const Preset& preset);

    wxString get_preset_item_name(unsigned int index);

    static wxColor different_color(wxColor const & color);

    virtual wxString get_preset_name(const Preset& preset);
    Preset::Type     get_type() { return m_type; }
    void             show_all(bool show_all);
    virtual void update();
    virtual void msw_rescale();
    virtual void sys_color_changed();
    virtual void OnSelect(wxCommandEvent& evt);

protected:
    typedef std::size_t Marker;
    std::function<void(int)>    on_selection_changed { nullptr };

    Preset::Type        m_type;
    std::string         m_main_bitmap_name;

    PresetBundle*       m_preset_bundle {nullptr};
    PresetCollection*   m_collection {nullptr};

    // Caching bitmaps for the all bitmaps, used in preset comboboxes
    static BitmapCache& bitmap_cache();

    // Indicator, that the preset is compatible with the selected printer.
    ScalableBitmap      m_bitmapCompatible;
    // Indicator, that the preset is NOT compatible with the selected printer.
    ScalableBitmap      m_bitmapIncompatible;

    int m_last_selected;
    int m_em_unit;

    // BBS: ams
    int  m_filament_idx       = -1;
    int m_first_ams_filament = 0;
    int m_last_ams_filament = 0;

    // parameters for an icon's drawing
    int icon_height;
    int norm_icon_width;
    int thin_icon_width;
    int wide_icon_width;
    int space_icon_width;
    int thin_space_icon_width;
    int wide_space_icon_width;

    // BBS: printer
    int m_first_printer_idx = 0;
    int m_last_printer_idx  = 0;

    std::string              m_selected_dev_id;

    PrinterTechnology printer_technology {ptAny};

    void invalidate_selection();
    void validate_selection(bool predicate = false);
    void update_selection();

    // BBS: ams
    int  update_ams_color();

#ifdef __linux__
    static const char* separator_head() { return "-- "; }
    static const char* separator_tail() { return " --"; }
#else // __linux__
    static const char* separator_head() { return "--"; }
    static const char* separator_tail() { return " --"; }
#endif // __linux__
    static wxString    separator(const std::string& label);

    wxBitmap* get_bmp(  std::string bitmap_key, bool wide_icons, const std::string& main_icon_name,
                        bool is_compatible = true, bool is_system = false, bool is_single_bar = false,
                        const std::string& filament_rgb = "", const std::string& extruder_rgb = "", const std::string& material_rgb = "");

    wxBitmap* get_bmp(  std::string bitmap_key, const std::string& main_icon_name, const std::string& next_icon_name,
                        bool is_enabled = true, bool is_compatible = true, bool is_system = false);

    wxBitmap *get_bmp(Preset const &preset);

private:
    void fill_width_height();
};


// ---------------------------------
// ***  PlaterPresetComboBox  ***
// ---------------------------------

class PlaterPresetComboBox : public PresetComboBox
{
public:
    PlaterPresetComboBox(wxWindow *parent, Preset::Type preset_type);
    ~PlaterPresetComboBox();

    ScalableButton* edit_btn { nullptr };

    // BBS
    wxButton* clr_picker { nullptr };
    wxColourData m_clrData;

    wxColor get_color() { return m_color; }

    bool switch_to_tab();
    void change_extruder_color();
    void show_add_menu();
    void show_edit_menu();

    wxString get_preset_name(const Preset& preset) override;
    void update() override;
    void msw_rescale() override;
    void OnSelect(wxCommandEvent& evt) override;
    void update_badge_according_flag();

    FilamentColor get_cur_color_info();
    void show_default_color_picker();
    void sync_colour_config(const std::vector<std::string> &clrs, bool is_gradient);
    void sys_color_changed() override;

private:
    // BBS
    wxColor m_color;
};


// ---------------------------------
// ***  TabPresetComboBox  ***
// ---------------------------------

class TabPresetComboBox : public PresetComboBox
{
    bool show_incompatible {false};
    bool m_enable_all {false};

public:
    TabPresetComboBox(wxWindow *parent, Preset::Type preset_type);
    ~TabPresetComboBox() {}
    void set_show_incompatible_presets(bool show_incompatible_presets) {
        show_incompatible = show_incompatible_presets;
    }

    wxString get_preset_name(const Preset& preset) override;
    void update() override;
    void update_dirty();
    void msw_rescale() override;
    void OnSelect(wxCommandEvent& evt) override;

    void set_enable_all(bool enable=true) { m_enable_all = enable; }

    PresetCollection*   presets()   const { return m_collection; }
    Preset::Type        type()      const { return m_type; }
};

// ---------------------------------
// ***  CalibrateFilamentComboBox  ***
// ---------------------------------

class CalibrateFilamentComboBox : public PlaterPresetComboBox
{
public:
    CalibrateFilamentComboBox(wxWindow *parent);
    ~CalibrateFilamentComboBox();

    void load_tray(DynamicPrintConfig & config);

    void update() override;
    void msw_rescale() override;
    void OnSelect(wxCommandEvent &evt) override;
    const Preset* get_selected_preset() { return m_selected_preset; }
    std::string get_tray_name() { return m_tray_name; }
    std::string get_tag_uid() { return m_tag_uid; }
    bool is_tray_exist() { return m_filament_exist; }
    bool is_compatible_with_printer() { return m_is_compatible; }

private:
    std::string m_tray_name;
    std::string m_filament_id;
    std::string m_tag_uid;
    std::string m_filament_type;
    std::string m_filament_color;
    bool m_filament_exist{false};
    bool m_is_compatible{true};
    const Preset* m_selected_preset = nullptr;
    std::map<wxString, std::pair<std::string, wxBitmap*>> m_nonsys_presets;
    std::map<wxString, std::pair<std::string, wxBitmap*>> m_system_presets;
};

} // namespace GUI
} // namespace Slic3r

#endif
