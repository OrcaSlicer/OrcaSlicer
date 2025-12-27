#ifndef slic3r_SavePresetDialog_hpp_
#define slic3r_SavePresetDialog_hpp_

//#include <wx/gdicmn.h>

#include "libslic3r/Preset.hpp"
#include "wxExtensions.hpp"
#include "GUI_Utils.hpp"
#include "Widgets/RadioGroup.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"

class wxString;
class wxStaticText;
class wxComboBox;
class wxStaticBitmap;

#define SAVE_PRESET_DIALOG_DEF_COLOUR wxColour(255, 255, 255)
#define SAVE_PRESET_DIALOG_INPUT_SIZE wxSize(FromDIP(360), FromDIP(24))
#define SAVE_PRESET_DIALOG_BUTTON_SIZE wxSize(FromDIP(60), FromDIP(24))

namespace Slic3r {
namespace GUI {

class SavePresetDialog : public DPIDialog
{
    enum ActionType
    {
        ChangePreset,
        AddPreset,
        Switch, 
        UndefAction
    };

    class Item : public wxWindow
    {
    public:
        enum ValidationType
        {
            Valid,
            NoValid,
            Warning
        };

        Item(Preset::Type type, const std::string& suffix, wxBoxSizer* sizer, SavePresetDialog* parent);

        void            update_valid_bmp();
        void accept();
        virtual void DoSetSize(int x, int y, int width, int height, int sizeFlags = wxSIZE_AUTO);

        bool            is_valid()      const { return m_valid_type != NoValid; }
        Preset::Type    type()          const { return m_type; }
        std::string     preset_name()   const { return m_preset_name; }
        //BBS: add project embedded preset relate logic
        bool save_to_project() const { return m_save_to_project; }

        Preset::Type    m_type;
        ValidationType  m_valid_type;
        std::string		m_preset_name;

        SavePresetDialog*   m_parent        {nullptr};
        wxStaticBitmap*     m_valid_bmp     {nullptr};
        wxComboBox*         m_combo         {nullptr};
        TextInput*         m_input_ctrl    {nullptr};
        wxStaticText*       m_valid_label   {nullptr};

        PresetCollection*   m_presets       {nullptr};

        //BBS: add project embedded preset relate logic
        bool                m_save_to_project {false};
        RadioGroup*         m_radio_group; // ORCA

        void update();
    };

    std::vector<Item*>   m_items;

    wxBoxSizer*         m_presets_sizer     {nullptr};
    wxStaticText*       m_label             {nullptr};
    wxBoxSizer*         m_radio_sizer       {nullptr};  
    ActionType          m_action            {UndefAction};

    std::string         m_ph_printer_name;
    std::string         m_old_preset_name;

public:
    SavePresetDialog(wxWindow *parent, Preset::Type type, std::string suffix = "");
    SavePresetDialog(wxWindow* parent, std::vector<Preset::Type> types, std::string suffix = "");
    ~SavePresetDialog();

    void AddItem(Preset::Type type, const std::string& suffix);

    std::string get_name();
    std::string get_name(Preset::Type type);
    void input_name_from_other(std::string new_preset_name);
    void confirm_from_other();

    bool enable_ok_btn() const;
    void add_info_for_edit_ph_printer(wxBoxSizer *sizer);
    void update_info_for_edit_ph_printer(const std::string &preset_name);
    void layout();
    //BBS: add project embedded preset relate logic
    bool get_save_to_project_selection(Preset::Type type);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_sys_color_changed() override {}

private:
    void build(std::vector<Preset::Type> types, std::string suffix = "");
    void on_select_cancel(wxCommandEvent &event);
    void update_physical_printers(const std::string &preset_name);
    void accept(wxCommandEvent &event);
};

} // namespace GUI
} // namespace Slic3r

#endif
