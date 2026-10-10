#include <catch_amalgamated.hpp>

#include <cmath>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <wx/app.h>
#include <wx/event.h>
#include <wx/evtloop.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/gdicmn.h>
#include <wx/init.h>
#include <wx/string.h>
#include <wx/strconv.h>
#include <wx/stopwatch.h>
#include <wx/utils.h>

#include "libslic3r/CurveModel.hpp"
#include "slic3r/GUI/CurveEditorPanel.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

namespace {

class InputApp : public wxApp
{
public:
    bool OnInit() override { return true; }
};

class WxRuntime
{
public:
    WxRuntime()
    {
        int argc     = 0;
        char* argv[] = {nullptr};
        if (!wxEntryStart(argc, argv) || !wxTheApp->CallOnInit())
            throw std::runtime_error("Native GUI checks require a display (use xvfb-run on Linux).");
    }
    ~WxRuntime()
    {
        wxTheApp->OnExit();
        wxEntryCleanup();
    }
};

class InputModel : public CurveModel
{
public:
    size_t maximum_point_count() const override { return 50; }
    const char* validate_points(const std::vector<double>&, const std::vector<double>&, int&) const override { return nullptr; }
    CurveView fitted_view(const std::vector<double>&, const std::vector<double>&) const override { return {20, 40, .2, .8}; }
    CurveViewLimits view_limits() const override { return {{0, 100, -1, 2}, 1, .01, .001}; }
    CurveView drag_bounds(int, const std::vector<double>&, const std::vector<double>&, const CurveView& view) const override
    { return view; }
    Rows default_rows() const override { return {{"20", "0.2"}, {"40", "0.8"}}; }
    Rows seed_rows() const override { return default_rows(); }
};

struct Navigation
{
    bool vertical;
    double value;
    double anchor;
};

class Fixture
{
public:
    Fixture()
    {
        static WxRuntime runtime;
        frame = std::make_unique<wxFrame>(nullptr, wxID_ANY, "Hidden curve editor check");
        panel = std::make_unique<CurveEditorPanel>(frame.get(), CurveEditorAppearance{"Length", "Factor", 1.0, "1.0", 0, 1},
                                                   model.view_limits());
        panel->SetSize(700, 400);
        reset();
        panel->on_pan = [this](double dx, double dy) {
            ++pan_frames;
            if (dx != 0)
                pans.push_back({false, dx, 0});
            if (dy != 0)
                pans.push_back({true, dy, 0});
            panel->set_view(model.panned_view(panel->view(), dx, dy));
        };
        panel->on_zoom = [this](double value, bool vertical, double anchor) {
            zooms.push_back({vertical, value, anchor});
            panel->set_view(model.zoomed_view(panel->view(), std::pow(1.1, -value), anchor, vertical));
        };
    }
    ~Fixture()
    {
        panel->finish_drag();
        panel->on_pan  = {};
        panel->on_zoom = {};
    }
    void reset()
    {
        panel->set_view({20, 40, .2, .8});
        pans.clear();
        zooms.clear();
        pan_frames = 0;
    }
    void drain(bool wait_for_pan = false)
    {
        // Exercise the real one-shot timer, including its native event dispatch.
        wxEventLoop loop;
        wxEventLoopActivator activate(&loop);
        wxStopWatch elapsed;
        const int previous_frames = pan_frames;
        while (elapsed.Time() < (wait_for_pan ? 1000 : 80)) {
            loop.YieldFor(wxEVT_CATEGORY_ALL);
            if (wait_for_pan && pan_frames != previous_frames)
                return;
            wxMilliSleep(1);
        }
        loop.YieldFor(wxEVT_CATEGORY_ALL);
    }
    void wheel(
        int rotation, bool horizontal = false, bool control = false, bool shift = false, wxPoint position = {300, 160}, bool flush = true)
    {
        wxMouseEvent event(wxEVT_MOUSEWHEEL);
        event.m_wheelRotation = rotation;
        event.m_wheelDelta    = 120;
        event.m_wheelAxis     = horizontal ? wxMOUSE_WHEEL_HORIZONTAL : wxMOUSE_WHEEL_VERTICAL;
        event.SetControlDown(control);
        event.SetShiftDown(shift);
        event.SetPosition(position);
        panel->GetEventHandler()->ProcessEvent(event);
        if (flush && !control)
            drain(!panel->HasCapture());
    }
    void magnify(double value)
    {
        wxMouseEvent event(wxEVT_MAGNIFY);
        event.m_magnification = float(value);
        event.SetPosition({300, 160});
        panel->GetEventHandler()->ProcessEvent(event);
    }
    void mouse(wxEventType type, wxPoint position, bool middle = false, bool left = false)
    {
        wxMouseEvent event(type);
        event.SetPosition(position);
        event.SetMiddleDown(middle);
        event.SetLeftDown(left);
        panel->GetEventHandler()->ProcessEvent(event);
    }
    void pinch(double factor, bool start = false, bool end = false)
    {
        wxZoomGestureEvent event;
        event.SetZoomFactor(factor);
        event.SetGestureStart(start);
        event.SetGestureEnd(end);
        event.SetPosition({300, 160});
        panel->GetEventHandler()->ProcessEvent(event);
    }
    wxRect plot_rect()
    {
        const auto callback = panel->on_zoom;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        panel->on_zoom = [&](double, bool vertical, double anchor) {
            if (vertical)
                y1 = anchor;
            else
                x1 = anchor;
        };
        wheel(1, false, true, false, {100, 100});
        wheel(1, false, true, true, {100, 100});
        panel->on_zoom = [&](double, bool vertical, double anchor) {
            if (vertical)
                y2 = anchor;
            else
                x2 = anchor;
        };
        wheel(1, false, true, false, {500, 300});
        wheel(1, false, true, true, {500, 300});
        panel->on_zoom   = callback;
        const int width  = int(std::lround(400 / (x2 - x1)));
        const int height = int(std::lround(200 / (y1 - y2)));
        return {int(std::lround(100 - x1 * width)), int(std::lround(100 - (1 - y1) * height)), width, height};
    }
    wxPoint middle_point()
    {
        const auto plot = plot_rect();
        return plot.GetPosition() + wxPoint(plot.width / 2, plot.height / 2);
    }
    InputModel model;
    std::unique_ptr<wxFrame> frame;
    std::unique_ptr<CurveEditorPanel> panel;
    std::vector<Navigation> pans, zooms;
    int pan_frames = 0;
};

} // namespace

