#include "BoneInfillDialog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>

#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/cursor.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/graphics.h>
#include <wx/pen.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/timer.h>
#include <wx/toplevel.h>

#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Fill/FillBone.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

#include "Field.hpp"
#include "format.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Tab.hpp"
#include "wxExtensions.hpp"
#include "Jobs/BoostThreadWorker.hpp"
#include "Jobs/Job.hpp"
#include "Jobs/PlaterWorker.hpp"
#include "Jobs/Worker.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DensityCurveGraph.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r { namespace GUI {

namespace {

constexpr const char *Accent = "#009688";

// Colours of the custom-drawn controls, picked when they paint, so they follow a theme switch.
wxColour themed(const char *light, const char *dark) { return wxColour(wxGetApp().dark_mode() ? dark : light); }

// The settings the dialog edits.
const std::vector<std::string> &edited_keys()
{
    static const std::vector<std::string> keys { "sparse_infill_density", "adaptive_infill_interior_density", "bone_falloff",
                                                 "bone_falloff_curve",    "bone_falloff_custom",              "bone_hollow_depth",
                                                 "bone_irregularity",     "bone_isotropy",                    "bone_max_bridge" };
    return keys;
}

double option_value(const DynamicPrintConfig &config, const std::string &key)
{
    const ConfigOption *opt = config.option(key);
    return opt->type() == coPercent ? static_cast<const ConfigOptionPercent *>(opt)->value : opt->getFloat();
}

void set_option_value(DynamicPrintConfig &config, const std::string &key, double value)
{
    if (config.option(key)->type() == coPercent)
        config.set_key_value(key, new ConfigOptionPercent(value));
    else
        config.set_key_value(key, new ConfigOptionFloat(value));
}

// Handles of the density graph: the wall density, four points along the falloff, the end of the falloff and the edge
// of the hollow core.
enum GraphHandle : size_t { Wall, Point0, Point1, Point2, Point3, End, Hollow };

// ---------------------------------------------------------------------------------------------
// A value as a bar filled from its minimum to its maximum, set by dragging or with the arrow keys.

class ValueSlider : public wxWindow
{
public:
    // Called while dragging (final = false), on release and on arrow keys (final = true).
    std::function<void(double, bool final)> on_change;

    ValueSlider(wxWindow *parent, double min, double max, int digits, const wxString &unit)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxFULL_REPAINT_ON_RESIZE)
        , m_min(min), m_max(max), m_digits(digits), m_unit(unit)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        rescale();
        Bind(wxEVT_PAINT, &ValueSlider::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
            SetFocus();
            m_start = m_value;
            if (!HasCapture())
                CaptureMouse();
            set_from(e.GetX(), false);
            e.Skip();
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent &e) {
            if (HasCapture())
                set_from(e.GetX(), false);
            e.Skip();
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &e) {
            if (HasCapture()) {
                ReleaseMouse();
                if (on_change)
                    on_change(m_value, true);
            }
            e.Skip();
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) {
            m_value = m_start;
            Refresh();
            if (on_change)
                on_change(m_value, true);
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent &e) {
            const int    key  = e.GetKeyCode();
            const double step = std::pow(10., -m_digits);
            if (key == WXK_LEFT || key == WXK_DOWN || key == WXK_RIGHT || key == WXK_UP) {
                set_value(m_value + ((key == WXK_LEFT || key == WXK_DOWN) ? -step : step));
                if (on_change)
                    on_change(m_value, true);
            } else
                e.Skip();
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent &e) { Refresh(); e.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &e) { Refresh(); e.Skip(); });
    }

    ~ValueSlider() override
    {
        if (HasCapture())
            ReleaseMouse();
    }

    void set_value(double value)
    {
        const double scale = std::pow(10., m_digits);
        m_value            = std::clamp(std::round(value * scale) / scale, m_min, m_max);
        Refresh();
    }

    void rescale()
    {
        SetMinSize(FromDIP(wxSize(150, 26)));
        Refresh();
    }

private:
    double   m_min, m_max;
    int      m_digits;
    wxString m_unit;
    double   m_value { 0. };
    double   m_start { 0. };

    void set_from(int x, bool final)
    {
        const int inset = FromDIP(2);
        set_value(m_min + (m_max - m_min) * std::clamp(double(x - inset) / std::max(1, GetClientSize().x - 2 * inset), 0., 1.));
        if (on_change)
            on_change(m_value, final);
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        const wxSize   size = GetClientSize();
        const double   radius = FromDIP(3);
        const wxColour accent(Accent);
        gc->SetPen(wxPen(HasFocus() ? accent : themed("#DBDBDB", "#4A4A51"), 1));
        gc->SetBrush(wxBrush(themed("#FFFFFF", "#2D2D31")));
        gc->DrawRoundedRectangle(0.5, 0.5, size.x - 1., size.y - 1., radius);
        const double inset = FromDIP(2);
        const double fill  = (size.x - 2. * inset) * (m_value - m_min) / std::max(1e-9, m_max - m_min);
        if (fill > 0.) {
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(wxColour(accent.Red(), accent.Green(), accent.Blue(), wxGetApp().dark_mode() ? 120 : 90)));
            gc->DrawRoundedRectangle(inset, inset, fill, size.y - 2. * inset, radius - 1.);
        }
        gc->SetFont(Label::Body_13, themed("#262E30", "#EFEFF0"));
        const wxString s = double_to_string(m_value, m_digits) + (m_unit.IsEmpty() ? "" : " " + m_unit);
        double         tw, th;
        gc->GetTextExtent(s, &tw, &th);
        gc->DrawText(s, (size.x - tw) / 2., (size.y - th) / 2.);
    }
};

