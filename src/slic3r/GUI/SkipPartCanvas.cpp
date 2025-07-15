#include <GL/glew.h>
#include "SkipPartCanvas.hpp"

#include <opencv2/opencv.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/nowide/fstream.hpp>
#include <expat.h>
#include <earcut/earcut.hpp>
#include <libslic3r/Color.hpp>
#include <filesystem>

wxDEFINE_EVENT(EVT_ZOOM_PERCENT, wxCommandEvent);
wxDEFINE_EVENT(EVT_CANVAS_PART, wxCommandEvent);

namespace Slic3r {
namespace GUI {

SkipPartCanvas::SkipPartCanvas(wxWindow *parent, const wxGLAttributes& dispAttrs)
    : wxGLCanvas(parent, dispAttrs) {
    context_ = new wxGLContext(this);
    this->Bind(wxEVT_PAINT, &SkipPartCanvas::OnPaint, this);
    this->Bind(wxEVT_MOUSEWHEEL, &SkipPartCanvas::OnMouseWheel, this);
    this->Bind(wxEVT_LEFT_DOWN, &SkipPartCanvas::OnMouseLeftDown, this);
    this->Bind(wxEVT_LEFT_DCLICK, &SkipPartCanvas::OnMouseLeftDown, this);
    this->Bind(wxEVT_LEFT_UP, &SkipPartCanvas::OnMouseLeftUp, this);
    this->Bind(wxEVT_RIGHT_DOWN, &SkipPartCanvas::OnMouseRightDown, this);
    this->Bind(wxEVT_RIGHT_UP, &SkipPartCanvas::OnMouseRightUp, this);
    this->Bind(wxEVT_SIZE, &SkipPartCanvas::OnSize, this);
    this->Bind(wxEVT_MOTION, &SkipPartCanvas::OnMouseMotion, this);
}

void SkipPartCanvas::LoadPickImage(const std::string & path)
{
    if(!std::filesystem::exists(path)) return;

    auto ParseShapeId = [](cv::Mat image, const std::vector<std::vector<cv::Point>> &contours, const std::vector<cv::Vec4i> &hierarchy, int root_idx) -> uint32_t {
        cv::Mat mask = cv::Mat::zeros(image.size(), CV_8UC1);

        cv::drawContours(mask, contours, root_idx, 255, cv::FILLED);

        int child = hierarchy[root_idx][2];
        while (child != -1) {
            cv::drawContours(mask, contours, child, 0, cv::FILLED);
            child = hierarchy[child][0];
        }
        std::vector<cv::Vec3b> pixels;
        for (int y = 0; y < image.rows; ++y) {
            for (int x = 0; x < image.cols; ++x) {
                if (mask.at<uchar>(y, x)) { pixels.push_back(image.at<cv::Vec3b>(y, x)); }
            }
        }

        std::map<cv::Vec3b, int, std::function<bool(const cv::Vec3b &, const cv::Vec3b &)>> colorCount(
            [](const cv::Vec3b &a, const cv::Vec3b &b) { return std::lexicographical_compare(a.val, a.val + 3, b.val, b.val + 3); });

        for (auto &c : pixels) colorCount[c]++;

        cv::Vec3b main_color;
        int       max_count   = 0;
        int       total_count = 0;
        for (const auto &kv : colorCount) {
            if (kv.second > max_count) {
                max_count  = kv.second;
                main_color = kv.first;
            }
            total_count += kv.second;
        }

        SkipIdHelper helper{main_color[2], main_color[1], main_color[0]};
        helper.reverse();
        return (max_count * 2 > total_count) ? helper.value : 0;
    };

    parts_state_.clear();
    parts_triangles_.clear();
    pick_parts_.clear();
    int preffered_w{FromDIP(400)}, preffered_h{FromDIP(400)};
    cv::Mat src_image = cv::imread(path, cv::IMREAD_UNCHANGED);
    cv::cvtColor(src_image, src_image, cv::COLOR_BGRA2BGR); // remove alpha
    float zoom_x{static_cast<float>(preffered_w) / src_image.cols};
    float zoom_y{static_cast<float>(preffered_h) / src_image.rows};
    float image_scale{0};
    if (abs(zoom_x - 1) > abs(zoom_y - 1))
        image_scale = zoom_x;
    else
        image_scale = zoom_y;
    image_view_scale_ = 1 / image_scale;
    pick_image_ = src_image;
    std::vector<cv::Mat> channels;
    cv::Mat gray; // convert to gray
    cv::cvtColor(pick_image_, gray, cv::COLOR_BGR2GRAY);
    cv::Mat mask; // convery to binary
    cv::threshold(gray, mask, 0, 255, cv::THRESH_BINARY);
    std::vector<std::vector<cv::Point>> pick_counters;
    std::vector<cv::Vec4i>              hierarchy;
    cv::findContours(mask, pick_counters, hierarchy, cv::RETR_TREE, cv::CHAIN_APPROX_TC89_KCOS);
    auto compute_depth = [&](int idx) {
        int depth = 0;
        while (hierarchy[idx][3] != -1) {
            depth++;
            idx = hierarchy[idx][3];
        }
        return depth;
    };
    for (int i = 0; i < pick_counters.size(); ++i) {
        int depth  = compute_depth(i);
        int parent = hierarchy[i][3];
        if (parent != -1) continue;

        auto id = ParseShapeId(pick_image_, pick_counters, hierarchy, i);
        if (id > 0) {
            std::vector<FloatPoint>              flat_points;
            std::vector<std::vector<FloatPoint>> polygon;

            // part body
            {
                polygon.emplace_back();
                for (const auto &pt : pick_counters[i]) {
                    FloatPoint fp{pt.x * 1.0f, pt.y * 1.0f};
                    polygon.back().push_back(fp);
                    flat_points.push_back(fp);
                }
                int child = hierarchy[i][2];
                while (child != -1) {
                    polygon.emplace_back();
                    for (const auto &pt : pick_counters[child]) {
                        FloatPoint fp{pt.x * 1.0f, pt.y * 1.0f};
                        polygon.back().push_back(fp);
                        flat_points.push_back(fp);
                    }
                    child = hierarchy[child][0];
                }
                std::vector<uint32_t>   indices = mapbox::earcut<uint32_t>(polygon);
                std::vector<FloatPoint> final_counter;
                for (size_t j = 0; j < indices.size(); j += 3) {
                    final_counter.push_back(flat_points[indices[j]]);
                    final_counter.push_back(flat_points[indices[j + 1]]);
                    final_counter.push_back(flat_points[indices[j + 2]]);
                }

                parts_triangles_[id].emplace_back(final_counter);
            }
            // part outlines
            {
                pick_parts_[id].emplace_back(pick_counters[i]);
                int child = hierarchy[i][2];
                while (child != -1) {
                    pick_parts_[id].emplace_back(pick_counters[child]);
                    child = hierarchy[child][0];
                }
            }
            if (parts_state_.find(id) == parts_state_.end()) parts_state_.emplace(id, psUnCheck);
        }
    }
}

void SkipPartCanvas::ZoomIn(const int zoom_percent)
{
    SetZoomPercent(zoom_percent_ + zoom_percent);
    Refresh();
}

void SkipPartCanvas::ZoomOut(const int zoom_percent)
{
    SetZoomPercent(zoom_percent_ - zoom_percent);
    Refresh();
}

void SkipPartCanvas::SwitchDrag(const bool drag_on)
{
    fixed_draging_ = drag_on;
    AutoSetCursor();
}


void SkipPartCanvas::UpdatePartsInfo(const PartsInfo& parts)
{
    for (auto const& part : parts) {
        if (auto res = parts_state_.find(part.first); res != parts_state_.end())
            res->second = part.second;
    }
    Refresh();
}

void DrawRoundedRect(float x, float y, float width, float height, float radius, const ColorRGB& color, int segments = 16)
{
    glColor3f(color.r(), color.g(), color.b());

    // 1. Draw center rectangle
    glBegin(GL_QUADS);
    glVertex2f(x + radius, y + radius);
    glVertex2f(x + width - radius, y + radius);
    glVertex2f(x + width - radius, y + height - radius);
    glVertex2f(x + radius, y + height - radius);
    glEnd();

    // 2. Draw side rectangles (excluding corners)
    glBegin(GL_QUADS);
    // Left
    glVertex2f(x, y + radius);
    glVertex2f(x + radius, y + radius);
    glVertex2f(x + radius, y + height - radius);
    glVertex2f(x, y + height - radius);

    // Right
    glVertex2f(x + width - radius, y + radius);
    glVertex2f(x + width, y + radius);
    glVertex2f(x + width, y + height - radius);
    glVertex2f(x + width - radius, y + height - radius);

    // Top
    glVertex2f(x + radius, y + height - radius);
    glVertex2f(x + width - radius, y + height - radius);
    glVertex2f(x + width - radius, y + height);
    glVertex2f(x + radius, y + height);

    // Bottom
    glVertex2f(x + radius, y);
    glVertex2f(x + width - radius, y);
    glVertex2f(x + width - radius, y + radius);
    glVertex2f(x + radius, y + radius);
    glEnd();

    // 3. Draw corners
    auto drawCorner = [&](float cx, float cy, float startAngle) {
        glBegin(GL_TRIANGLE_FAN);
        glVertex2f(cx, cy);
        for (int i = 0; i <= segments; ++i) {
            float angle = startAngle + (M_PI * 0.5f) * (float)i / segments;
            glVertex2f(cx + cosf(angle) * radius, cy + sinf(angle) * radius);
        }
        glEnd();
    };

    drawCorner(x + radius, y + radius, M_PI);             // bottom-left
    drawCorner(x + width - radius, y + radius, 1.5f * M_PI); // bottom-right
    drawCorner(x + width - radius, y + height - radius, 0.0f); // top-right
    drawCorner(x + radius, y + height - radius, 0.5f * M_PI); // top-left
}


void SkipPartCanvas::Render()
{
    constexpr float border_w = 3.f;
    constexpr int  uncheckd_stencil =1;
    constexpr int  checkd_stencil = 2;
    constexpr int  skipped_stencil = 3;

    SetCurrent(*context_);
    glPushAttrib(GL_ALL_ATTRIB_BITS);

    int w, h;
    GetClientSize(&w, &h);
#if defined(__APPLE__)
    double scale = GetDPIScaleFactor();
    glViewport(0, 0, w * scale, h * scale);
#else
    glViewport(0, 0, w, h);
#endif
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    auto view_rect = ViewPtToImagePt(wxPoint(w, h));
    glOrtho(offset_.x, view_rect.x, view_rect.y, offset_.y, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClearColor(parent_color_.r(), parent_color_.g(), parent_color_.b(), 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    float rx = offset_.x;
    float ry = offset_.y;
    float rw = view_rect.x - offset_.x;
    float rh = view_rect.y - offset_.y;
    float radius = std::min(rw, rh) * 0.05f;

    DrawRoundedRect(rx, ry, rw, rh, radius, ColorRGB{0.9f, 0.9f, 0.9f});

    glEnable(GL_BLEND);
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glEnable(GL_STENCIL_TEST);

    auto draw_shape = [this, border_w](const int stencil, const PartState part_type, const ColorRGB& rgb) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilFunc(GL_ALWAYS, stencil, 0xFF);
        glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);

        for (const auto& contour : parts_triangles_) {
            auto part_info = parts_state_.find(contour.first);
            if (part_info == parts_state_.end() || part_info->second != part_type)
                continue;
            glColor3f(1, 1, 1);
            for (const auto &contour_item : contour.second) {
                glBegin(GL_TRIANGLES);
                for (size_t i = 0; i < contour_item.size(); i += 3) {
                    glVertex2f(contour_item[i][0], contour_item[i][1]);
                    glVertex2f(contour_item[i + 1][0], contour_item[i + 1][1]);
                    glVertex2f(contour_item[i + 2][0], contour_item[i + 2][1]);
                }
                glEnd();
            }
        }

        for (const auto& contour : pick_parts_) {
            if (contour.first != this->hover_id_) continue;
            auto part_info = parts_state_.find(contour.first);
            if (part_info == parts_state_.end() || part_info->second != part_type)
                continue;

            glColor3f(rgb.r(), rgb.g(), rgb.b());
            glLineWidth(border_w);
            for (const auto &contour_item : contour.second) {
                glBegin(GL_LINE_LOOP);
                for (const auto &pt : contour_item) { glVertex2f(pt.x, pt.y); }
                glEnd();
            }
        }
    };
    // draw unchecked shapes
    // stencil1 => unchecked
    draw_shape(uncheckd_stencil, psUnCheck, ColorRGB{0, 174 / 255.f, 66 / 255.f});

    // draw checked shapes
    // stencil2 => checked
    draw_shape(checkd_stencil, psChecked, ColorRGB{208 / 255.f, 27 / 255.f, 66 / 255.f});

    // draw skipped shapes
    // stencil3 => skipped
    draw_shape(skipped_stencil, psSkipped, ColorRGB{95 / 255.f, 95 / 255.f, 95 / 255.f});

    auto draw_mask = [this, view_rect, border_w, w, h](const int stencil, const PartState part_type,
        const ColorRGB& background, const ColorRGB& line, const ColorRGB& bound) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilFunc(GL_EQUAL, stencil, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);          // Don't change stencil
        glColor3f(background.r(), background.g(), background.b());
        glBegin(GL_POLYGON);
        glVertex2f(offset_.x, offset_.y);
        glVertex2f(offset_.x, view_rect.y);
        glVertex2f(view_rect.x, view_rect.y);
        glVertex2f(view_rect.x, offset_.y);
        glEnd();
        // draw main color
        glColor3f(line.r(), line.g(), line.b());
        // re-draw shape bound
        for (const auto& contour : pick_parts_) {
            auto part_info = parts_state_.find(contour.first);
            if (part_info == parts_state_.end() || part_info->second != part_type)
                continue;
            glColor3f(bound.r(), bound.g(), bound.b());
            glLineWidth(border_w);
            for (const auto &contour_item : contour.second) {
                glBegin(GL_LINE_LOOP);
                for (const auto &pt : contour_item) { glVertex2f(pt.x, pt.y); }
                glEnd();
            }
        }
    };