wxIMPLEMENT_APP_NO_MAIN(InputApp);

TEST_CASE("Middle-button dragging follows the pointer on both axes without editing points", "[CurveEditorGUI][RequiresDisplay]")
{
    Fixture f;
    f.panel->set_data({20, 30, 40}, {.2, .5, .8}, {"first", "middle", "last"}, [](double) { return .5; });
    const auto plot = f.plot_rect();
    const auto start = f.middle_point();
    const auto before = f.panel->view();
    int selected = 0, moved = 0;
    f.panel->on_select = [&](int) { ++selected; };
    f.panel->on_move = [&](int, double, double) { ++moved; };
    f.mouse(wxEVT_MIDDLE_DOWN, start, true);
    REQUIRE(f.panel->HasCapture());
    f.mouse(wxEVT_MOTION, start, true);
    f.drain();
    CHECK(f.pan_frames == 0);
    f.mouse(wxEVT_MOTION, start + wxPoint(30, 20), true);
    f.drain(true);
    REQUIRE(f.pan_frames == 1);
    CHECK(f.panel->HasCapture());
    CHECK_THAT(f.panel->view().min_x, WithinAbs(before.min_x - 30.0 / plot.width * 20, 1e-12));
    CHECK_THAT(f.panel->view().min_y, WithinAbs(before.min_y + 20.0 / plot.height * .6, 1e-12));
    f.mouse(wxEVT_MOTION, start + wxPoint(50, 10), true);
    f.drain(true);
    REQUIRE(f.pan_frames == 2);
    CHECK(f.panel->HasCapture());
    // Include the final release position even if no motion packet preceded it.
    f.mouse(wxEVT_MIDDLE_UP, start + wxPoint(55, 7));
    CHECK_FALSE(f.panel->HasCapture());
    REQUIRE(f.pan_frames == 3);
    CHECK_THAT(f.panel->view().min_x, WithinAbs(before.min_x - 55.0 / plot.width * 20, 1e-12));
    CHECK_THAT(f.panel->view().min_y, WithinAbs(before.min_y + 7.0 / plot.height * .6, 1e-12));
    CHECK_THAT(f.panel->view().max_x - f.panel->view().min_x, WithinAbs(20, 1e-12));
    CHECK_THAT(f.panel->view().max_y - f.panel->view().min_y, WithinAbs(.6, 1e-12));
    CHECK(selected == 0);
    CHECK(moved == 0);
    CHECK(f.zooms.empty());
    f.drain();
    CHECK(f.pan_frames == 3);
}