// ---------------------------------------------------------------------------------------------
// The revert arrow of a modified value, as in the settings tabs. Empty while the value is the saved one.

class RevertIcon : public wxWindow
{
public:
    std::function<void()> on_click;

    explicit RevertIcon(wxWindow *parent) : wxWindow(parent, wxID_ANY), m_bmp(this, "undo")
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(m_bmp.GetBmpSize());
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
            dc.Clear();
            if (m_modified)
                dc.DrawBitmap(m_bmp.bmp(), 0, 0, true);
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &e) {
            if (m_modified)
                CallAfter([this] {
                    if (on_click)
                        on_click();
                });
            e.Skip();
        });
    }

    bool AcceptsFocus() const override { return false; }

    void set_modified(bool modified)
    {
        if (modified == m_modified)
            return;
        m_modified = modified;
        SetCursor(modified ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
        if (modified)
            SetToolTip(_L("Click to reset the value to the one saved in the preset."));
        else
            UnsetToolTip();
        Refresh();
    }

    void rescale()
    {
        m_bmp.msw_rescale();
        SetMinSize(m_bmp.GetBmpSize());
        Refresh();
    }

private:
    ScalableBitmap m_bmp;
    bool           m_modified { false };
};

// ---------------------------------------------------------------------------------------------
// Preview: a vertical section through a sample long bone filled with the bone infill.

struct BoneSection
{
    struct Layer
    {
        double z0, z1;
        // Half widths of the part and of its sparse infill region.
        double outer, inner;
        // Extents along the section of the walls cut by it.
        std::vector<std::pair<double, double>> walls;
    };
    double             height { 0. };
    double             radius { 0. };
    std::vector<Layer> layers;
};

BoneSection make_bone_section(const BoneParams &params, double layer_height, int bottom_layers, int top_layers,
                              const std::function<void()> &throw_if_canceled)
{
    // A long bone: a shaft wide enough for a hollow canal at the default settings, between two wider ends.
    constexpr double height = 80.;
    auto radius_at = [](double z) { return 10. + 7. * (std::exp(-sqr((z - 8.) / 9.)) + std::exp(-sqr((z - 72.) / 9.))); };
    auto disc      = [](double radius) {
        constexpr int n = 96;
        Polygon       poly;
        for (int i = 0; i < n; ++i)
            poly.points.emplace_back(scaled<coord_t>(radius * std::cos(2. * M_PI * i / n)), scaled<coord_t>(radius * std::sin(2. * M_PI * i / n)));
        return ExPolygons { ExPolygon(std::move(poly)) };
    };

    const int           n = int(std::lround(height / layer_height));
    std::vector<double> radii(n);
    for (int l = 0; l < n; ++l)
        radii[l] = radius_at((l + 0.5) * layer_height);
    std::vector<ExPolygons> outlines(n);
    std::vector<BoneLayer>  layers;
    std::vector<double>     inner(n, 0.);
    layers.reserve(n);
    for (int l = 0; l < n; ++l) {
        outlines[l] = disc(radii[l]);
        // Solid skins where there are fewer than the shell layers above or below, as the slicer leaves them.
        if (l >= bottom_layers && l + top_layers < n)
            inner[l] = *std::min_element(radii.begin() + (l - bottom_layers), radii.begin() + (l + top_layers + 1)) - params.shell;
        layers.push_back({ l * layer_height, (l + 1) * layer_height, &outlines[l], inner[l] > params.line_width ? disc(inner[l]) : ExPolygons() });
    }
    const std::vector<Polylines> walls = generate_bone_walls(layers, params, throw_if_canceled);

    BoneSection section;
    section.height              = n * layer_height;
    section.radius              = *std::max_element(radii.begin(), radii.end());
    const double     half_width = 0.5 * params.line_width;
    const ExPolygons strip { ExPolygon(Polygon { Point(scaled<coord_t>(-section.radius - 1.), scaled<coord_t>(-half_width)),
                                                 Point(scaled<coord_t>(section.radius + 1.), scaled<coord_t>(-half_width)),
                                                 Point(scaled<coord_t>(section.radius + 1.), scaled<coord_t>(half_width)),
                                                 Point(scaled<coord_t>(-section.radius - 1.), scaled<coord_t>(half_width)) }) };
    for (int l = 0; l < n; ++l) {
        BoneSection::Layer layer { l * layer_height, (l + 1) * layer_height, radii[l], std::max(0., inner[l]), {} };
        for (const Polyline &piece : intersection_pl(walls[l], strip)) {
            const BoundingBox box = piece.bounding_box();
            layer.walls.emplace_back(unscaled(box.min.x()) - half_width, unscaled(box.max.x()) + half_width);
        }
        section.layers.push_back(std::move(layer));
    }
    return section;
}

