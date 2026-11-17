#include "CurveEditorPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <wx/cursor.h>
#include <wx/dc.h>
#include <wx/dcclient.h>
#include <wx/debug.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/recguard.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/window.h>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif
#ifdef __WXGTK__
#include <gdk/gdk.h>
#include <glib.h>
#include <gtk/gtk.h>
#endif
#include "libslic3r/CurveModel.hpp"

namespace Slic3r::GUI {
namespace {

#ifdef __WXMSW__
bool register_touchpad_input(bool enabled)
{
    // Thread registration is required for GetCurrentInputMessageSource to
    // distinguish a Precision Touchpad from a mouse. It is one switch for the
    // whole GUI thread, not per window. Older Windows versions lack the API
    // and retain the manual mode.
    using RegisterTouchpad = BOOL (WINAPI*)(BOOL);
    static const auto register_thread = reinterpret_cast<RegisterTouchpad>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "RegisterTouchpadCapableThread"));
    return register_thread && register_thread(enabled ? TRUE : FALSE);
}

bool has_precision_touchpad()
{
    UINT32 count = 0;
    if (!::GetPointerDevices(&count, nullptr) || count == 0) return false;
    std::vector<POINTER_DEVICE_INFO> devices(count);
    if (!::GetPointerDevices(&count, devices.data())) return false;
    devices.resize(count);
    return std::any_of(devices.begin(), devices.end(), [](const auto& device) {
        // POINTER_DEVICE_TYPE_TOUCH_PAD is gated by the SDK target version.
        return device.pointerDeviceType == static_cast<POINTER_DEVICE_TYPE>(4);
    });
}

// Panels that rely on the thread's registration. All of them live on the GUI thread.
int touchpad_input_users = 0;

bool acquire_automatic_touchpad_input()
{
    if (touchpad_input_users == 0) {
        if (!register_touchpad_input(true)) return false;
        // A legacy driver can still report a touchpad as a mouse. Preserve the
        // manual fallback unless Windows exposes an actual Precision Touchpad.
        if (!has_precision_touchpad()) {
            register_touchpad_input(false);
            return false;
        }
    }
    ++touchpad_input_users;
    return true;
}

void release_automatic_touchpad_input()
{
    // Only the last panel switches the thread back: the others still read the input source.
    if (--touchpad_input_users == 0) register_touchpad_input(false);
}
#endif

#ifndef __WXOSX__
constexpr int touch_gestures = wxTOUCH_PAN_GESTURES | wxTOUCH_ZOOM_GESTURE;
#endif

#ifdef __WXGTK__
// GTK's pan recognizers also fire for a left-button drag, which must not pan the view from an empty
// plot area as a finger on a touchscreen does. GTK reports a pan while it dispatches the pointer or
// touch event that moved it, so that event names the device. A pan event that arrives outside
// such a dispatch was sent by the program and follows no mouse.
bool gtk_pan_from_touchscreen()
{
    const auto event = std::unique_ptr<GdkEvent, decltype(&gdk_event_free)>(gtk_get_current_event(), gdk_event_free);
    if (!event) return true;
    auto* device = gdk_event_get_source_device(event.get());
    return device && gdk_device_get_source(device) == GDK_SOURCE_TOUCHSCREEN;
}

#if GTK_CHECK_VERSION(3, 20, 0)
bool gtk_has_touchpad()
{
    auto* display = gdk_display_get_default();
    auto* seat = display ? gdk_display_get_default_seat(display) : nullptr;
    if (!seat) return false;
    const auto devices = std::unique_ptr<GList, decltype(&g_list_free)>(
        gdk_seat_get_slaves(seat, GDK_SEAT_CAPABILITY_POINTER), g_list_free);
    for (GList* item = devices.get(); item; item = item->next) {
        if (gdk_device_get_source(GDK_DEVICE(item->data)) == GDK_SOURCE_TOUCHPAD) return true;
    }
    return false;
}
#endif
#endif

} // namespace

