#include "MultipartImportDialog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/dcbuffer.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/pen.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/string.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/LabeledCheckBox.hpp"
#include "Widgets/StateColor.hpp"
#include "format.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r { namespace GUI {

namespace {

const wxColour BgNormalColor("#FFFFFF");
const wxColour BgSelectColor("#E5F0EE");
const wxColour BorderNormalColor("#CECECE");
const wxColour BorderSelectedColor("#009688");
const wxColour TextTitleColor("#262E30");
const wxColour TextDetailColor("#6B6B6B");

// Preview size in DIP.
constexpr int PreviewWidth  = 260;
constexpr int PreviewHeight = 180;
// The picture is drawn this many times larger and scaled down, which smooths its edges.
constexpr int Supersample = 2;
// Space between separate objects in the preview, in mm.
constexpr double ObjectGap = 6.;

// Part colours, so that the same part has the same colour in both previews.
const std::array<wxColour, 8> PartColours = {wxColour("#F28C28"), wxColour("#3B82F6"), wxColour("#E5484D"), wxColour("#8A9099"),
                                             wxColour("#2BB673"), wxColour("#F5C518"), wxColour("#A855F7"), wxColour("#F2F2F2")};

// A model part in world coordinates. object is the index of the ModelObject it came from.
struct ScenePart
{
    std::vector<Vec3f>   vertices;
    std::vector<stl_triangle_vertex_indices> indices;
    BoundingBoxf3        bbox;
    size_t               object;
};

// Isometric camera looking at the plate from the front left.
struct Camera
{
    Vec3d  forward = Vec3d(0.55, 1., -0.75).normalized();
    Vec3d  right   = Vec3d(forward.y(), -forward.x(), 0.).normalized();
    Vec3d  up      = right.cross(forward);
    double scale   = 1.;
    Vec2d  shift   = Vec2d::Zero();

    Vec3d project(const Vec3d& p) const { return {p.dot(right) * scale + shift.x(), -p.dot(up) * scale + shift.y(), p.dot(forward)}; }
};

class Raster
{
public:
    Raster(int width, int height, const wxColour& background)
        : m_width(width), m_height(height), m_image(width, height), m_depth(size_t(width) * height, std::numeric_limits<float>::max())
    {
        m_image.SetRGB(wxRect(0, 0, width, height), background.Red(), background.Green(), background.Blue());
    }

    // a, b and c are screen positions with depth in z. shade(w) returns the colour at barycentric weights w.
    template<class Shade> void fill_triangle(const Vec3d& a, const Vec3d& b, const Vec3d& c, Shade&& shade)
    {
        const double area = (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
        if (std::abs(area) < 1e-9)
            return;
        const int x0 = std::max(0, int(std::floor(std::min({a.x(), b.x(), c.x()}))));
        const int x1 = std::min(m_width - 1, int(std::ceil(std::max({a.x(), b.x(), c.x()}))));
        const int y0 = std::max(0, int(std::floor(std::min({a.y(), b.y(), c.y()}))));
        const int y1 = std::min(m_height - 1, int(std::ceil(std::max({a.y(), b.y(), c.y()}))));
        unsigned char* rgb = m_image.GetData();
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double px = x + 0.5, py = y + 0.5;
                const double w0 = ((b.x() - px) * (c.y() - py) - (b.y() - py) * (c.x() - px)) / area;
                const double w1 = ((c.x() - px) * (a.y() - py) - (c.y() - py) * (a.x() - px)) / area;
                const double w2 = 1. - w0 - w1;
                if (w0 < 0. || w1 < 0. || w2 < 0.)
                    continue;
                const float  depth = float(w0 * a.z() + w1 * b.z() + w2 * c.z());
                const size_t idx   = size_t(y) * m_width + x;
                if (depth >= m_depth[idx])
                    continue;
                m_depth[idx]       = depth;
                const wxColour col = shade(Vec3d(w0, w1, w2));
                rgb[idx * 3]       = col.Red();
                rgb[idx * 3 + 1]   = col.Green();
                rgb[idx * 3 + 2]   = col.Blue();
            }
    }