TEST_CASE("Middle-button navigation cancels capture and pending motion when interrupted", "[CurveEditorGUI][RequiresDisplay][Regression]")
{
    const int interruption = GENERATE(0, 1, 2, 3, 4);
    Fixture f;
    const auto start = f.middle_point();
    f.mouse(wxEVT_MIDDLE_DOWN, start, true);
    f.mouse(wxEVT_MOTION, start + wxPoint(30, 20), true);
    REQUIRE(f.panel->HasCapture());
    switch (interruption) {
    case 0: {
        wxMouseCaptureLostEvent lost;
        f.panel->GetEventHandler()->ProcessEvent(lost);
        break;
    }
    case 1: f.panel->finish_drag(); break;
    case 2: f.panel->set_view({20, 40, .2, .8}); break;
    case 3: f.panel->set_view_limits(f.model.view_limits()); break;
    case 4: f.panel->SetSize(800, 450); break;
    }
    CHECK_FALSE(f.panel->HasCapture());
    f.mouse(wxEVT_MOTION, start + wxPoint(40, 30), true);
    f.drain();
    CHECK(f.pans.empty());
    CHECK_THAT(f.panel->view().min_x, WithinAbs(20, 1e-12));
    CHECK_THAT(f.panel->view().min_y, WithinAbs(.2, 1e-12));
    f.wheel(120);
    CHECK(f.pan_frames == 1);
}

TEST_CASE("Middle-button dragging stops when a motion event reports the button released", "[CurveEditorGUI][RequiresDisplay][Regression]")
{
    Fixture f;
    const auto start = f.middle_point();
    f.mouse(wxEVT_MIDDLE_DOWN, start, true);
    f.mouse(wxEVT_MOTION, start + wxPoint(30, 20), true);
    f.mouse(wxEVT_MOTION, start + wxPoint(40, 30));
    REQUIRE(f.pan_frames == 1);
    CHECK_FALSE(f.panel->HasCapture());
    f.mouse(wxEVT_MOTION, start + wxPoint(60, 60));
    f.mouse(wxEVT_MIDDLE_UP, start + wxPoint(60, 60));
    f.drain();
    CHECK(f.pan_frames == 1);
}

TEST_CASE("Captured middle-button dragging stays within viewport limits outside the panel", "[CurveEditorGUI][RequiresDisplay]")
{
    Fixture f;
    const auto start = f.middle_point();
    f.mouse(wxEVT_MIDDLE_DOWN, start, true);
    f.mouse(wxEVT_MOTION, start + wxPoint(100000, 100000), true);
    f.drain(true);
    CHECK(f.panel->HasCapture());
    CHECK_THAT(f.panel->view().min_x, WithinAbs(0, 1e-12));
    CHECK_THAT(f.panel->view().max_y, WithinAbs(2, 1e-12));
    f.mouse(wxEVT_MIDDLE_UP, start - wxPoint(100000, 100000));
    CHECK_FALSE(f.panel->HasCapture());
    CHECK_THAT(f.panel->view().max_x, WithinAbs(100, 1e-12));
    CHECK_THAT(f.panel->view().min_y, WithinAbs(-1, 1e-12));
}