std::vector<CurveEditorPanel::AxisTick> CurveEditorPanel::axis_ticks(double minimum, double maximum, int pixels, const wxReadOnlyDC& dc, bool horizontal, int gap)
{
    const double range = maximum - minimum;
    if (!(range > 0.0) || !std::isfinite(range) || pixels <= 0)
        return {};
    const int spacing = horizontal ? dc.GetTextExtent("0000").x + gap : dc.GetCharHeight() + gap;
    const int intervals = std::max(1, pixels / std::max(1, spacing));
    const double target = range / intervals;
    double magnitude = std::pow(10.0, std::floor(std::log10(target)));
    if (magnitude == 0.0)
        magnitude = std::numeric_limits<double>::denorm_min();
    // Increase through 1, 2, 5 x 10^n until the actual labels fit.
    for (int attempt = 0; attempt < 12; ++attempt) {
        for (double multiplier : {1.0, 2.0, 5.0}) {
            const double step = magnitude * multiplier;
            if (step < target)
                continue;
            if (!std::isfinite(step))
                return {};
            std::vector<AxisTick> ticks;
            const double first = std::ceil(minimum / step);
            double previous_end = -std::numeric_limits<double>::infinity();
            bool fits = true;
            for (int i = 0; i <= intervals; ++i) {
                const double value = (first + i) * step;
                if (!std::isfinite(value) || value > maximum)
                    break;
                if (value < minimum)
                    continue;
                const double largest = std::max(std::abs(minimum), std::abs(maximum));
                const int precision = std::clamp(int(std::ceil(std::log10(largest) - std::log10(step))) + 2, 6, 17);
                const wxString label = value == 0.0 ? wxString("0") : wxString::Format("%.*g", precision, value);
                const wxSize extent = dc.GetTextExtent(label);
                const double position = (value - minimum) / range * pixels;
                const int size = horizontal ? extent.x : extent.y;
                // Keep horizontal labels inside the plot, including at either end.
                const double start = horizontal ?
                    std::clamp(position - size / 2.0, 0.0, double(std::max(0, pixels - size))) : position - size / 2.0;
                if ((horizontal && size > pixels) || start < previous_end + gap) {
                    fits = false;
                    break;
                }
                previous_end = start + size;
                ticks.push_back({value, label});
            }
            if (fits)
                return ticks;
        }
        magnitude *= 10.0;
    }
    return {};
}

int CurveEditorPanel::axis_label_width(double minimum, double maximum, double minimum_span, int pixels, const wxReadOnlyDC& dc, int gap)
{
    int width = 0;
    auto measure = [&](double lower, double upper) {
        for (const auto& tick : axis_ticks(lower, upper, pixels, dc, false, gap))
            width = std::max(width, dc.GetTextExtent(tick.label).x);
    };
    // Reserve room for the full range and maximum zoom near either limit and zero.
    // These samples include negative signs, decimal places and scientific notation.
    const double span = std::min(minimum_span, maximum - minimum);
    measure(minimum, maximum);
    measure(minimum, minimum + span);
    measure(maximum - span, maximum);
    if (minimum < 0.0 && maximum > 0.0) {
        measure(std::max(minimum, -span), 0.0);
        measure(0.0, std::min(maximum, span));
    }
    return width;
}