    // A dashed line drawn over everything.
    void dashed_line(const Vec3d& a, const Vec3d& b, const wxColour& colour, double dash, double width)
    {
        const double length = (b - a).head<2>().norm();
        for (double t = 0.; t <= length; t += 0.5) {
            if (std::fmod(t, 2. * dash) > dash)
                continue;
            const Vec2d p = a.head<2>() + (b - a).head<2>() * (length > 0. ? t / length : 0.);
            for (int dy = 0; dy < int(width); ++dy)
                for (int dx = 0; dx < int(width); ++dx) {
                    const int x = int(p.x()) + dx, y = int(p.y()) + dy;
                    if (x >= 0 && y >= 0 && x < m_width && y < m_height)
                        m_image.SetRGB(x, y, colour.Red(), colour.Green(), colour.Blue());
                }
        }
    }

    wxImage& image() { return m_image; }

private:
    int                m_width;
    int                m_height;
    wxImage            m_image;
    std::vector<float> m_depth;
};

wxColour scaled(const wxColour& c, double f)
{
    auto channel = [f](unsigned char v) { return (unsigned char) std::clamp(int(std::lround(v * f)), 0, 255); };
    return {channel(c.Red()), channel(c.Green()), channel(c.Blue())};
}

} // namespace

struct MultipartImportDialog::Scene
{
    BoundingBoxf           bed;
    std::vector<ScenePart> parts;
    size_t                 objects{0};
    BoundingBoxf3          bbox;

    // Where each object goes for each choice, as a translation of its world position.
    Vec3d              single_offset = Vec3d::Zero();
    std::vector<Vec3d> separate_offsets;
    std::vector<Vec3d> in_place_offsets;

    Scene(const Model& model, const BoundingBoxf& bed_area) : bed(bed_area)
    {
        objects = model.objects.size();
        for (size_t idx = 0; idx < objects; ++idx) {
            const ModelObject* object = model.objects[idx];
            // An object the file does not place has no instance; it is shown where its mesh is.
            std::vector<Transform3d> instances;
            for (const ModelInstance* instance : object->instances)
                instances.emplace_back(instance->get_matrix());
            if (instances.empty())
                instances.emplace_back(Transform3d::Identity());
            for (const Transform3d& instance : instances)
                for (const ModelVolume* volume : object->volumes) {
                    if (!volume->is_model_part())
                        continue;
                    // As Model::convert_multipart_object() does: undo the centering of the object, then place it.
                    Geometry::Transformation volume_trafo = volume->get_transformation();
                    volume_trafo.set_offset(volume_trafo.get_offset() - object->origin_translation);
                    const Transform3d          trafo = instance * volume_trafo.get_matrix();
                    const indexed_triangle_set& its  = volume->mesh().its;
                    ScenePart                   part{{}, its.indices, {}, idx};
                    part.vertices.reserve(its.vertices.size());
                    for (const stl_vertex& v : its.vertices) {
                        const Vec3d p = trafo * v.cast<double>();
                        part.vertices.emplace_back(p.cast<float>());
                        part.bbox.merge(p);
                    }
                    if (part.bbox.defined) {
                        bbox.merge(part.bbox);
                        parts.emplace_back(std::move(part));
                    }
                }
        }
        layout();
    }

    void layout()
    {
        const Vec2d bed_center = bed.center();
        if (!bbox.defined)
            return;
        // One object: the parts keep their places and the whole rests on the plate, in its middle.
        single_offset = Vec3d(bed_center.x() - bbox.center().x(), bed_center.y() - bbox.center().y(), -bbox.min.z());
        // Without Auto-Drop, the separate objects keep their places too.
        in_place_offsets.assign(objects, single_offset);

        // Separate objects: each one rests on the plate, and they are arranged in rows around the middle of the plate.
        std::vector<BoundingBoxf3> boxes(objects);
        for (const ScenePart& part : parts)
            boxes[part.object].merge(part.bbox);
        const double     row_limit = std::max(bed.size().x(), 1.);
        std::vector<Vec2d> corner(objects, Vec2d::Zero());
        double           x = 0., y = 0., row_depth = 0., width = 0.;
        for (size_t idx = 0; idx < objects; ++idx) {
            if (!boxes[idx].defined)
                continue;
            const Vec3d size = boxes[idx].size();
            if (x > 0. && x + size.x() > row_limit) {
                x = 0.;
                y += row_depth + ObjectGap;
                row_depth = 0.;
            }
            corner[idx] = Vec2d(x, y);
            width       = std::max(width, x + size.x());
            row_depth   = std::max(row_depth, size.y());
            x += size.x() + ObjectGap;
        }
        const Vec2d start = bed_center - Vec2d(width, y + row_depth) / 2.;
        separate_offsets.assign(objects, Vec3d::Zero());
        for (size_t idx = 0; idx < objects; ++idx)
            if (boxes[idx].defined)
                separate_offsets[idx] = Vec3d(start.x() + corner[idx].x() - boxes[idx].min.x(), start.y() + corner[idx].y() - boxes[idx].min.y(),
                                              -boxes[idx].min.z());
    }