class BonePreview : public wxWindow
{
public:
    explicit BonePreview(wxWindow *parent) : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        rescale();
        SetToolTip(_L("A section through a sample long bone with these settings. Your model's walls, layer height and "
                      "shape change the result, so slice it to see the real infill."));
        Bind(wxEVT_PAINT, &BonePreview::on_paint, this);
    }

    bool AcceptsFocus() const override { return false; }

    void set_section(std::shared_ptr<const BoneSection> section)
    {
        m_section = std::move(section);
        m_busy    = false;
        Refresh();
    }

    void set_busy(bool busy)
    {
        m_busy = busy;
        Refresh();
    }

    void rescale()
    {
        SetMinSize(FromDIP(wxSize(200, 240)));
        Refresh();
    }

private:
    std::shared_ptr<const BoneSection> m_section;
    bool                               m_busy { true };

    // A round length in the bottom left corner, at most a third of the width.
    void draw_scale_bar(wxDC &dc, double px_per_mm)
    {
        double length = 1.;
        for (double l : { 2., 5., 10., 20., 50., 100. })
            if (l * px_per_mm <= GetClientSize().x / 3.)
                length = l;
        const int      x0 = FromDIP(8), x1 = x0 + int(std::lround(length * px_per_mm)), y = GetClientSize().y - FromDIP(8), tick = FromDIP(3);
        const wxColour colour = themed("#262E30", "#EFEFF0");
        dc.SetPen(wxPen(colour, FromDIP(2)));
        dc.DrawLine(x0, y, x1, y);
        dc.DrawLine(x0, y - tick, x0, y + 1);
        dc.DrawLine(x1, y - tick, x1, y + 1);
        dc.SetFont(Label::Body_12);
        dc.SetTextForeground(colour);
        // TRN: Label of the scale bar of the bone infill preview. %1% is a length.
        const wxString label  = format_wxstr(_L("%1% mm"), double_to_string(length, 0));
        const wxSize   extent = dc.GetTextExtent(label);
        dc.DrawText(label, x0 + (x1 - x0 - extent.x) / 2, y - tick - FromDIP(2) - extent.y);
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        const wxSize size = GetClientSize();
        if (m_section && !m_section->layers.empty()) {
            const double pad   = FromDIP(8);
            const double scale = std::min((size.x - 2. * pad) / (2. * m_section->radius), (size.y - 2. * pad) / m_section->height);
            const double cx = size.x / 2., bottom = (size.y + m_section->height * scale) / 2.;
            // Whole pixels, at least one wide, so thin walls don't vanish.
            auto rect = [&](double x0, double x1, double z0, double z1) {
                const int px0 = int(std::floor(cx + x0 * scale)), px1 = std::max(px0 + 1, int(std::ceil(cx + x1 * scale)));
                const int py0 = int(std::floor(bottom - z1 * scale)), py1 = std::max(py0 + 1, int(std::ceil(bottom - z0 * scale)));
                dc.DrawRectangle(px0, py0, px1 - px0, py1 - py0);
            };
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(themed("#BDBDBD", "#8A8A8F")));
            for (const BoneSection::Layer &l : m_section->layers)
                rect(-l.outer, l.outer, l.z0, l.z1);
            dc.SetBrush(wxBrush(themed("#F4F4F4", "#1E1E21")));
            for (const BoneSection::Layer &l : m_section->layers)
                if (l.inner > 0.)
                    rect(-l.inner, l.inner, l.z0, l.z1);
            dc.SetBrush(wxBrush(themed("#262E30", "#F0F0F0")));
            for (const BoneSection::Layer &l : m_section->layers)
                for (const auto &[x0, x1] : l.walls)
                    rect(x0, x1, l.z0, l.z1);
            draw_scale_bar(dc, scale);
        }
        if (m_busy) {
            dc.SetFont(Label::Body_12);
            dc.SetTextForeground(themed("#6B6B6B", "#B3B3B5"));
            dc.DrawText(_L("Updating..."), FromDIP(6), FromDIP(4));
        }
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------

struct BoneInfillDialog::priv
{
    struct Row
    {
        std::string     key;
        double          min, max;
        int             digits;
        wxStaticText   *label { nullptr };
        RevertIcon     *revert { nullptr };
        ::TextInput    *input { nullptr };
        ValueSlider    *slider { nullptr };
        // Reads the value typed in the input.
        std::function<void()> commit;
    };

    BoneInfillDialog  *dlg;
    Tab               *tab;
    DynamicPrintConfig config;
    // For the preview.
    double line_width { 0.45 }, shell { 0.9 };
    int    bottom_layers { 3 }, top_layers { 4 };

    DensityCurveGraph *graph { nullptr };
    // The settings when a drag of the graph began, restored if it is cancelled, and the extent of its axis then, in
    // mm, with its units per mm.
    DynamicPrintConfig drag_start;
    double             drag_x_max { 10. };
    double             drag_scale { 1. };
    // The graph shows depths in mm rather than in percent of the depth of the center of the model.
    bool               depth_in_mm { false };
    ::CheckBox        *mm_check { nullptr };
    // Depth below the walls of the deepest point of the models, and the name of that model. Unknown until measured,
    // or without a model.
    double             model_depth { std::numeric_limits<double>::quiet_NaN() };
    wxString           model_name;
    wxStaticText      *curve_label { nullptr };
    RevertIcon        *curve_revert { nullptr };
    ::ComboBox        *curve_combo { nullptr };
    ::CheckBox        *sliders_check { nullptr };
    wxScrolledWindow  *list { nullptr };
    std::vector<Row>   rows;
    BonePreview       *preview { nullptr };
    Button            *refresh { nullptr };
    // The preview follows every change, dragging included: a new one starts when the last is done, with the
    // latest settings. Its worker shows no busy cursor, which would flicker while dragging, so a timer
    // delivers its results while it works.
    std::unique_ptr<BoostThreadWorker> worker;
    wxTimer                            worker_pump;
    bool                               preview_running { false };
    bool                               preview_pending { false };
    // Measures the models once. Its own worker, so that the preview jobs don't cancel it.
    std::unique_ptr<PlaterWorker<BoostThreadWorker>> depth_worker;
    // Set while the dialog is destroyed: focus and capture events of its children are ignored.
    bool closing { false };

