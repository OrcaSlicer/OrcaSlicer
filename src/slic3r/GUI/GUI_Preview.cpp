//#include "stdlib.h"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Layer.hpp"
#include "IMSlider.hpp"
#include "GUI_Preview.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#if ENABLE_OPENGL_AUTO_AA_SAMPLES
#include "GUI_Init.hpp"
#endif // ENABLE_OPENGL_AUTO_AA_SAMPLES
#include "I18N.hpp"
#include "3DScene.hpp"
#include "BackgroundSlicingProcess.hpp"
#include "OpenGLManager.hpp"
#include "GLCanvas3D.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "format.hpp"

#include <wx/listbook.h>
#include <wx/notebook.h>
#include <wx/glcanvas.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/choice.h>
#include <wx/combo.h>
#include <wx/combobox.h>
#include <wx/checkbox.h>

// this include must follow the wxWidgets ones or it won't compile on Windows -> see http://trac.wxwidgets.org/ticket/2421
#include "libslic3r/Print.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "NotificationManager.hpp"

#ifdef _WIN32
#include "BitmapComboBox.hpp"
#endif

namespace Slic3r {
namespace GUI {

View3D::View3D(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process)
    : m_canvas_widget(nullptr)
    , m_canvas(nullptr)
{
    init(parent, bed, model, config, process);
}

View3D::~View3D()
{
    delete m_canvas;
    delete m_canvas_widget;
}

bool View3D::init(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process)
{
    if (!Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0 /* disable wxTAB_TRAVERSAL */))
        return false;

#if ENABLE_OPENGL_AUTO_AA_SAMPLES
    const GUI_InitParams* const init_params = wxGetApp().init_params;
    m_canvas_widget = OpenGLManager::create_wxglcanvas(*this, (init_params != nullptr) ? init_params->opengl_aa : false);
#else
    m_canvas_widget = OpenGLManager::create_wxglcanvas(*this);
    if (m_canvas_widget == nullptr)
        return false;

    m_canvas = new GLCanvas3D(m_canvas_widget, bed);
    m_canvas->set_context(wxGetApp().init_glcontext(*m_canvas_widget));

    m_canvas->allow_multisample(OpenGLManager::can_multisample());
    // XXX: If have OpenGL
    m_canvas->enable_picking(true);
    m_canvas->get_selection().set_mode(Selection::Instance);
    m_canvas->enable_moving(true);
    // XXX: more config from 3D.pm
    m_canvas->set_model(model);
    m_canvas->set_process(process);
    m_canvas->set_type(GLCanvas3D::ECanvasType::CanvasView3D);
    m_canvas->set_config(config);
    m_canvas->enable_gizmos(true);
    m_canvas->enable_selection(true);
    m_canvas->enable_main_toolbar(true);
    //BBS: GUI refactor: GLToolbar
    m_canvas->enable_select_plate_toolbar(false);
    m_canvas->enable_assemble_view_toolbar(true);
    m_canvas->enable_separator_toolbar(true);
    m_canvas->enable_labels(true);
    m_canvas->enable_slope(true);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_canvas_widget, 1, wxALL | wxEXPAND, 0);

    SetSizer(main_sizer);
    SetMinSize(GetSize());
    GetSizer()->SetSizeHints(this);

    return true;
}

void View3D::set_as_dirty()
{
    if (m_canvas != nullptr) {
        m_canvas->set_as_dirty();
    }
}

void View3D::bed_shape_changed()
{
    if (m_canvas != nullptr)
        m_canvas->bed_shape_changed();
}

void View3D::plates_count_changed()
{
    if (m_canvas != nullptr)
        m_canvas->plates_count_changed();
}

void View3D::select_view(const std::string& direction)
{
    if (m_canvas != nullptr)
        m_canvas->select_view(direction);
}

//BBS
void View3D::select_curr_plate_all()
{
    if (m_canvas != nullptr)
        m_canvas->select_curr_plate_all();
}

void View3D::select_object_from_idx(std::vector<int>& object_idxs) {
    if (m_canvas != nullptr)
        m_canvas->select_object_from_idx(object_idxs);
}

