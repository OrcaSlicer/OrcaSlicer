#include "GLGizmoSimplify.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"

#include <GL/glew.h>

#include <thread>

namespace Slic3r::GUI {

// Extend call after only when Simplify gizmo is still alive
static void call_after_if_active(std::function<void()> fn, GUI_App* app = &wxGetApp())
{
    // check application GUI
    if (app == nullptr) return;
    app->CallAfter([fn, app]() {
        // app must exist because it call this
        // if (app == nullptr) return;
        const Plater *plater = app->plater();
        if (plater == nullptr) return;
        const GLCanvas3D *canvas = plater->canvas3D();
        if (canvas == nullptr) return;
        const GLGizmosManager &mng = canvas->get_gizmos_manager();
        // check if simplify is still activ gizmo
        if (mng.get_current_type() != GLGizmosManager::Simplify) return;
        fn();
    });
}

static ModelVolume* get_model_volume(const Selection& selection, Model& model)
{
    const Selection::IndicesList& idxs = selection.get_volume_idxs();
    // only one selected volume
    if (idxs.size() != 1)
        return nullptr;
    const GLVolume* selected_volume = selection.get_volume(*idxs.begin());
    if (selected_volume == nullptr)
        return nullptr;

    const GLVolume::CompositeID& cid = selected_volume->composite_id;
    const ModelObjectPtrs& objs = model.objects;
    if (cid.object_id < 0 || objs.size() <= static_cast<size_t>(cid.object_id))
        return nullptr;
    const ModelObject* obj = objs[cid.object_id];
    if (cid.volume_id < 0 || obj->volumes.size() <= static_cast<size_t>(cid.volume_id))
        return nullptr;
    return obj->volumes[cid.volume_id];
}

GLGizmoSimplify::GLGizmoSimplify(GLCanvas3D &       parent,
                                 const std::string &icon_filename,
                                 unsigned int       sprite_id)
    : GLGizmoBase(parent, icon_filename, -1)
    , m_volume(nullptr)
    , m_show_wireframe(false)
    , m_move_to_center(false)
    // translation for GUI size
    , tr_mesh_name(_u8L("Mesh name"))
    , tr_triangles(_u8L("Triangles"))
    , tr_detail_level(_u8L("Detail level"))
    , tr_decimate_ratio(_u8L("Decimate ratio"))
{}

GLGizmoSimplify::~GLGizmoSimplify()
{
    stop_worker_thread_request();
    if (m_worker.joinable())
        m_worker.join();
    m_glmodel.reset();
}

bool GLGizmoSimplify::on_esc_key_down() {
    return false;
    /*if (!m_is_worker_running)
        return false;
    stop_worker_thread_request();
    return true;*/
}

// while opening needs GLGizmoSimplify to set window position
void GLGizmoSimplify::add_simplify_suggestion_notification(
    const std::vector<size_t> &object_ids,
    const std::vector<ModelObject*>&    objects,
    NotificationManager &      manager)
{
    std::vector<size_t> big_ids;
    big_ids.reserve(object_ids.size());
    auto is_big_object = [&objects](size_t object_id) {
        const uint32_t triangles_to_suggest_simplify = 1000000;
        if (object_id >= objects.size()) return false; // out of object index
        ModelVolumePtrs &volumes = objects[object_id]->volumes;
        if (volumes.size() != 1) return false; // not only one volume
        size_t triangle_count = volumes.front()->mesh().its.indices.size();
        if (triangle_count < triangles_to_suggest_simplify)
            return false; // small volume
        return true;
    };
    std::copy_if(object_ids.begin(), object_ids.end(),
                 std::back_inserter(big_ids), is_big_object);
    if (big_ids.empty()) return;

    for (size_t object_id : big_ids) {
        std::string t = GUI::format(_L(
            "Processing model '%1%' with more than 1M triangles "
            "could be slow. It is highly recommended to simplify the model."), objects[object_id]->name);
        std::string hypertext = _u8L("Simplify model");

        std::function<bool(wxEvtHandler *)> open_simplify =
            [object_id](wxEvtHandler *) {
                auto plater = wxGetApp().plater();
                if (object_id >= plater->model().objects.size()) return true;

                Selection &selection = plater->canvas3D()->get_selection();
                selection.clear();
                selection.add_object((unsigned int) object_id);

                auto &manager = plater->get_view3D_canvas3D()->get_gizmos_manager();
                bool  close_notification = true;
                if(!manager.open_gizmo(GLGizmosManager::Simplify))
                    return close_notification;
                GLGizmoSimplify* simplify = dynamic_cast<GLGizmoSimplify*>(manager.get_current());
                if (simplify == nullptr) return close_notification;
                simplify->set_center_position();
                return close_notification;
            };
        manager.push_simplify_suggestion_notification(
            t, objects[object_id]->id(), hypertext, open_simplify);
    }
}

std::string GLGizmoSimplify::on_get_name() const
{
    return _u8L("Simplify");
}

void GLGizmoSimplify::on_render_input_window(float x, float y, float bottom_limit)
{
    create_gui_cfg();

    const Selection &selection = m_parent.get_selection();
    const ModelVolume *act_volume = get_model_volume(selection, wxGetApp().plater()->model());
    if (act_volume == nullptr) {
        stop_worker_thread_request();
        close();
        if (! m_parent.get_selection().is_single_volume()) {
            MessageDialog msg((wxWindow*)wxGetApp().mainframe,
                _L("Simplification is currently only allowed when a single part is selected"),
                _L("Error"));
            msg.ShowModal();
        }
        return;
    }

    bool is_cancelling = false;
    bool is_worker_running = false;
    bool is_result_ready = false;
    int progress = 0;
    {
        std::lock_guard lk(m_state_mutex);
        is_cancelling = m_state.status == State::cancelling;
        is_worker_running = m_state.status == State::running;
        is_result_ready = bool(m_state.result);
        progress = m_state.progress;
    }

    // Whether to trigger calculation after rendering is done.
    bool start_process = false;


    // Check selection of new volume
    // Do not reselect object when processing
    if (act_volume != m_volume) {
        bool change_window_position = (m_volume == nullptr);
        // select different model

        // close suggestion notification
        auto notification_manager = wxGetApp().plater()->get_notification_manager();
        notification_manager->remove_simplify_suggestion_with_id(act_volume->get_object()->id());

        m_volume = act_volume;
        m_configuration.decimate_ratio = 50.; // default value
        m_configuration.fix_count_by_ratio(m_volume->mesh().its.indices.size());
        init_model(m_volume->mesh().its);

        // Start processing. If we switched from another object, process will
        // stop the background thread and it will restart itself later.
        start_process = true;

        // set window position
        if (m_move_to_center && change_window_position) {
            m_move_to_center = false;
            auto parent_size = m_parent.get_canvas_size();
            ImVec2 pos(parent_size.get_width() / 2 - m_gui_cfg->window_offset_x,
                       parent_size.get_height() / 2 - m_gui_cfg->window_offset_y);
            ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        }else if (change_window_position) {
            ImVec2 pos = ImGui::GetMousePos();
            pos.x -= m_gui_cfg->window_offset_x;
            pos.y -= m_gui_cfg->window_offset_y;
            // minimal top left value
            ImVec2 tl(m_gui_cfg->window_padding, m_gui_cfg->window_padding + m_parent.get_main_toolbar_height());
            if (pos.x < tl.x) pos.x = tl.x;
            if (pos.y < tl.y) pos.y = tl.y;
            // maximal bottom right value
            auto parent_size = m_parent.get_canvas_size();
            ImVec2 br(
                parent_size.get_width() - (2 * m_gui_cfg->window_offset_x + m_gui_cfg->window_padding),
                parent_size.get_height() - (2 * m_gui_cfg->window_offset_y + m_gui_cfg->window_padding));
            if (pos.x > br.x) pos.x = br.x;
            if (pos.y > br.y) pos.y = br.y;
            ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        }
    }
    float space_size = m_imgui->get_style_scaling() * 8;
    float mesh_size = m_imgui->calc_text_size(_L("Mesh name")).x + m_imgui->scaled(2.0f);
    float triangle_size = m_imgui->calc_text_size(_L("Triangles")).x + m_imgui->scaled(2.0f);
    float text_left_width = std::max(triangle_size,mesh_size);

    float detail_size = m_imgui->calc_text_size(_L("Detail level")).x + m_imgui->scaled(2.0f);
    float decimate_size = m_imgui->calc_text_size(_L("Decimate ratio")).x + m_imgui->scaled(2.0f);
    float circle_size = m_imgui->scaled(2.0f);
    float bottom_left_width = std::max(detail_size, decimate_size) + circle_size;

    float slider_width = m_imgui->scaled(5.0f);

    m_imgui->push_common_window_style(m_parent.get_scale());
    int flag = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize |
               ImGuiWindowFlags_NoCollapse;
    m_imgui->begin(on_get_name(), flag);

    m_imgui->text(tr_mesh_name + ":");
    // BBS: somehow the calculated utf8 width is too narrow, have to add 35 here
    ImGui::SameLine(text_left_width + space_size);
    std::string name = m_volume->name;
    if (name.length() > m_gui_cfg->max_char_in_name)
        name = name.substr(0, m_gui_cfg->max_char_in_name - 3) + "...";
    m_imgui->text_colored(ImVec4(0.42f, 0.42f, 0.42f, 1.00f), name);

    m_imgui->text(tr_triangles + ":");
    ImGui::SameLine(text_left_width + space_size);

    size_t orig_triangle_count = m_volume->mesh().its.indices.size();
    m_imgui->text_colored(ImVec4(0.42f, 0.42f, 0.42f, 1.00f), std::to_string(orig_triangle_count));

    ImGui::Separator();

    if (m_imgui->bbl_radio_button("##use_error", !m_configuration.use_count)) {
        m_configuration.use_count = !m_configuration.use_count;
        start_process = true;
    }

    ImGui::SameLine();
    m_imgui->disabled_begin(m_configuration.use_count);
    ImGui::Text("%s", tr_detail_level.c_str());
    std::vector<std::string> reduce_captions = {
        static_cast<std::string>(_u8L("Extra high")),
        static_cast<std::string>(_u8L("High")),
        static_cast<std::string>(_u8L("Medium")),
        static_cast<std::string>(_u8L("Low")),
        static_cast<std::string>(_u8L("Extra low"))
    };
    ImGui::SameLine(bottom_left_width);
    ImGui::PushItemWidth(bottom_left_width - space_size);
    static int reduction = 2;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.81f, 0.81f, 0.81f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.81f, 0.81f, 0.81f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.81f, 0.81f, 0.81f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_ORCA); // ORCA Use orca color for step slider text
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGuiWrapper::COL_ORCA); // ORCA Use orca color for step slider thumb

    if (m_imgui->bbl_sliderin("##ReductionLevel", &reduction, 0, 4, reduce_captions[reduction].c_str())) {
        if (reduction < 0) reduction = 0;
        if (reduction > 4) reduction = 4;
        switch (reduction) {
        case 0: m_configuration.max_error = 1e-3f; break;
        case 1: m_configuration.max_error = 1e-2f; break;
        case 2: m_configuration.max_error = 0.1f; break;
        case 3: m_configuration.max_error = 0.5f; break;
        case 4: m_configuration.max_error = 1.f; break;
        }
        start_process = true;
    }
    ImGui::PopStyleColor(5);
    m_imgui->disabled_end(); // !use_count

    if (m_imgui->bbl_radio_button("##use_count", m_configuration.use_count)) {
        m_configuration.use_count = !m_configuration.use_count;
        start_process = true;
    }

    ImGui::SameLine();

    // show preview result triangle count (percent)
    if (!m_configuration.use_count) {
        m_configuration.wanted_count = static_cast<uint32_t>(m_triangle_count);
        m_configuration.decimate_ratio =
            (1.0f - (m_configuration.wanted_count / (float) orig_triangle_count)) * 100.f;
    }

    m_imgui->disabled_begin(!m_configuration.use_count);
    ImGui::Text("%s", tr_decimate_ratio.c_str());
    ImGui::SameLine(bottom_left_width);

    const char * format = (m_configuration.decimate_ratio > 10)? "%.0f %%":
        ((m_configuration.decimate_ratio > 1)? "%.1f %%":"%.2f %%");

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
    ImGui::PushItemWidth(slider_width + space_size);
    if (m_imgui->bbl_slider_float_style("##decimate_ratio", &m_configuration.decimate_ratio, 0.f, 100.f, format)) {
        if (m_configuration.decimate_ratio < 0.f)
            m_configuration.decimate_ratio = 0.01f;
        if (m_configuration.decimate_ratio > 100.f)
            m_configuration.decimate_ratio = 100.f;
        m_configuration.fix_count_by_ratio(orig_triangle_count);
        start_process = true;
    }
    ImGui::SameLine();
    ImGui::PopStyleVar(1);

    ImGui::PushItemWidth(ImGui::CalcTextSize("100.00%").x + space_size);

    ImGui::BBLDragFloat("##decimate_ratio_input", &m_configuration.decimate_ratio, 0.05f, 0.0f, 0.0f, "%.2f%%");

    ImGui::NewLine();
    ImGui::SameLine(bottom_left_width + space_size);
    ImGui::Text(_u8L("%d triangles").c_str(), m_configuration.wanted_count);
    m_imgui->disabled_end(); // use_count

    m_imgui->bbl_checkbox(_L("Show wireframe").c_str(), m_show_wireframe);

     // draw progress bar
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,12);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(10,20));
    if (is_worker_running) { // apply or preview
        // draw progress bar
        std::string progress_text = GUI::format("%1%", std::to_string(progress)) + "%%";
        ImVec2 progress_size(bottom_left_width - space_size, 0.0f);
        ImGui::BBLProgressBar2(progress / 100., progress_size);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.42f, 0.42f, 0.42f, 1.00f), progress_text.c_str());
        ImGui::SameLine(bottom_left_width + slider_width +  m_imgui->scaled(1.0f));
    } else {
        ImGui::Dummy(ImVec2(bottom_left_width - space_size, -1));
        ImGui::SameLine(bottom_left_width + slider_width +  m_imgui->scaled(1.0f));
    }
    ImGui::PopStyleVar(3);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,12);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(10,3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(10,0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.15f, 0.18f, 0.19f, 1.00f));

    m_imgui->disabled_begin(is_worker_running || ! is_result_ready);
    m_imgui->push_confirm_button_style();
    if (m_imgui->bbl_button(_L("Apply"))) {
        apply_simplify();
    }
    else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && is_worker_running) {
        ImGui::SetTooltip("%s", _u8L("Can't apply when processing preview.").c_str());
    }
    m_imgui->pop_confirm_button_style();
    m_imgui->disabled_end(); // state !settings

    ImGui::SameLine();

    m_imgui->disabled_begin(is_cancelling);
    m_imgui->push_cancel_button_style();
    if (m_imgui->bbl_button(_L("Cancel"))) {
        close();
    }
    else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && is_cancelling)
        ImGui::SetTooltip("%s", _u8L("Operation already cancelling. Please wait a few seconds.").c_str());
    m_imgui->pop_cancel_button_style();
    m_imgui->disabled_end(); // state cancelling

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(1);

    m_imgui->end();
    m_imgui->pop_common_window_style();
    if (start_process)
        process();
}