    priv(BoneInfillDialog *dlg, Tab *tab) : dlg(dlg), tab(tab) {}

    const DynamicPrintConfig &saved() const { return wxGetApp().preset_bundle->prints.get_selected_preset().config; }

    bool modified(const std::string &key) const
    {
        const ConfigOption *saved_opt = saved().option(key);
        return saved_opt != nullptr && *saved_opt != *config.option(key);
    }

    bool curve_modified() const { return modified("bone_falloff_curve") || modified("bone_falloff_custom"); }

    void         build();
    wxSizer     *build_row(Row &row);
    void         update_row(Row &row);
    void         update_all();
    double       row_max(const std::string &key) const;
    double       graph_scale() const;
    double       graph_x_max(const BoneGradient &g) const;
    DensityCurve graph_curve() const;
    void         drag_graph(const DensityCurveGraph::Drag &drag);
    void         decorate(wxStaticText *label, RevertIcon *revert, bool is_modified);
    void         set_value(const std::string &key, double value);
    void         show_sliders(bool sliders);
    void         request_preview();
    void         start_preview();
    void         measure_models();
    void         apply();
};

void BoneInfillDialog::priv::build()
{
    const DynamicPrintConfig &tab_config = *tab->get_config();
    for (const std::string &key : edited_keys())
        config.set_key_value(key, tab_config.option(key)->clone());

    // The preview approximates the walls and skins of the part.
    const auto  *nozzles = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    const double nozzle  = nozzles != nullptr && !nozzles->values.empty() ? nozzles->values.front() : 0.4;
    line_width           = tab_config.option<ConfigOptionFloatOrPercent>("sparse_infill_line_width")->get_abs_value(nozzle);
    if (line_width <= 0.)
        line_width = 1.125 * nozzle;
    shell         = tab_config.opt_int("wall_loops") * line_width;
    bottom_layers = tab_config.opt_int("bottom_shell_layers");
    top_layers    = tab_config.opt_int("top_shell_layers");

    auto *sizer = new wxBoxSizer(wxVERTICAL);

    // Graph, with the curve type above it, as in the other density graphs.
    auto *graph_header = new wxBoxSizer(wxHORIZONTAL);
    depth_in_mm        = wxGetApp().app_config->get_bool("bone_graph_depth_mm");
    mm_check           = new ::CheckBox(dlg);
    mm_check->SetValue(depth_in_mm);
    // Percent needs the depth of the model, measured once the dialog is open.
    mm_check->Enable(false);
    mm_check->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        depth_in_mm = mm_check->GetValue();
        wxGetApp().app_config->set_bool("bone_graph_depth_mm", depth_in_mm);
        graph->set_curve(graph_curve());
        e.Skip();
    });
    auto *mm_label = new wxStaticText(dlg, wxID_ANY, _L("Depth in mm"));
    mm_label->SetToolTip(_L("Show the depth below the walls in millimeters instead of in percent of the depth of the center of "
                            "the model. Percent needs a model on the plate."));
    graph_header->Add(mm_check, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, dlg->FromDIP(6));
    graph_header->Add(mm_label, 0, wxALIGN_CENTER_VERTICAL);
    graph_header->AddStretchSpacer();
    curve_label = new wxStaticText(dlg, wxID_ANY, _(print_config_def.get("bone_falloff_curve")->label));
    curve_label->SetToolTip(_(print_config_def.get("bone_falloff_curve")->tooltip));
    curve_revert           = new RevertIcon(dlg);
    curve_revert->on_click = [this] {
        config.set_key_value("bone_falloff_curve", saved().option("bone_falloff_curve")->clone());
        config.set_key_value("bone_falloff_custom", saved().option("bone_falloff_custom")->clone());
        update_all();
        request_preview();
    };
    curve_combo = new ::ComboBox(dlg, wxID_ANY, wxEmptyString, wxDefaultPosition, dlg->FromDIP(wxSize(150, -1)), 0, nullptr, wxCB_READONLY);
    for (const std::string &label : print_config_def.get("bone_falloff_curve")->enum_labels)
        curve_combo->Append(_(label));
    curve_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) {
        config.set_key_value("bone_falloff_curve", new ConfigOptionEnum<BoneFalloffCurve>(BoneFalloffCurve(curve_combo->GetSelection())));
        update_all();
        request_preview();
    });
    graph_header->Add(curve_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, dlg->FromDIP(6));
    graph_header->Add(curve_revert, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, dlg->FromDIP(6));
    graph_header->Add(curve_combo, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(graph_header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, dlg->FromDIP(16));

    graph = new DensityCurveGraph(dlg, wxSize(600, 320));
    graph->set_x_digits(0);
    graph->SetToolTip(_L("Drag the points to shape how the density changes below the walls. Dragging one of the middle "
                         "points switches to a custom curve. Drag the edge of the red area to set the hollow core."));
    graph->on_drag = [this](const DensityCurveGraph::Drag &drag) { drag_graph(drag); };
    sizer->Add(graph, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, dlg->FromDIP(10));

    // Parameters and preview side by side.
    auto *bottom = new wxBoxSizer(wxHORIZONTAL);

    auto *params_column = new wxBoxSizer(wxVERTICAL);
    params_column->Add(new Label(dlg, Label::Head_16, _L("Parameter Tuning")), 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, dlg->FromDIP(6));
    auto *params_box = new StaticBox(dlg);
    params_box->SetCornerRadius(dlg->FromDIP(4));
    params_box->SetBackgroundColor(StateColor(std::make_pair(0xFFFFFF, int(StateColor::Normal))));
    params_box->SetBorderColor(StateColor(std::make_pair(0xDBDBDB, int(StateColor::Normal))));
    list = new wxScrolledWindow(params_box, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    list->SetBackgroundColour(*wxWHITE);
    list->SetScrollRate(0, dlg->FromDIP(10));
    auto *list_sizer = new wxBoxSizer(wxVERTICAL);

    auto *toggle_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto *toggle_label = new wxStaticText(list, wxID_ANY, _L("Show sliders"));
    sliders_check      = new ::CheckBox(list);
    sliders_check->SetValue(wxGetApp().app_config->get_bool("bone_tuning_sliders"));
    sliders_check->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        wxGetApp().app_config->set_bool("bone_tuning_sliders", sliders_check->GetValue());
        show_sliders(sliders_check->GetValue());
        e.Skip();
    });
    toggle_sizer->Add(toggle_label, 1, wxALIGN_CENTER_VERTICAL);
    toggle_sizer->Add(sliders_check, 0, wxALIGN_CENTER_VERTICAL);
    list_sizer->Add(toggle_sizer, 0, wxEXPAND | wxALL, dlg->FromDIP(8));

    rows = { { "sparse_infill_density", 2., 99., 0 }, { "adaptive_infill_interior_density", 1., 100., 0 }, { "bone_falloff", 0.5, 30., 1 },
             { "bone_hollow_depth", 0., 30., 1 },      { "bone_irregularity", 0., 100., 0 },               { "bone_isotropy", 0., 100., 0 },
             { "bone_max_bridge", 0., 10., 1 } };
    for (Row &row : rows)
        list_sizer->Add(build_row(row), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dlg->FromDIP(8));
    list->SetSizer(list_sizer);

    auto *box_sizer = new wxBoxSizer(wxVERTICAL);
    box_sizer->Add(list, 1, wxEXPAND | wxALL, dlg->FromDIP(2));
    params_box->SetSizer(box_sizer);
    params_box->SetMinSize(dlg->FromDIP(wxSize(360, 240)));
    params_column->Add(params_box, 1, wxEXPAND);
    bottom->Add(params_column, 3, wxEXPAND | wxRIGHT, dlg->FromDIP(16));

    auto *preview_column = new wxBoxSizer(wxVERTICAL);
    preview_column->Add(new Label(dlg, Label::Head_16, _L("Approximate Preview")), 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, dlg->FromDIP(6));
    auto *preview_box = new StaticBox(dlg);
    preview_box->SetCornerRadius(dlg->FromDIP(4));
    preview_box->SetBackgroundColor(StateColor(std::make_pair(0xFFFFFF, int(StateColor::Normal))));
    preview_box->SetBorderColor(StateColor(std::make_pair(0xDBDBDB, int(StateColor::Normal))));
    preview = new BonePreview(preview_box);
    refresh = new Button(preview_box, "", "refresh", 0, 16);
    refresh->SetStyle(ButtonStyle::Regular, ButtonType::Icon);
    refresh->SetToolTip(_L("Update the preview"));
    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { request_preview(); });
    auto *preview_sizer = new wxBoxSizer(wxVERTICAL);
    preview_sizer->Add(preview, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, dlg->FromDIP(4));
    preview_sizer->Add(refresh, 0, wxALIGN_RIGHT | wxALL, dlg->FromDIP(4));
    preview_box->SetSizer(preview_sizer);
    preview_column->Add(preview_box, 1, wxEXPAND);
    bottom->Add(preview_column, 2, wxEXPAND);

    sizer->Add(bottom, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, dlg->FromDIP(16));

    auto *buttons = new DialogButtons(dlg, { "Cancel", "Apply" });
    buttons->GetAPPLY()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        apply();
        dlg->EndModal(wxID_OK);
    });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { dlg->EndModal(wxID_CANCEL); });
    sizer->Add(buttons, 0, wxEXPAND | wxTOP, dlg->FromDIP(10));

    worker = std::make_unique<BoostThreadWorker>(nullptr, "bone_preview_worker");
    worker_pump.Bind(wxEVT_TIMER, [this](wxTimerEvent &) { worker->process_events(); });
    measure_models();

    dlg->SetSizerAndFit(sizer);
    show_sliders(sliders_check->GetValue());
    update_all();
}

