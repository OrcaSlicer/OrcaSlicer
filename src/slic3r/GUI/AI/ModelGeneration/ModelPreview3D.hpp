#pragma once

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelection.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionRefinement.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "ModelColorPreviewShader.hpp"
#include "ModelPreviewPalette.hpp"
#include "ModelPreviewColorControls.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <unordered_set>
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/log/trivial.hpp>
#include <boost/filesystem/fstream.hpp>
#include <glad/gl.h>
#include <wx/dcclient.h>
#include <wx/glcanvas.h>
#include <wx/panel.h>
#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/timer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r::GUI {
class ModelPreview3D final : public wxPanel
{
public:
    enum class SelectionGesture { Lasso, Brush, Protect, Similar, Orbit };
    using SelectionState = AI::SurfaceSelectionPersistence::SelectionState;
    using FaceColorOverrides = AI::SurfaceSelectionPersistence::FaceColorOverrides;
    explicit ModelPreview3D(wxWindow* parent)
        : wxPanel(parent)
    {
        SetBackgroundColour(wxGetApp().get_window_default_clr());
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_canvas = OpenGLManager::create_wxglcanvas(*this);
        m_canvas->SetMinSize(wxSize(FromDIP(360), FromDIP(300)));
        sizer->Add(m_canvas, 1, wxEXPAND);
        m_region_prepare_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
        sizer->Add(m_region_prepare_status, 0, wxEXPAND | wxALL, FromDIP(6));
        m_region_prepare_status->Hide();
        m_region_prepare_timer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { finish_region_preparation(); }, m_region_prepare_timer.GetId());
        m_surface_timer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { finish_surface_selection(); }, m_surface_timer.GetId());
        m_color_trial = new ModelPreviewColorControls(this);
        sizer->Add(m_color_trial, 0, wxEXPAND);
        m_color_trial->Hide();
        m_color_trial->on_changed = [this] {
            m_trial_toggle_started = std::chrono::steady_clock::now();
            m_color_trial_enabled = m_color_trial->enabled();
            m_trial_palette = m_color_trial->colors();
            m_canvas->Refresh(false);
            BOOST_LOG_TRIVIAL(info) << "AI color trial toggled: enabled=" << m_color_trial_enabled
                << ", palette=" << m_trial_palette.size() << ", geometry_reloaded=false";
        };
        SetSizer(sizer);

        m_context = wxGetApp().init_glcontext(*m_canvas);
        m_canvas->Bind(wxEVT_PAINT, &ModelPreview3D::on_paint, this);
        m_canvas->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            m_canvas->Refresh(false);
            event.Skip();
        });
        m_canvas->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            m_dragging = true;
            m_drag_moved = false;
            m_drag_start = event.GetPosition();
            m_last_mouse = event.GetPosition();
            m_drawing_selection = m_selection_enabled && !event.AltDown() &&
                m_selection_gesture != SelectionGesture::Orbit && m_selection_gesture != SelectionGesture::Similar;
            if (m_drawing_selection) {
                m_stroke.clear();
                m_stroke.emplace_back(event.GetX(), event.GetY());
                m_canvas->Refresh(false);
            }
            if (!m_canvas->HasCapture())
                m_canvas->CaptureMouse();
            m_canvas->SetFocus();
        });
        m_canvas->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            if (m_drawing_selection) {
                m_stroke.emplace_back(event.GetX(), event.GetY());
                submit_surface_selection();
            } else if (m_selection_enabled && !m_drag_moved && !event.AltDown() &&
                       m_selection_gesture == SelectionGesture::Similar)
                select_at(event.GetPosition());
            finish_drag();
        });
        m_canvas->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
            if (!wxGetMouseState().LeftIsDown())
                finish_drag();
        });
        m_canvas->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (event.RightIsDown() && m_canvas->HasCapture()) {
                const wxPoint current = event.GetPosition();
                const double radius = std::max(0.001, 0.5 * m_bounds.size().norm());
                const double scale = 2.0 * fitted_half_height(m_canvas->GetClientSize().x, m_canvas->GetClientSize().y)
                    / std::max(1, m_canvas->GetClientSize().y) / radius;
                m_pan_x += (current.x - m_last_mouse.x) * scale;
                m_pan_y -= (current.y - m_last_mouse.y) * scale;
                m_last_mouse = current; m_canvas->Refresh(false); return;
            }
            if (!m_dragging || !event.LeftIsDown())
                return;
            const wxPoint current = event.GetPosition();
            if (m_drawing_selection) {
                if ((Vec2d(current.x, current.y) - m_stroke.back()).squaredNorm() >= 4.0)
                    m_stroke.emplace_back(current.x, current.y);
                m_canvas->Refresh(false);
                return;
            }
            if (!m_drag_moved) {
                const wxPoint distance = current - m_drag_start;
                m_drag_moved = distance.x * distance.x + distance.y * distance.y > FromDIP(3) * FromDIP(3);
            }
            if (!m_drag_moved)
                return;
            m_yaw += (current.x - m_last_mouse.x) * 0.012;
            m_pitch = std::clamp(
                m_pitch + (current.y - m_last_mouse.y) * 0.012,
                -1.5707963267948966, -0.15);
            m_last_mouse = current;
            m_canvas->Refresh(false);
        });
        m_canvas->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
            if (m_drawing_selection) return;
            const int delta = event.GetWheelDelta();
            if (delta == 0)
                return;
            const double turns = double(event.GetWheelRotation()) / double(delta);
            m_zoom = std::clamp(m_zoom * std::pow(1.15, turns), 0.45, 12.0);
            m_canvas->Refresh(false);
        });
        m_canvas->Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent& event) {
            if (m_drawing_selection) finish_drag();
            m_last_mouse = event.GetPosition();
            if (!m_canvas->HasCapture()) m_canvas->CaptureMouse();
        });
        m_canvas->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) { finish_drag(); });
        m_canvas->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
            m_dragging = false; m_drawing_selection = false; m_stroke.clear(); m_canvas->Refresh(false);
        });
        m_canvas->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (!event.ControlDown() && (event.GetKeyCode() == 'F' || event.GetKeyCode() == 'f')) {
                focus_selection(); return;
            }
            if (!m_selection_enabled) {
                event.Skip();
                return;
            }
            if (event.ControlDown() && (event.GetKeyCode() == 'Z' || event.GetKeyCode() == 'z')) {
                if (event.ShiftDown()) redo_selection(); else undo_selection();
                return;
            }
            if (event.GetKeyCode() == WXK_ESCAPE) {
                if (m_drawing_selection || selection_busy()) {
                    cancel_surface_selection(); finish_drag(); notify_selection_changed();
                } else clear_selection();
                return;
            }
            event.Skip();
        });
    }

    ~ModelPreview3D() override {
        cancel_surface_selection();
        m_surface_timer.Stop();
        if (m_surface_worker.joinable()) m_surface_worker.join();
        m_region_prepare_timer.Stop();
        // The worker owns only CPU data. Joining here makes destruction safe;
        // ordinary model switches merely invalidate its generation and never wait.
        if (m_region_prepare_worker.joinable()) m_region_prepare_worker.join();
        clear();
    }

    struct ViewState { double yaw, pitch, zoom; BoundingBoxf3 bounds; double pan_x, pan_y; };
    ViewState view_state() const { return {m_yaw, m_pitch, m_zoom, m_bounds, m_pan_x, m_pan_y}; }
    void restore_view(const ViewState& state) {
        m_yaw = state.yaw; m_pitch = state.pitch; m_zoom = state.zoom; m_bounds = state.bounds;
        m_pan_x = state.pan_x; m_pan_y = state.pan_y;
        m_canvas->Refresh(false);
    }

    struct FileStamp {
        std::filesystem::file_time_type modified;
        uintmax_t bytes {0};
        bool valid {false};
    };

    // CPU-only data: prepare on a worker, then adopt on the GUI thread. Neither
    // OBJ parsing nor per-triangle color/normal preparation needs an OpenGL context.
    struct PreparedModel {
        GLModel::Geometry geometry;
        indexed_triangle_set mesh;
        std::vector<RGBA> vertex_colors;
        BoundingBoxf3 bounds;
        boost::filesystem::path path;
        FileStamp stamp;
        size_t triangles {0};
        size_t colors {0};
        std::vector<PreviewPalette::Color> trial_palette;
        std::shared_ptr<const PreviewPalette::Histogram> trial_histogram;
        std::string geometry_id;
        FaceColorOverrides face_color_overrides;
        std::optional<SelectionState> selection;
        std::optional<ModelPreviewColorControls::State> color_trial;
    };

    static bool prepare_model(const boost::filesystem::path& path, PreparedModel& prepared,
                              std::string& error, const FaceColorOverrides& explicit_overrides = {},
                              const boost::filesystem::path& metadata_path = {})
    {
        const auto started = std::chrono::steady_clock::now();
        prepared = PreparedModel {};
        const FileStamp initial_stamp = file_stamp(path);
        TriangleMesh mesh;
        ObjInfo obj_info;
        if (!AI::load_model_artifact(path, mesh, obj_info, error) || mesh.empty())
            return false;

        const indexed_triangle_set& its = mesh.its;
        prepared.geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(its);
        prepared.face_color_overrides = explicit_overrides;
        auto record = metadata_path;
        if (record.empty()) { record = path; record.replace_extension(".json"); }
        boost::system::error_code record_error;
        const auto record_bytes = boost::filesystem::file_size(record, record_error);
        boost::filesystem::ifstream record_stream(record);
        // Bound auxiliary state independently of the mesh, before JSON allocates.
        if (record_stream && !record_error && record_bytes <= 128ULL * 1024 * 1024) {
            const auto metadata = nlohmann::json::parse(record_stream, nullptr, false);
            std::string state_error;
            if (metadata.is_object()) {
                if (metadata.contains("color_trial")) {
                    ModelPreviewColorControls::State trial;
                    if (AI::ColorTrialPersistence::decode(metadata["color_trial"], its.indices.size(), prepared.geometry_id, trial, state_error)) {
                        trial.source = 2;
                        trial.notice = _L("已恢复此版本保存的试色。");
                        prepared.color_trial = std::move(trial);
                    }
                }
                if (metadata.contains("local_selection")) {
                    SelectionState selection;
                    if (AI::SurfaceSelectionPersistence::decode(metadata["local_selection"], its.indices.size(), prepared.geometry_id, selection, state_error))
                        prepared.selection = std::move(selection);
                }
                if (explicit_overrides.empty() && metadata.contains("face_color_intent"))
                    AI::SurfaceSelectionPersistence::decode_colors(metadata["face_color_intent"], its.indices.size(), prepared.geometry_id, prepared.face_color_overrides, state_error);
            }
            if (!state_error.empty()) BOOST_LOG_TRIVIAL(warning) << "Saved local editing state ignored: " << state_error;
        }
        std::unordered_map<size_t, PreviewPalette::Color> locked_colors;
        for (const auto& item : prepared.face_color_overrides) {
            if (item.first >= its.indices.size()) { error = "Local face color no longer matches this model."; return false; }
            locked_colors[item.first] = item.second;
        }
        const bool has_vertex_colors = obj_info.vertex_colors.size() == its.vertices.size();
        const bool has_face_colors = obj_info.face_colors.size() == its.indices.size();
        const RGBA fallback {ColorRGBA::ORCA().r(), ColorRGBA::ORCA().g(), ColorRGBA::ORCA().b(), 1.0f};
        GLModel::Geometry geometry;
        geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2};
        geometry.reserve_vertices(its.indices.size() * 3);
        geometry.reserve_indices(its.indices.size() * 3);
        std::unordered_set<uint32_t> observed_colors;
        auto trial_histogram = std::make_shared<PreviewPalette::Histogram>();
        // Shared OBJ vertices often occur in six triangles. Pack/count their color
        // once while retaining all original face corners and the exact RGB8 output.
        std::vector<uint32_t> packed_vertex_colors;
        std::vector<uint8_t> counted_vertices;
        if (has_vertex_colors) {
            packed_vertex_colors.reserve(obj_info.vertex_colors.size());
            counted_vertices.assign(obj_info.vertex_colors.size(), 0);
            for (const RGBA& color : obj_info.vertex_colors)
                packed_vertex_colors.push_back(preview_rgb8(color[0], color[1], color[2]));
        }
        for (size_t face_index = 0; face_index < its.indices.size(); ++face_index) {
            const auto lock = locked_colors.find(face_index);
            const auto& indices = its.indices[face_index];
            const Vec3f& a = its.vertices[indices[0]];
            const Vec3f& b = its.vertices[indices[1]];
            const Vec3f& c = its.vertices[indices[2]];
            Vec3f normal = (b - a).cross(c - a);
            const double area_weight = normal.norm();
            if (normal.squaredNorm() > 1e-12f) normal.normalize();
            else normal = Vec3f::UnitZ();
            const auto base = static_cast<unsigned int>(geometry.vertices_count());
            const RGBA uniform_color = has_face_colors ? obj_info.face_colors[face_index] : fallback;
            const uint32_t uniform_packed = has_vertex_colors ? 0 :
                preview_rgb8(uniform_color[0], uniform_color[1], uniform_color[2]);
            if (!has_vertex_colors)
                observed_colors.insert(uniform_packed);
            for (size_t corner = 0; corner < 3; ++corner) {
                const auto vertex_index = indices[corner];
                const RGBA& color = has_vertex_colors ? obj_info.vertex_colors[vertex_index] : uniform_color;
                const uint32_t packed = has_vertex_colors ? packed_vertex_colors[vertex_index] : uniform_packed;
                trial_histogram->add(packed, area_weight);
                if (has_vertex_colors && !counted_vertices[vertex_index]) {
                    observed_colors.insert(packed);
                    counted_vertices[vertex_index] = 1;
                }
                const uint32_t shown = lock == locked_colors.end() ? packed
                    : preview_rgb8(lock->second[0], lock->second[1], lock->second[2]);
                geometry.add_vertex(its.vertices[indices[corner]], normal,
                    Vec2f(float(shown), lock == locked_colors.end() ? color[3] : -1.0f));
            }
            geometry.add_triangle(base, base + 1, base + 2);
        }
        if (geometry.is_empty()) {
            error = "The OBJ contains no renderable triangles.";
            return false;
        }

        prepared.bounds = mesh.bounding_box();
        prepared.triangles = its.indices.size();
        prepared.colors = observed_colors.size();
        const auto palette_started = std::chrono::steady_clock::now();
        prepared.trial_palette = trial_histogram->palette(6, {}, true);
        prepared.trial_histogram = std::move(trial_histogram);
        BOOST_LOG_TRIVIAL(info) << "AI color trial palette: colors=" << prepared.trial_palette.size()
            << ", clustering_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - palette_started).count();
        prepared.geometry = std::move(geometry);
        prepared.path = path;
        prepared.stamp = file_stamp(path);
        if (!same_stamp(initial_stamp, prepared.stamp))
            prepared.stamp.valid = false;
        if (has_vertex_colors) {
            prepared.mesh = std::move(mesh.its);
            prepared.vertex_colors = std::move(obj_info.vertex_colors);
        }
        BOOST_LOG_TRIVIAL(info) << "AI model preview CPU prepare: triangles=" << prepared.triangles
            << ", elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
        return true;
    }

    bool load_prepared_model(PreparedModel&& prepared, const std::vector<std::string>& palette,
                             size_t& triangle_count, Vec3d& dimensions, size_t& color_count, std::string& error)
    {
        if (m_context == nullptr || !m_context->IsOK() || !m_canvas->SetCurrent(*m_context)) {
            error = "OpenGL preview context is unavailable.";
            return false;
        }
        if (prepared.geometry.is_empty()) {
            error = "The OBJ contains no renderable triangles.";
            return false;
        }
        cache_current_preview();
        clear_current_preview();
        auto model = std::make_unique<GLModel>();
        model->init_from(std::move(prepared.geometry));
        m_models.emplace_back(std::move(model));
        m_bounds = prepared.bounds;
        m_pending_mesh = std::move(prepared.mesh);
        m_pending_vertex_colors = std::move(prepared.vertex_colors);
        m_geometry_id = std::move(prepared.geometry_id);
        m_face_color_overrides = std::move(prepared.face_color_overrides);
        m_pending_selection = std::move(prepared.selection);
        m_model_path = std::move(prepared.path);
        m_model_stamp = prepared.stamp;
        m_triangle_count = prepared.triangles;
        m_color_count = prepared.colors;
        m_trial_palette = std::move(prepared.trial_palette);
        m_trial_histogram = std::move(prepared.trial_histogram);
        triangle_count = m_triangle_count;
        dimensions = m_model_dimensions = prepared.bounds.size().cast<double>();
        color_count = m_color_count;
        m_palette = palette;
        m_has_model = true;
        m_color_trial->load(m_trial_histogram, m_trial_palette);
        if (prepared.color_trial) m_color_trial->restore(*prepared.color_trial);
        m_paint_diagnostics_logged = false;
        m_render_diagnostics_logged = false;
        front_view();
        notify_selection_changed();
        return true;
    }

    bool try_load_cached_model(const boost::filesystem::path& path, const std::vector<std::string>& palette,
                               size_t& triangle_count, Vec3d& dimensions, size_t& color_count)
    {
        if (!m_cached_preview || m_cached_preview->path != path ||
            !same_stamp(m_cached_preview->stamp, file_stamp(path)) ||
            m_context == nullptr || !m_context->IsOK() || !m_canvas->SetCurrent(*m_context))
            return false;
        auto cached = std::move(m_cached_preview);
        cache_current_preview();
        clear_current_preview();
        m_models = std::move(cached->models);
        m_pending_mesh = std::move(cached->mesh);
        m_pending_vertex_colors = std::move(cached->vertex_colors);
        m_geometry_id = std::move(cached->geometry_id);
        m_face_color_overrides = std::move(cached->face_color_overrides);
        m_pending_selection = std::move(cached->selection);
        m_model_path = std::move(cached->path);
        m_model_stamp = cached->stamp;
        m_bounds = cached->bounds;
        m_triangle_count = triangle_count = cached->triangles;
        m_color_count = color_count = cached->colors;
        m_trial_palette = std::move(cached->trial_palette);
        m_trial_histogram = std::move(cached->trial_histogram);
        dimensions = m_model_dimensions = cached->dimensions;
        m_palette = palette;
        m_has_model = true;
        m_color_trial->load(m_trial_histogram, m_trial_palette);
        if (cached->color_trial) m_color_trial->restore(*cached->color_trial);
        m_paint_diagnostics_logged = false;
        m_render_diagnostics_logged = false;
        front_view();
        notify_selection_changed();
        BOOST_LOG_TRIVIAL(info) << "AI model preview cache hit: triangles=" << triangle_count;
        return true;
    }

    bool load_model(const boost::filesystem::path& path, const std::vector<std::string>& palette,
                    size_t& triangle_count, Vec3d& dimensions, size_t& color_count, std::string& error,
                    const FaceColorOverrides& explicit_overrides = {})
    {
        // Accepting an already displayed preview should not reparse the OBJ or
        // rebuild GPU buffers. The file stamp still invalidates external edits.
        if (m_has_model && path == m_model_path && palette == m_palette && same_stamp(m_model_stamp, file_stamp(path))) {
            triangle_count = m_triangle_count; color_count = m_color_count;
            dimensions = m_model_dimensions;
            return true;
        }
        if (try_load_cached_model(path, palette, triangle_count, dimensions, color_count))
            return true;
        PreparedModel prepared;
        return prepare_model(path, prepared, error, explicit_overrides) && load_prepared_model(std::move(prepared), palette,
            triangle_count, dimensions, color_count, error);
    }

    void clear()
    {
        clear_current_preview();
        m_cached_preview.reset();
    }