void GLGizmoSimplify::close() {
    // close gizmo == open it again
    GLGizmosManager &gizmos_mgr = m_parent.get_gizmos_manager();
    gizmos_mgr.open_gizmo(GLGizmosManager::EType::Simplify);
}

void GLGizmoSimplify::stop_worker_thread_request()
{
    std::lock_guard lk(m_state_mutex);
    if (m_state.status == State::running)
        m_state.status = State::Status::cancelling;
}


// Following is called from a UI thread when the worker terminates
// worker calls it through a CallAfter.
void GLGizmoSimplify::worker_finished()
{
    {
        std::lock_guard lk(m_state_mutex);
        if (m_state.status == State::running) {
            // Someone started the worker again, before this callback
            // was called. Do nothing.
            return;
        }
    }
    if (m_worker.joinable())
        m_worker.join();
    if (GLGizmoBase::m_state == Off)
        return;
    if (m_state.result)
        init_model(*m_state.result);
    if (m_state.config != m_configuration || m_state.mv != m_volume) {
        // Settings were changed, restart the worker immediately.
        process();
    }
    request_rerender(true);
}

void GLGizmoSimplify::process()
{
    if (m_volume == nullptr || m_volume->mesh().its.indices.empty())
        return;

    bool configs_match = false;
    bool result_valid  = false;
    bool is_worker_running = false;
    {
        std::lock_guard lk(m_state_mutex);
        configs_match = (m_volume == m_state.mv && m_state.config == m_configuration);
        result_valid = bool(m_state.result);
        is_worker_running = m_state.status == State::running;
    }

    if ((result_valid || is_worker_running) && configs_match) {
        // Either finished or waiting for result already. Nothing to do.
        return;
    }

    if (is_worker_running && ! configs_match) {
        // Worker is running with outdated config. Stop it. It will
        // restart itself when cancellation is done.
        stop_worker_thread_request();
        return;
    }

    if (m_worker.joinable()) {
        // This can happen when process() is called after previous worker terminated,
        // but before the worker_finished callback was called. In this case, just join the thread,
        // the callback will check this and do nothing.
        m_worker.join();
    }

    // Copy configuration that will be used.
    m_state.config = m_configuration;
    m_state.mv = m_volume;
    m_state.status = State::running;

    // Create a copy of current mesh to pass to the worker thread.
    // Using unique_ptr instead of pass-by-value to avoid an extra
    // copy (which would happen when passing to std::thread).
    auto its = std::make_unique<indexed_triangle_set>(m_volume->mesh().its);

    m_worker = std::thread([this](std::unique_ptr<indexed_triangle_set> its) {

        // Checks that the UI thread did not request cancellation, throws if so.
        std::function<void(void)> throw_on_cancel = [this]() {
            std::lock_guard lk(m_state_mutex);
            if (m_state.status == State::cancelling)
                throw SimplifyCanceledException();
        };

        // Called by worker thread, updates progress bar.
        // Using CallAfter so the rerequest function is run in UI thread.
        std::function<void(int)> statusfn = [this](int percent) {
            std::lock_guard lk(m_state_mutex);
            m_state.progress = percent;
            call_after_if_active([this]() { request_rerender(); });
        };

        // Initialize.
        uint32_t triangle_count = 0;
        float    max_error = std::numeric_limits<float>::max();
        {
            std::lock_guard lk(m_state_mutex);
            if (m_state.config.use_count)
                triangle_count = m_state.config.wanted_count;
            if (! m_state.config.use_count)
                max_error = m_state.config.max_error;
            m_state.progress = 0;
            m_state.result.reset();
            m_state.status = State::Status::running;
        }

        // Start the actual calculation.
        try {
            its_quadric_edge_collapse(*its, triangle_count, &max_error, throw_on_cancel, statusfn);
        } catch (SimplifyCanceledException &) {
            std::lock_guard lk(m_state_mutex);
            m_state.status = State::idle;
        }

        std::lock_guard lk(m_state_mutex);
        if (m_state.status == State::Status::running) {
            // We were not cancelled, the result is valid.
            m_state.status = State::Status::idle;
            m_state.result = std::move(its);
        }

        // Update UI. Use CallAfter so the function is run on UI thread.
        call_after_if_active([this]() { worker_finished(); });
    }, std::move(its));
}