//BBS
void View3D::remove_curr_plate_all()
{
    if (m_canvas != nullptr)
        m_canvas->remove_curr_plate_all();
}

void View3D::select_all()
{
    if (m_canvas != nullptr)
        m_canvas->select_all();
}

void View3D::deselect_all()
{
    if (m_canvas != nullptr)
        m_canvas->deselect_all();
}

void View3D::exit_gizmo()
{
    if (m_canvas != nullptr)
        m_canvas->exit_gizmo();
}

void View3D::delete_selected()
{
    if (m_canvas != nullptr)
        m_canvas->delete_selected();
}

void View3D::center_selected()
{
    if (m_canvas != nullptr)
        m_canvas->do_center();
}

void View3D::drop_selected()
{
    if (m_canvas != nullptr)
        m_canvas->do_drop();
}

void View3D::center_selected_plate(const int plate_idx) {
    if (m_canvas != nullptr)
        m_canvas->do_center_plate(plate_idx);
}

void View3D::mirror_selection(Axis axis)
{
    if (m_canvas != nullptr)
        m_canvas->mirror_selection(axis);
}

bool View3D::is_layers_editing_enabled() const
{
    return (m_canvas != nullptr) ? m_canvas->is_layers_editing_enabled() : false;
}

bool View3D::is_layers_editing_allowed() const
{
    return (m_canvas != nullptr) ? m_canvas->is_layers_editing_allowed() : false;
}

void View3D::enable_layers_editing(bool enable)
{
    if (m_canvas != nullptr)
        m_canvas->enable_layers_editing(enable);
}

bool View3D::is_dragging() const
{
    return (m_canvas != nullptr) ? m_canvas->is_dragging() : false;
}

bool View3D::is_reload_delayed() const
{
    return (m_canvas != nullptr) ? m_canvas->is_reload_delayed() : false;
}

void View3D::reload_scene(bool refresh_immediately, bool force_full_scene_refresh)
{
    if (m_canvas != nullptr)
        m_canvas->reload_scene(refresh_immediately, force_full_scene_refresh);
}

void View3D::render()
{
    if (m_canvas != nullptr)
        //m_canvas->render();
        m_canvas->set_as_dirty();
}

Preview::Preview(
    wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config,
    BackgroundSlicingProcess* process, GCodeProcessorResult* gcode_result, std::function<void()> schedule_background_process_func)
    : m_config(config)
    , m_process(process)
    , m_gcode_result(gcode_result)
    , m_schedule_background_process(schedule_background_process_func)
{
    if (init(parent, bed, model))
        load_print();
}

void Preview::update_gcode_result(GCodeProcessorResult* gcode_result)
{
    m_gcode_result = gcode_result;

    return;
}

bool Preview::init(wxWindow* parent, Bed3D& bed, Model* model)
{
    if (!Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0 /* disable wxTAB_TRAVERSAL */))
        return false;

    // to match the background of the sliders
#ifdef _WIN32
    wxGetApp().UpdateDarkUI(this);
#else
    SetBackgroundColour(GetParent()->GetBackgroundColour());
#endif // _WIN32

    m_canvas_widget = OpenGLManager::create_wxglcanvas(*this);
#endif // ENABLE_OPENGL_AUTO_AA_SAMPLES
    if (m_canvas_widget == nullptr)
        return false;

    m_canvas = new GLCanvas3D(m_canvas_widget, bed);
    m_canvas->set_context(wxGetApp().init_glcontext(*m_canvas_widget));
    m_canvas->allow_multisample(OpenGLManager::can_multisample());
    m_canvas->set_config(m_config);
    m_canvas->set_model(model);
    m_canvas->set_process(m_process);
    m_canvas->set_type(GLCanvas3D::ECanvasType::CanvasPreview);
    m_canvas->get_gcode_viewer().enable_legend(true);
    //BBS: GUI refactor: GLToolbar
    if (wxGetApp().is_editor()) {
        m_canvas->enable_select_plate_toolbar(true);
    }
    m_canvas->enable_assemble_view_toolbar(false);

    // sizer, m_canvas_widget
    m_canvas_widget->Bind(wxEVT_KEY_DOWN, &Preview::update_layers_slider_from_canvas, this);

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_canvas_widget, 1, wxALL | wxEXPAND, 0);

    SetSizer(main_sizer);
    SetMinSize(GetSize());
    GetSizer()->SetSizeHints(this);

    bind_event_handlers();

    return true;
}