TEST_CASE("Middle-button dragging keeps other mouse and touchpad actions from taking over", "[CurveEditorGUI][RequiresDisplay][Regression]")
{
    Fixture f;
    f.panel->set_touchpad_controls(true);
    const auto start = f.middle_point();
    f.mouse(wxEVT_MIDDLE_DOWN, {0, 0}, true);
    CHECK_FALSE(f.panel->HasCapture());
    f.mouse(wxEVT_MOTION, start, true);
    f.drain();
    CHECK(f.pans.empty());
    f.mouse(wxEVT_MIDDLE_DOWN, start, true);
    REQUIRE(f.panel->HasCapture());
    f.mouse(wxEVT_LEFT_DOWN, start, true, true);
    f.mouse(wxEVT_LEFT_UP, start, true);
    f.wheel(120, false, false, false, start, false);
    f.wheel(120, false, true, false, start, false);
    f.magnify(.25);
    f.pinch(1, true);
    f.pinch(1.25, false, true);
    wxPanGestureEvent pan;
    pan.SetDelta({20, 10});
    pan.SetGestureStart(true);
    f.panel->GetEventHandler()->ProcessEvent(pan);
    pan.SetGestureStart(false);
    pan.SetGestureEnd(true);
    f.panel->GetEventHandler()->ProcessEvent(pan);
    f.drain();
    CHECK(f.panel->HasCapture());
    CHECK(f.pans.empty());
    CHECK(f.zooms.empty());
    f.mouse(wxEVT_MOTION, start + wxPoint(20, 10), true);
    f.mouse(wxEVT_MIDDLE_UP, start + wxPoint(20, 10));
    REQUIRE(f.pan_frames == 1);
    f.wheel(120);
    CHECK(f.pan_frames == 2);
}

TEST_CASE("Mouse navigation retains its axis modifiers when touchpad mode is disabled", "[CurveEditorGUI][RequiresDisplay]")
{
    Fixture f;
    CAPTURE(f.panel->has_automatic_touchpad_controls());
    f.wheel(120);
    REQUIRE(f.pans.size() == 1);
    CHECK_FALSE(f.pans[0].vertical);
    CHECK(f.pans[0].value < 0);
    CHECK_THAT(f.pans[0].value, WithinAbs(-2.0, 1e-12));
    f.wheel(120, false, false, true);
    REQUIRE(f.pans.size() == 2);
    CHECK(f.pans[1].vertical);
    CHECK(f.pans[1].value > 0);
    CHECK_THAT(f.pans[1].value, WithinAbs(.06, 1e-12));
    f.wheel(120, false, true);
    f.wheel(120, false, true, true);
    REQUIRE(f.zooms.size() == 2);
    CHECK_FALSE(f.zooms[0].vertical);
    CHECK(f.zooms[1].vertical);
}

TEST_CASE("Touchpad scrolling routes both native axes and preserves fractional movement", "[CurveEditorGUI][RequiresDisplay]")
{
    Fixture f;
    f.panel->set_touchpad_controls(true);
    f.wheel(1);
    REQUIRE(f.pans.size() == 1);
    const double fine = f.pans.front().value;
    f.wheel(120);
    f.wheel(40, true);
    REQUIRE(f.pans.size() == 3);
    CHECK(f.pans[0].vertical);
    CHECK_FALSE(f.pans[2].vertical);
    CHECK_THAT(fine * 120, WithinAbs(f.pans[1].value, 1e-12));
    CHECK(f.pans[2].value > 0);
}

TEST_CASE("Paired touchpad axis packets apply one frame with equal screen distances", "[CurveEditorGUI][RequiresDisplay][Regression]")
{
    Fixture f;
    f.panel->set_touchpad_controls(true);
    const auto plot   = f.plot_rect();
    const auto before = f.panel->view();
    f.wheel(40, true, false, false, {300, 160}, false);
    f.wheel(40, false, false, false, {300, 160}, false);
    CHECK(f.pans.empty());
    f.drain(true);
    REQUIRE(f.pan_frames == 1);
    const auto after      = f.panel->view();
    const double pixels_x = (after.min_x - before.min_x) / (before.max_x - before.min_x) * plot.width;
    const double pixels_y = (after.min_y - before.min_y) / (before.max_y - before.min_y) * plot.height;
    CHECK(pixels_x > 0);
    CHECK_THAT(pixels_x, WithinAbs(pixels_y, 1e-10));
}