void GLGizmoSimplify::apply_simplify() {

    const Selection& selection = m_parent.get_selection();
    int object_idx = selection.get_object_idx();

    auto plater = wxGetApp().plater();
    plater->take_snapshot(GUI::format("Simplify %1%", m_volume->name));
    plater->clear_before_change_mesh(object_idx);

    ModelVolume* mv = get_model_volume(selection, wxGetApp().model());
    assert(mv == m_volume);

    mv->set_mesh(std::move(*m_state.result));
    m_state.result.reset();
    mv->calculate_convex_hull();
    mv->invalidate_convex_hull_2d();
    mv->set_new_unique_id();
    mv->get_object()->invalidate_bounding_box();
    mv->get_object()->ensure_on_bed();

    // fix hollowing, sla support points, modifiers, ...
    plater->changed_mesh(object_idx);
    // Fix warning icon in object list
    wxGetApp().obj_list()->update_item_error_icon(object_idx, -1);
    close();
}

bool GLGizmoSimplify::on_is_activable() const
{
    return m_parent.get_selection().is_single_full_object() ||
        m_parent.get_selection().is_single_volume();
}

void GLGizmoSimplify::on_set_state()
{
    // Closing gizmo. e.g. selecting another one
    if (GLGizmoBase::m_state == GLGizmoBase::Off) {
        m_parent.toggle_model_objects_visibility(true);

        stop_worker_thread_request();
        m_volume = nullptr; // invalidate selected model
        m_glmodel.reset();
    } else if (GLGizmoBase::m_state == GLGizmoBase::On) {
        // when open by hyperlink it needs to show up
        request_rerender();
    }
}