Preview::~Preview()
{
    unbind_event_handlers();

    if (m_canvas != nullptr)
        delete m_canvas;

    if (m_canvas_widget != nullptr)
        delete m_canvas_widget;
}

void Preview::set_as_dirty()
{
    if (m_canvas != nullptr)
        m_canvas->set_as_dirty();
}

void Preview::bed_shape_changed()
{
    if (m_canvas != nullptr)
        m_canvas->bed_shape_changed();
}

void Preview::select_view(const std::string& direction)
{
    m_canvas->select_view(direction);
}

void Preview::set_drop_target(wxDropTarget* target)
{
    if (target != nullptr)
        SetDropTarget(target);
}

//BBS: add only gcode mode
void Preview::load_print(bool keep_z_range, bool only_gcode)
{
    PrinterTechnology tech = m_process->current_printer_technology();
    if (tech == ptFFF)
        load_print_as_fff(keep_z_range, only_gcode);

    Layout();
}

//BBS: add only gcode mode
void Preview::reload_print(bool only_gcode)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" %1%: enter")%__LINE__;

    //BBS: add m_loaded_print logic
    //m_loaded = false;
    m_loaded_print = nullptr;

    load_print(false, only_gcode);
    m_only_gcode = only_gcode;
}

//BBS: always load shell at preview
void Preview::load_shells(const Print& print, bool force_previewing)
{
    m_canvas->load_shells(print, force_previewing);
}

//BBS: always load shell at preview
void Preview::reset_shells()
{
    m_canvas->reset_shells();
}

void Preview::msw_rescale()
{
    // rescale warning legend on the canvas
    get_canvas3d()->msw_rescale();

    // rescale legend
    reload_print(m_only_gcode);
}

void Preview::sys_color_changed()
{
    //TODO
    // m_layers_slider->sys_color_changed();
}

void Preview::on_tick_changed(Type type)
{
    //if (type == Type::PausePrint) {
    //    m_schedule_background_process();
    //}
    m_keep_current_preview_type = false;
    reload_print(false);
}

void Preview::bind_event_handlers()
{
    this->Bind(wxEVT_SIZE, &Preview::on_size, this);
}

void Preview::unbind_event_handlers()
{
    this->Unbind(wxEVT_SIZE, &Preview::on_size, this);
}

void Preview::show_sliders(bool show)
{
    show_moves_sliders(show);
    show_layers_sliders(show);
}

void Preview::show_moves_sliders(bool show)
{
    ;//TODO
}

void Preview::show_layers_sliders(bool show)
{
    ;//TODO
}


void Preview::on_size(wxSizeEvent& evt)
{
    evt.Skip();
    Refresh();
}

void Preview::check_layers_slider_values(std::vector<CustomGCode::Item>& ticks_from_model, const std::vector<double>& layers_z)
{
    // All ticks that would end up outside the slider range should be erased.
    // TODO: this should be placed into more appropriate part of code,
    // this function is e.g. not called when the last object is deleted
    unsigned int old_size = ticks_from_model.size();
    ticks_from_model.erase(std::remove_if(ticks_from_model.begin(), ticks_from_model.end(),
                     [layers_z](CustomGCode::Item val)
        {
            auto it = std::lower_bound(layers_z.begin(), layers_z.end(), val.print_z - epsilon());
            return it == layers_z.end();
        }),
        ticks_from_model.end());
    if (ticks_from_model.size() != old_size)
        m_schedule_background_process();
}