private:
    void clear_current_preview()
    {
        cancel_surface_selection();
        m_protected_faces.clear();
        m_foreground_faces.clear(); m_selection_domain.clear();
        m_pending_selection.reset(); m_geometry_id.clear(); m_face_color_overrides.clear();
        m_selection_redo.clear();
        m_stroke.clear();
        m_drawing_selection = false;
        ++m_region_generation;
        m_deferred_selection = {};
        m_region_prepare_failed = false;
        m_region_prepare_status->Hide();
        if (m_context != nullptr && m_canvas != nullptr)
            m_canvas->SetCurrent(*m_context);
        m_models.clear();
        m_selection_model.reset();
        m_protection_model.reset();
        m_region_editor = std::make_shared<AI::VertexColorRegionEditor>();
        m_pending_mesh = indexed_triangle_set {};
        m_pending_vertex_colors = std::vector<RGBA> {};
        m_selection_history.clear();
        m_palette.clear();
        m_has_model = false;
        m_color_trial_enabled = false;
        m_trial_palette.clear();
        m_trial_histogram.reset();
        m_trial_toggle_started.reset();
        if (m_color_trial) m_color_trial->clear();
        m_selection_enabled = false;
        m_model_path.clear();
        m_model_stamp = FileStamp {};
        if (m_canvas != nullptr)
            m_canvas->SetCursor(wxCursor(wxCURSOR_ARROW));
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
    }