void GLGizmoSimplify::create_gui_cfg() {
    if (m_gui_cfg.has_value()) return;
    int space_size = m_imgui->calc_text_size(std::string_view{":MM"}).x;
    GuiCfg cfg;
    cfg.top_left_width = std::max(m_imgui->calc_text_size(tr_mesh_name).x,
                                  m_imgui->calc_text_size(tr_triangles).x)
        + space_size;

    const float radio_size = ImGui::GetFrameHeight();
    cfg.bottom_left_width =
        std::max(m_imgui->calc_text_size(tr_detail_level).x,
                 m_imgui->calc_text_size(tr_decimate_ratio).x) +
        space_size + radio_size;

    cfg.input_width   = cfg.bottom_left_width * 1.5;
    cfg.window_offset_x = (cfg.bottom_left_width + cfg.input_width)/2;
    cfg.window_offset_y = ImGui::GetTextLineHeightWithSpacing() * 5;

    m_gui_cfg = cfg;
}

void GLGizmoSimplify::request_rerender(bool force) {
    int64_t now = m_parent.timestamp_now();
    if (force || now > m_last_rerender_timestamp + 250) { // 250 ms
        set_dirty();
        m_parent.schedule_extra_frame(0);
        m_last_rerender_timestamp = now;
    }
}