// Find an index of a value in a sorted vector, which is in <z-eps, z+eps>.
// Returns -1 if there is no such member.
static int find_close_layer_idx(const std::vector<double> &zs, double &z, double eps)
{
    if (zs.empty()) return -1;
    auto it_h = std::lower_bound(zs.begin(), zs.end(), z);
    if (it_h == zs.end()) {
        auto it_l = it_h;
        --it_l;
        if (z - *it_l < eps) return int(zs.size() - 1);
    } else if (it_h == zs.begin()) {
        if (*it_h - z < eps) return 0;
    } else {
        auto it_l = it_h;
        --it_l;
        double dist_l = z - *it_l;
        double dist_h = *it_h - z;
        if (std::min(dist_l, dist_h) < eps) { return (dist_l < dist_h) ? int(it_l - zs.begin()) : int(it_h - zs.begin()); }
    }
    return -1;
}

void Preview::update_layers_slider_mode()
{
    //    true  -> single-extruder printer profile OR
    //             multi-extruder printer profile , but whole model is printed by only one extruder
    //    false -> multi-extruder printer profile , and model is printed by several extruders
    bool one_extruder_printed_model = true;
    bool can_change_color = true;
    // extruder used for whole model for multi-extruder printer profile
    int only_extruder = -1;

    // BBS
    if (wxGetApp().filaments_cnt() > 1) {
        //const ModelObjectPtrs& objects = wxGetApp().plater()->model().objects;
        auto plate_extruders = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_extruders_without_support();
        for (auto extruder : plate_extruders) {
            if (extruder != plate_extruders[0])
                can_change_color = false;
        }
        // check if whole model uses just only one extruder
        if (!plate_extruders.empty()) {
            //const int extruder = objects[0]->config.has("extruder") ? objects[0]->config.option("extruder")->getInt() : 0;
            only_extruder = plate_extruders[0];
            //    auto is_one_extruder_printed_model = [objects, extruder]() {
            //        for (ModelObject *object : objects) {
            //            if (object->config.has("extruder") && object->config.option("extruder")->getInt() != extruder) /*return false*/;

            //            for (ModelVolume *volume : object->volumes)
            //                if ((volume->config.has("extruder") && volume->config.option("extruder")->getInt() != extruder) || !volume->mmu_segmentation_facets.empty()) return false;

            //            for (const auto &range : object->layer_config_ranges)
            //                if (range.second.has("extruder") && range.second.option("extruder")->getInt() != extruder) return false;
            //        }
            //        return true;
            //    };

            //    if (is_one_extruder_printed_model())
            //        only_extruder = extruder;
            //    else
            //        one_extruder_printed_model = false;
        }
    }

    IMSlider *m_layers_slider = m_canvas->get_gcode_viewer().get_layers_slider();
    m_layers_slider->SetModeAndOnlyExtruder(one_extruder_printed_model, only_extruder, can_change_color);
}

void Preview::update_layers_slider_from_canvas(wxKeyEvent &event)
{
    if (event.HasModifiers()) {
        event.Skip();
        return;
    }

    const auto key = event.GetKeyCode();

    IMSlider *m_layers_slider = m_canvas->get_gcode_viewer().get_layers_slider();
    IMSlider *m_moves_slider  = m_canvas->get_gcode_viewer().get_moves_slider();
    if (key == 'L') {
        if(!m_layers_slider->switch_one_layer_mode())
            event.Skip();
        m_canvas->set_as_dirty();
    }
    /*else if (key == WXK_SHIFT)
        m_layers_slider->UseDefaultColors(false);*/
    else
        event.Skip();
}

