// Orca: This file is ported from latest PrusaSlicer

#include <algorithm>
#include <wx/dcbuffer.h>

#include "RammingChart.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

wxDEFINE_EVENT(EVT_WIPE_TOWER_CHART_CHANGED, wxCommandEvent);


void Chart::draw() {
    wxAutoBufferedPaintDC dc(this); // unbuffered DC caused flickering on win

    // scaling button and tick line from text size gives better result compared to dc.GetContentScale
    int text_width, text_height;
    dc.GetTextExtent("m",&text_width,&text_height);
    side       = text_width;
    int tick_w = text_width / 2;

    dc.SetBrush(GetBackgroundColour());
    dc.SetPen(GetBackgroundColour());
    dc.DrawRectangle(GetClientRect());  // otherwise the background would end up black on windows

    dc.SetPen(    wxPen(StateColor::darkModeColorFor(wxColour("#DBDBDB")), 1)); // input box border color
    dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#F1F1F1")))); // sidebar titlebar bg color
    dc.DrawRectangle(m_rect);
    
    if (visible_area.m_width < 0.499) {
        dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#FF6F00"))); // Use orange color for warning
        dc.DrawText(_(L("NO RAMMING AT ALL")),wxPoint(m_rect.GetLeft()+m_rect.GetWidth()/2-legend_side,m_rect.GetBottom()-m_rect.GetHeight()/2));
        return;
    }
    
    
    if (!m_line_to_draw.empty()) {
        for (unsigned int i=0;i<m_line_to_draw.size()-2;++i) {
            int color = 444*((m_rect.GetBottom()-(m_line_to_draw)[i])/double(m_rect.GetHeight()));
            dc.SetPen( wxPen( wxColor(std::min(222,color), 222-std::max(color-222,0), 60), 1) ); // adding blue color sligtly gives a bit more modern look instead using raw red & green
            dc.DrawLine(m_rect.GetLeft()+1+i,(m_line_to_draw)[i],m_rect.GetLeft()+1+i,m_rect.GetBottom());        
        }
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#363636")), 1));
        for (unsigned int i=0;i<m_line_to_draw.size()-2;++i) {
            if (splines)
                dc.DrawLine(m_rect.GetLeft()+i,(m_line_to_draw)[i],m_rect.GetLeft()+i+1,(m_line_to_draw)[i+1]);
            else {
                dc.DrawLine(m_rect.GetLeft()+i,(m_line_to_draw)[i],m_rect.GetLeft()+i+1,(m_line_to_draw)[i]);
                dc.DrawLine(m_rect.GetLeft()+i+1,(m_line_to_draw)[i],m_rect.GetLeft()+i+1,(m_line_to_draw)[i+1]);
            }
        }
    }
    
    // draw draggable buttons
    dc.SetBrush(StateColor::darkModeColorFor(wxColour("#009688"))); // orca color for draggable circles
    dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#363636")), 1));
    for (auto& button : m_buttons)
        //dc.DrawRectangle(math_to_screen(button.get_pos())-wxPoint(side/2.,side/2.), wxSize(side,side));
        dc.DrawCircle(math_to_screen(button.get_pos()),side/2.);
        //dc.DrawRectangle(math_to_screen(button.get_pos()-wxPoint2DDouble(0.125,0))-wxPoint(0,5),wxSize(50,10));

    dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#363636"))); // Label color

    // draw x-axis:
    float last_mark = -10000;
    for (float math_x=int(visible_area.m_x*10)/10 ; math_x < (visible_area.m_x+visible_area.m_width) ; math_x+=0.1f) {
        int x = math_to_screen(wxPoint2DDouble(math_x,visible_area.m_y)).x;
        int y = m_rect.GetBottom();
        if (x-last_mark < legend_side) continue;
        dc.DrawLine(x,y+tick_w+1,x,y-tick_w); // +1 for border; make sure drawn on both size
        auto label = math_x == 0 ? "0" : wxString().Format(wxT("%.1f") , math_x); // prefer "0" to match text with Y "0"
        dc.GetTextExtent(label,&text_width,&text_height);// center text with lines
        dc.DrawText(label ,wxPoint(x - text_width * .5, y + .8 * scale_unit));
        last_mark = x;
    }
    
    // draw y-axis:
    last_mark=10000;
    for (int math_y=visible_area.m_y ; math_y < (visible_area.m_y+visible_area.m_height) ; math_y+=1) {
        int y = math_to_screen(wxPoint2DDouble(visible_area.m_x,math_y)).y;
        int x = m_rect.GetLeft();
        if (last_mark-y < legend_side) continue;    
        dc.DrawLine(x-tick_w,y,x+tick_w+1,y); // +1 for border; make sure drawn on both size
        auto label = wxString()<<math_y;
        dc.GetTextExtent(label,&text_width,&text_height);// center text with lines & make it right aligned
        dc.DrawText(label ,wxPoint(x - scale_unit - text_width, y - .5 * text_height + 1));
        last_mark = y;
    }
    
    // axis labels:
    wxString label = _(L("Time")) + " (" + _(L("s")) + ")";
    dc.GetTextExtent(label,&text_width,&text_height);
    dc.DrawText(label,wxPoint(0.5*(m_rect.GetRight()+m_rect.GetLeft())-text_width/2.f, m_rect.GetBottom()+0.6*legend_side));
    label = _(L("Volumetric speed")) + " (" + _(L(u8"mm³/s")) + ")";
    dc.GetTextExtent(label,&text_width,&text_height);
    dc.DrawRotatedText(label,wxPoint(0,0.5*(m_rect.GetBottom()+m_rect.GetTop())+text_width/2.f),90);

    // draw a label with the value above each button
    for (auto& button : m_buttons) {
        if (!visible_area.Contains(button.get_pos()))
            continue; 
        
        wxPoint button_screen_pos = math_to_screen(button.get_pos());
        wxString value_label = wxString().Format(wxT("%.1f"), button.get_pos().m_y);

        int label_width, label_height;
        dc.GetTextExtent(value_label, &label_width, &label_height);

        const int padding = 4;
        // Calculate label x position
        int label_x = button_screen_pos.x - (label_width/2); // centered with button
        label_x = std::clamp(label_x, m_rect.GetLeft() + (padding*2), m_rect.GetRight() - label_width - (padding*2)); // adjust to fit within chart bounds
        
        // Calculate label y position
        int label_y = button_screen_pos.y - (side/2) - label_height - (padding*2); // above button
        if (label_y - (padding*2) < m_rect.GetTop()) { // move below the button if there isn't enough space
            label_y = button_screen_pos.y + (side/2) + (padding*2); 
        }
        
        // Draw label background
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#DBDBDB")), 1));
        wxColour bg_color = StateColor::darkModeColorFor(wxColour("#F1F1F1"));
        dc.SetBrush(wxBrush(wxColour(bg_color.Red(), bg_color.Green(), bg_color.Blue(), 204))); // 80% opacity
        wxRect label_rect(label_x - padding, label_y - padding, label_width + (2*padding), label_height + (2*padding));
        dc.DrawRoundedRectangle(label_rect, 2);
        
        // Draw the label text
        dc.SetTextForeground(StateColor::darkModeColorFor("#363636")); 
        dc.DrawText(value_label, wxPoint(label_x, label_y));
    }
}