    draw_mask(checkd_stencil, psChecked, ColorRGB{239 / 255.f, 175 / 255.f, 175 / 255.f},
        ColorRGB{225 / 255.f, 71 / 255.f, 71 / 255.f}, ColorRGB{208 / 255.f, 27 / 255.f, 27 / 255.f});

    draw_mask(skipped_stencil, psSkipped, ColorRGB{159 / 255.f, 159 / 255.f, 159 / 255.f},
        ColorRGB{95 / 255.f, 95 / 255.f, 95 / 255.f}, ColorRGB{95 / 255.f, 95 / 255.f, 95 / 255.f});

    glDisable(GL_STENCIL_TEST);

    glPopAttrib();
    glFlush();
}

void SkipPartCanvas::DebugLogLine(std::string str)
{
    //if (!log_ctrl)
    //    return;
    //log_ctrl->AppendText(str + "\n");
}

void SkipPartCanvas::SendSelectEvent(int id, PartState state) {
    wxCommandEvent evt(EVT_CANVAS_PART);
    evt.SetExtraLong(id);
    evt.SetInt(static_cast<int>(state));
    wxPostEvent(this, evt);
}
void SkipPartCanvas::SendZoomEvent(int zoom_percent) {
    wxCommandEvent evt(EVT_ZOOM_PERCENT);
    evt.SetInt(zoom_percent_);
    wxPostEvent(this, evt);
}

inline double SkipPartCanvas::Zoom() const
{
    return zoom_percent_ / 100.0f;
}

inline wxPoint SkipPartCanvas::ViewPtToImagePt(const wxPoint& view_pt) const
{
    return wxPoint(view_pt.x * image_view_scale_ / Zoom(), view_pt.y * image_view_scale_ / Zoom()) + offset_;
}

uint32_t SkipPartCanvas::GetIdAtImagePt(const wxPoint& image_pt) const
{
    if (image_pt.x >= 0 && image_pt.x < pick_image_.cols
        && image_pt.y >= 0 && image_pt.y < pick_image_.rows) {
        // at(row, col)=>at(y, x)
        cv::Vec3b bgr = pick_image_.at<cv::Vec3b>(image_pt.y, image_pt.x);
        SkipIdHelper helper{bgr[2], bgr[1], bgr[0]};
        helper.reverse();
        return helper.value;
    } else {
        return 0;
    }
}

inline uint32_t SkipPartCanvas::GetIdAtViewPt(const wxPoint& view_pt) const
{
    wxPoint pt_at_image = ViewPtToImagePt(view_pt);
    return GetIdAtImagePt(pt_at_image);
}

void SkipPartCanvas::SetZoomPercent(const int value)
{
    zoom_percent_ = std::clamp(value, 100, 1000);
    std::ostringstream oss;
    oss << "zoom to " << zoom_percent_;
    DebugLogLine(oss.str());

    SendZoomEvent(zoom_percent_);
}

void SkipPartCanvas::SetOffset(const wxPoint& value)
{
    int w, h;
    GetClientSize(&w, &h);
    int max_w = static_cast<int>(w * (1 - 1 / Zoom())) >= 0 ? static_cast<int>(w * (1 - 1 / Zoom())) : 0;
    int max_h = static_cast<int>(w * (1 - 1 / Zoom())) >= 0 ? static_cast<int>(h * (1 - 1 / Zoom())) : 0;
    offset_.x = std::clamp(value.x, 0, max_w);
    offset_.y = std::clamp(value.y, 0, max_h);
}

void SkipPartCanvas::AutoSetCursor()
{
    if(is_draging_ || fixed_draging_)
        SetCursor(wxCursor(wxCURSOR_HAND));
    else
        SetCursor(wxCursor(wxCURSOR_NONE));
}

void SkipPartCanvas::StartDrag(const wxPoint& mouse_pt)
{
    drag_start_pt_ = mouse_pt;
    drag_start_offset_ = offset_;
    is_draging_ = true;
    AutoSetCursor();
}

void SkipPartCanvas::ProcessDrag(const wxPoint& mouse_pt)
{
    wxPoint drag_offset = (mouse_pt - drag_start_pt_) * image_view_scale_;
    SetOffset(- wxPoint(drag_offset.x / Zoom(), drag_offset.y / Zoom()) + drag_start_offset_);
    Refresh();
}

void SkipPartCanvas::ProcessHover(const wxPoint& mouse_pt)
{
    auto id_at_mouse = GetIdAtViewPt(mouse_pt);
    int new_hover_id { -1 };
    auto part_state = parts_state_.find(id_at_mouse);
    if (part_state != parts_state_.end() && part_state->second == psUnCheck) {
        new_hover_id = id_at_mouse;
    };
    if (new_hover_id != this->hover_id_) {
        this->hover_id_ = new_hover_id;
        Refresh();
    }
}

void SkipPartCanvas::EndDrag()
{
    is_draging_ = false;
    AutoSetCursor();
}