TEST_CASE("Circular touchpad navigation returns to its original view without axis drift", "[CurveEditorGUI][RequiresDisplay][Regression]")
{
    Fixture f;
    f.panel->set_touchpad_controls(true);
    const auto before                  = f.panel->view();
    const std::array<wxPoint, 8> steps = {{{120, 0}, {120, 120}, {0, 120}, {-120, 120}, {-120, 0}, {-120, -120}, {0, -120}, {120, -120}}};
    for (const auto& step : steps) {
        f.wheel(step.x, true, false, false, {300, 160}, false);
        f.wheel(step.y, false, false, false, {300, 160}, false);
        f.drain(true);
    }
    CHECK(f.pan_frames == steps.size());
    CHECK_THAT(f.panel->view().min_x, WithinAbs(before.min_x, 1e-12));
    CHECK_THAT(f.panel->view().max_x, WithinAbs(before.max_x, 1e-12));
    CHECK_THAT(f.panel->view().min_y, WithinAbs(before.min_y, 1e-12));
    CHECK_THAT(f.panel->view().max_y, WithinAbs(before.max_y, 1e-12));
}

TEST_CASE("Native pan gestures suppress compatibility wheel packets until the gesture ends", "[CurveEditorGUI][RequiresDisplay][Regression]")
{
    Fixture f;
    f.panel->set_touchpad_controls(true);
    wxPanGestureEvent pan;
    pan.SetGestureStart(true);
    pan.SetDelta({20, 10});
    f.panel->GetEventHandler()->ProcessEvent(pan);
    f.wheel(120, true);
    REQUIRE(f.pan_frames == 1);
    REQUIRE(f.pans.size() == 2);
    CHECK(f.pans[0].value < 0);
    pan.SetGestureStart(false);
    pan.SetGestureEnd(true);
    pan.SetDelta({0, 0});
    f.panel->GetEventHandler()->ProcessEvent(pan);
    f.wheel(120, true);
    CHECK(f.pan_frames == 2);
    CHECK(f.pans.back().value > 0);
}

TEST_CASE("Native pinch gestures suppress compatibility zoom packets until the gesture ends",
          "[CurveEditorGUI][RequiresDisplay][Regression]")
{
    Fixture f;
    f.panel->set_touchpad_controls(true);
    f.pinch(1, true);
    f.wheel(120, false, true);
    CHECK(f.zooms.empty());
    f.pinch(1.1, false, true);
    REQUIRE(f.zooms.size() == 2);
    f.wheel(120, false, true);
    CHECK(f.zooms.size() == 4);
}

TEST_CASE("Resetting the graph view discards a queued pan", "[CurveEditorGUI][RequiresDisplay][Regression]")
{
    Fixture f;
    f.wheel(120, false, false, false, {300, 160}, false);
    f.panel->set_view({10, 50, 0, 1});
    f.drain();
    CHECK(f.pans.empty());
    CHECK_THAT(f.panel->view().min_x, WithinAbs(10, 1e-12));
    CHECK_THAT(f.panel->view().max_y, WithinAbs(1, 1e-12));
}

TEST_CASE("Zoom applies queued panning before calculating the cursor anchor", "[CurveEditorGUI][RequiresDisplay][Regression]")
{
    Fixture reference;
    reference.panel->set_touchpad_controls(true);
    reference.wheel(40, true);
    reference.wheel(40);
    reference.wheel(120, false, true);
    const auto expected = reference.panel->view();

    Fixture f;
    f.panel->set_touchpad_controls(true);
    f.wheel(40, true, false, false, {300, 160}, false);
    f.wheel(40, false, false, false, {300, 160}, false);
    f.wheel(120, false, true);
    const auto actual = f.panel->view();
    CHECK(f.pan_frames == 1);
    CHECK_THAT(actual.min_x, WithinAbs(expected.min_x, 1e-10));
    CHECK_THAT(actual.max_x, WithinAbs(expected.max_x, 1e-10));
    CHECK_THAT(actual.min_y, WithinAbs(expected.min_y, 1e-10));
    CHECK_THAT(actual.max_y, WithinAbs(expected.max_y, 1e-10));
    f.drain();
    CHECK(f.pan_frames == 1);
}

TEST_CASE("Vertical touchpad scrolling follows Shift wheel direction and leaves the horizontal range unchanged",
          "[CurveEditorGUI][RequiresDisplay][Regression]")
{
    const int rotation = GENERATE(-120, -1, 1, 120);
    Fixture mouse;
    mouse.wheel(rotation, false, false, true);
    const auto expected = mouse.panel->view();

    Fixture touchpad;
    touchpad.panel->set_touchpad_controls(true);
    const auto before = touchpad.panel->view();
    touchpad.wheel(rotation);
    const auto actual = touchpad.panel->view();

    CHECK_THAT(actual.min_x, WithinAbs(before.min_x, 1e-12));
    CHECK_THAT(actual.max_x, WithinAbs(before.max_x, 1e-12));
    CHECK((actual.min_y - before.min_y) * (expected.min_y - before.min_y) > 0.0);
    CHECK_THAT(actual.max_y - actual.min_y, WithinAbs(before.max_y - before.min_y, 1e-12));
    CHECK(std::abs(actual.min_y - before.min_y) > 0.0);
}