public:
    void reset_view()
    {
        m_pan_x = m_pan_y = 0.0;
        m_yaw = -0.65;
        m_pitch = -1.05;
        m_zoom = 1.0;
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
    }

    void front_view()
    {
        m_pan_x = m_pan_y = 0.0;
        // Generated portrait OBJ files are Z-up and face +X. Rotate +X toward
        // the orthographic camera so identity can be compared without asking
        // the user to find a precise angle by dragging a two-million-face mesh.
        m_yaw = -1.5707963267948966;
        m_pitch = -1.5707963267948966;
        m_zoom = 1.0;
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
    }

    void refresh()
    {
        if (m_canvas != nullptr) {
            m_canvas->Refresh(false);
            m_canvas->Update();
        }
    }

    void set_selection_enabled(bool enabled)
    {
        if (!enabled) m_deferred_selection = {};
        const bool selection_enabled = enabled && region_editing_ready();
        if (selection_enabled == m_selection_enabled)
            return;
        m_selection_enabled = selection_enabled;
        if (m_canvas != nullptr)
            m_canvas->SetCursor(wxCursor(m_selection_enabled ? wxCURSOR_CROSS : wxCURSOR_ARROW));
        if (!m_selection_enabled) {
            m_deferred_selection = {};
            cancel_surface_selection();
            if (!m_region_prepare_failed) m_region_prepare_status->Hide();
        } else ensure_region_editor();
    }

    void set_selection_gesture(SelectionGesture gesture, double radius_pixels = 18.0)
    {
        m_selection_gesture = gesture;
        m_brush_radius = std::clamp(radius_pixels, 3.0, 100.0);
    }
    SelectionState selection_state() const {
        if (!m_region_editor->ready() && m_pending_selection) return *m_pending_selection;
        return {m_region_editor->selected_faces(), m_protected_faces, m_foreground_faces, m_selection_domain};
    }
    const std::string& geometry_id() const { return m_geometry_id; }
    const FaceColorOverrides& face_color_overrides() const { return m_face_color_overrides; }
    nlohmann::json selection_metadata() const {
        return AI::SurfaceSelectionPersistence::encode(selection_state(), m_triangle_count, m_geometry_id);
    }
    nlohmann::json face_color_metadata() const {
        return AI::SurfaceSelectionPersistence::encode_colors(m_face_color_overrides, m_triangle_count, m_geometry_id);
    }
    void restore_selection_state(SelectionState state)
    {
        if (!ensure_region_editor()) {
            m_deferred_selection = [this, state = std::move(state)]() mutable { restore_selection_state(std::move(state)); };
            return;
        }
        if (!m_region_editor->restore_selection(state.selected)) return;
        m_protected_faces = std::move(state.protected_faces);
        if (m_protected_faces.size() != state.selected.size()) m_protected_faces.assign(state.selected.size(), 0);
        m_foreground_faces = std::move(state.foreground); m_selection_domain = std::move(state.domain);
        if (m_foreground_faces.size() != state.selected.size()) m_foreground_faces.assign(state.selected.size(), 0);
        if (m_selection_domain.size() != state.selected.size()) m_selection_domain = state.selected;
        rebuild_selection_model(); notify_selection_changed(); m_canvas->Refresh(false);
    }
    size_t protected_face_count() const { return std::count(m_protected_faces.begin(), m_protected_faces.end(), uint8_t(1)); }
    bool selection_busy() const { return m_surface_task && !m_surface_task->canceled; }
    void refine_selection_boundary()
    {
        if (!m_region_editor->ready() || selection_busy()) return;
        if (m_surface_worker.joinable()) {
            if (m_surface_task && !m_surface_task->done) return;
            finish_surface_selection();
        }
        auto task = std::make_shared<SurfaceTask>();
        task->generation = m_region_generation; task->refinement = true;
        task->started = std::chrono::steady_clock::now();
        m_surface_task = task;
        auto indices = [](const std::vector<uint8_t>& mask) {
            std::vector<size_t> result;
            for (size_t i=0; i<mask.size(); ++i) if (mask[i]) result.push_back(i);
            return result;
        };
        auto editor = m_region_editor;
        auto roi = indices(m_selection_domain), foreground = indices(m_foreground_faces), background = indices(m_protected_faces);
        try {
        m_surface_worker = std::thread([task, editor, roi = std::move(roi), foreground = std::move(foreground), background = std::move(background)] {
            try {
                auto result = AI::SurfaceSelectionRefinement::refine(editor->mesh(), editor->vertex_colors(), roi,
                    foreground, background, [task] { return task->canceled.load(); });
                task->result.faces = std::move(result.faces); task->result.canceled = result.canceled;
                task->error = result.error;
            } catch (const std::exception& e) { task->error = e.what(); }
            task->done = true;
        });
        } catch (const std::exception& e) {
            task->error = e.what(); task->done = true;
            finish_surface_selection(); return;
        }
        m_surface_timer.Start(40);
        show_region_preparation_status(_L("正在贴合选区边界，可按 Esc 取消……")); notify_selection_changed();
    }

    void set_selection_operation(AI::RegionSelectionOperation operation)
    {
        m_selection_operation = operation;
    }

    void set_selection_settings(const AI::RegionSelectionSettings& settings)
    {
        m_selection_settings = settings;
    }

    void set_selection_preview_color(const ColorRGBA& color)
    {
        m_selection_preview_color = color;
        if (m_selection_model != nullptr)
            m_selection_model->set_color(m_selection_preview_color);
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
    }

    void set_selection_changed_callback(std::function<void(size_t)> callback)
    {
        m_selection_changed = std::move(callback);
    }

    void clear_selection(bool record_history = true)
    {
        cancel_surface_selection();
        const bool canceled_click = bool(m_deferred_selection);
        m_deferred_selection = {};
        if (canceled_click && m_selection_enabled && current_region_preparation())
            show_region_preparation_status(_L("已清空等待中的点选；正在准备局部选择，可继续旋转模型。"));
        if (record_history && (m_region_editor->selected_face_count() > 0 || protected_face_count() > 0))
            push_selection_history(m_region_editor->selected_faces());
        m_protected_faces.assign(m_region_editor->selected_faces().size(), 0);
        m_foreground_faces.assign(m_region_editor->selected_faces().size(), 0);
        m_selection_domain.assign(m_region_editor->selected_faces().size(), 0);
        m_region_editor->clear_selection();
        rebuild_selection_model();
        notify_selection_changed();
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
    }

    size_t selected_face_count() const { return selection_busy() ? 0 : m_region_editor->selected_face_count(); }
    std::vector<size_t> selected_face_indices() const {
        std::vector<size_t> result;
        const auto& selected = m_region_editor->selected_faces();
        for (size_t i = 0; i < selected.size(); ++i) if (selected[i]) result.push_back(i);
        return result;
    }
    void set_gray_view(bool enabled) { m_gray_view = enabled; m_canvas->Refresh(false); }
    void set_selection_overlay_visible(bool visible) { m_selection_overlay_visible = visible; m_canvas->Refresh(false); }
    bool focus_selection() {
        if (!m_region_editor->ready() || !m_region_editor->selected_face_count()) return false;
        const auto& mesh = m_region_editor->mesh();
        const auto& selected = m_region_editor->selected_faces();
        BoundingBoxf3 bounds;
        for (size_t i = 0; i < selected.size(); ++i) if (selected[i])
            for (int k = 0; k < 3; ++k) bounds.merge(mesh.vertices[mesh.indices[i][k]].cast<double>());
        const auto rotation = view_rotation();
        const Vec3d offset = rotation.linear() * (bounds.center() - m_bounds.center()).cast<double>();
        const double radius = std::max(0.001, 0.5 * m_bounds.size().norm());
        m_pan_x = -offset.x() / radius; m_pan_y = -offset.y() / radius;
        const double aspect = double(std::max(1, m_canvas->GetClientSize().x)) / std::max(1, m_canvas->GetClientSize().y);
        const Vec3d full = rotation.linear().cwiseAbs() * (0.5 * m_bounds.size().cast<double>());
        const Vec3d patch = rotation.linear().cwiseAbs() * (0.5 * bounds.size().cast<double>());
        m_zoom = std::clamp(std::max(full.y(), full.x() / aspect) /
            std::max(0.001, 1.25 * std::max(patch.y(), patch.x() / aspect)), 0.45, 12.0);
        m_canvas->Refresh(false);
        return true;
    }
    ModelPreviewColorControls::State color_trial_state() const { return m_color_trial->state(); }
    nlohmann::json color_trial_metadata() const {
        AI::ColorTrialPersistence::State saved = m_color_trial->state();
        saved.source = 2;
        return AI::ColorTrialPersistence::encode(saved, m_triangle_count, m_geometry_id);
    }
    void restore_color_trial(const ModelPreviewColorControls::State& state) { m_color_trial->restore(state); }
    void set_color_controls_visible(bool visible) { m_color_trial->Show(visible && m_has_model); Layout(); }
    // Capture on the UI thread; callers own the copy and cannot mutate preview
    // controls, project slots or the source mesh through this snapshot.
    PreviewPalette::ColorTrialMapping color_trial_mapping() const {
        return {m_color_trial_enabled, m_color_trial->mapping_colors(), m_trial_palette};
    }
    PreviewPalette::ColorTrialMapping import_color_mapping(bool use_current_trial = true) const {
        // Original-color viewing must not silently commit the six-color trial.
        // Native matching chooses its target count independently of feed slots.
        return use_current_trial && m_color_trial_enabled ? color_trial_mapping()
                                                        : PreviewPalette::ColorTrialMapping {};
    }
    bool region_selection_preparing() const { return !m_region_editor->ready() && region_editing_ready() && bool(m_region_preparation); }
    bool region_editing_ready() const {
        return !m_region_prepare_failed && (m_region_editor->ready() || current_region_preparation() || (!m_pending_mesh.indices.empty() &&
            m_pending_vertex_colors.size() == m_pending_mesh.vertices.size()));
    }
    bool can_undo_selection() const { return selection_busy() || bool(m_deferred_selection) || !m_selection_history.empty(); }

    bool selection_matches_face_evidence(const std::vector<size_t>& face_indices) const
    {
        if (!m_region_editor->ready() || face_indices.empty())
            return false;
        std::vector<uint8_t> expected(m_region_editor->selected_faces().size(), 0);
        bool has_valid_face = false;
        for (size_t face_index : face_indices) {
            if (face_index >= expected.size())
                continue;
            expected[face_index] = 1;
            has_valid_face = true;
        }
        return has_valid_face && expected == m_region_editor->selected_faces();
    }

    bool undo_selection()
    {
        if (selection_busy()) { cancel_surface_selection(); notify_selection_changed(); return true; }
        if (m_deferred_selection) {
            m_deferred_selection = {};
            if (current_region_preparation())
                show_region_preparation_status(_L("已撤销等待中的点选；正在准备局部选择，可继续旋转模型。"));
            return true;
        }
        if (m_selection_history.empty())
            return false;
        m_selection_redo.push_back(selection_state());
        SelectionState previous = std::move(m_selection_history.back());
        m_selection_history.pop_back();
        if (!m_region_editor->restore_selection(previous.selected))
            return false;
        m_protected_faces = std::move(previous.protected_faces);
        m_foreground_faces = std::move(previous.foreground); m_selection_domain = std::move(previous.domain);
        rebuild_selection_model();
        notify_selection_changed();
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
        return true;
    }

    bool redo_selection()
    {
        if (selection_busy() || m_selection_redo.empty()) return false;
        m_selection_history.push_back(selection_state());
        auto state = std::move(m_selection_redo.back()); m_selection_redo.pop_back();
        restore_selection_state(std::move(state));
        return true;
    }

    bool apply_selection_color(const RGBA& color, const boost::filesystem::path& source,
                               const boost::filesystem::path& destination, std::string& error)
    {
        if (source != m_model_path || !same_stamp(m_model_stamp, file_stamp(source))) {
            error = "The source model changed while recoloring. Please reload it.";
            return false;
        }
        return m_region_editor->apply_color_to_obj_copy(color, source, destination, error);
    }

    bool select_palette_material(size_t palette_index)
    {
        std::vector<ColorRGBA> decoded;
        decode_colors(m_palette, decoded);
        if (decoded.empty() || palette_index >= decoded.size())
            return false;
        if (!ensure_region_editor()) {
            if (region_editing_ready()) m_deferred_selection = [this, palette_index] { select_palette_material(palette_index); };
            return false;
        }
        std::vector<RGBA> palette;
        palette.reserve(decoded.size());
        for (const ColorRGBA& color : decoded)
            palette.push_back({color.r(), color.g(), color.b(), color.a()});

        const std::vector<uint8_t> previous = m_region_editor->selected_faces();
        m_region_editor->select_palette_material(palette, palette_index);
        if (previous != m_region_editor->selected_faces())
            push_selection_history(previous);
        rebuild_selection_model();
        notify_selection_changed();
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
        return true;
    }

    size_t select_elevated_overhang_regions()
    {
        if (!ensure_region_editor()) {
            if (region_editing_ready()) m_deferred_selection = [this] { select_elevated_overhang_regions(); };
            return 0;
        }
        const std::vector<uint8_t> previous = m_region_editor->selected_faces();
        const size_t localized = m_region_editor->select_elevated_overhang_regions();
        if (localized == 0)
            return 0;
        if (previous != m_region_editor->selected_faces())
            push_selection_history(previous);
        rebuild_selection_model();
        notify_selection_changed();
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
        return localized;
    }

    size_t select_face_evidence(const std::vector<size_t>& face_indices)
    {
        if (face_indices.empty())
            return 0;
        if (!ensure_region_editor()) {
            if (region_editing_ready()) m_deferred_selection = [this, face_indices] { select_face_evidence(face_indices); };
            return 0;
        }
        const std::vector<uint8_t> previous = m_region_editor->selected_faces();
        const size_t localized = m_region_editor->select_faces(face_indices);
        if (localized == 0)
            return 0;
        if (previous != m_region_editor->selected_faces())
            push_selection_history(previous);
        rebuild_selection_model();
        notify_selection_changed();
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
        return localized;
    }