void GLGizmoSimplify::set_center_position() {
    m_move_to_center = true;
}


void GLGizmoSimplify::init_model(const indexed_triangle_set& its)
{
    if (its.indices.empty())
        return;

    m_glmodel.reset();
    m_glmodel.init_from(its);
    m_parent.toggle_model_objects_visibility(true); // selected volume may have changed
    m_parent.toggle_model_objects_visibility(false, m_c->selection_info()->model_object(),
        m_c->selection_info()->get_active_instance(), m_volume);

    if (const Selection&sel = m_parent.get_selection(); sel.get_volume_idxs().size() == 1)
        m_glmodel.set_color(sel.get_volume(*sel.get_volume_idxs().begin())->color);
    m_triangle_count = its.indices.size();
}

void GLGizmoSimplify::on_render()
{
    if (! m_glmodel.is_initialized())
        return;

    const auto& selection   = m_parent.get_selection();
    const auto& volume_idxs = selection.get_volume_idxs();
    if (volume_idxs.empty() || volume_idxs.size() != 1) return;
    const GLVolume *selected_volume = selection.get_volume(*volume_idxs.begin());

    // Check that the GLVolume still belongs to the ModelObject we work on.
    if (m_volume != get_model_volume(selection, wxGetApp().model()))
        return;

    const Transform3d trafo_matrix = selected_volume->world_matrix();
    auto* gouraud_shader = wxGetApp().get_shader("gouraud_light");
    glsafe(::glEnable(GL_DEPTH_TEST));
    gouraud_shader->start_using();
    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d& view_matrix = camera.get_view_matrix();
    const Transform3d view_model_matrix = view_matrix * trafo_matrix;
    gouraud_shader->set_uniform("view_model_matrix", view_model_matrix);
    gouraud_shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
    gouraud_shader->set_uniform("view_normal_matrix", view_normal_matrix);
    m_glmodel.render();
    gouraud_shader->stop_using();

    if (m_show_wireframe) {
#if SLIC3R_OPENGL_ES
        auto* contour_shader = wxGetApp().get_shader("wireframe");
#else
        auto *contour_shader = wxGetApp().get_shader("mm_contour");
#endif // SLIC3R_OPENGL_ES
        contour_shader->start_using();
        contour_shader->set_uniform("offset", OpenGLManager::get_gl_info().is_mesa() ? 0.0005 : 0.00001);
        contour_shader->set_uniform("view_model_matrix", view_model_matrix);
        contour_shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        const ColorRGBA color = m_glmodel.get_color();
        m_glmodel.set_color(ColorRGBA::WHITE());
#if !SLIC3R_OPENGL_ES
        if (!OpenGLManager::get_gl_info().is_core_profile())
            glsafe(::glLineWidth(1.0f));
        glsafe(::glPolygonMode(GL_FRONT_AND_BACK, GL_LINE));
#endif // !SLIC3R_OPENGL_ES
        //ScopeGuard offset_fill_guard([]() { glsafe(::glDisable(GL_POLYGON_OFFSET_FILL)); });
        //glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
        //glsafe(::glPolygonOffset(5.0, 5.0));
        m_glmodel.render();
#if !SLIC3R_OPENGL_ES
        glsafe(::glPolygonMode(GL_FRONT_AND_BACK, GL_FILL));
#endif // !SLIC3R_OPENGL_ES
        m_glmodel.set_color(color);
        contour_shader->stop_using();
    }

    glsafe(::glPopAttrib());
}


CommonGizmosDataID GLGizmoSimplify::on_get_requirements() const
{
    return CommonGizmosDataID(
        int(CommonGizmosDataID::SelectionInfo));
}


void GLGizmoSimplify::Configuration::fix_count_by_ratio(size_t triangle_count)
{
    if (decimate_ratio <= 0.f)
        wanted_count = static_cast<uint32_t>(triangle_count);
    else if (decimate_ratio >= 100.f)
        wanted_count = 0;
    else
        wanted_count = static_cast<uint32_t>(std::round(
            triangle_count * (100.f - decimate_ratio) / 100.f));
}

} // namespace Slic3r::GUI