void Preview::update_layers_slider(const std::vector<double>& layers_z, bool keep_z_range)
{
    IMSlider *m_layers_slider = m_canvas->get_gcode_viewer().get_layers_slider();
    // Save the initial slider span.
    double z_low     = m_layers_slider->GetLowerValueD();
    double z_high    = m_layers_slider->GetHigherValueD();
    bool   was_empty = m_layers_slider->GetMaxValue() == 0;

    bool force_sliders_full_range = was_empty;
    if (!keep_z_range) {
        bool span_changed = layers_z.empty() || std::abs(layers_z.back() - m_layers_slider->GetMaxValueD()) > epsilon() /*1e-6*/;
        force_sliders_full_range |= span_changed;
    }
    bool snap_to_min = force_sliders_full_range || m_layers_slider->is_lower_at_min();
    bool snap_to_max = force_sliders_full_range || m_layers_slider->is_higher_at_max();

    // Detect and set manipulation mode for double slider
    update_layers_slider_mode();

    Plater* plater = wxGetApp().plater();
    //BBS: replace model custom gcode with current plate custom gcode
    CustomGCode::Info ticks_info_from_curr_plate;
    if (wxGetApp().is_editor())
        ticks_info_from_curr_plate = plater->model().get_curr_plate_custom_gcodes();
    else {
        ticks_info_from_curr_plate.mode   = CustomGCode::Mode::SingleExtruder;
        ticks_info_from_curr_plate.gcodes = m_gcode_result->custom_gcode_per_print_z;
    }
    check_layers_slider_values(ticks_info_from_curr_plate.gcodes, layers_z);

    // first of all update extruder colors to avoid crash, when we are switching printer preset from MM to SM
    m_layers_slider->SetExtruderColors(plater->get_extruder_colors_from_plater_config(wxGetApp().is_editor() ? nullptr : m_gcode_result));
    m_layers_slider->SetSliderValues(layers_z);
    assert(m_layers_slider->GetMinValue() == 0);
    m_layers_slider->SetMaxValue(layers_z.empty() ? 0 : layers_z.size() - 1);

    int idx_low  = 0;
    int idx_high = m_layers_slider->GetMaxValue();
    if (!layers_z.empty()) {
        if (!snap_to_min) {
            int idx_new = find_close_layer_idx(layers_z, z_low, epsilon() /*1e-6*/);
            if (idx_new != -1) idx_low = idx_new;
        }
        if (!snap_to_max) {
            int idx_new = find_close_layer_idx(layers_z, z_high, epsilon() /*1e-6*/);
            if (idx_new != -1) idx_high = idx_new;
        }
    }
    m_layers_slider->SetSelectionSpan(idx_low, idx_high);

    auto curr_plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    auto curr_print_seq = curr_plate->get_real_print_seq();
    bool sequential_print = (curr_print_seq == PrintSequence::ByObject);
    m_layers_slider->SetDrawMode(sequential_print);
    
    m_layers_slider->SetTicksValues(ticks_info_from_curr_plate);

    auto print_mode_stat = m_gcode_result->print_statistics.modes.front();
    m_layers_slider->SetLayersTimes(m_canvas->get_gcode_layers_times_cache(), print_mode_stat.time);

    // Suggest the auto color change, if model looks like sign
    if (m_layers_slider->IsNewPrint()) {
        const Print &print = wxGetApp().plater()->fff_print();

        // bool is_possible_auto_color_change = false;
        for (auto object : print.objects()) {
            double object_x = double(object->size().x());
            double object_y = double(object->size().y());

            // if it's sign, than object have not to be a too height
            double  height      = object->height();
            coord_t longer_side = std::max(object_x, object_y);
            auto    num_layers  = int(object->layers().size());
            if (height / longer_side > 0.3 || num_layers < 2) continue;

            const ExPolygons &bottom      = object->get_layer(0)->lslices;
            double            bottom_area = area(bottom);

            // at least 25% of object's height have to be a solid
            int i, min_solid_height = int(0.25 * num_layers);
            for (i = 1; i <= min_solid_height; ++i) {
                double cur_area = area(object->get_layer(i)->lslices);
                if (!equivalent_areas(bottom_area, cur_area)) {
                    // but due to the elephant foot compensation, the first layer may be slightly smaller than the others
                    if (i == 1 && fabs(cur_area - bottom_area) / bottom_area < 0.1) {
                        // So, let process this case and use second layer as a bottom
                        bottom_area = cur_area;
                        continue;
                    }
                    break;
                }
            }
            if (i < min_solid_height) continue;
        }
    }
}