private:
    struct CachedPreview {
        std::vector<std::unique_ptr<GLModel>> models;
        indexed_triangle_set mesh;
        std::vector<RGBA> vertex_colors;
        BoundingBoxf3 bounds;
        Vec3d dimensions {Vec3d::Zero()};
        boost::filesystem::path path;
        FileStamp stamp;
        size_t triangles {0};
        size_t colors {0};
        std::vector<PreviewPalette::Color> trial_palette;
        std::shared_ptr<const PreviewPalette::Histogram> trial_histogram;
        std::string geometry_id;
        FaceColorOverrides face_color_overrides;
        std::optional<SelectionState> selection;
        std::optional<ModelPreviewColorControls::State> color_trial;
    };

    static FileStamp file_stamp(const boost::filesystem::path& path)
    {
        FileStamp stamp;
        std::error_code error;
        const std::filesystem::path native_path(path.native());
        stamp.bytes = std::filesystem::file_size(native_path, error);
        if (error)
            return stamp;
        stamp.modified = std::filesystem::last_write_time(native_path, error);
        stamp.valid = !error;
        return stamp;
    }

    static bool same_stamp(const FileStamp& first, const FileStamp& second)
    {
        return first.valid && second.valid && first.bytes == second.bytes && first.modified == second.modified;
    }

    void cache_current_preview()
    {
        m_cached_preview.reset();
        // A/B review needs one prior model. Never retain the much larger selection
        // adjacency/BVH or let large artifacts accumulate through version browsing.
        if (!m_has_model || !m_model_stamp.valid)
            return;
        const auto& mesh = m_region_editor->ready() ? m_region_editor->mesh()
            : current_region_preparation() ? m_region_preparation->mesh : m_pending_mesh;
        const auto& colors = m_region_editor->ready() ? m_region_editor->vertex_colors()
            : current_region_preparation() ? m_region_preparation->colors : m_pending_vertex_colors;
        size_t bytes = mesh.vertices.capacity() * sizeof(Vec3f) +
                       mesh.indices.capacity() * sizeof(stl_triangle_vertex_indices) +
                       colors.capacity() * sizeof(RGBA);
        for (const auto& model : m_models)
            bytes += model->cpu_memory_used() + model->gpu_memory_used();
        constexpr size_t cache_limit = size_t(384) * 1024 * 1024;
        if (bytes > cache_limit)
            return;
        m_cached_preview = std::make_unique<CachedPreview>();
        m_cached_preview->geometry_id = m_geometry_id;
        m_cached_preview->face_color_overrides = m_face_color_overrides;
        m_cached_preview->selection = selection_state();
        m_cached_preview->color_trial = m_color_trial->state();
        m_cached_preview->models = std::move(m_models);
        if (m_region_editor->ready()) {
            // Retain only compact source arrays, never the adjacency/BVH. This
            // keeps the first local before/after comparison free of OBJ parsing.
            m_cached_preview->mesh = m_region_editor->mesh();
            m_cached_preview->vertex_colors = m_region_editor->vertex_colors();
        } else if (current_region_preparation()) {
            m_cached_preview->mesh = m_region_preparation->mesh;
            m_cached_preview->vertex_colors = m_region_preparation->colors;
        } else {
            m_cached_preview->mesh = std::move(m_pending_mesh);
            m_cached_preview->vertex_colors = std::move(m_pending_vertex_colors);
        }
        m_cached_preview->bounds = m_bounds;
        m_cached_preview->dimensions = m_model_dimensions;
        m_cached_preview->path = m_model_path;
        m_cached_preview->stamp = m_model_stamp;
        m_cached_preview->triangles = m_triangle_count;
        m_cached_preview->colors = m_color_count;
        m_cached_preview->trial_histogram = m_trial_histogram;
        // New model loads reset the trial; never relabel a previous custom or
        // project palette as a fresh automatic suggestion on a cache hit.
        m_cached_preview->trial_palette = m_trial_histogram ? m_trial_histogram->palette(6, {}, true) : m_trial_palette;
    }

    struct RegionPreparation {
        uint64_t generation {0};
        // These compact source arrays stay immutable while the worker builds its
        // private editor, so switching models can still cache the source safely.
        indexed_triangle_set mesh;
        std::vector<RGBA> colors;
        AI::VertexColorRegionEditor editor;
        std::atomic<bool> done {false};
        bool success {false};
        std::string error;
        std::chrono::steady_clock::time_point started;
    };

    bool current_region_preparation() const {
        return m_region_preparation && m_region_preparation->generation == m_region_generation;
    }

    void show_region_preparation_status(const wxString& message) {
        m_region_prepare_status->SetLabel(message);
        m_region_prepare_status->Show();
        Layout();
    }

    bool ensure_region_editor()
    {
        if (m_region_editor->ready())
            return true;
        if (!region_editing_ready())
            return false;
        show_region_preparation_status(_L("正在准备局部选择，可继续旋转模型；点选会在准备完成后执行。"));
        // An invalidated worker is allowed to finish without blocking navigation.
        // The timer starts the latest model only after that worker has exited.
        if (m_region_preparation) return false;
        auto task = std::make_shared<RegionPreparation>();
        task->generation = m_region_generation;
        task->mesh = std::move(m_pending_mesh);
        task->colors = std::move(m_pending_vertex_colors);
        task->started = std::chrono::steady_clock::now();
        m_region_preparation = task;
        try {
            m_region_prepare_worker = std::thread([task] {
                try { task->success = task->editor.initialize(task->mesh, task->colors, task->error); }
                catch (const std::exception& error) { task->error = error.what(); }
                catch (...) { task->error = "Unknown region preparation failure"; }
                task->done.store(true, std::memory_order_release);
            });
            m_region_prepare_timer.Start(50);
        } catch (const std::exception& error) {
            m_pending_mesh = std::move(task->mesh);
            m_pending_vertex_colors = std::move(task->colors);
            m_region_preparation.reset();
            m_region_prepare_failed = true;
            show_region_preparation_status(_L("局部选择准备失败，请重新加载模型后重试。"));
            BOOST_LOG_TRIVIAL(warning) << "AI model preview selection worker failed: " << error.what();
            set_selection_enabled(false);
        }
        return false;
    }

    void finish_region_preparation()
    {
        if (!m_region_preparation || !m_region_preparation->done.load(std::memory_order_acquire)) return;
        if (m_region_prepare_worker.joinable()) m_region_prepare_worker.join();
        auto task = std::move(m_region_preparation);
        m_region_prepare_timer.Stop();
        if (task->generation != m_region_generation) {
            if (m_selection_enabled || m_deferred_selection) ensure_region_editor();
            return;
        }
        BOOST_LOG_TRIVIAL(info) << "AI model preview selection prepare: elapsed_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - task->started).count()
            << ", background=true, success=" << task->success;
        if (!task->success) {
            m_region_prepare_failed = true;
            m_deferred_selection = {};
            show_region_preparation_status(_L("局部选择准备失败，请重新加载模型后重试。"));
            BOOST_LOG_TRIVIAL(warning) << "AI model preview selection initialization failed: " << task->error;
            set_selection_enabled(false);
            return;
        }
        m_region_editor = std::make_shared<AI::VertexColorRegionEditor>(std::move(task->editor));
        if (m_pending_selection) {
            auto state = std::move(*m_pending_selection); m_pending_selection.reset();
            m_region_editor->restore_selection(state.selected);
            m_protected_faces = std::move(state.protected_faces);
            m_foreground_faces = std::move(state.foreground); m_selection_domain = std::move(state.domain);
            rebuild_selection_model();
        }
        m_region_prepare_status->Hide();
        Layout();
        auto deferred = std::move(m_deferred_selection);
        m_deferred_selection = {};
        if (deferred) deferred();
        else if (m_selection_enabled) notify_selection_changed();
    }

    void push_selection_history(const std::vector<uint8_t>& selected_faces)
    {
        const size_t bytes = std::max(size_t(1), selected_faces.size() + m_protected_faces.size() +
            m_foreground_faces.size() + m_selection_domain.size());
        const size_t max_history = std::max(size_t(1), std::min(size_t(20), (size_t(64)*1024*1024)/bytes));
        if (m_selection_history.size() >= max_history)
            m_selection_history.erase(m_selection_history.begin());
        m_selection_redo.clear();
        m_selection_history.push_back({selected_faces, m_protected_faces, m_foreground_faces, m_selection_domain});
    }

    void finish_drag()
    {
        m_dragging = false;
        m_drawing_selection = false;
        m_stroke.clear();
        if (m_canvas) m_canvas->Refresh(false);
        if (m_canvas != nullptr && m_canvas->HasCapture())
            m_canvas->ReleaseMouse();
    }

    void notify_selection_changed()
    {
        if (m_selection_changed)
            m_selection_changed(selected_face_count());
    }

    struct SurfaceTask {
        std::atomic<bool> canceled {false}, done {false};
        uint64_t generation {0};
        SelectionGesture gesture {SelectionGesture::Lasso};
        bool refinement {false};
        AI::SurfaceSelection::Result result;
        std::string error;
        std::chrono::steady_clock::time_point started;
    };

    void cancel_surface_selection()
    {
        if (m_surface_task) m_surface_task->canceled = true;
        m_drawing_selection = false;
        m_stroke.clear();
    }

    void submit_surface_selection()
    {
        if (m_stroke.empty()) return;
        if (m_surface_task && !m_surface_task->done) {
            show_region_preparation_status(_L("正在计算选区；可按 Esc 取消，完成后继续补选。"));
            return;
        }
        const int width = std::max(1, m_canvas->GetClientSize().x);
        const int height = std::max(1, m_canvas->GetClientSize().y);
        const double radius = std::max(0.001, 0.5 * m_bounds.size().norm());
        const double half_height = fitted_half_height(width, height);
        const Transform3d view = Geometry::translation_transform(Vec3d(m_pan_x * radius, m_pan_y * radius, -3.0 * radius)) *
            view_rotation() * Geometry::translation_transform(-m_bounds.center().cast<double>());
        auto request = [this, stroke = selection_outline(), gesture = m_selection_gesture,
                        brush_radius = m_brush_radius, view, half_height, width, height] {
            start_surface_selection(stroke, gesture, brush_radius, view, half_height, width, height);
        };
        if (!ensure_region_editor()) {
            m_deferred_selection = std::move(request);
            show_region_preparation_status(_L("已记住本次范围，模型准备好后自动选择；可按 Esc 取消。"));
        } else request();
    }

    std::vector<Vec2d> selection_outline() const
    {
        if (m_selection_gesture != SelectionGesture::Lasso || m_stroke.size() < 2) return m_stroke;
        Vec2d lo = m_stroke.front(), hi = lo;
        double area = 0;
        for (size_t i=0; i<m_stroke.size(); ++i) {
            lo = lo.cwiseMin(m_stroke[i]); hi = hi.cwiseMax(m_stroke[i]);
            const auto& a = m_stroke[i]; const auto& b = m_stroke[(i+1)%m_stroke.size()];
            area += a.x()*b.y()-a.y()*b.x();
        }
        // A diagonal drag is a rectangle; a curved path is a freehand lasso.
        // This also makes short coarse selections useful without a precision loop.
        if ((hi-lo).squaredNorm() > 36 && std::abs(area) < 0.08 * (hi-lo).squaredNorm())
            return {lo, Vec2d(hi.x(),lo.y()), hi, Vec2d(lo.x(),hi.y())};
        return m_stroke;
    }

    void start_surface_selection(const std::vector<Vec2d>& stroke, SelectionGesture gesture, double brush_radius,
                                 const Transform3d& view, double half_height, int width, int height)
    {
        if (m_surface_worker.joinable()) {
            if (m_surface_task && !m_surface_task->done) return;
            finish_surface_selection();
        }
        auto task = std::make_shared<SurfaceTask>();
        task->generation = m_region_generation; task->gesture = gesture;
        task->started = std::chrono::steady_clock::now();
        m_surface_task = task;
        auto editor = m_region_editor; // A model switch leaves the worker's immutable geometry alive.
        const Transform3d inverse = view.inverse();
        const double half_width = half_height * double(width) / height;
        try {
        m_surface_worker = std::thread([task, editor, stroke, brush_radius, view, inverse, half_width, half_height, width, height] {
            try {
                auto project = [=](const Vec3f& point) -> std::optional<Vec2d> {
                    const Vec3d p = view * point.cast<double>();
                    if (p.z() >= 0) return std::nullopt;
                    return Vec2d((p.x() / half_width + 1) * width / 2.0, (1 - p.y() / half_height) * height / 2.0);
                };
                auto pick = [=](const Vec2d& point) -> std::optional<size_t> {
                    const Vec3d origin = inverse * Vec3d((2 * point.x() / width - 1) * half_width,
                        (1 - 2 * point.y() / height) * half_height, 0);
                    return editor->pick_face(origin, inverse.linear() * Vec3d(0, 0, -1));
                };
                auto canceled = [task] { return task->canceled.load(); };
                task->result = task->gesture == SelectionGesture::Lasso && stroke.size() >= 3
                    ? AI::SurfaceSelection::visible_polygon_faces(editor->mesh(), stroke, project, pick, canceled)
                    : AI::SurfaceSelection::visible_brush_faces(editor->mesh(), stroke, brush_radius, project, pick, canceled);
            } catch (const std::exception& e) { task->error = e.what(); }
            task->done = true;
        });
        } catch (const std::exception& e) {
            task->error = e.what(); task->done = true;
            finish_surface_selection(); return;
        }
        m_surface_timer.Start(40);
        show_region_preparation_status(_L("正在选择可见表面，模型可继续转动；Esc 取消。"));
        notify_selection_changed();
    }

    void finish_surface_selection()
    {
        if (!m_surface_task || !m_surface_task->done) return;
        if (m_surface_worker.joinable()) m_surface_worker.join();
        m_surface_timer.Stop();
        auto task = std::move(m_surface_task);
        if (task->generation != m_region_generation) return;
        if (task->canceled || task->result.canceled) {
            m_region_prepare_status->Hide(); notify_selection_changed(); return;
        }
        if (!task->error.empty()) {
            show_region_preparation_status(task->refinement
                ? _L("请先圈选局部范围，在要修改处涂抹补选、在不要修改处涂抹保护，再贴合边界。范围过大时请缩小重试。")
                : _L("本次选区未完成，请缩小范围重试；当前模型保持原样。"));
            BOOST_LOG_TRIVIAL(warning) << "Surface selection failed: " << task->error;
            notify_selection_changed(); return;
        }
        auto selected = m_region_editor->selected_faces();
        if (m_protected_faces.size() != selected.size()) m_protected_faces.assign(selected.size(), 0);
        if (m_foreground_faces.size() != selected.size()) m_foreground_faces.assign(selected.size(), 0);
        if (m_selection_domain.size() != selected.size()) m_selection_domain = selected;
        push_selection_history(selected);
        if (task->refinement) std::fill(selected.begin(), selected.end(), 0);
        for (size_t face : task->result.faces) {
            if (face >= selected.size()) continue;
            if (task->gesture == SelectionGesture::Protect) {
                m_selection_domain[face] = 1;
                m_protected_faces[face] = 1; m_foreground_faces[face] = 0; selected[face] = 0;
            } else if (task->gesture == SelectionGesture::Brush) {
                // Explicitly painting here can reverse a mistaken protection stroke.
                m_protected_faces[face] = 0; m_foreground_faces[face] = 1; m_selection_domain[face] = 1; selected[face] = 1;
            } else if (!m_protected_faces[face]) { selected[face] = 1; m_selection_domain[face] = 1; }
        }
        m_region_editor->restore_selection(selected);
        rebuild_selection_model();
        show_region_preparation_status(task->result.faces.empty()
            ? _L("范围内未选到可见表面；可放大模型或用涂抹补选。")
            : _L("范围已更新。圈选会保留保护区；涂抹补选可重新纳入误保护的地方。"));
        BOOST_LOG_TRIVIAL(info) << "Surface selection: elapsed_ms=" <<
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - task->started).count()
            << ", hit_faces=" << task->result.faces.size() << ", selected_faces=" << selected_face_count();
        notify_selection_changed(); m_canvas->Refresh(false);
    }

    void select_at(const wxPoint& point)
    {
        if (m_canvas == nullptr)
            return;
        int width = 0;
        int height = 0;
        m_canvas->GetClientSize(&width, &height);
        if (width <= 0 || height <= 0)
            return;
        const Vec3d size = m_bounds.size().cast<double>();
        const double radius = std::max(0.001, 0.5 * size.norm());
        const double half_height = fitted_half_height(width, height);
        const double half_width = half_height * double(width) / double(height);
        const double screen_x = 2.0 * double(point.x) / double(width) - 1.0;
        const double screen_y = 1.0 - 2.0 * double(point.y) / double(height);
        const Vec3d center = m_bounds.center().cast<double>();
        const Transform3d view_model =
            Geometry::translation_transform(Vec3d(m_pan_x * radius, m_pan_y * radius, -3.0 * radius)) *
            view_rotation() *
            Geometry::translation_transform(-center);
        const Transform3d inverse = view_model.inverse();
        const Vec3d origin = inverse * Vec3d(screen_x * half_width, screen_y * half_height, 0.0);
        const Vec3d direction = inverse.linear() * Vec3d(0.0, 0.0, -1.0);
        if (!ensure_region_editor()) {
            if (region_editing_ready()) {
                // Save the model-space ray, not screen coordinates: users may
                // continue orbiting while the editor is being prepared.
                m_deferred_selection = [this, origin, direction, operation = m_selection_operation, settings = m_selection_settings] {
                    select_ray(origin, direction, operation, settings);
                };
                show_region_preparation_status(_L("已记住最近一次点选；准备完成后自动选中，可继续旋转模型。"));
            }
            return;
        }
        select_ray(origin, direction, m_selection_operation, m_selection_settings);
    }

    void select_ray(const Vec3d& origin, const Vec3d& direction,
                    AI::RegionSelectionOperation operation, const AI::RegionSelectionSettings& settings)
    {
        if (selection_busy()) return;
        const std::optional<size_t> face = m_region_editor->pick_face(origin, direction);
        if (!face)
            return;
        const std::vector<uint8_t> previous = m_region_editor->selected_faces();
        m_region_editor->update_selection(*face, operation, settings);
        auto mask = m_region_editor->selected_faces();
        for (size_t i = 0; i < std::min(mask.size(), m_protected_faces.size()); ++i)
            if (m_protected_faces[i]) mask[i] = 0;
        m_region_editor->restore_selection(mask);
        if (previous != m_region_editor->selected_faces())
            push_selection_history(previous);
        rebuild_selection_model();
        notify_selection_changed();
        m_canvas->Refresh(false);
    }

    void rebuild_selection_model()
    {
        m_selection_model.reset();
        m_protection_model.reset();
        if (!m_region_editor->ready())
            return;
        build_mask_model(m_region_editor->selected_faces(), m_selection_preview_color, m_selection_model);
        build_mask_model(m_protected_faces, ColorRGBA(0.3f, 0.5f, 0.95f, 1.0f), m_protection_model);
    }

    void build_mask_model(const std::vector<uint8_t>& selected, const ColorRGBA& color, std::unique_ptr<GLModel>& target)
    {
        if (selected.size() != m_region_editor->mesh().indices.size()) return;
        GLModel::Geometry geometry;
        geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};
        const indexed_triangle_set& mesh = m_region_editor->mesh();
        for (size_t face_index = 0; face_index < mesh.indices.size(); ++face_index) {
            if (!selected[face_index])
                continue;
            const stl_triangle_vertex_indices& face = mesh.indices[face_index];
            const Vec3f& a = mesh.vertices[face[0]];
            const Vec3f& b = mesh.vertices[face[1]];
            const Vec3f& c = mesh.vertices[face[2]];
            Vec3f normal = (b - a).cross(c - a);
            if (normal.squaredNorm() > 1e-12f)
                normal.normalize();
            else
                normal = Vec3f::UnitZ();
            const unsigned int base = static_cast<unsigned int>(geometry.vertices_count());
            geometry.add_vertex(a, normal);
            geometry.add_vertex(b, normal);
            geometry.add_vertex(c, normal);
            geometry.add_triangle(base, base + 1, base + 2);
        }
        if (!geometry.is_empty()) {
            target = std::make_unique<GLModel>();
            target->init_from(std::move(geometry));
            target->set_color(color);
        }
    }

    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(m_canvas);
        const bool context_ok = m_context != nullptr && m_context->IsOK();
        const bool current_ok = context_ok && m_canvas->SetCurrent(*m_context);
        if (!m_paint_diagnostics_logged) {
            BOOST_LOG_TRIVIAL(info) << "AI model preview paint: has_model=" << m_has_model
                                    << ", context_ok=" << context_ok
                                    << ", current_ok=" << current_ok
                                    << ", shown=" << m_canvas->IsShownOnScreen();
            m_paint_diagnostics_logged = true;
        }
        if (!current_ok)
            return;
        if (!wxGetApp().init_opengl())
            return;

        int width = 0;
        int height = 0;
        m_canvas->GetClientSize(&width, &height);