CurveEditorPanel::CurveEditorPanel(wxWindow* parent, const CurveEditorAppearance& appearance, const CurveViewLimits& view_limits)
    : wxPanel(parent), m_appearance(appearance), m_view_limits(view_limits), m_pan_timer(this)
{
    // Cocoa doesn't emit capture-lost events when another window becomes active.
    m_top_level = wxGetTopLevelParent(this);
    if (m_top_level)
        m_top_level->Bind(wxEVT_ACTIVATE, &CurveEditorPanel::on_activate, this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { flush_pan(); }, m_pan_timer.GetId());
    SetMinSize(FromDIP(wxSize(420, 180)));
    Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
        finish_drag();
        m_plot_rect.reset();
        SetMinSize(FromDIP(wxSize(420, 180)));
        event.Skip();
    });
    #ifndef __WXOSX__
        SetBackgroundStyle(wxBG_STYLE_PAINT);
    #endif
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_chart(); });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        if (m_middle_panning) finish_drag();
        m_hovered_point = -1;
        set_cursor(wxCURSOR_ARROW);
        Refresh();
        event.Skip();
    });
    auto begin_point_drag = [this](wxMouseEvent& event) {
        if (m_middle_panning || m_dragged_point >= 0) return;
        flush_pan();
        if (before_drag) before_drag();
        // A click anywhere on the graph takes the keyboard focus, which its shortcuts require.
        SetFocus();
        m_dragged_point = hit_test(event.GetPosition());
        if (m_dragged_point < 0) return;
        m_hovered_point = m_dragged_point;
        m_drag_start = event.GetPosition();
        m_drag_previous = m_drag_start;
        m_drag_x = m_x[m_dragged_point];
        m_drag_y = m_y[m_dragged_point];
        select_point(m_dragged_point);
        if (on_select) on_select(m_dragged_point);
        if (!HasCapture()) CaptureMouse();
    };
    Bind(wxEVT_LEFT_DOWN, begin_point_drag);
    Bind(wxEVT_LEFT_DCLICK, begin_point_drag);
    auto begin_middle_pan = [this](wxMouseEvent& event) {
        if (m_middle_panning || m_dragged_point >= 0) return;
        if (!on_pan || !chart_rect().Contains(event.GetPosition())) {
            event.Skip();
            return;
        }
        flush_pan();
        if (before_drag) before_drag();
        m_middle_panning = true;
        m_drag_previous = event.GetPosition();
        m_hovered_point = -1;
        SetFocus();
        set_cursor(wxCURSOR_SIZING);
        if (!HasCapture()) CaptureMouse();
        Refresh();
    };
    Bind(wxEVT_MIDDLE_DOWN, begin_middle_pan);
    Bind(wxEVT_MIDDLE_DCLICK, begin_middle_pan);
    Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        if (m_middle_panning) {
            if (event.MiddleIsDown()) {
                const wxPoint delta = event.GetPosition() - m_drag_previous;
                m_drag_previous = event.GetPosition();
                if (delta != wxPoint(0, 0)) queue_pan(-double(delta.x), double(delta.y));
            } else {
                flush_pan();
                finish_drag();
            }
        } else if (m_dragged_point >= 0) {
            if (event.LeftIsDown()) drag_point(event.GetPosition());
            else finish_drag();
        } else {
            const int hovered = hit_test(event.GetPosition());
            if (hovered != m_hovered_point) {
                m_hovered_point = hovered;
                Refresh();
            }
            set_cursor(hovered >= 0 ? wxCURSOR_HAND : wxCURSOR_ARROW);
        }
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        m_hovered_point = -1;
        if (m_dragged_point < 0 && !m_middle_panning) set_cursor(wxCURSOR_ARROW);
        Refresh();
    });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
        if (m_middle_panning) return;
        finish_drag();
        m_hovered_point = hit_test(event.GetPosition());
        set_cursor(m_hovered_point >= 0 ? wxCURSOR_HAND : wxCURSOR_ARROW);
        Refresh();
    });
    Bind(wxEVT_MIDDLE_UP, [this](wxMouseEvent& event) {
        if (!m_middle_panning) return;
        const wxPoint delta = event.GetPosition() - m_drag_previous;
        queue_pan(-double(delta.x), double(delta.y));
        flush_pan();
        finish_drag();
    });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { finish_drag(); });
    Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
        const bool zoom = event.ControlDown();
        if (event.AltDown() || event.MetaDown() || (zoom ? !on_zoom : !on_pan)) {
            event.Skip();
            return;
        }
        if (m_dragged_point >= 0 || m_middle_panning || event.GetWheelDelta() <= 0) return;
        // A native gesture already owns this input sequence; don't also apply
        // its compatibility wheel messages.
        if (m_pan_gesture_active || m_zoom_gesture_active) return;
        const auto input = scroll_input(event.GetWheelAxis() == wxMOUSE_WHEEL_HORIZONTAL);
        const bool vertical = input.touchpad && !zoom ?
            event.GetWheelAxis() == wxMOUSE_WHEEL_VERTICAL : event.ShiftDown();
        // Native precise input can retain a fraction that wxMouseEvent's
        // integer wheel rotation has rounded away.
        double steps = input.pixels ? *input.pixels / FromDIP(30) :
                                      double(event.GetWheelRotation()) / event.GetWheelDelta();
        if (!std::isfinite(steps) || steps == 0.0) return;
        if (zoom) {
            if (input.touchpad && !event.ShiftDown()) {
                zoom_view(steps, false, event.GetPosition());
                zoom_view(steps, true, event.GetPosition());
                return;
            }
            zoom_view(steps, vertical, event.GetPosition());
            return;
        }
        // Mouse mode maps wheel-up to lower X; touchpad input follows native axes.
        if (!vertical && event.GetWheelAxis() != wxMOUSE_WHEEL_HORIZONTAL) steps = -steps;
        // Equal screen distances on both axes keep a circular gesture circular,
        // regardless of the plot's aspect ratio or the current numeric ranges.
        double pixels = steps * FromDIP(30);
        if (!input.touchpad) {
            // Keep the existing mouse sensitivity: one notch moves 10% of
            // the visible range. Gesture input instead uses screen distances.
            const wxRect plot = chart_rect();
            pixels = steps * 0.1 * (vertical ? plot.height : plot.width);
        }
        queue_pan(vertical ? 0.0 : pixels, vertical ? pixels : 0.0);
    });
    Bind(wxEVT_MAGNIFY, [this](wxMouseEvent& event) {
        if ((!m_touchpad_controls && !m_automatic_touchpad_controls) || !on_zoom) { event.Skip(); return; }
        if (m_dragged_point < 0 && !m_middle_panning) pinch_zoom(1.0 + event.GetMagnification(), event.GetPosition());
    });
    Bind(wxEVT_GESTURE_PAN, [this](wxPanGestureEvent& event) {
        if ((!m_touchpad_controls && !m_automatic_touchpad_controls) || !on_pan) { event.Skip(); return; }
        // A gesture runs from an accepted start to its end. One that starts during a mouse drag
        // is ignored as a whole, so it cannot take over halfway when the drag ends. So is one
        // that GTK recognizes in a drag of anything but a finger on a touchscreen.
        const bool mouse_drag = m_dragged_point >= 0 || m_middle_panning;
        if (event.IsGestureStart()) {
            m_pan_gesture_active = !mouse_drag;
#ifdef __WXGTK__
            m_pan_gesture_active = m_pan_gesture_active && gtk_pan_from_touchscreen();
#endif
            if (m_pan_gesture_active) cancel_pan();
        }
        if (m_pan_gesture_active && !mouse_drag && !m_zoom_gesture_active) {
            const auto delta = event.GetDelta();
            queue_pan(-double(delta.x), double(delta.y));
        }
        if (event.IsGestureEnd()) m_pan_gesture_active = false;
    });
    Bind(wxEVT_GESTURE_ZOOM, [this](wxZoomGestureEvent& event) {
        if ((!m_touchpad_controls && !m_automatic_touchpad_controls) || !on_zoom) { event.Skip(); return; }
        // As for the pan gesture: only an accepted start begins a zoom.
        const bool mouse_drag = m_dragged_point >= 0 || m_middle_panning;
        if (event.IsGestureStart()) {
            m_gesture_zoom_factor = 1.0;
            m_zoom_gesture_active = !mouse_drag;
            if (m_zoom_gesture_active) cancel_pan();
        }
        const double factor = event.GetZoomFactor();
        if (m_zoom_gesture_active && std::isfinite(factor) && factor > 0.0) {
            // Follow the cumulative factor during a mouse drag too, so the view does not jump after it.
            if (!mouse_drag) pinch_zoom(factor / m_gesture_zoom_factor, event.GetPosition());
            m_gesture_zoom_factor = factor;
        }
        if (event.IsGestureEnd()) {
            m_gesture_zoom_factor = 1.0;
            m_zoom_gesture_active = false;
        }
    });
