#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <wx/app.h>
#include <wx/event.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/gdicmn.h>
#include <wx/init.h>
#include <wx/string.h>
#include <wx/strconv.h>

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
        panel->on_pan = [this](double value, bool vertical) {
            pans.push_back({vertical, value, 0});
            panel->set_view(model.panned_view(panel->view(), vertical ? 0 : value, vertical ? value : 0));
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
    }
    void wheel(int rotation, bool horizontal = false, bool control = false, bool shift = false, wxPoint position = {300, 160})
    {
        wxMouseEvent event(wxEVT_MOUSEWHEEL);
        event.m_wheelRotation = rotation;
        event.m_wheelDelta    = 120;
        event.m_wheelAxis     = horizontal ? wxMOUSE_WHEEL_HORIZONTAL : wxMOUSE_WHEEL_VERTICAL;
        event.SetControlDown(control);
        event.SetShiftDown(shift);
        event.SetPosition(position);
        panel->GetEventHandler()->ProcessEvent(event);
    }
    void magnify(double value)
    {
        wxMouseEvent event(wxEVT_MAGNIFY);
        event.m_magnification = float(value);
        event.SetPosition({300, 160});
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
    wxPoint middle_point()
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
        panel->on_zoom = callback;
        return {int(std::lround(100 + (.5 - x1) * 400 / (x2 - x1))), int(std::lround(100 + (.5 - y1) * 200 / (y2 - y1)))};
    }
    InputModel model;
    std::unique_ptr<wxFrame> frame;
    std::unique_ptr<CurveEditorPanel> panel;
    std::vector<Navigation> pans, zooms;
};

} // namespace

wxIMPLEMENT_APP_NO_MAIN(InputApp);

TEST_CASE("Mouse navigation retains its axis modifiers when touchpad mode is disabled", "[CurveEditorGUI][RequiresDisplay]")
{
    Fixture f;
    f.wheel(120);
    REQUIRE(f.pans.size() == 1);
    CHECK_FALSE(f.pans[0].vertical);
    CHECK(f.pans[0].value < 0);
    f.wheel(120, false, false, true);
    REQUIRE(f.pans.size() == 2);
    CHECK(f.pans[1].vertical);
    CHECK(f.pans[1].value > 0);
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

TEST_CASE("Invalid gesture values and disabled touchpad mode leave the graph unchanged", "[CurveEditorGUI][RequiresDisplay]")
{
    Fixture f;
    f.panel->set_touchpad_controls(true);
    f.magnify(-1);
    f.magnify(std::numeric_limits<double>::infinity());
    f.pinch(0);
    f.pinch(std::numeric_limits<double>::quiet_NaN());
    CHECK(f.zooms.empty());
    f.panel->set_touchpad_controls(false);
    f.magnify(.25);
    f.pinch(1.2);
    CHECK(f.zooms.empty());
    wxPanGestureEvent pan;
    pan.SetDelta({20, 10});
    f.panel->GetEventHandler()->ProcessEvent(pan);
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
    REQUIRE(f.pans.size() == 2);
    CHECK_FALSE(f.pans[0].vertical);
    CHECK(f.pans[1].vertical);
    CHECK(f.pans[0].value < 0);
    CHECK(f.pans[1].value > 0);
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