    // Draws the plate with the parts moved by offsets (one per object). group draws a dashed box around all parts.
    wxImage render(const wxSize& size, const std::vector<Vec3d>& offsets, bool group, bool dark) const
    {
        const wxColour background = dark ? wxColour("#2D2D31") : wxColour("#F2F3F5");
        const wxColour plate      = dark ? wxColour("#45474D") : wxColour("#D5D8DD");
        const wxColour grid       = dark ? wxColour("#55585F") : wxColour("#C2C6CC");
        const int      width = size.x * Supersample, height = size.y * Supersample;

        const double thickness = std::max(bed.size().x(), bed.size().y()) * 0.02;
        const Vec3d  b0(bed.min.x(), bed.min.y(), 0.), b1(bed.max.x(), bed.min.y(), 0.), b2(bed.max.x(), bed.max.y(), 0.),
            b3(bed.min.x(), bed.max.y(), 0.);
        const Vec3d down(0., 0., -thickness);

        // Fit the plate and the parts into the picture.
        Camera             camera;
        std::vector<Vec3d> extent = {b0, b1, b2, b3, b0 + down, b1 + down, b3 + down};
        BoundingBoxf3      moved;
        for (const ScenePart& part : parts) {
            BoundingBoxf3 box = part.bbox;
            box.translate(offsets[part.object]);
            moved.merge(box);
        }
        if (moved.defined)
            for (int corner = 0; corner < 8; ++corner)
                extent.emplace_back(corner & 1 ? moved.max.x() : moved.min.x(), corner & 2 ? moved.max.y() : moved.min.y(),
                                    corner & 4 ? moved.max.z() : moved.min.z());
        BoundingBoxf screen;
        for (const Vec3d& p : extent)
            screen.merge(camera.project(p).head<2>());
        const double margin = 10. * Supersample;
        camera.scale        = std::min((width - 2. * margin) / std::max(screen.size().x(), 1e-6),
                                       (height - 2. * margin) / std::max(screen.size().y(), 1e-6));
        camera.shift        = Vec2d(width, height) / 2. - screen.center() * camera.scale;

        Raster raster(width, height, background);

        // The plate: its top with a grid, and the two sides facing the camera.
        const double step = std::max(10., std::pow(10., std::floor(std::log10(std::max(bed.size().x(), 1.)))) / 2.);
        const double line = 1.2 / camera.scale;
        auto         top  = [&](const Vec3d& p, const Vec3d& q, const Vec3d& r) {
            raster.fill_triangle(camera.project(p), camera.project(q), camera.project(r), [&](const Vec3d& w) {
                const Vec3d world = w.x() * p + w.y() * q + w.z() * r;
                const bool  on_grid = std::fmod(world.x() - bed.min.x() + line / 2., step) < line ||
                                     std::fmod(world.y() - bed.min.y() + line / 2., step) < line;
                return on_grid ? grid : plate;
            });
        };
        top(b0, b1, b2);
        top(b0, b2, b3);
        auto side = [&](const Vec3d& p, const Vec3d& q, double shade) {
            const wxColour colour = scaled(plate, shade);
            auto           flat   = [&colour](const Vec3d&) { return colour; };
            raster.fill_triangle(camera.project(p), camera.project(q), camera.project(q + down), flat);
            raster.fill_triangle(camera.project(p), camera.project(q + down), camera.project(p + down), flat);
        };
        side(b0, b1, 0.8);
        side(b3, b0, 0.65);

        // The parts, flat shaded with a light from above the camera.
        const Vec3d light = (camera.up * 0.8 - camera.forward * 0.5 - camera.right * 0.3).normalized();
        for (const ScenePart& part : parts) {
            const Vec3d     offset = offsets[part.object];
            const wxColour& colour = PartColours[part.object % PartColours.size()];
            for (const stl_triangle_vertex_indices& face : part.indices) {
                const Vec3d a = part.vertices[face[0]].cast<double>() + offset;
                const Vec3d b = part.vertices[face[1]].cast<double>() + offset;
                const Vec3d c = part.vertices[face[2]].cast<double>() + offset;
                const Vec3d normal = (b - a).cross(c - a);
                if (normal.squaredNorm() <= 0.)
                    continue;
                const wxColour shaded = scaled(colour, 0.45 + 0.6 * std::abs(normal.normalized().dot(light)));
                raster.fill_triangle(camera.project(a), camera.project(b), camera.project(c), [&shaded](const Vec3d&) { return shaded; });
            }
        }

        if (group && moved.defined) {
            BoundingBoxf3 box = moved;
            box.offset(2.);
            box.min.z() = 0.;
            auto corner = [&box](int i) {
                return Vec3d(i & 1 ? box.max.x() : box.min.x(), i & 2 ? box.max.y() : box.min.y(), i & 4 ? box.max.z() : box.min.z());
            };
            for (int i = 0; i < 8; ++i)
                for (int axis : {1, 2, 4})
                    if (!(i & axis))
                        raster.dashed_line(camera.project(corner(i)), camera.project(corner(i | axis)), BorderSelectedColor, 4. * Supersample,
                                           1.5 * Supersample);
        }

        return raster.image().Scale(size.x, size.y, wxIMAGE_QUALITY_HIGH);
    }
};