#ifdef __WXMSW__
    m_automatic_touchpad_controls = acquire_automatic_touchpad_input();
#elif defined(__WXOSX__)
    m_automatic_touchpad_controls = true;
#elif defined(__WXGTK__)
#if GTK_CHECK_VERSION(3, 20, 0)
    m_automatic_touchpad_controls = gtk_has_touchpad();
#endif
#endif
#ifndef __WXOSX__
    if (m_automatic_touchpad_controls)
        EnableTouchEvents(touch_gestures);
#endif
}

CurveEditorPanel::~CurveEditorPanel()
{
    if (m_top_level)
        m_top_level->Unbind(wxEVT_ACTIVATE, &CurveEditorPanel::on_activate, this);
    // The owner may be partly destroyed by now.
    after_drag = {};
    finish_drag();
#ifdef __WXMSW__
    if (m_automatic_touchpad_controls) release_automatic_touchpad_input();
#endif
}

void CurveEditorPanel::on_activate(wxActivateEvent& event)
{
    event.Skip();
    if (!event.GetActive()) {
        finish_drag();
        m_hovered_point = -1;
        m_pan_gesture_active = m_zoom_gesture_active = false;
        m_gesture_zoom_factor = 1.0;
    }
}

CurveEditorPanel::NativeScroll CurveEditorPanel::scroll_input([[maybe_unused]] bool horizontal) const
{
    // wxMouseEvent carries an axis but no device identity. A vertical packet pans X
    // for the existing mouse scheme and Y for a touchpad; only native metadata can
    // keep both mappings without a manual mode switch. Magnification already uses
    // wxEVT_MAGNIFY where the backend supplies it.
#ifdef __WXMSW__
    INPUT_MESSAGE_SOURCE source{};
    if (::GetCurrentInputMessageSource(&source) && source.deviceType != IMDT_UNAVAILABLE) {
        // IMDT_TOUCHPAD is absent from headers targeting Windows 7.
        if (source.deviceType == static_cast<INPUT_MESSAGE_DEVICE_TYPE>(0x10)) return {true, {}};
        if (m_automatic_touchpad_controls) return {false, {}};
    }
#elif defined(__WXOSX__)
    if (const auto input = mac_scroll_input(horizontal)) return *input;
#elif defined(__WXGTK__)
#if GTK_CHECK_VERSION(3, 20, 0)
    const auto event = std::unique_ptr<GdkEvent, decltype(&gdk_event_free)>(gtk_get_current_event(), gdk_event_free);
    if (event && event->type == GDK_SCROLL) {
        auto* device = gdk_event_get_source_device(event.get());
        bool touchpad = m_touchpad_controls;
        if (device) {
            if (gdk_device_get_source(device) == GDK_SOURCE_TOUCHPAD) touchpad = true;
            else if (m_automatic_touchpad_controls) touchpad = false;
        }
        double dx = 0.0, dy = 0.0;
        if (touchpad && gdk_event_get_scroll_deltas(event.get(), &dx, &dy))
            return {true, (horizontal ? dx : -dy) * FromDIP(30)};
        return {touchpad, {}};
    }
#endif
#endif
    return {m_touchpad_controls, {}};
}