 void SkipPartCanvas::OnPaint(wxPaintEvent &event)
 {
    wxPaintDC dc(this);
    if (!IsShown()) return;

    SetCurrent(*context_);

    Render();
    SwapBuffers();
 }

void SkipPartCanvas::OnSize(wxSizeEvent& event)
{
    event.Skip();
}

void SkipPartCanvas::OnMouseLeftDown(wxMouseEvent& event)
{
    DebugLogLine("OnMouseLeftDown");
    if (!event.LeftIsDown()) {
        event.Skip();
        DebugLogLine("skip----OnMouseLeftDown");
        return;
    }
    if (fixed_draging_)
        StartDrag(wxPoint(event.GetX(), event.GetY()));
    left_down_ = true;
}

void SkipPartCanvas::OnMouseLeftUp(wxMouseEvent& event)
{
    DebugLogLine("OnMouseLeftUp");
    if (event.LeftIsDown() || !left_down_) {
        event.Skip();
        DebugLogLine("skip----OnMouseLeftUp");
        return;
    }
    auto id_at_mouse = GetIdAtViewPt(wxPoint(event.GetX(), event.GetY()));
    auto part_state = parts_state_.find(id_at_mouse);
    if (part_state != parts_state_.end() && part_state->second != psSkipped) {
        if (part_state->second == psUnCheck)
            part_state = parts_state_.insert_or_assign(part_state->first, psChecked).first;
        else
            part_state = parts_state_.insert_or_assign(part_state->first, psUnCheck).first;
        // if (select_callback_)
        //     select_callback_(part_state->first, part_state->second);
        SendSelectEvent(part_state->first, part_state->second);
    }
    left_down_ = false;
    if (fixed_draging_)
        EndDrag();
    else {
        Refresh();
    }
}

void SkipPartCanvas::OnMouseRightDown(wxMouseEvent& event)
{
    DebugLogLine("OnMouseRightDown");
    if (!event.RightIsDown()) {
        event.Skip();
        DebugLogLine("skip----OnMouseRightDown");
        return;
    }
    StartDrag(wxPoint(event.GetX(), event.GetY()));
}

void SkipPartCanvas::OnMouseRightUp(wxMouseEvent& event)
{
    DebugLogLine("OnMouseRightUp");
    if (event.RightIsDown() || !is_draging_) {
        event.Skip();
        DebugLogLine("skip----OnMouseRightUp");
        return;
    }
    EndDrag();
}

void SkipPartCanvas::OnMouseMotion(wxMouseEvent& event)
{
    ProcessHover(wxPoint(event.GetX(), event.GetY()));
    if (!event.RightIsDown() && !(event.LeftIsDown() && fixed_draging_)) {
        event.Skip();
        return;
    }
    ProcessDrag(wxPoint(event.GetX(), event.GetY()));
}

void SkipPartCanvas::OnMouseWheel(wxMouseEvent& event)
{
    wxPoint view_mouse = wxPoint(event.GetX(), event.GetY());
    auto pre_image_pos = ViewPtToImagePt(view_mouse);
    SetZoomPercent(zoom_percent_ + 10 * (event.GetWheelRotation() / 120.0));
    auto now_image_pos = ViewPtToImagePt(view_mouse);
    SetOffset(offset_ - (now_image_pos - pre_image_pos));
    Refresh();
}

// Base class with error messages management

void _BBS_3MF_Base::add_error(const std::string &error) const
{
    boost::unique_lock l(mutex);
    m_errors.push_back(error);
}
void _BBS_3MF_Base::clear_errors() { m_errors.clear(); }

void _BBS_3MF_Base::log_errors()
{
    for (const std::string &error : m_errors) BOOST_LOG_TRIVIAL(error) << error;
}


ModelSettingHelper::ModelSettingHelper(const std::string &path) : path_(path) {}

bool ModelSettingHelper::Parse()
{
    boost::nowide::fstream fs(path_);
    if (!fs) {
        add_error("Failed to open file\n");
        return false;
    }
    XML_Parser parser = XML_ParserCreate(nullptr);
    if (!parser) {
        add_error("Unable to create parser");
        return false;
    }
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, ModelSettingHelper::StartElementHandler, ModelSettingHelper::EndElementHandler);