#if defined(__APPLE__)
        const double dpi_scale = m_canvas->GetDPIScaleFactor();
        width = std::max(1, int(std::lround(width * dpi_scale)));
        height = std::max(1, int(std::lround(height * dpi_scale)));
#else
        width = std::max(1, width);
        height = std::max(1, height);
#endif
        while (::glGetError() != GL_NO_ERROR) {}
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
        glsafe(::glDisable(GL_SCISSOR_TEST));
        glsafe(::glDisable(GL_BLEND));
        glsafe(::glDisable(GL_STENCIL_TEST));
        glsafe(::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glDepthFunc(GL_LESS));
        glsafe(::glViewport(0, 0, width, height));
        const wxColour background = wxGetApp().get_window_default_clr();
        glsafe(::glClearColor(background.Red() / 255.0f, background.Green() / 255.0f, background.Blue() / 255.0f, 1.0f));
        glsafe(::glClearDepth(1.0));
        glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

        if (m_has_model) {
            if (!m_color_shader) {
                m_color_shader = std::make_unique<GLShaderProgram>();
                if (!initialize_model_color_shader(*m_color_shader)) {
                    BOOST_LOG_TRIVIAL(error) << "AI model vertex-color shader could not be initialized";
                    m_color_shader.reset();
                }
            }
            GLShaderProgram* shader = m_color_shader.get();
            if (shader != nullptr) {
                glsafe(::glEnable(GL_DEPTH_TEST));
                glsafe(::glDisable(GL_CULL_FACE));
                shader->start_using();

                const Vec3d size = m_bounds.size().cast<double>();
                const double radius = std::max(0.001, 0.5 * size.norm());
                const double aspect = double(width) / double(height);
                const double half_height = fitted_half_height(width, height);
                const double half_width = half_height * aspect;
                const double near_z = 0.01 * radius;
                const double far_z = 8.0 * radius;
                Transform3d projection = Transform3d::Identity();
                projection.matrix().setZero();
                projection.matrix()(0, 0) = 1.0 / half_width;
                projection.matrix()(1, 1) = 1.0 / half_height;
                projection.matrix()(2, 2) = -2.0 / (far_z - near_z);
                projection.matrix()(2, 3) = -(far_z + near_z) / (far_z - near_z);
                projection.matrix()(3, 3) = 1.0;

                const Vec3d center = m_bounds.center().cast<double>();
                const Transform3d view_model =
                    Geometry::translation_transform(Vec3d(m_pan_x * radius, m_pan_y * radius, -3.0 * radius)) *
                    view_rotation() *
                    Geometry::translation_transform(-center);
                const Matrix3d normal_matrix = view_model.matrix().block(0, 0, 3, 3).inverse().transpose();
                shader->set_uniform("view_model_matrix", view_model);
                shader->set_uniform("projection_matrix", projection);
                shader->set_uniform("view_normal_matrix", normal_matrix);
                shader->set_uniform("use_uniform_color", false);
                shader->set_uniform("gray_view", m_gray_view);
                shader->set_uniform("preview_lighting", m_color_trial->lighting());
                shader->set_uniform("preview_lightness_weight", PreviewPalette::lightness_weight);
                shader->set_uniform("preview_color_count", m_color_trial_enabled && !m_gray_view ? int(m_trial_palette.size()) : 0);
                if (m_color_trial_enabled) for (size_t i = 0; i < m_trial_palette.size(); ++i) {
                    shader->set_uniform(("preview_rgb[" + std::to_string(i) + "]").c_str(), m_trial_palette[i]);
                    shader->set_uniform(("preview_lab[" + std::to_string(i) + "]").c_str(), PreviewPalette::to_lab(m_color_trial->mapping_colors()[i]));
                }
                // Pure separation must not introduce intermediate colors at
                // multisample edges or through framebuffer dithering. Restore
                // both states immediately after drawing the trial surface.
                const bool pure_separation = m_color_trial_enabled && !m_gray_view && !m_color_trial->lighting();
                const bool multisample = pure_separation && ::glIsEnabled(GL_MULTISAMPLE);
                const bool dither = pure_separation && ::glIsEnabled(GL_DITHER);
                if (pure_separation) {
                    glsafe(::glDisable(GL_MULTISAMPLE));
                    glsafe(::glDisable(GL_DITHER));
                }
                for (const std::unique_ptr<GLModel>& model : m_models)
                    model->render(shader);
                if (multisample) glsafe(::glEnable(GL_MULTISAMPLE));
                if (dither) glsafe(::glEnable(GL_DITHER));
                if ((m_selection_model || m_protection_model) && m_selection_enabled && m_selection_overlay_visible) {
                    shader->set_uniform("gray_view", false);
                    shader->set_uniform("use_uniform_color", true);
                    shader->set_uniform("preview_color_count", 0);
                    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
                    glsafe(::glPolygonOffset(-1.0f, -1.0f));
                    if (m_selection_model) m_selection_model->render(shader);
                    if (m_protection_model) m_protection_model->render(shader);
                    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
                }
                if (!m_render_diagnostics_logged) {
                    const GLenum error = ::glGetError();
                    BOOST_LOG_TRIVIAL(info) << "AI model preview render: groups=" << m_models.size()
                                            << ", viewport=" << width << "x" << height
                                            << ", shader=" << shader->get_id()
                                            << ", gl_error=" << error;
                    m_render_diagnostics_logged = true;
                }
                shader->stop_using();
                glsafe(::glDisable(GL_DEPTH_TEST));
                if (m_drawing_selection && !m_stroke.empty()) {
                    GLModel::Geometry line;
                    line.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3N3};
                    const double cw = std::max(1, m_canvas->GetClientSize().x), ch = std::max(1, m_canvas->GetClientSize().y);
                    auto add_point = [&](const Vec2d& p) {
                        line.add_vertex(Vec3f(float(2*p.x()/cw-1), float(1-2*p.y()/ch), 0), Vec3f(0, 0, 1));
                    };
                    const auto outline = selection_outline();
                    for (const auto& p : outline) add_point(p);
                    for (unsigned int i = 1; i < outline.size(); ++i) line.add_line(i-1, i);
                    if (m_selection_gesture == SelectionGesture::Lasso && outline.size() > 2)
                        line.add_line(unsigned(outline.size()-1), 0);
                    else {
                        const unsigned int base = unsigned(line.vertices_count());
                        for (unsigned int i = 0; i < 32; ++i) {
                            const double a = i * 6.283185307179586 / 32;
                            add_point(m_stroke.back() + m_brush_radius * Vec2d(std::cos(a), std::sin(a)));
                        }
                        for (unsigned int i = 0; i < 32; ++i) line.add_line(base+i, base+(i+1)%32);
                    }
                    if (!line.is_empty()) {
                        GLModel feedback; feedback.init_from(std::move(line));
                        feedback.set_color(m_selection_gesture == SelectionGesture::Protect
                            ? ColorRGBA(0.3f,0.5f,0.95f,1) : ColorRGBA(1,0.55f,0,1));
                        shader->start_using();
                        shader->set_uniform("view_model_matrix", Transform3d::Identity());
                        shader->set_uniform("projection_matrix", Transform3d::Identity());
                        shader->set_uniform("use_uniform_color", true);
                        shader->set_uniform("preview_color_count", 0);
                        shader->set_uniform("preview_lighting", false);
                        feedback.render(shader); shader->stop_using();
                    }
                }
            } else if (!m_render_diagnostics_logged) {
                BOOST_LOG_TRIVIAL(error) << "AI model preview render: vertex-color shader is unavailable";
                m_render_diagnostics_logged = true;
            }
        }
        m_canvas->SwapBuffers();
        if (m_trial_toggle_started) {
            BOOST_LOG_TRIVIAL(info) << "AI color trial frame submitted: enabled=" << m_color_trial_enabled
                << ", elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - *m_trial_toggle_started).count();
            m_trial_toggle_started.reset();
        }
    }

    wxGLCanvas* m_canvas {nullptr};
    ModelPreviewColorControls* m_color_trial {nullptr};
    std::vector<PreviewPalette::Color> m_trial_palette;
    std::shared_ptr<const PreviewPalette::Histogram> m_trial_histogram;
    bool m_color_trial_enabled {false};
    bool m_gray_view {false};
    double m_pan_x {0.0}, m_pan_y {0.0};
    std::optional<std::chrono::steady_clock::time_point> m_trial_toggle_started;
    wxGLContext* m_context {nullptr};
    std::vector<std::unique_ptr<GLModel>> m_models;
    std::unique_ptr<GLModel> m_selection_model;
    std::unique_ptr<GLModel> m_protection_model;
    std::unique_ptr<GLShaderProgram> m_color_shader;
    std::shared_ptr<AI::VertexColorRegionEditor> m_region_editor = std::make_shared<AI::VertexColorRegionEditor>();
    wxStaticText* m_region_prepare_status {nullptr};
    wxTimer m_region_prepare_timer;
    std::thread m_region_prepare_worker;
    std::shared_ptr<RegionPreparation> m_region_preparation;
    uint64_t m_region_generation {0};
    bool m_region_prepare_failed {false};
    std::function<void()> m_deferred_selection;
    indexed_triangle_set m_pending_mesh;
    std::vector<RGBA> m_pending_vertex_colors;
    std::unique_ptr<CachedPreview> m_cached_preview;
    boost::filesystem::path m_model_path;
    FileStamp m_model_stamp;
    size_t m_triangle_count {0};
    size_t m_color_count {0};
    std::vector<SelectionState> m_selection_history, m_selection_redo;
    std::vector<uint8_t> m_protected_faces;
    std::vector<uint8_t> m_foreground_faces, m_selection_domain;
    std::optional<SelectionState> m_pending_selection;
    std::string m_geometry_id;
    FaceColorOverrides m_face_color_overrides;
    SelectionGesture m_selection_gesture {SelectionGesture::Lasso};
    std::vector<Vec2d> m_stroke;
    double m_brush_radius {18.0};
    bool m_drawing_selection {false};
    wxTimer m_surface_timer;
    std::thread m_surface_worker;
    std::shared_ptr<SurfaceTask> m_surface_task;
    std::vector<std::string> m_palette;
    BoundingBoxf3 m_bounds;
    Vec3d m_model_dimensions {Vec3d::Zero()};
    wxPoint m_last_mouse;
    wxPoint m_drag_start;
    std::function<void(size_t)> m_selection_changed;
    AI::RegionSelectionSettings m_selection_settings;
    AI::RegionSelectionOperation m_selection_operation {AI::RegionSelectionOperation::Replace};
    ColorRGBA m_selection_preview_color {1.0f, 0.55f, 0.0f, 1.0f};
    Transform3d view_rotation() const
    {
        // Generated and imported OBJ files use OrcaSlicer's Z-up convention.
        // Orbit around Z first, then tilt the camera so the build plate stays
        // horizontal instead of presenting portrait bases sideways.
        return Geometry::rotation_transform(m_pitch * Vec3d::UnitX()) *
               Geometry::rotation_transform(m_yaw * Vec3d::UnitZ());
    }

    double fitted_half_height(int width, int height) const
    {
        const double aspect = double(std::max(1, width)) / double(std::max(1, height));
        const Vec3d half_extents = 0.5 * m_bounds.size().cast<double>();
        const Vec3d view_extents = view_rotation().linear().cwiseAbs() * half_extents;
        constexpr double fit_margin = 1.08;
        return std::max(0.001, std::max(view_extents.y(), view_extents.x() / aspect) * fit_margin / m_zoom);
    }

    double m_yaw {-0.65};
    double m_pitch {-1.05};
    double m_zoom {1.0};
    bool m_dragging {false};
    bool m_drag_moved {false};
    bool m_selection_enabled {false};
    bool m_selection_overlay_visible {true};
    bool m_has_model {false};
    bool m_paint_diagnostics_logged {false};
    bool m_render_diagnostics_logged {false};
};

} // namespace Slic3r::GUI
