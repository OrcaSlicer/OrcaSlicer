#include "DragCanvas.hpp"
#include "wxExtensions.hpp"
#include "GUI_App.hpp"

namespace Slic3r { namespace GUI {

#define CANVAS_WIDTH FromDIP(240)
#define SHAPE_SIZE FromDIP(20)
#define SHAPE_GAP (2 * SHAPE_SIZE)
#define LINE_HEIGHT (SHAPE_SIZE + FromDIP(5))
static const wxColour CANVAS_BORDER_COLOR = wxColour(0xCECECE);

DragCanvas::DragCanvas(wxWindow* parent, const std::vector<std::string>& colors, const std::vector<int>& order)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_drag_mode(DragMode::NONE)
    , m_max_shape_pos(wxPoint(0, 0))
{
    SetBackgroundColour(*wxWHITE);

    m_background_color = StateColor::darkModeColorFor("#FFFFFF"); // ORCA
    m_border_color     = StateColor::darkModeColorFor("#DBDBDB"); // ORCA

    m_arrow_bmp = create_scaled_bitmap("plate_settings_arrow", this, 16);

    set_shape_list(colors, order);

    Bind(wxEVT_PAINT, &DragCanvas::on_paint, this);
    Bind(wxEVT_ERASE_BACKGROUND, &DragCanvas::on_erase, this);
    Bind(wxEVT_LEFT_DOWN, &DragCanvas::on_mouse, this);
    Bind(wxEVT_LEFT_UP, &DragCanvas::on_mouse, this);
    Bind(wxEVT_MOTION, &DragCanvas::on_mouse, this);
    Bind(wxEVT_ENTER_WINDOW, &DragCanvas::on_mouse, this);
    Bind(wxEVT_LEAVE_WINDOW, &DragCanvas::on_mouse, this);
}

DragCanvas::~DragCanvas()
{
    for (int i = 0; i < m_dragshape_list.size(); i++) {
        delete m_dragshape_list[i];
    }
    m_dragshape_list.clear();

    if (m_drag_image)
        delete m_drag_image;
}

void DragCanvas::set_shape_list(const std::vector<std::string>& colors, const std::vector<int>& order)
{
    m_dragshape_list.clear();

    for (int i = 0; i < order.size(); i++) {
        wxBitmap* bmp = get_extruder_color_icon(colors[order[i] - 1], std::to_string(order[i]), SHAPE_SIZE, SHAPE_SIZE);
        DragShape* shape = new DragShape(*bmp, order[i]);
        m_dragshape_list.push_back(shape);
    }

    // wrapping lines
    for (int i = 0; i < order.size(); i++) {
        int shape_pos_x = FromDIP(10) + i * SHAPE_GAP;
        int shape_pos_y = FromDIP(5);
        while (shape_pos_x + SHAPE_SIZE > CANVAS_WIDTH) {
            shape_pos_x -= CANVAS_WIDTH;
            shape_pos_y += LINE_HEIGHT;

            int row = shape_pos_y / LINE_HEIGHT + 1;
            if (row > 1) {
                if (row % 2 == 0) {
                    shape_pos_x += (SHAPE_GAP - SHAPE_SIZE);
                }
                else {
                    shape_pos_x -= (SHAPE_GAP - SHAPE_SIZE);
                    shape_pos_x += SHAPE_GAP;
                }
            }
        }

        m_max_shape_pos.x = std::max(m_max_shape_pos.x, shape_pos_x);
        m_max_shape_pos.y = std::max(m_max_shape_pos.y, shape_pos_y);
        m_dragshape_list[i]->SetPosition(wxPoint(shape_pos_x, shape_pos_y));
    }

    int rows = m_max_shape_pos.y / LINE_HEIGHT + 1;
    SetMinSize(wxSize(CANVAS_WIDTH, LINE_HEIGHT * rows + FromDIP(5)));
}

std::vector<int> DragCanvas::get_shape_list_order()
{
    std::vector<int> res;
    std::vector<DragShape*> ordered_list = get_ordered_shape_list();
    res.reserve(ordered_list.size());
    for (auto& item : ordered_list) {
        res.push_back(item->get_index());
    }
    return res;
}

std::vector<DragShape*> DragCanvas::get_ordered_shape_list()
{
    std::vector<DragShape*> ordered_list = m_dragshape_list;
    std::sort(ordered_list.begin(), ordered_list.end(), [](const DragShape* l, const DragShape* r) {
        if (l->GetPosition().y < r->GetPosition().y)
            return true;
        else if (l->GetPosition().y == r->GetPosition().y) {
            return l->GetPosition().x < r->GetPosition().x;
        }
        else {
            return false;
        }
        });
    return ordered_list;
}

void DragCanvas::on_paint(wxPaintEvent& event)
{
    wxPaintDC dc(this);

    for (int i = 0; i < m_dragshape_list.size(); i++) {
        m_dragshape_list[i]->paint(dc, m_dragshape_list[i] == m_slot_shape);

        auto arrow_pos = m_dragshape_list[i]->GetPosition() - wxSize(SHAPE_GAP - SHAPE_SIZE, 0);
        if (arrow_pos.x < 0) {
            arrow_pos.x = m_max_shape_pos.x;
            arrow_pos.y -= LINE_HEIGHT;
        }
        arrow_pos += wxSize((SHAPE_GAP - SHAPE_SIZE - m_arrow_bmp.GetWidth() / dc.GetContentScaleFactor()) / 2, (SHAPE_SIZE - m_arrow_bmp.GetHeight() / dc.GetContentScaleFactor()) / 2);
        dc.DrawBitmap(m_arrow_bmp, arrow_pos);
    }
}

void DragCanvas::on_erase(wxEraseEvent& event)
{
    wxSize size = GetSize();
    if (event.GetDC())
    {
        auto& dc = *(event.GetDC());
        dc.SetPen(  m_border_color);     // ORCA
        dc.SetBrush(m_background_color); // ORCA
        dc.DrawRectangle({ 0,0 }, size);
    }
    else 
    {
        wxClientDC dc(this);
        dc.SetPen(  m_border_color);     // ORCA
        dc.SetBrush(m_background_color); // ORCA
        dc.DrawRectangle({ 0,0 }, size);
    }
}

void DragCanvas::on_mouse(wxMouseEvent& event)
{
    if (event.LeftDown())
    {
        DragShape* shape = find_shape(event.GetPosition());
        if (shape)
        {
            m_drag_mode = DragMode::DRAGGING;
            m_drag_start_pos = event.GetPosition();
            m_dragging_shape = shape;

            if (m_drag_image) {
                delete m_drag_image;
                m_drag_image = nullptr;
            }
            m_drag_image = new wxDragImage(m_dragging_shape->GetBitmap());

            wxPoint offset = m_drag_start_pos - m_dragging_shape->GetPosition();
            bool success = m_drag_image->BeginDrag(offset, this);
            if (!success)
            {
                delete m_drag_image;
                m_drag_image = nullptr;
                m_drag_mode = DragMode::NONE;
            }
        }
    }
    else if (event.Dragging() && m_drag_mode == DragMode::DRAGGING)
    {
        DragShape* shape = find_shape(event.GetPosition());

        if (shape) {
            if (shape != m_dragging_shape) {
                m_slot_shape = shape;
                Refresh();
                Update();
            }
        }
        else {
            if (m_slot_shape) {
                m_slot_shape = nullptr;
                Refresh();
                Update();
            }
        }
        m_drag_image->Move(event.GetPosition());
        m_drag_image->Show();
    }
    else if (event.LeftUp() && m_drag_mode != DragMode::NONE)
    {
        m_drag_mode = DragMode::NONE;

        if (m_drag_image) {
            m_drag_image->Hide();
            m_drag_image->EndDrag();

            // swap position
            if (m_slot_shape && m_dragging_shape) {
                auto highlighted_pos = m_slot_shape->GetPosition();
                m_slot_shape->SetPosition(m_dragging_shape->GetPosition());
                m_dragging_shape->SetPosition(highlighted_pos);
                m_slot_shape = nullptr;
                m_dragging_shape = nullptr;
            }
        }
        Refresh();
        Update();
    }
}

DragShape* DragCanvas::find_shape(const wxPoint& pt) const
{
    for (auto& shape : m_dragshape_list) {
        if (shape->hit_test(pt))
            return shape;
    }
    return nullptr;
}


DragShape::DragShape(const wxBitmap& bitmap, int index)
    : m_bitmap(bitmap)
    , m_pos(wxPoint(0,0))
    , m_index(index)
{
}

bool DragShape::hit_test(const wxPoint& pt) const
{
    wxRect rect(wxRect(m_pos.x, m_pos.y, m_bitmap.GetWidth(), m_bitmap.GetHeight()));
    return rect.Contains(pt.x, pt.y);
}

void DragShape::paint(wxDC& dc, bool highlight)
{
    dc.DrawBitmap(m_bitmap, m_pos);
    if (highlight)
    {
        dc.SetPen(*wxWHITE_PEN);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(m_pos.x, m_pos.y, m_bitmap.GetWidth(), m_bitmap.GetHeight());
    }
}

}}