void CurveEditorPanel::queue_pan(double pixels_x, double pixels_y)
{
    m_pending_pan_x += pixels_x;
    m_pending_pan_y += pixels_y;
    // Start once rather than restarting: continuous input must not postpone
    // every frame. Horizontal and vertical packets share a single update.
    if (!m_pan_timer.IsRunning()) m_pan_timer.StartOnce(16);
}

void CurveEditorPanel::flush_pan()
{
    wxRecursionGuard guard(m_pan_depth);
    if (guard.IsInside()) return;
    const double pixels_x = m_pending_pan_x;
    const double pixels_y = m_pending_pan_y;
    cancel_pan();
    if (!on_pan || (pixels_x == 0.0 && pixels_y == 0.0)) return;
    const wxRect plot = chart_rect();
    on_pan(pixels_x / plot.width * (m_view.max_x - m_view.min_x),
           pixels_y / plot.height * (m_view.max_y - m_view.min_y));
}

void CurveEditorPanel::cancel_pan()
{
    m_pan_timer.Stop();
    m_pending_pan_x = m_pending_pan_y = 0.0;
}

void CurveEditorPanel::set_touchpad_controls(bool enabled)
{
    if (m_touchpad_controls == enabled) return;
    finish_drag();
    m_touchpad_controls = enabled;
    m_gesture_zoom_factor = 1.0;
    m_pan_gesture_active = m_zoom_gesture_active = false;
    // Cocoa supplies scrolling and incremental magnification directly. Adding
    // its gesture recognizers could intercept point dragging or duplicate zoom.
#ifndef __WXOSX__
    EnableTouchEvents((enabled || m_automatic_touchpad_controls) ? touch_gestures : wxTOUCH_NONE);
#endif
}