void Chart::mouse_right_button_clicked(wxMouseEvent& event) {
    if (!manual_points_manipulation)
        return;
    wxPoint point = event.GetPosition();
    int button_index = which_button_is_clicked(point);
    if (button_index != -1 && m_buttons.size()>2) {
        m_buttons.erase(m_buttons.begin()+button_index);
        recalculate_line();
    }
}
    


void Chart::mouse_clicked(wxMouseEvent& event) {
    m_uniform = (event.GetModifiers() == wxMOD_CONTROL);
    wxPoint point = event.GetPosition();
    int button_index = which_button_is_clicked(point);
    if ( button_index != -1) {
        m_dragged = &m_buttons[button_index];
        m_previous_mouse = point;            
    }
}
    
    
    
void Chart::mouse_moved(wxMouseEvent& event) {
    wxPoint pos = event.GetPosition();    
    wxRect rect = m_rect;
    if (!event.Dragging() || !m_dragged){
        // change cursor while button hovered && drag
        SetCursor((which_button_is_clicked(pos) != -1) ? wxCursor(wxCURSOR_SIZENS) : wxNullCursor);
        return;
    }
    rect.Deflate(side/2.);
    if (!(rect.Contains(pos))) {  // the mouse left chart area
        mouse_left_window(event);
        return;
    }    
    int delta_x = pos.x - m_previous_mouse.x;
    int delta_y = pos.y - m_previous_mouse.y;

    double new_y = m_dragged->get_pos().m_y - double(delta_y) / m_rect.GetHeight() * visible_area.m_height;

    if (m_uniform)
        for (ButtonToDrag& b : m_buttons)
            b.move(fixed_x?0:double(delta_x)/m_rect.GetWidth() * visible_area.m_width, new_y - b.get_pos().m_y); 
    else
        m_dragged->move(fixed_x?0:double(delta_x)/m_rect.GetWidth() * visible_area.m_width, new_y - m_dragged->get_pos().m_y); 

    m_previous_mouse = pos;
    recalculate_line();
}



