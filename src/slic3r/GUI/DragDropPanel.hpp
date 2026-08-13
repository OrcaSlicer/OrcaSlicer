#ifndef slic3r_DragDropPanel_hpp_
#define slic3r_DragDropPanel_hpp_

#include "GUI.hpp"
#include "GUI_Utils.hpp"
#include "Widgets/Label.hpp"

#include <wx/simplebook.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <vector>


namespace Slic3r { namespace GUI {

wxDECLARE_EVENT(wxEVT_DRAG_DROP_COMPLETED, wxCommandEvent);

wxColor Hex2Color(const std::string& str);

class ColorPanel;
class DragDropPanel : public wxPanel
{
public:
    DragDropPanel(wxWindow *parent, const wxString &label, bool is_auto, bool has_title = true, bool is_sub = false);

    void AddColorBlock(const wxColour &color, const std::string &type, int filament_id, bool update_ui = true);
    void RemoveColorBlock(ColorPanel *panel, bool update_ui = true);
    void DoDragDrop(ColorPanel *panel, const wxColour &color, const std::string &type, int filament_id);
    void UpdateLabel(const wxString &label);

    std::vector<int> GetAllFilaments() const;

    void set_is_draging(bool is_draging) { m_is_draging = is_draging; }
    bool is_draging() const { return m_is_draging; }

    std::vector<ColorPanel *> get_filament_blocks() const { return m_filament_blocks; }

private:
    wxBoxSizer *m_sizer;
    wxGridSizer *m_grid_item_sizer;
    Label       *m_title_label = nullptr;
    bool         m_is_auto;

    std::vector<ColorPanel *> m_filament_blocks;

    void NotifyDragDropCompleted();
private:
    bool m_is_draging = false;
};

///////////////   ColorPanel  start ////////////////////////
// The UI panel of drag item
class ColorPanel : public wxPanel
{
public:
    ColorPanel(DragDropPanel *parent, const wxColour &color, int filament_id, const std::string& type);

    wxColour GetColor() const { return m_color; }
    int      GetFilamentId() const { return m_filament_id; }
    std::string GetType() const { return m_type; }

private:
    void OnLeftDown(wxMouseEvent &event);
    void OnLeftUp(wxMouseEvent &event);
    void OnPaint(wxPaintEvent &event);

    DragDropPanel *m_parent;
    wxColor        m_color;
    std::string    m_type;
    int            m_filament_id;

};

// Drop zone for one extruder that can split into two sub-zones (High Flow / Standard)
// when the extruder mixes nozzle volume types (Hybrid flow), and collapses back to a
// single unified zone otherwise.
class SeparatedDragDropPanel : public wxPanel
{
public:
    SeparatedDragDropPanel(wxWindow *parent, const wxString &label, bool use_separation = false);

    void AddColorBlock(const wxColour &color, const std::string &type, int filament_id, bool is_high_flow = false, bool update_ui = true);
    void RemoveColorBlock(ColorPanel *panel, bool update_ui = true);

    std::vector<int> GetAllFilaments() const;
    std::vector<int> GetHighFlowFilaments() const;
    std::vector<int> GetStandardFilaments() const;
    std::vector<int> GetTPUHighFlowFilaments() const;

    std::vector<ColorPanel *> get_filament_blocks() const;
    std::vector<ColorPanel *> get_high_flow_blocks() const;
    std::vector<ColorPanel *> get_standard_blocks() const;

    void SetUseSeparation(bool use_separation);
    bool IsUseSeparation() const { return m_use_separation; }
    void ClearAllBlocks();
    void UpdateLabel(const wxString &label);

private:
    void UpdateLayout();

    wxBoxSizer   *m_main_sizer;
    wxPanel      *m_content_panel;
    wxBoxSizer   *m_content_sizer;
    wxPanel      *m_separator = nullptr;
    Label        *m_label;

    DragDropPanel *m_high_flow_panel;
    DragDropPanel *m_standard_panel;

    DragDropPanel *m_unified_panel;

    bool m_use_separation;
};

}} // namespace Slic3r::GUI

#endif /* slic3r_DragDropPanel_hpp_ */
