#ifndef slic3r_GUI_DragCanvas_hpp_
#define slic3r_GUI_DragCanvas_hpp_

#include "wx/bitmap.h"
#include "wx/dragimag.h"

namespace Slic3r { namespace GUI {

class DragShape : public wxObject
{
public:
    DragShape(const wxBitmap& bitmap, int index);
    ~DragShape() {}

    wxPoint GetPosition() const { return m_pos; }
    void SetPosition(const wxPoint& pos) { m_pos = pos; }

    const wxBitmap& GetBitmap() const { return m_bitmap; }
    void SetBitmap(const wxBitmap& bitmap) { m_bitmap = bitmap; }

    int get_index() { return m_index; }

    bool hit_test(const wxPoint& pt) const;
    void paint(wxDC& dc, bool highlight = false);

protected:
    wxPoint     m_pos;
    wxBitmap    m_bitmap;
    int         m_index;
};


enum class DragMode {
    NONE,
    DRAGGING,
};
class DragCanvas : public wxPanel
{
public:
    DragCanvas(wxWindow* parent, const std::vector<std::string>& colors, const std::vector<int>& order);
    ~DragCanvas();
    void set_shape_list(const std::vector<std::string>& colors, const std::vector<int>& order);
    std::vector<int> get_shape_list_order();
    std::vector<DragShape*> get_ordered_shape_list();

protected:
    void on_paint(wxPaintEvent& event);
    void on_erase(wxEraseEvent& event);
    void on_mouse(wxMouseEvent& event);
    DragShape* find_shape(const wxPoint& pt) const;

private:
    std::vector<DragShape*>    m_dragshape_list;
    DragMode                   m_drag_mode;
    DragShape*                 m_dragging_shape{ nullptr };
    DragShape*                 m_slot_shape{ nullptr }; // The shape that's being highlighted
    wxDragImage*               m_drag_image{ nullptr };
    wxPoint                    m_drag_start_pos;
    wxBitmap                   m_arrow_bmp;
    wxPoint                    m_max_shape_pos;
    wxColour                   m_background_color; // ORCA
    wxColour                   m_border_color; // ORCA
};


}}
#endif