void Chart::mouse_double_clicked(wxMouseEvent& event) {
    if (!manual_points_manipulation)
        return;
    wxPoint point = event.GetPosition();
    if (!m_rect.Contains(point))     // the click is outside the chart
        return;
    m_buttons.push_back(screen_to_math(point));
    std::sort(m_buttons.begin(),m_buttons.end());
    recalculate_line();
    return;
}




void Chart::recalculate_line() {
    m_line_to_draw.clear();
    m_total_volume = 0.f;

    std::vector<wxPoint> points;
    for (auto& but : m_buttons) {
        points.push_back(wxPoint(math_to_screen(but.get_pos())));
        if (points.size()>1 && points.back().x==points[points.size()-2].x) points.pop_back();
        if (points.size()>1 && points.back().x > m_rect.GetRight()) {
            points.pop_back();
            break;
        }
    }

    // The calculation wouldn't work in case the ramming is to be turned off completely.
    if (points.size()>1) {
        std::sort(points.begin(),points.end(),[](wxPoint& a,wxPoint& b) { return a.x < b.x; });

        // Cubic spline interpolation: see https://en.wikiversity.org/wiki/Cubic_Spline_Interpolation#Methods
        const bool boundary_first_derivative = true; // true - first derivative is 0 at the leftmost and rightmost point
                                                     // false - second ---- || -------
        const int N = points.size()-1; // last point can be accessed as N, we have N+1 total points
        std::vector<float> diag(N+1);
        std::vector<float> mu(N+1);
        std::vector<float> lambda(N+1);
        std::vector<float> h(N+1);
        std::vector<float> rhs(N+1);
        
        // let's fill in inner equations
        for (int i=1;i<=N;++i) h[i] = points[i].x-points[i-1].x;
        std::fill(diag.begin(),diag.end(),2.f);
        for (int i=1;i<=N-1;++i) {
            mu[i] = h[i]/(h[i]+h[i+1]);
            lambda[i] = 1.f - mu[i];
            rhs[i] = 6 * ( float(points[i+1].y-points[i].y  )/(h[i+1]*(points[i+1].x-points[i-1].x)) -
                           float(points[i].y  -points[i-1].y)/(h[i]  *(points[i+1].x-points[i-1].x))   );
        }

        // now fill in the first and last equations, according to boundary conditions:
        if (boundary_first_derivative) {
            const float endpoints_derivative = 0;
            lambda[0] = 1;
            mu[N]     = 1;
            rhs[0] = (6.f/h[1]) * (float(points[0].y-points[1].y)/(points[0].x-points[1].x) - endpoints_derivative);
            rhs[N] = (6.f/h[N]) * (endpoints_derivative - float(points[N-1].y-points[N].y)/(points[N-1].x-points[N].x));
        }
        else {
            lambda[0] = 0;
            mu[N]     = 0;
            rhs[0]    = 0;
            rhs[N]    = 0;
        }

        // the trilinear system is ready to be solved:
        for (int i=1;i<=N;++i) {
            float multiple = mu[i]/diag[i-1];    // let's subtract proper multiple of above equation
            diag[i]-= multiple * lambda[i-1];
            rhs[i] -= multiple * rhs[i-1];
        }
        // now the back substitution (vector mu contains invalid values from now on):
        rhs[N] = rhs[N]/diag[N];
        for (int i=N-1;i>=0;--i)
            rhs[i] = (rhs[i]-lambda[i]*rhs[i+1])/diag[i];

        unsigned int i=1;
        float y=0.f;
        for (int x=m_rect.GetLeft(); x<=m_rect.GetRight() ; ++x) {
            if (splines) {
                if (i<points.size()-1 && points[i].x < x ) {
                    ++i;
                }
                if (points[0].x > x)
                    y = points[0].y;
                else
                    if (points[N].x < x)
                        y = points[N].y;
                    else
                        y = (rhs[i-1]*pow(points[i].x-x,3)+rhs[i]*pow(x-points[i-1].x,3)) / (6*h[i]) +
                            (points[i-1].y-rhs[i-1]*h[i]*h[i]/6.f) * (points[i].x-x)/h[i] +
                            (points[i].y  -rhs[i]  *h[i]*h[i]/6.f) * (x-points[i-1].x)/h[i];
                m_line_to_draw.push_back(y);
            }
            else {
                float x_math = screen_to_math(wxPoint(x,0)).m_x;
                if (i+2<=points.size() && m_buttons[i+1].get_pos().m_x-0.125 < x_math)
                    ++i;
                m_line_to_draw.push_back(math_to_screen(wxPoint2DDouble(x_math,m_buttons[i].get_pos().m_y)).y);
            }

            m_line_to_draw.back() = std::max(m_line_to_draw.back(), m_rect.GetTop()-1);
            m_line_to_draw.back() = std::min(m_line_to_draw.back(), m_rect.GetBottom()-1);
            m_total_volume += (m_rect.GetBottom() - m_line_to_draw.back()) * (visible_area.m_width / m_rect.GetWidth()) * (visible_area.m_height / m_rect.GetHeight());
        }
    }

    wxPostEvent(this->GetParent(), wxCommandEvent(EVT_WIPE_TOWER_CHART_CHANGED));
    Refresh();
}