//BBS: add only gcode mode
void Preview::load_print_as_fff(bool keep_z_range, bool only_gcode)
{
    if (wxGetApp().mainframe == nullptr || wxGetApp().is_recreating_gui())
        // avoid processing while mainframe is being constructed
        return;

    //BBS: add m_loaded_print logic
    const Print *print = m_process->fff_print();
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" %1%: previous print %2%, new print %3%")%__LINE__ %m_loaded_print %print;
    if ((m_loaded_print&&(m_loaded_print == print)) || m_process->current_printer_technology() != ptFFF) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" %1%: already loaded before, return directly")%__LINE__;
        return;
    }

    // we require that there's at least one object and the posSlice step
    // is performed on all of them(this ensures that _shifted_copies was
    // populated and we know the number of layers)
    bool has_layers = false;
    //BBS: always load shell at preview
    load_shells(*print, true);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" %1%: print: %2%, gcode_result %3%, check started")%__LINE__ %print %m_gcode_result;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: print is step done, posSlice %2%, posSupportMaterial %3%, psGCodeExport %4%") % __LINE__ % print->is_step_done(posSlice) %print->is_step_done(posSupportMaterial) % print->is_step_done(psGCodeExport);
    if (print->is_step_done(posSlice)) {
        for (const PrintObject* print_object : print->objects())
            if (! print_object->layers().empty()) {
                has_layers = true;
                break;
            }
    }
    if (print->is_step_done(posSupportMaterial)) {
        for (const PrintObject* print_object : print->objects())
            if (! print_object->support_layers().empty()) {
                has_layers = true;
                break;
            }
    }

    //BBS: support preview gcode directly even if no slicing
    bool directly_preview = print->is_step_done(psGCodeExport) && !m_gcode_result->moves.empty();
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": directly_preview: %1%, gcode_result moves %2%, has_layers %3%") % directly_preview % m_gcode_result->moves.size() %has_layers;
    if (wxGetApp().is_editor() && !has_layers && !directly_preview) {
        show_sliders(false);
        m_canvas_widget->Refresh();
        return;
    }
    //BBS: for direct preview, don't keep z range
    else if (directly_preview && !has_layers)
        keep_z_range = false;

    libvgcode::EViewType gcode_view_type = m_canvas->get_gcode_view_type();
    const bool gcode_preview_data_valid = !m_gcode_result->moves.empty();
    const bool is_pregcode_preview = !gcode_preview_data_valid && wxGetApp().is_editor();

    const std::vector<std::string> tool_colors = wxGetApp().plater()->get_extruder_colors_from_plater_config(m_gcode_result);
    const std::vector<CustomGCode::Item>& color_print_values = wxGetApp().is_editor() ?
        wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes : m_gcode_result->custom_gcode_per_print_z;
    std::vector<std::string> color_print_colors;
    if (!color_print_values.empty()) {
        color_print_colors = wxGetApp().plater()->get_colors_for_color_print(m_gcode_result);
        color_print_colors.push_back("#808080"); // gray color for pause print or custom G-code
    }

    std::vector<double> zs;

    if (IsShown()) {
        m_canvas->set_selected_extruder(0);
        bool is_slice_result_valid = wxGetApp().plater()->get_partplate_list().get_curr_plate()->is_slice_result_valid();
        if (gcode_preview_data_valid && (is_slice_result_valid || only_gcode)) {
            // Load the real G-code preview.
            //BBS: add more log
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": will load gcode_preview from result, moves count %1%") % m_gcode_result->moves.size();
            //BBS: add only gcode mode
            m_canvas->load_gcode_preview(*m_gcode_result, tool_colors, color_print_colors, only_gcode);
            // the view type may have been changed by the call m_canvas->load_gcode_preview()
            gcode_view_type = m_canvas->get_gcode_view_type();
            //BBS show sliders
            show_moves_sliders();

            //Orca: keep shell preview on but make it more transparent
            m_canvas->set_shells_on_previewing(true);
            m_canvas->set_shell_transparence();
            Refresh();
            zs = m_canvas->get_gcode_layers_zs();
            //BBS: add m_loaded_print logic
            //m_loaded = true;
            m_loaded_print = print;
        }
        else if (is_pregcode_preview) {
            // Load the initial preview based on slices, not the final G-code.
            //BBS: only display shell before slicing result out
            //m_canvas->load_preview(colors, color_print_values);
            show_moves_sliders(false);
            Refresh();
            //zs = m_canvas->get_volumes_print_zs(true);
        }

        if (!zs.empty() && !m_keep_current_preview_type) {
            unsigned int number_extruders = wxGetApp().is_editor() ?
                (unsigned int)print->extruders().size() :
                m_canvas->get_gcode_extruders_count();
            std::vector<CustomGCode::Item> gcodes = wxGetApp().is_editor() ?
                //BBS
                wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes :
                m_gcode_result->custom_gcode_per_print_z;
            const wxString choice = !gcodes.empty() ?
                _L("Multicolor Print") :
                (number_extruders > 1) ? _L("Filaments") : _L("Line Type");
        }

        if (zs.empty()) {
            // all layers filtered out
            //BBS
            show_layers_sliders(false);
            m_canvas_widget->Refresh();
        }
        else
            update_layers_slider(zs, keep_z_range);
    }
}