MultipartImportCard::MultipartImportCard(wxWindow* parent, const wxString& title, const wxString& detail) : wxPanel(parent)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    const int margin = FromDIP(12);

    m_preview = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(PreviewWidth), FromDIP(PreviewHeight)));
    m_title   = new Label(this, Label::Head_14, title);
    m_detail  = new Label(this, Label::Body_12, detail);
    m_detail->Wrap(FromDIP(PreviewWidth));

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_preview, 0, wxLEFT | wxRIGHT | wxTOP, margin);
    sizer->Add(m_title, 0, wxLEFT | wxRIGHT | wxTOP, margin);
    sizer->Add(m_detail, 0, wxLEFT | wxRIGHT | wxTOP, margin);
    sizer->AddSpacer(margin);
    SetSizer(sizer);

    // A click on a child selects the whole card, so pass it on to the panel.
    auto forward = [this](wxMouseEvent& event) {
        wxMouseEvent forwarded(event);
        forwarded.SetEventObject(this);
        forwarded.SetId(GetId());
        ProcessWindowEvent(forwarded);
    };
    for (wxWindow* child : {static_cast<wxWindow*>(m_preview), static_cast<wxWindow*>(m_title), static_cast<wxWindow*>(m_detail)}) {
        child->Bind(wxEVT_LEFT_DOWN, forward);
        child->Bind(wxEVT_LEFT_DCLICK, forward);
    }

    Bind(wxEVT_PAINT, &MultipartImportCard::on_paint, this);
    Bind(wxEVT_ENTER_WINDOW, &MultipartImportCard::on_hover, this);
    Bind(wxEVT_LEAVE_WINDOW, &MultipartImportCard::on_hover, this);
    update_colours();
}

void MultipartImportCard::set_preview(const wxBitmap& preview) { m_preview->SetBitmap(preview); }

void MultipartImportCard::set_detail(const wxString& detail)
{
    m_detail->SetLabel(detail);
    m_detail->Wrap(FromDIP(PreviewWidth));
    Layout();
}

void MultipartImportCard::select(bool selected)
{
    m_selected = selected;
    update_colours();
}

void MultipartImportCard::on_hover(wxMouseEvent& event)
{
    // Moving onto a child leaves the panel; the card stays hovered while the pointer is inside it.
    const bool hover = GetClientRect().Contains(ScreenToClient(wxGetMousePosition()));
    if (hover != m_hover) {
        m_hover = hover;
        Refresh();
    }
    event.Skip();
}

void MultipartImportCard::update_colours()
{
    const wxColour bg = StateColor::darkModeColorFor(m_selected ? BgSelectColor : BgNormalColor);
    for (wxWindow* window : {static_cast<wxWindow*>(this), static_cast<wxWindow*>(m_preview), static_cast<wxWindow*>(m_title),
                             static_cast<wxWindow*>(m_detail)})
        window->SetBackgroundColour(bg);
    m_title->SetForegroundColour(StateColor::darkModeColorFor(TextTitleColor));
    m_detail->SetForegroundColour(StateColor::darkModeColorFor(TextDetailColor));
    Refresh();
}