std::vector<float> Chart::get_ramming_speed(float sampling) const {
    std::vector<float> speeds_out;
    
    const int number_of_samples = std::round( visible_area.m_width / sampling);
    if (number_of_samples>0) {
        const int dx = (m_line_to_draw.size()-1) / number_of_samples;
        for (int j=0;j<number_of_samples;++j) {
            float left =  screen_to_math(wxPoint(0,m_line_to_draw[j*dx])).m_y;
            float right = screen_to_math(wxPoint(0,m_line_to_draw[(j+1)*dx])).m_y;
            speeds_out.push_back((left+right)/2.f);            
        }
    }
    return speeds_out;
}


std::vector<std::pair<float,float>> Chart::get_buttons() const {
    std::vector<std::pair<float, float>> buttons_out;
    for (const auto& button : m_buttons)
        buttons_out.push_back(std::make_pair(float(button.get_pos().m_x),float(button.get_pos().m_y)));
    return buttons_out;
}
    
    
    

BEGIN_EVENT_TABLE(Chart, wxWindow)
EVT_MOTION(Chart::mouse_moved)
EVT_LEFT_DOWN(Chart::mouse_clicked)
EVT_LEFT_UP(Chart::mouse_released)
EVT_LEFT_DCLICK(Chart::mouse_double_clicked)
EVT_RIGHT_DOWN(Chart::mouse_right_button_clicked)
EVT_LEAVE_WINDOW(Chart::mouse_left_window)
EVT_PAINT(Chart::paint_event)
END_EVENT_TABLE()