AssembleView::AssembleView(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process)
    : m_canvas_widget(nullptr)
    , m_canvas(nullptr)
{
    init(parent, bed, model, config, process);
}

AssembleView::~AssembleView()
{
    delete m_canvas;
    delete m_canvas_widget;
}

bool AssembleView::init(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process)
{
    if (!Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0 /* disable wxTAB_TRAVERSAL */))
        return false;

#if ENABLE_OPENGL_AUTO_AA_SAMPLES
    const GUI_InitParams* const init_params = wxGetApp().init_params;
    m_canvas_widget = OpenGLManager::create_wxglcanvas(*this, (init_params != nullptr) ? init_params->opengl_aa : false);
#else
    m_canvas_widget = OpenGLManager::create_wxglcanvas(*this);
#endif // ENABLE_OPENGL_AUTO_AA_SAMPLES
    if (m_canvas_widget == nullptr)
        return false;

    m_canvas = new GLCanvas3D(m_canvas_widget, bed);
    m_canvas->set_context(wxGetApp().init_glcontext(*m_canvas_widget));

    m_canvas->allow_multisample(OpenGLManager::can_multisample());
    // XXX: If have OpenGL
    m_canvas->enable_picking(true);
    m_canvas->enable_moving(true);
    // XXX: more config from 3D.pm
    m_canvas->set_model(model);
    m_canvas->set_process(process);
    m_canvas->set_type(GLCanvas3D::ECanvasType::CanvasAssembleView);
    m_canvas->set_config(config);
    m_canvas->enable_gizmos(true);
    m_canvas->enable_selection(true);
    m_canvas->enable_main_toolbar(false);
    m_canvas->enable_labels(false);
    m_canvas->enable_slope(false);
    //BBS: GUI refactor: GLToolbar
    m_canvas->enable_assemble_view_toolbar(false);
    m_canvas->enable_return_toolbar(true);
    m_canvas->enable_separator_toolbar(false);
    //m_canvas->set_show_world_axes(true);//wait for GitHub users to see if they have this requirement
    // BBS: set volume_selection_mode to Volume
    //same to 3d //m_canvas->get_selection().set_volume_selection_mode(Selection::Instance);
    //m_canvas->get_selection().lock_volume_selection_mode();

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_canvas_widget, 1, wxALL | wxEXPAND, 0);

    SetSizer(main_sizer);
    SetMinSize(GetSize());
    GetSizer()->SetSizeHints(this);

    return true;
}

void AssembleView::set_as_dirty()
{
    if (m_canvas != nullptr)
        m_canvas->set_as_dirty();
}

void AssembleView::render()
{
    if (m_canvas != nullptr)
        m_canvas->set_as_dirty();
}

bool AssembleView::is_reload_delayed() const
{
    return (m_canvas != nullptr) ? m_canvas->is_reload_delayed() : false;
}

void AssembleView::reload_scene(bool refresh_immediately, bool force_full_scene_refresh)
{
    if (m_canvas != nullptr) {
        if (!m_canvas->is_initialized()) {
            m_canvas->render(true);
        }
        m_canvas->reload_scene(refresh_immediately, force_full_scene_refresh);
    }
}

void AssembleView::select_view(const std::string& direction)
{
    if (m_canvas != nullptr)
        m_canvas->select_view(direction);
}


} // namespace GUI
} // namespace Slic3r