void CurveEditorPanel::pinch_zoom(double factor, const wxPoint& position)
{
    if (!std::isfinite(factor) || factor <= 0.0 || factor == 1.0) return;
    const double steps = std::log(factor) / std::log(1.1);
    zoom_view(steps, false, position);
    zoom_view(steps, true, position);
}

void CurveEditorPanel::zoom_view(double steps, bool vertical, const wxPoint& position)
{
    flush_pan();
    wxRecursionGuard guard(m_zoom_depth);
    if (guard.IsInside()) return;
    const wxRect plot = chart_rect();
    const double anchor = vertical ? 1.0 - double(position.y - plot.y) / plot.height :
                                         double(position.x - plot.x) / plot.width;
    on_zoom(steps, vertical, std::clamp(anchor, 0.0, 1.0));
}

void CurveEditorPanel::set_data(const std::vector<double>& x, const std::vector<double>& y,
                               const std::vector<wxString>& tooltips, Interpolator interpolate)
{
    wxCHECK_RET(x.size() == y.size() && x.size() == tooltips.size(), "Curve points and tooltips must have equal sizes");
    if (x.size() != m_x.size()) finish_drag();
    m_x = x;
    m_y = y;
    m_tooltips = tooltips;
    m_interpolate = std::move(interpolate);
    if (m_dragged_point < 0 && !m_middle_panning) {
        m_hovered_point = -1;
        set_cursor(wxCURSOR_ARROW);
    }
    Refresh();
}

void CurveEditorPanel::set_view(const CurveEditorView& view)
{
    wxCHECK_RET(CurveModel::valid_view(view), "Invalid curve viewport");
    // Updating the viewport from a pan callback must retain middle-button capture.
    if (!m_middle_panning || m_pan_depth == 0) finish_drag();
    m_view = view;
    m_hovered_point = -1;
    Refresh();
}

void CurveEditorPanel::select_point(int row)
{
    m_selected_point = row;
    Refresh();
}

void CurveEditorPanel::set_view_limits(const CurveViewLimits& limits)
{
    m_plot_rect.reset();
    if (m_middle_panning) finish_drag();
    else cancel_pan();
    m_view_limits = limits;
    Refresh();
}

void CurveEditorPanel::finish_drag()
{
    cancel_pan();
    const bool point_dragged = m_dragged_point >= 0;
    m_dragged_point = -1;
    m_middle_panning = false;
    if (HasCapture()) ReleaseMouse();
    set_cursor(wxCURSOR_ARROW);
    Refresh();
    if (point_dragged && after_drag) after_drag();
}

