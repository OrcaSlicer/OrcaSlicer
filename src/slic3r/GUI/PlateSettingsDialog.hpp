#ifndef slic3r_GUI_PlateSettingsDialog_hpp_
#define slic3r_GUI_PlateSettingsDialog_hpp_

#include "Plater.hpp"
#include "PartPlate.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/RadioBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "DragCanvas.hpp"
#include "libslic3r/ParameterUtils.hpp"

namespace Slic3r { namespace GUI {

wxDECLARE_EVENT(EVT_SET_BED_TYPE_CONFIRM, wxCommandEvent);
wxDECLARE_EVENT(EVT_NEED_RESORT_LAYERS, wxCommandEvent);

struct LayerSeqInfo {
    int begin_layer_number;
    int end_layer_number;
    std::vector<int> print_sequence;

    bool operator<(const LayerSeqInfo& another) const;
};

class LayerNumberTextInput : public ComboBox {
public:
    enum class Type {
        Begin,
        End
    };
    enum class ValueType {
        Custom,
        End
    };
    LayerNumberTextInput(wxWindow* parent, int layer_number, wxSize size, Type type, ValueType value_type = ValueType::Custom);
    void link(LayerNumberTextInput* layer_input) { 
        if (m_another_layer_input) return; 
        m_another_layer_input = layer_input; 
        layer_input->link(this); }
    void set_layer_number(int layer_number);
    int get_layer_number();
    Type get_input_type() { return m_type; }
    ValueType get_value_type() { return m_value_type; }
    bool is_layer_number_valid();

protected:
    void update_label();

private:
    LayerNumberTextInput* m_another_layer_input{ nullptr };
    int m_layer_number;
    Type m_type;
    ValueType m_value_type;
};

class OtherLayersSeqPanel : public wxPanel {
public:
    OtherLayersSeqPanel(wxWindow* parent);

    void sync_layers_print_seq(int selection, const std::vector<LayerSeqInfo>& seq);

    int get_layers_print_seq_choice() { return m_other_layer_print_seq_choice->GetSelection(); };

    std::vector<LayerSeqInfo> get_layers_print_seq_infos() { return m_layer_seq_infos; }

protected:
    void append_layer(const LayerSeqInfo* layer_info = nullptr);
    void popup_layer();
    void clear_all_layers();

private:
    ScalableBitmap  m_bmp_delete;
    ScalableBitmap  m_bmp_add;
    ComboBox* m_other_layer_print_seq_choice{ nullptr };
    wxPanel* m_layer_input_panel{ nullptr };
    std::vector<wxBoxSizer*> m_layer_input_sizer_list;
    std::vector<LayerNumberTextInput*> m_begin_layer_input_list;
    std::vector<LayerNumberTextInput*> m_end_layer_input_list;
    std::vector<DragCanvas*> m_drag_canvas_list;
    std::vector<LayerSeqInfo> m_layer_seq_infos;
};

class PlateSettingsDialog : public DPIDialog
{
public:
    enum ButtonStyle {
        ONLY_CONFIRM = 0,
        CONFIRM_AND_CANCEL = 1,
        MAX_STYLE_NUM = 2
    };
    PlateSettingsDialog(
        wxWindow* parent,
        const wxString& title = wxEmptyString,
        bool only_layer_seq = false,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long            style = wxCLOSE_BOX | wxCAPTION
    );

    ~PlateSettingsDialog();
    void sync_bed_type(BedType type);
    void sync_print_seq(int print_seq = 0);
    void sync_first_layer_print_seq(int selection, const std::vector<int>& seq = std::vector<int>());
    void sync_other_layers_print_seq(int selection, const std::vector<LayerPrintSequence>& seq);
    void sync_spiral_mode(bool spiral_mode, bool as_global);
    wxString to_bed_type_name(BedType bed_type);
    wxString to_print_sequence_name(PrintSequence print_seq);
    void on_dpi_changed(const wxRect& suggested_rect) override;

    int get_print_seq_choice() {
        int choice = 0;
        if (m_print_seq_choice != nullptr)
            choice =  m_print_seq_choice->GetSelection();
        return choice;
    }

    BedType get_bed_type_choice();

    wxString get_plate_name() const;
    void set_plate_name(const wxString& name);

    int get_first_layer_print_seq_choice() {
        int choice = 0;
        if (m_first_layer_print_seq_choice != nullptr)
            choice = m_first_layer_print_seq_choice->GetSelection();
        return choice;
    };

    int get_other_layers_print_seq_choice() {
        if (m_other_layers_seq_panel)
            return m_other_layers_seq_panel->get_layers_print_seq_choice();
        return 0;
    };

    std::vector<int> get_first_layer_print_seq();

    std::vector<LayerPrintSequence> get_other_layers_print_seq_infos() {
        const std::vector<LayerSeqInfo>& layer_seq_infos = m_other_layers_seq_panel->get_layers_print_seq_infos();
        std::vector<LayerPrintSequence> result;
        result.reserve(layer_seq_infos.size());
        for (int i = 0; i < layer_seq_infos.size(); i++) {
            LayerPrintSequence item = std::make_pair(std::make_pair(layer_seq_infos[i].begin_layer_number, layer_seq_infos[i].end_layer_number), layer_seq_infos[i].print_sequence);
            result.push_back(item);
        }
        return result;
    }

    int get_spiral_mode_choice() {
        int choice = 0;
        if (m_spiral_mode_choice != nullptr)
            choice = m_spiral_mode_choice->GetSelection();
        return choice;
    };

    bool get_spiral_mode(){
        return false;
    }

protected:
    void add_layers();
    void delete_layers();

protected:
    ComboBox* m_bed_type_choice { nullptr };
    std::vector<BedType> m_cur_combox_bed_types;
    ComboBox* m_print_seq_choice { nullptr };
    ComboBox* m_first_layer_print_seq_choice { nullptr };
    ComboBox* m_spiral_mode_choice { nullptr };
    DragCanvas* m_drag_canvas;
    OtherLayersSeqPanel* m_other_layers_seq_panel;
    TextInput *m_ti_plate_name;
};

class PlateNameEditDialog : public DPIDialog
{
public:
    enum ButtonStyle { ONLY_CONFIRM = 0, CONFIRM_AND_CANCEL = 1, MAX_STYLE_NUM = 2 };
    PlateNameEditDialog(wxWindow *      parent,
                        wxWindowID      id    = wxID_ANY,
                        const wxString &title = wxEmptyString,
                        const wxPoint & pos   = wxDefaultPosition,
                        const wxSize &  size  = wxDefaultSize,
                        long            style = wxCLOSE_BOX | wxCAPTION);

    ~PlateNameEditDialog();
    void     on_dpi_changed(const wxRect &suggested_rect) override;

    wxString get_plate_name() const;
    void     set_plate_name(const wxString &name);

protected:
    TextInput *m_ti_plate_name;
};
}} // namespace Slic3r::GUI

#endif