wxSizer *BoneInfillDialog::priv::build_row(Row &row)
{
    const ConfigOptionDef *def  = print_config_def.get(row.key);
    const wxString         unit = _(def->sidetext);
    row.label                   = new wxStaticText(list, wxID_ANY, _(def->label));
    row.label->SetToolTip(_(def->tooltip));
    const std::string key  = row.key;
    row.revert             = new RevertIcon(list);
    row.revert->on_click   = [this, key] { set_value(key, option_value(saved(), key)); };

    row.input = new ::TextInput(list, wxEmptyString, unit, wxEmptyString, wxDefaultPosition, dlg->FromDIP(wxSize(150, -1)), wxTE_PROCESS_ENTER);
    ::TextInput *input = row.input;
    row.commit         = [this, key, input]() {
        if (closing)
            return;
        double value;
        wxString text = input->GetTextCtrl()->GetValue();
        text.Replace(",", ".");
        const ConfigOptionDef *def = print_config_def.get(key);
        if (text.ToCDouble(&value))
            set_value(key, std::clamp(value, double(def->min), double(def->max)));
        else
            update_all();
    };
    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [commit = row.commit](wxCommandEvent &) { commit(); });
    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [commit = row.commit](wxFocusEvent &e) {
        commit();
        e.Skip();
    });

    row.slider            = new ValueSlider(list, row.min, row.max, row.digits, unit);
    row.slider->on_change = [this, key](double value, bool) { set_value(key, value); };

    auto *sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(row.label, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(row.revert, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, dlg->FromDIP(6));
    sizer->Add(row.input, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(row.slider, 0, wxALIGN_CENTER_VERTICAL);
    return sizer;
}

void BoneInfillDialog::priv::decorate(wxStaticText *label, RevertIcon *revert, bool is_modified)
{
    label->SetForegroundColour(is_modified ? wxGetApp().get_label_clr_modified() : wxGetApp().get_label_clr_default());
    label->Refresh();
    revert->set_modified(is_modified);
}

void BoneInfillDialog::priv::update_row(Row &row)
{
    const double value = option_value(config, row.key);
    // Leave the text being typed alone.
    if (!row.input->GetTextCtrl()->HasFocus())
        row.input->GetTextCtrl()->ChangeValue(double_to_string(value, row.digits == 0 ? 0 : 2));
    row.slider->set_value(value);
    decorate(row.label, row.revert, modified(row.key));
}

void BoneInfillDialog::priv::update_all()
{
    for (Row &row : rows)
        update_row(row);
    curve_combo->SetSelection(int(config.opt_enum<BoneFalloffCurve>("bone_falloff_curve")));
    decorate(curve_label, curve_revert, curve_modified());
    graph->set_curve(graph_curve());
}

double BoneInfillDialog::priv::row_max(const std::string &key) const
{
    return std::find_if(rows.begin(), rows.end(), [&key](const Row &row) { return row.key == key; })->max;
}

double BoneInfillDialog::priv::graph_scale() const
{
    return !depth_in_mm && std::isfinite(model_depth) && model_depth > 0. ? 100. / model_depth : 1.;
}

double BoneInfillDialog::priv::graph_x_max(const BoneGradient &g) const
{
    // In percent, from the walls to the center of the model.
    if (graph_scale() != 1.)
        return 100.;
    // In mm, room for the falloff and the hollow core, and the whole depth of the model.
    double need = std::max({ 10., g.falloff * 1.5, g.hollow_depth * 1.25 });
    if (std::isfinite(model_depth))
        need = std::max(need, model_depth * 1.05);
    return std::ceil(need / 5.) * 5.;
}

DensityCurve BoneInfillDialog::priv::graph_curve() const
{
    const BoneGradient g = BoneGradient::from_config(config);
    // Graph units per mm.
    const double       s = graph_scale();
    DensityCurve       curve;
    curve.density        = [g, s](double x) { return g.density(x / s); };
    curve.x_max          = graph_x_max(g);
    curve.x_title        = s != 1. ? _L("Depth from the walls to the center (%)") : _L("Depth below the walls (mm)");
    curve.y_title        = _L("Infill density");
    curve.spread         = 0.35 * config.option<ConfigOptionPercent>("bone_irregularity")->value / 100.;
    curve.guides         = { g.falloff * s };
    curve.shade_from     = g.hollow_from() * s;
    curve.shade_title    = _L("Hollow core");
    // In percent the axis ends at the center, so the mark only shows in mm.
    if (std::isfinite(model_depth)) {
        curve.mask_from = model_depth * s;
        if (model_name.IsEmpty())
            curve.mask_label = _L("Center of the model");
        else
            // TRN: Marks the deepest point of a model on the density graph of the bone infill. %1% is the name of the model.
            curve.mask_label = format_wxstr(_L("Center of %1%"), model_name);
    }
    auto readout = [](double density, double depth) {
        // TRN: A point of the density graph of the bone infill: %1% is a density in percent, %2% a depth in mm.
        return format_wxstr(_L("%1%%% at %2% mm"), double_to_string(std::round(density * 100.), 0), double_to_string(depth, 1));
    };
    const DensityCurve::HandleKind middle = g.curve == BoneFalloffCurve::Custom ? DensityCurve::HandleKind::Point : DensityCurve::HandleKind::Ring;
    curve.handles.push_back({ DensityCurve::HandleKind::Point, 0., g.wall_density, readout(g.wall_density, 0.) });
    for (int k = 1; k <= 4; ++k) {
        const double depth = g.falloff * k / 5.;
        curve.handles.push_back({ middle, depth * s, g.density(depth), readout(g.density(depth), depth) });
    }
    curve.handles.push_back({ DensityCurve::HandleKind::Point, g.falloff * s, g.core_density, readout(g.core_density, g.falloff) });
    curve.handles.push_back({ DensityCurve::HandleKind::Edge, g.hollow_depth > 0. ? g.hollow_depth * s : curve.x_max, 0.,
                              g.hollow_depth > 0. ? format_wxstr(_L("Hollow below %1% mm"), double_to_string(g.hollow_depth, 1)) :
                                                    _L("Drag left to set a hollow core") });
    return curve;
}

void BoneInfillDialog::priv::drag_graph(const DensityCurveGraph::Drag &drag)
{
    if (closing)
        return;
    switch (drag.phase) {
    case DensityCurveGraph::Drag::Phase::Begin:
        drag_start = config;
        drag_scale = graph_scale();
        drag_x_max = graph_x_max(BoneGradient::from_config(config)) / drag_scale;
        return;
    case DensityCurveGraph::Drag::Phase::Cancel:
        config = drag_start;
        update_all();
        request_preview();
        return;
    default: break;
    }

    BoneGradient g = BoneGradient::from_config(config);
    const double x = drag.x / drag_scale;
    // Percentages snap to whole numbers and depths to tenths of a millimetre, and both stay within the limits of the
    // inputs below, so that the graph never sets a value they can't show.
    auto percent = [](double v, double lo, double hi) { return std::clamp(std::round(v * 100.), lo, hi) / 100.; };
    auto mm      = [](double v, double lo, double hi) { return std::clamp(std::round(v * 10.) / 10., lo, hi); };
    switch (GraphHandle(drag.handle)) {
    case Wall: g.wall_density = percent(drag.density, 2., 99.); break;
    case End:
        g.falloff      = mm(x, 0.5, std::min(drag_x_max, row_max("bone_falloff")));
        g.core_density = percent(drag.density, 1., 100.);
        break;
    case Hollow: g.hollow_depth = x >= drag_x_max * 0.98 ? 0. : mm(x, 0.5, std::min(drag_x_max, row_max("bone_hollow_depth"))); break;
    default: {
        if (std::abs(g.wall_density - g.core_density) < 0.005)
            return;
        // Dragging a point along the curve turns it custom, starting from the shape it had.
        if (g.curve != BoneFalloffCurve::Custom) {
            for (size_t k = 0; k < g.custom.size(); ++k)
                g.custom[k] = g.progress((k + 1) / 5.);
            g.curve = BoneFalloffCurve::Custom;
        }
        const size_t k = drag.handle - Point0;
        g.custom[k]    = std::clamp(std::round((g.wall_density - drag.density) / (g.wall_density - g.core_density) * 1000.) / 1000., 0., 1.);
    }
    }
    config.set_key_value("sparse_infill_density", new ConfigOptionPercent(std::round(g.wall_density * 100.)));
    config.set_key_value("adaptive_infill_interior_density", new ConfigOptionPercent(std::round(g.core_density * 100.)));
    config.set_key_value("bone_falloff", new ConfigOptionFloat(g.falloff));
    config.set_key_value("bone_hollow_depth", new ConfigOptionFloat(g.hollow_depth));
    config.set_key_value("bone_falloff_curve", new ConfigOptionEnum<BoneFalloffCurve>(g.curve));
    if (g.curve == BoneFalloffCurve::Custom)
        config.set_key_value("bone_falloff_custom", new ConfigOptionString(BoneGradient::serialize_custom(g.custom)));
    update_all();
    request_preview();
}

void BoneInfillDialog::priv::set_value(const std::string &key, double value)
{
    if (closing)
        return;
    set_option_value(config, key, value);
    update_all();
    request_preview();
}

void BoneInfillDialog::priv::show_sliders(bool sliders)
{
    for (Row &row : rows) {
        row.input->Show(!sliders);
        row.slider->Show(sliders);
    }
    list->Layout();
    list->FitInside();
}

void BoneInfillDialog::priv::request_preview()
{
    if (closing)
        return;
    preview_pending = true;
    if (!preview_running)
        start_preview();
}

void BoneInfillDialog::priv::start_preview()
{
    // A picture needs no finer layers, and these keep it at about a second.
    constexpr double layer_height = 0.4;
    const BoneParams params       = BoneParams::from_config(config, line_width, shell);
    auto             result       = std::make_shared<BoneSection>();
    preview_pending               = false;
    preview_running               = true;
    preview->set_busy(true);
    if (!worker_pump.IsRunning())
        worker_pump.Start(50);
    queue_job(
        *worker,
        [params, result, bottom_layers = bottom_layers, top_layers = top_layers](Job::Ctl &ctl) {
            *result = make_bone_section(params, layer_height, bottom_layers, top_layers, [&ctl] {
                if (ctl.was_canceled())
                    throw CanceledException();
            });
        },
        [this, result](bool canceled, std::exception_ptr &eptr) {
            // Nothing reports what is left in eptr here, and the worker would rethrow it into the timer.
            if (eptr) {
                try {
                    std::rethrow_exception(eptr);
                } catch (const CanceledException &) {
                    canceled = true;
                } catch (const std::exception &e) {
                    BOOST_LOG_TRIVIAL(error) << "Bone infill preview failed: " << e.what();
                    canceled = true;
                } catch (...) {
                    canceled = true;
                }
                eptr = nullptr;
            }
            preview_running = false;
            if (!canceled)
                preview->set_section(result);
            if (preview_pending && !closing)
                start_preview();
            else
                worker_pump.Stop();
        });
}

void BoneInfillDialog::priv::measure_models()
{
    // The models are copied here, on the main thread; the worker only reads the copies.
    struct PlacedMesh
    {
        indexed_triangle_set its;
        Transform3d          trafo;
        std::string          name;
    };
    auto meshes = std::make_shared<std::vector<PlacedMesh>>();
    for (const ModelObject *object : wxGetApp().model().objects)
        if (object->printable)
            for (const ModelInstance *instance : object->instances)
                if (instance->printable) {
                    meshes->push_back({ object->raw_indexed_triangle_set(), instance->get_matrix(), object->name });
                    break;
                }
    if (meshes->empty())
        return;

    // Depth of the deepest point and the name of its model.
    auto deepest = std::make_shared<std::pair<double, std::string>>(0., std::string());
    depth_worker = std::make_unique<PlaterWorker<BoostThreadWorker>>(dlg, nullptr, "bone_depth_worker");
    queue_job(
        *depth_worker,
        [meshes, deepest](Job::Ctl &ctl) {
            auto throw_if_canceled = [&ctl] {
                if (ctl.was_canceled())
                    throw CanceledException();
            };
            // Coarser than a print: the depth needs no more than the half voxel of the distance field.
            constexpr double step = 0.5;
            for (const PlacedMesh &mesh : *meshes) {
                double z0 = std::numeric_limits<double>::max(), z1 = std::numeric_limits<double>::lowest();
                for (const stl_vertex &v : mesh.its.vertices) {
                    const double z = (mesh.trafo * v.cast<double>()).z();
                    z0             = std::min(z0, z);
                    z1             = std::max(z1, z);
                }
                std::vector<float> zs;
                for (double z = z0 + 0.5 * step; z < z1; z += step)
                    zs.push_back(float(z));
                if (zs.empty())
                    continue;
                MeshSlicingParamsEx params;
                params.trafo                         = mesh.trafo;
                const std::vector<ExPolygons> slices = slice_mesh_ex(mesh.its, zs, params, throw_if_canceled);
                std::vector<BoneLayer>        layers;
                layers.reserve(zs.size());
                for (size_t i = 0; i < zs.size(); ++i)
                    layers.push_back({ zs[i] - 0.5 * step, zs[i] + 0.5 * step, &slices[i], {} });
                const double depth = bone_max_depth(layers, throw_if_canceled);
                if (depth > deepest->first)
                    *deepest = { depth, mesh.name };
            }
        },
        [this, deepest](bool canceled, std::exception_ptr &eptr) {
            if (eptr) {
                try {
                    std::rethrow_exception(eptr);
                } catch (const CanceledException &) {
                    eptr     = nullptr;
                    canceled = true;
                } catch (...) {
                    // Left in eptr: the worker reports it.
                }
            }
            // The graph measures depth from the inner edge of the walls.
            if (!canceled && !eptr && deepest->first > shell) {
                model_depth = deepest->first - shell;
                model_name  = from_u8(deepest->second);
                mm_check->Enable(true);
                graph->set_curve(graph_curve());
            }
        });
}

void BoneInfillDialog::priv::apply()
{
    // The Apply button doesn't take the focus, so a value still being typed is read here.
    for (Row &row : rows)
        if (row.input->IsShown() && row.input->GetTextCtrl()->HasFocus())
            row.commit();
    DynamicPrintConfig new_config = *tab->get_config();
    for (const std::string &key : edited_keys())
        new_config.set_key_value(key, config.option(key)->clone());
    tab->load_config(new_config);
}

BoneInfillDialog::BoneInfillDialog(wxWindow *parent, Tab *tab)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Bone Infill Density Graph"),
                wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX)
    , p(std::make_unique<priv>(this, tab))
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);
    p->build();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
    // The dark walk recolours every label: mark the modified ones again.
    p->update_all();
    p->request_preview();
}