void MultipartImportCard::on_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc)
        return;
    const wxRect   rect   = GetClientRect();
    const bool     active = m_hover || m_selected;
    const wxColour border = StateColor::darkModeColorFor(active ? BorderSelectedColor : BorderNormalColor);
    gc->SetBrush(wxBrush(GetBackgroundColour()));
    gc->SetPen(wxPen(border, m_selected ? FromDIP(2) : 1));
    const double inset = m_selected ? FromDIP(2) / 2. + 0.5 : 1.;
    gc->DrawRoundedRectangle(inset, inset, rect.width - 2 * inset, rect.height - 2 * inset, FromDIP(8));
}

MultipartImportDialog::MultipartImportDialog(
    wxWindow* parent, const Model& model, const BoundingBoxf& bed, const wxString& title, const wxString& question, bool show_auto_drop)
    : DPIDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_scene(std::make_unique<Scene>(model, bed))
{
    SetBackgroundColour(*wxWHITE);
    const int gap = FromDIP(20);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* question_label = new Label(this, Label::Head_14, question, LB_AUTO_WRAP);
    question_label->Wrap(FromDIP(2 * PreviewWidth + 48));
    sizer->Add(question_label, 0, wxLEFT | wxRIGHT | wxTOP, gap);

    const int parts = int(m_scene->objects);
    m_single_card   = new MultipartImportCard(this, _L("Load as a single object"),
                                              format_wxstr(_L_PLURAL("The %1% part keeps its position and moves with the object. You can split it later.",
                                                                     "The %1% parts keep their positions and move together. You can split them later.", parts),
                                                           parts));
    m_separate_card = new MultipartImportCard(this, _L("Load as separate objects"), wxEmptyString);

    auto* cards = new wxBoxSizer(wxHORIZONTAL);
    cards->Add(m_single_card, 1, wxEXPAND | wxRIGHT, FromDIP(12));
    cards->Add(m_separate_card, 1, wxEXPAND);
    sizer->Add(cards, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

    for (auto [card, single] : {std::make_pair(m_single_card, true), std::make_pair(m_separate_card, false)}) {
        card->Bind(wxEVT_LEFT_DOWN, [this, single = single](wxMouseEvent&) { select(single); });
        card->Bind(wxEVT_LEFT_DCLICK, [this, single = single](wxMouseEvent&) {
            select(single);
            EndModal(wxID_OK);
        });
    }

    if (show_auto_drop) {
        m_auto_drop = new LabeledCheckBox(this, _L("Auto-Drop"));
        m_auto_drop->SetValue(true);
        m_auto_drop->SetToolTip(_L("Place every object on the plate. When disabled, the objects keep their heights."));
        m_auto_drop->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
            update_previews();
            event.Skip();
        });
        sizer->Add(m_auto_drop, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    }

    auto* buttons = new DialogButtons(this, {"OK"});
    buttons->GetOK()->SetLabel(_L("Load"));
    buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    sizer->Add(buttons, 0, wxEXPAND | wxTOP, FromDIP(8));

    select(true);
    update_previews();

    SetSizerAndFit(sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

MultipartImportDialog::~MultipartImportDialog() = default;

bool MultipartImportDialog::auto_drop() const { return m_auto_drop == nullptr || m_auto_drop->GetValue(); }

void MultipartImportDialog::select(bool single_object)
{
    m_single_object = single_object;
    m_single_card->select(single_object);
    m_separate_card->select(!single_object);
}

void MultipartImportDialog::update_previews()
{
    const bool   dark = wxGetApp().dark_mode();
    const double factor = GetContentScaleFactor();
    const wxSize size(int(std::lround(FromDIP(PreviewWidth) * factor)), int(std::lround(FromDIP(PreviewHeight) * factor)));
    const std::vector<Vec3d> single(m_scene->objects, m_scene->single_offset);
    m_single_card->set_preview(wxBitmap(m_scene->render(size, single, true, dark), -1, factor));

    const bool drop = auto_drop();
    m_separate_card->set_preview(wxBitmap(m_scene->render(size, drop ? m_scene->separate_offsets : m_scene->in_place_offsets, false, dark), -1, factor));
    m_separate_card->set_detail(drop ? _L("Each part becomes its own object, lying on the plate. On an empty plate they are arranged next to each other.") :
                                       _L("Each part becomes its own object and keeps its height above the plate."));
    Layout();
}

void MultipartImportDialog::on_dpi_changed(const wxRect&)
{
    update_previews();
    GetSizer()->SetSizeHints(this);
    Refresh();
}

void MultipartImportDialog::on_sys_color_changed()
{
    m_single_card->select(m_single_object);
    m_separate_card->select(!m_single_object);
    update_previews();
}

}} // namespace Slic3r::GUI