TEST_CASE("Touchpad pinch inputs resize both axes around the pointer", "[CurveEditorGUI][RequiresDisplay]")
{
    const bool magnification = GENERATE(false, true);
    const bool zoom_in       = GENERATE(false, true);
    Fixture f;
    f.panel->set_touchpad_controls(true);
    const auto before   = f.panel->view();
    const double factor = zoom_in ? 1.25 : 0.5;
    if (magnification)
        f.magnify(factor - 1);
    else
        f.wheel(zoom_in ? 240 : -240, false, true);
    REQUIRE(f.zooms.size() == 2);
    const auto after = f.panel->view();
    CHECK_FALSE(f.zooms[0].vertical);
    CHECK(f.zooms[1].vertical);
    CHECK((after.max_x - after.min_x < before.max_x - before.min_x) == zoom_in);
    CHECK((after.max_y - after.min_y < before.max_y - before.min_y) == zoom_in);
    CHECK_THAT(after.min_x + f.zooms[0].anchor * (after.max_x - after.min_x),
               WithinAbs(before.min_x + f.zooms[0].anchor * (before.max_x - before.min_x), 1e-10));
    CHECK_THAT(after.min_y + f.zooms[1].anchor * (after.max_y - after.min_y),
               WithinAbs(before.min_y + f.zooms[1].anchor * (before.max_y - before.min_y), 1e-10));
}

TEST_CASE("Native zoom gestures apply cumulative factors once and reset between gestures", "[CurveEditorGUI][RequiresDisplay]")
{
    Fixture f;
    f.panel->set_touchpad_controls(true);
    f.pinch(1, true);
    f.pinch(1.1);
    f.pinch(1.21, false, true);
    REQUIRE(f.zooms.size() == 4);
    CHECK_THAT(f.zooms[0].value, WithinAbs(1, 1e-12));
    CHECK_THAT(f.zooms[2].value, WithinAbs(1, 1e-12));
    CHECK_THAT(f.panel->view().max_x - f.panel->view().min_x, WithinAbs(20 / 1.21, 1e-10));
    f.reset();
    f.pinch(1, true);
    f.pinch(1.1, false, true);
    REQUIRE(f.zooms.size() == 2);
    CHECK_THAT(f.zooms[0].value, WithinAbs(1, 1e-12));
}

TEST_CASE("Invalid gesture values leave the graph unchanged", "[CurveEditorGUI][RequiresDisplay]")
{
    Fixture f;
    f.panel->set_touchpad_controls(true);
    f.magnify(-1);
    f.magnify(std::numeric_limits<double>::infinity());
    f.pinch(0);
    f.pinch(std::numeric_limits<double>::quiet_NaN());
    CHECK(f.zooms.empty());
    f.drain();
    CHECK(f.pans.empty());
    CHECK_THAT(f.panel->view().min_x, WithinAbs(20, 1e-12));
}

TEST_CASE("Native diagonal panning preserves the scale and moves both coordinates", "[CurveEditorGUI][RequiresDisplay]")
{
    Fixture f;
    f.panel->set_touchpad_controls(true);
    wxPanGestureEvent pan;
    pan.SetDelta({20, 10});
    f.panel->GetEventHandler()->ProcessEvent(pan);
    f.drain(true);
    REQUIRE(f.pans.size() == 2);
    CHECK_FALSE(f.pans[0].vertical);
    CHECK(f.pans[1].vertical);
    CHECK(f.pans[0].value < 0);
    CHECK(f.pans[1].value > 0);
    CHECK(f.pan_frames == 1);
    CHECK_THAT(f.panel->view().max_x - f.panel->view().min_x, WithinAbs(20, 1e-12));
    CHECK_THAT(f.panel->view().max_y - f.panel->view().min_y, WithinAbs(.6, 1e-12));
}

