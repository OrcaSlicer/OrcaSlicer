#ifndef ORCASLICER_SPOOLMANIMPORTDIALOG_HPP
#define ORCASLICER_SPOOLMANIMPORTDIALOG_HPP

#include <Spoolman.hpp>
#include <slic3r/GUI/Widgets/Button.hpp>
#include "GUI_Utils.hpp"
#include "PresetComboBoxes.hpp"
#define EM wxGetApp().em_unit()

namespace Slic3r { namespace GUI {
class SpoolmanViewCtrl;

enum Column { COL_CHECK = 0, COL_ID, COL_COLOR, COL_VENDOR, COL_NAME, COL_MATERIAL, COL_PRESET_DATA, COL_COUNT };

//-----------------------------------------
// SpoolmanNode
//-----------------------------------------

class SpoolmanNode
{
public:
    explicit SpoolmanNode(const SpoolmanFilamentShrPtr& filament) : m_filament(filament) {}

    int      get_id() const { return m_filament->id; };
    wxColour get_color() const { return {m_filament->color}; }
    wxString get_vendor_name() const { return m_filament->vendor ? wxString::FromUTF8(m_filament->vendor->name) : wxString(); }
    wxString get_filament_name() const { return wxString::FromUTF8(m_filament->name); }
    wxString get_material() const { return wxString::FromUTF8(m_filament->material); }
    bool     get_has_preset_data() const { return !m_filament->preset_data.empty(); }

    bool get_checked() { return m_checked; };
    // return if value has changed
    bool set_checked(bool value)
    {
        if (m_checked == value)
            return false;
        m_checked = value;
        return true;
    };

    SpoolmanFilamentShrPtr get_filament() { return m_filament; }

protected:
    SpoolmanFilamentShrPtr m_filament;
    bool                   m_checked{false};
};

typedef std::shared_ptr<SpoolmanNode> SpoolmanNodeShrPtr;

// Static helper method
namespace {
SpoolmanNode* get_node(const wxDataViewItem& item)
{
    if (!item.IsOk())
        return nullptr;
    return static_cast<SpoolmanNode*>(item.GetID());
}
} // namespace

//-----------------------------------------
// SpoolmanViewItem
//-----------------------------------------

class SpoolmanViewModel : public wxDataViewModel
{
public:
    SpoolmanViewModel() {}

    wxDataViewItem AddFilament(const SpoolmanFilamentShrPtr& filament);

    void SetAllToggles(bool value);

    std::vector<SpoolmanFilamentShrPtr> GetSelectedFilaments();

    wxString     GetColumnType(unsigned int col) const override;
    unsigned int GetColumnCount() const override { return 5; }

    // returns a nullptr item. this control only has a single tier
    wxDataViewItem GetParent(const wxDataViewItem& item) const override { return wxDataViewItem(nullptr); };
    unsigned int   GetChildren(const wxDataViewItem& parent, wxDataViewItemArray& array) const override;

    void SetAssociatedControl(SpoolmanViewCtrl* ctrl) { m_ctrl = ctrl; }

    void GetValue(wxVariant& variant, const wxDataViewItem& item, unsigned int col) const override;
    bool SetValue(const wxVariant& variant, const wxDataViewItem& item, unsigned int col) override;

    // Not using container functionality
    bool IsContainer(const wxDataViewItem& item) const override { return false; };

    // Is the container just a header or an item with all columns
    // In our case it is an item with all columns
    bool HasContainerColumns(const wxDataViewItem& WXUNUSED(item)) const override { return true; }

protected:
    wxWindow*                       m_parent{nullptr};
    SpoolmanViewCtrl*               m_ctrl{nullptr};
    std::vector<SpoolmanNodeShrPtr> m_top_children;
};

//-----------------------------------------
// SpoolmanViewCtrl
//-----------------------------------------

class SpoolmanViewCtrl : public wxDataViewCtrl
{
public:
    SpoolmanViewCtrl(wxWindow* parent);
    ~SpoolmanViewCtrl()
    {
        if (m_model)
            m_model->DecRef();
    }

    SpoolmanViewModel* get_model() const { return m_model; }

protected:
    SpoolmanViewModel* m_model;
};

//-----------------------------------------
// SpoolmanImportDialog
//-----------------------------------------

class SpoolmanImportDialog : public DPIDialog
{
public:
    SpoolmanImportDialog(wxWindow* parent);

    void EndModal(int retCode) override;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

    void on_import();

    Spoolman*          m_spoolman{Spoolman::get_instance()};
    SpoolmanViewCtrl*  m_svc;
    TabPresetComboBox* m_preset_combobox;
    wxCheckBox*        m_detach_checkbox;
    wxCheckBox*        m_ignore_preset_data_checkbox;
};
}} // namespace Slic3r::GUI

#endif // ORCASLICER_SPOOLMANIMPORTDIALOG_HPP