    try {
        char buffer[4000];
        while (fs.read(buffer, sizeof(buffer)) || fs.gcount() > 0) {
            auto ret = XML_Parse(parser, buffer, static_cast<int>(fs.gcount()), fs.eof());
            if (ret != XML_STATUS_OK) {
                add_error("return value of XML_Parse doesn't match XM_STATUS_OK");
                XML_ParserFree(parser);
                return false;
            }
        }
    }
    catch (std::exception& e) {
        add_error(std::string("exception:") + e.what());
        XML_ParserFree(parser);
        return false;
    }
    XML_ParserFree(parser);
    return true;
}

void XMLCALL ModelSettingHelper::StartElementHandler(void *userData, const XML_Char *name, const XML_Char **atts)
{
    ModelSettingHelper *self = static_cast<ModelSettingHelper *>(userData);
    if (strcmp(name, "plate") == 0) {
        self->context_.current_plate = PlateInfo(); // start a new plate
        self->context_.in_plate      = true;
    } else if (strcmp(name, "metadata") == 0 && self->context_.in_plate) {
        std::string key, value;
        for (int i = 0; atts[i]; i += 2) {
            if (strcmp(atts[i], "key") == 0) key = atts[i + 1];
            if (strcmp(atts[i], "value") == 0) value = atts[i + 1];
        }
        if (key == "index") { self->context_.current_plate.index = std::stoi(value); }
        if (key == "label_object_enabled") { self->context_.current_plate.label_object_enabled = value == "true"; }
    } else if (strcmp(name, "object") == 0 && self->context_.in_plate) {
        ObjectInfo obj;
        for (int i = 0; atts[i]; i += 2) {
            if (strcmp(atts[i], "identify_id") == 0) obj.identify_id = atoi(atts[i + 1]);
            if (strcmp(atts[i], "name") == 0) obj.name = atts[i + 1];
        }
        self->context_.current_plate.objects.push_back(obj);
    }
}

void XMLCALL ModelSettingHelper::EndElementHandler(void *userData, const XML_Char *name)
{
    ModelSettingHelper *self = static_cast<ModelSettingHelper *>(userData);
    if (strcmp(name, "plate") == 0 && self->context_.in_plate) {
        self->context_.plates.push_back(self->context_.current_plate);
        self->context_.current_plate = PlateInfo(); // reset
        self->context_.in_plate      = false;
    }
}

std::vector<ObjectInfo> ModelSettingHelper::GetPlateObjects(int plate_idx) {
    for (const auto &plate : context_.plates) {
        if (plate.index == plate_idx) {
            return plate.objects;
        }
    }
    return std::vector<ObjectInfo>();
}

bool ModelSettingHelper::GetLabelObjectEnabled(int plate_idx)
{
    for (const auto &plate : context_.plates) {
        if (plate.index == plate_idx) { return plate.label_object_enabled; }
    }
    return false;
}

void ModelSettingHelper::DataHandler(const XML_Char *s, int len)
{
    // do nothing
}
}
}