TEST_CASE("Repeated reciprocal pinch gestures do not move the pointer anchor", "[CurveEditorGUI][RequiresDisplay][Regression]")
{
    Fixture f;
    f.panel->set_touchpad_controls(true);
    for (int i = 0; i < 100; ++i) {
        f.magnify(.25);
        f.magnify(-.2);
    }
    CHECK_THAT(f.panel->view().min_x, WithinAbs(20, 1e-5));
    CHECK_THAT(f.panel->view().max_y, WithinAbs(.8, 1e-5));
}

TEST_CASE("Changing curve bounds leaves the plot geometry and cursor fractions stable", "[CurveEditorGUI][RequiresDisplay][Regression]")
{
    Fixture f;
    f.wheel(1, false, true);
    const double horizontal = f.zooms.back().anchor;
    f.wheel(1, false, true, true);
    const double vertical              = f.zooms.back().anchor;
    const std::vector<CurveView> views = {{0, 100, -1, 2}, {.01, .011, -.001, 0}, {80, 100, 1, 2}};
    for (const auto& view : views) {
        f.panel->set_view(view);
        f.wheel(1, false, true);
        CHECK_THAT(f.zooms.back().anchor, WithinAbs(horizontal, 1e-12));
        f.wheel(1, false, true, true);
        CHECK_THAT(f.zooms.back().anchor, WithinAbs(vertical, 1e-12));
    }
}

TEST_CASE("Graph point selection and dragging release capture when cancelled", "[CurveEditorGUI][RequiresDisplay][Regression]")
{
    Fixture f;
    f.panel->set_touchpad_controls(true);
    f.panel->set_data({20, 30, 40}, {.2, .5, .8}, {"first", "middle", "last"}, [](double) { return .5; });
    const wxPoint position = f.middle_point();
    int selected = -1, moved = -1;
    double moved_x = 0, moved_y = 0;
    f.panel->on_select = [&](int row) { selected = row; };
    f.panel->on_move   = [&](int row, double x, double y) {
        moved   = row;
        moved_x = x;
        moved_y = y;
    };
    wxMouseEvent down(wxEVT_LEFT_DOWN);
    down.SetPosition(position);
    f.panel->GetEventHandler()->ProcessEvent(down);
    REQUIRE(selected == 1);
    REQUIRE(f.panel->HasCapture());
    f.mouse(wxEVT_MIDDLE_DOWN, position, true, true);
    f.mouse(wxEVT_MIDDLE_UP, position, false, true);
    CHECK(f.panel->HasCapture());
    f.wheel(120);
    CHECK(f.pans.empty());
    f.magnify(.25);
    f.pinch(1.1, true);
    CHECK(f.zooms.empty());
    wxMouseEvent motion(wxEVT_MOTION);
    motion.SetLeftDown(true);
    motion.SetPosition(position);
    f.panel->GetEventHandler()->ProcessEvent(motion);
    CHECK(moved == -1);
    motion.SetPosition(position + wxPoint(20, 0));
    f.panel->GetEventHandler()->ProcessEvent(motion);
    REQUIRE(moved == 1);
    CHECK(moved_x > 30);
    CHECK_THAT(moved_y, WithinAbs(.5, 1e-12));
    wxMouseCaptureLostEvent lost;
    f.panel->GetEventHandler()->ProcessEvent(lost);
    CHECK_FALSE(f.panel->HasCapture());
    moved = -1;
    motion.SetPosition(position + wxPoint(30, 0));
    f.panel->GetEventHandler()->ProcessEvent(motion);
    CHECK(moved == -1);
    f.wheel(120);
    CHECK(f.pans.size() == 1);
}