BoneInfillDialog::~BoneInfillDialog()
{
    // The worker delivers what is pending as it stops, and the children can send focus events as they are
    // destroyed: both happen while p still exists.
    p->closing = true;
    p->worker_pump.Stop();
    p->worker.reset();
    p->depth_worker.reset();
    DestroyChildren();
}

void BoneInfillDialog::on_dpi_changed(const wxRect &)
{
    p->curve_revert->rescale();
    p->curve_combo->Rescale();
    p->curve_combo->SetMinSize(FromDIP(wxSize(150, -1)));
    p->sliders_check->Rescale();
    p->mm_check->Rescale();
    p->refresh->Rescale();
    p->graph->rescale();
    p->preview->rescale();
    for (priv::Row &row : p->rows) {
        row.input->Rescale();
        row.input->SetMinSize(FromDIP(wxSize(150, -1)));
        row.slider->rescale();
        row.revert->rescale();
    }
    p->update_all();
    p->list->SetScrollRate(0, FromDIP(10));
    p->list->FitInside();
    GetSizer()->SetSizeHints(this);
    Refresh();
}

void BoneInfillDialog::on_sys_color_changed()
{
    p->curve_revert->rescale();
    for (priv::Row &row : p->rows)
        row.revert->rescale();
    p->update_all();
    Refresh();
}

}} // namespace Slic3r::GUI