void CurveEditorPanel::set_cursor(wxStockCursor cursor)
{
    // Motion events arrive continuously; build a native cursor only for a change.
    if (m_cursor == cursor) return;
    m_cursor = cursor;
    SetCursor(wxCursor(cursor));
}

void CurveEditorPanel::drag_point(const wxPoint& position)
{
    if (position == m_drag_previous) return;
    m_drag_previous = position;
    const wxRect plot = chart_rect();
    const wxPoint delta = position - m_drag_start;
    const double x_span = m_view.max_x - m_view.min_x;
    const double y_span = m_view.max_y - m_view.min_y;
    const double x = delta.x == 0 ? m_drag_x : m_view.min_x +
        std::clamp((m_drag_x - m_view.min_x) / x_span + double(delta.x) / plot.width, 0.0, 1.0) * x_span;
    const double y = delta.y == 0 ? m_drag_y : m_view.min_y +
        std::clamp((m_drag_y - m_view.min_y) / y_span - double(delta.y) / plot.height, 0.0, 1.0) * y_span;
    if (on_move) on_move(m_dragged_point, x, y);
}

bool CurveEditorPanel::point_visible(double x, double y) const
{
    return x >= m_view.min_x && x <= m_view.max_x && y >= m_view.min_y && y <= m_view.max_y;
}

wxRect CurveEditorPanel::chart_rect(const wxReadOnlyDC* paint_dc) const
{
    const wxSize size = GetClientSize();
    const wxSize dpi = GetDPI();
    const wxFont font = GetFont();
    if (m_plot_rect && m_plot_size == size && m_plot_dpi == dpi && m_plot_font == font) return *m_plot_rect;
    std::optional<wxInfoDC> info_dc;
    if (!paint_dc) {
        info_dc.emplace(const_cast<CurveEditorPanel*>(this));
        info_dc->SetFont(font);
        paint_dc = &*info_dc;
    }
    const wxReadOnlyDC& dc = *paint_dc;
    const int gap = FromDIP(8);
    const int top = dc.GetCharHeight() + gap + dc.GetCharHeight() / 2;
    const int bottom = 2 * dc.GetCharHeight() + 3 * gap;
    const int height = std::max(1, size.y - top - bottom);
    const auto& bounds = m_view_limits.bounds;
    const int label_width = std::max(dc.GetTextExtent(m_appearance.reference_label).x,
        axis_label_width(bounds.min_y, bounds.max_y, m_view_limits.minimum_span, height, dc, gap));
    // Leave room for a hovered point's marker at the right edge.
    const int right = gap;
    // Tick length, label spacing and a small outer padding.
    const int left = label_width + 3 * FromDIP(2);
    m_plot_size = size;
    m_plot_dpi = dpi;
    m_plot_font = font;
    m_plot_rect = wxRect(left, top, std::max(1, size.x - left - right), height);
    return *m_plot_rect;
}

wxPoint CurveEditorPanel::chart_point(double x_value, double y_value, const wxRect& plot) const
{
    // Bound off-screen coordinates before converting to integers; drawing is clipped to the plot.
    const double x = std::clamp((x_value - m_view.min_x) / (m_view.max_x - m_view.min_x), -1.0, 2.0);
    const double y = std::clamp((y_value - m_view.min_y) / (m_view.max_y - m_view.min_y), -1.0, 2.0);
    return wxPoint(plot.x + int(x * plot.width), plot.y + plot.height - int(y * plot.height));
}

int CurveEditorPanel::hit_test(const wxPoint& position) const
{
    const wxRect plot = chart_rect();
    int closest = -1;
    double distance = double(FromDIP(9)) * FromDIP(9);
    for (size_t i = 0; i < m_x.size(); ++i) {
        if (!point_visible(m_x[i], m_y[i]))
            continue;
        const wxPoint delta = position - chart_point(m_x[i], m_y[i], plot);
        const double squared = double(delta.x) * delta.x + double(delta.y) * delta.y;
        if (squared < distance) {
            closest = int(i);
            distance = squared;
        }
    }
    return closest;
}

} // namespace Slic3r::GUI