TEST_CASE("A gesture ending during mouse dragging does not block later wheel navigation", "[CurveEditorGUI][RequiresDisplay][Regression]")
{
    const bool zoom_gesture = GENERATE(false, true);
    const bool middle = GENERATE(false, true);
    Fixture f;
    f.panel->set_touchpad_controls(true);
    f.panel->set_data({20, 30, 40}, {.2, .5, .8}, {"first", "middle", "last"}, [](double) { return .5; });
    const auto position = f.middle_point();
    wxPanGestureEvent pan;
    pan.SetGestureStart(true);
    if (zoom_gesture)
        f.pinch(1, true);
    else
        f.panel->GetEventHandler()->ProcessEvent(pan);

    wxMouseEvent down(middle ? wxEVT_MIDDLE_DOWN : wxEVT_LEFT_DOWN);
    down.SetPosition(position);
    f.panel->GetEventHandler()->ProcessEvent(down);
    REQUIRE(f.panel->HasCapture());
    if (zoom_gesture)
        f.pinch(1.1, false, true);
    else {
        pan.SetGestureStart(false);
        pan.SetGestureEnd(true);
        pan.SetDelta({20, 10});
        f.panel->GetEventHandler()->ProcessEvent(pan);
    }
    CHECK(f.zooms.empty());
    CHECK(f.pans.empty());
    wxMouseCaptureLostEvent lost;
    f.panel->GetEventHandler()->ProcessEvent(lost);
    f.wheel(120);
    CHECK(f.pan_frames == 1);
}

TEST_CASE("Alternative wheel modifiers do not navigate the graph", "[CurveEditorGUI][RequiresDisplay]")
{
    Fixture f;
    const bool touchpad = GENERATE(false, true);
    f.panel->set_touchpad_controls(touchpad);
    wxMouseEvent event(wxEVT_MOUSEWHEEL);
    event.m_wheelRotation = 120;
    event.m_wheelDelta    = 120;
    event.SetAltDown(true);
    f.panel->GetEventHandler()->ProcessEvent(event);
    event.SetAltDown(false);
    event.SetMetaDown(true);
    f.panel->GetEventHandler()->ProcessEvent(event);
    CHECK(f.pans.empty());
    CHECK(f.zooms.empty());
}

namespace {
struct CsvFile
{
    wxString path = wxFileName::CreateTempFileName(wxString::FromUTF8(u8"orca-\u0442\u043e\u0447\u043a\u0438-"));
    ~CsvFile()
    {
        if (!path.empty())
            wxRemoveFile(path);
    }
};
} // namespace

TEST_CASE("CSV file IO retains Unicode filenames and complete UTF8 contents", "[CurveEditorGUI][CSV]")
{
    CsvFile file;
    REQUIRE_FALSE(file.path.empty());
    InputModel model;
    const CurveModel::Rows rows = {{"20", "0.20000000000000001"}, {"40", "0.80000000000000004"}};
    std::string csv;
    int row;
    REQUIRE(model.write_csv(rows, csv, row) == nullptr);
    wxTempFile output(file.path);
    REQUIRE(output.IsOpened());
    REQUIRE(output.Write(wxString::FromUTF8(csv), wxConvUTF8));
    REQUIRE(output.Commit());
    wxFile input(file.path);
    wxString text;
    REQUIRE(input.ReadAll(&text, wxConvUTF8));
    CHECK(text.utf8_string() == csv);
}

TEST_CASE("Discarding a failed CSV replacement preserves an existing file", "[CurveEditorGUI][CSV][Regression]")
{
    CsvFile file;
    {
        wxFile original(file.path, wxFile::write);
        REQUIRE(original.Write("original", wxConvUTF8));
    }
    {
        wxTempFile replacement(file.path);
        REQUIRE(replacement.IsOpened());
        REQUIRE(replacement.Write("partial", wxConvUTF8));
        replacement.Discard();
    }
    wxFile restored(file.path);
    wxString text;
    REQUIRE(restored.ReadAll(&text, wxConvUTF8));
    CHECK(text == "original");
}

TEST_CASE("CSV file reading retains embedded nulls so validation rejects partial data", "[CurveEditorGUI][CSV][Regression]")
{
    CsvFile file;
    std::string binary = "x,y\n20,0.2\n40,0.8";
    binary.push_back('\0');
    binary += "unread tail";
    {
        wxFile output(file.path, wxFile::write);
        REQUIRE(output.Write(binary.data(), binary.size()) == binary.size());
    }
    wxFile input(file.path);
    wxString text;
    REQUIRE(input.ReadAll(&text, wxConvUTF8));
    CHECK(text.utf8_string().size() == binary.size());
    InputModel model;
    CurveModel::Rows rows = {{"20", "0.2"}, {"40", "0.8"}};
    const auto original   = rows;
    int line;
    CHECK(model.read_csv(text.utf8_string(), rows, line) != nullptr);
    CHECK(rows == original);
}
