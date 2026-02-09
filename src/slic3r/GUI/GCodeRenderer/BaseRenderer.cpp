#include "BaseRenderer.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/FilamentGroupPopup.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "libslic3r/Print.hpp"
#include "../Utils/HelioDragon.hpp"
#include <imgui/imgui_internal.h>
#include <GL/glew.h>
namespace
{
    std::string get_view_type_string(Slic3r::GUI::gcode::EViewType view_type)
    {
        if (view_type == Slic3r::GUI::gcode::EViewType::Summary)
            return _u8L("Summary");
        else if (view_type == Slic3r::GUI::gcode::EViewType::FeatureType)
            return _u8L("Line Type");
        else if (view_type == Slic3r::GUI::gcode::EViewType::Height)
            return _u8L("Layer Height");
        else if (view_type == Slic3r::GUI::gcode::EViewType::Width)
            return _u8L("Line Width");
        else if (view_type == Slic3r::GUI::gcode::EViewType::Feedrate)
            return _u8L("Speed");
        else if (view_type == Slic3r::GUI::gcode::EViewType::FanSpeed)
            return _u8L("Fan Speed");
        else if (view_type == Slic3r::GUI::gcode::EViewType::Temperature)
            return _u8L("Temperature");
        else if (view_type == Slic3r::GUI::gcode::EViewType::VolumetricRate)
            return _u8L("Flow");
        else if (view_type == Slic3r::GUI::gcode::EViewType::Tool)
            return _u8L("Tool");
        else if (view_type == Slic3r::GUI::gcode::EViewType::ColorPrint)
            return _u8L("Filament");
        else if (view_type == Slic3r::GUI::gcode::EViewType::LayerTime)
            return _u8L("Layer Time");
        // helio
        else if (view_type == Slic3r::GUI::gcode::EViewType::ThermalIndexMin)
            return _u8L("Thermal Index (min)");
        else if (view_type == Slic3r::GUI::gcode::EViewType::ThermalIndexMax)
            return _u8L("Thermal Index (max)");
        else if (view_type == Slic3r::GUI::gcode::EViewType::ThermalIndexMean)
            return _u8L("Thermal Index (mean)");
        // end helio
        return "";
    }

    static std::array<float, 4> decode_color(const std::string& color) {
        static const float INV_255 = 1.0f / 255.0f;
        std::array<float, 4> ret = { 0.0f, 0.0f, 0.0f, 1.0f };
        const char* c = color.data() + 1;
        if (color.size() == 7 && color.front() == '#') {
            for (size_t j = 0; j < 3; ++j) {
                int digit1 = Slic3r::GUI::hex_digit_to_int(*c++);
                int digit2 = Slic3r::GUI::hex_digit_to_int(*c++);
                if (digit1 == -1 || digit2 == -1)
                    break;
                ret[j] = float(digit1 * 16 + digit2) * INV_255;
            }
        }
        else if (color.size() == 9 && color.front() == '#') {
            for (size_t j = 0; j < 4; ++j) {
                int digit1 = Slic3r::GUI::hex_digit_to_int(*c++);
                int digit2 = Slic3r::GUI::hex_digit_to_int(*c++);
                if (digit1 == -1 || digit2 == -1)
                    break;
                ret[j] = float(digit1 * 16 + digit2) * INV_255;
            }
        }
        return ret;
    }

    std::vector<std::array<float, 4>> decode_colors(const std::vector<std::string>& colors) {
        std::vector<std::array<float, 4>> output(colors.size(), { 0.0f, 0.0f, 0.0f, 1.0f });
        for (size_t i = 0; i < colors.size(); ++i) {
            output[i] = decode_color(colors[i]);
        }
        return output;
    }

    // Round to a bin with minimum two digits resolution.
            // Equivalent to conversion to string with sprintf(buf, "%.2g", value) and conversion back to float, but faster.
    static float round_to_bin(const float value)
    {
        //    assert(value > 0);
        constexpr float const scale[5] = { 100.f,  1000.f,  10000.f,  100000.f,  1000000.f };
        constexpr float const invscale[5] = { 0.01f,  0.001f,  0.0001f,  0.00001f,  0.000001f };
        constexpr float const threshold[5] = { 0.095f, 0.0095f, 0.00095f, 0.000095f, 0.0000095f };
        // Scaling factor, pointer to the tables above.
        int                   i = 0;
        // While the scaling factor is not yet large enough to get two integer digits after scaling and rounding:
        for (; value < threshold[i] && i < 4; ++i);
        return std::round(value * scale[i]) * invscale[i];
    }
}
namespace Slic3r
{
    namespace GUI
    {
        namespace gcode
        {
            const std::vector<Color> BaseRenderer::Extrusion_Role_Colors{ {
                { 0.90f, 0.70f, 0.70f, 1.0f },   // erNone
                { 1.00f, 0.90f, 0.30f, 1.0f },   // erPerimeter
                { 1.00f, 0.49f, 0.22f, 1.0f },   // erExternalPerimeter
                { 0.12f, 0.12f, 1.00f, 1.0f },   // erOverhangPerimeter
                { 0.69f, 0.19f, 0.16f, 1.0f },   // erInternalInfill
                { 0.59f, 0.33f, 0.80f, 1.0f },   // erSolidInfill
                { 0.90f, 0.70f, 0.70f, 1.0f },   // erFloatingVerticalShell
                { 0.94f, 0.25f, 0.25f, 1.0f },   // erTopSolidInfill
                { 0.40f, 0.36f, 0.78f, 1.0f },   // erBottomSurface
                { 1.00f, 0.55f, 0.41f, 1.0f },   // erIroning
                { 0.30f, 0.50f, 0.73f, 1.0f },   // erBridgeInfill
                { 1.00f, 1.00f, 1.00f, 1.0f },   // erGapFill
                { 0.00f, 0.53f, 0.43f, 1.0f },   // erSkirt
                { 0.00f, 0.23f, 0.43f, 1.0f },   // erBrim
                { 0.00f, 1.00f, 0.00f, 1.0f },   // erSupportMaterial
                { 0.00f, 0.50f, 0.00f, 1.0f },   // erSupportMaterialInterface
                { 0.00f, 0.25f, 0.00f, 1.0f },   // erSupportTransition
                { 0.60f, 1.00f, 0.60f, 1.0f },   // erSupportIroning
                { 0.70f, 0.89f, 0.67f, 1.0f },   // erWipeTower
                { 0.37f, 0.82f, 0.58f, 1.0f },    // erCustom
                { 0.85f, 0.65f, 0.95f, 1.0f }    // erFlush
            } };
            const std::vector<Color> BaseRenderer::Options_Colors{ {
                { 0.803f, 0.135f, 0.839f, 1.0f },   // Retractions
                { 0.287f, 0.679f, 0.810f, 1.0f },   // Unretractions
                { 0.900f, 0.900f, 0.900f, 1.0f },   // Seams
                { 0.758f, 0.744f, 0.389f, 1.0f },   // ToolChanges
                { 0.856f, 0.582f, 0.546f, 1.0f },   // ColorChanges
                { 0.322f, 0.942f, 0.512f, 1.0f },   // PausePrints
                { 0.886f, 0.825f, 0.262f, 1.0f }    // CustomGCodes
            } };
            const std::vector<Color> BaseRenderer::Travel_Colors{ {
                { 0.219f, 0.282f, 0.609f, 1.0f }, // Move
                { 0.112f, 0.422f, 0.103f, 1.0f }, // Extrude
                { 0.505f, 0.064f, 0.028f, 1.0f }  // Retract
            } };
            // Normal ranges
            // blue to red
            // Normal ranges
            const std::vector<ColorRGBA> BaseRenderer::Range_Colors{ {
                decode_color_to_float_array("#FF00FF"),  // bluish
                decode_color_to_float_array("#FF55A9"),
                decode_color_to_float_array("#FE8778"),
                decode_color_to_float_array("#FFB847"),
                decode_color_to_float_array("#FFD925"),
                decode_color_to_float_array("#FFFF00"),
                decode_color_to_float_array("#D8FF00"),
                decode_color_to_float_array("#ADFF04"),
                decode_color_to_float_array("#76FF01"),
                decode_color_to_float_array("#00FF00")    // reddish
            } };

            const std::vector<ColorRGBA> BaseRenderer::Thermal_Index_Range_Colors{ {
                decode_color_to_float_array("#0b2c7a"), // bluish
                decode_color_to_float_array("#005478"),
                decode_color_to_float_array("#006f86"),
                decode_color_to_float_array("#008e8f"),
                decode_color_to_float_array("#00b27c"),
                decode_color_to_float_array("#04d70f"),
                decode_color_to_float_array("#75b400"),
                decode_color_to_float_array("#949100"),
                decode_color_to_float_array("#a16c00"),
                decode_color_to_float_array("#a04800"),
                decode_color_to_float_array("#922616") // reddish
            } };
            //const std::vector<LegacyRenderer::Color> LegacyRenderer::Range_Colors {{
            //    {0.043f, 0.173f, 0.478f, 1.0f}, // bluish
            //    {0.075f, 0.349f, 0.522f, 1.0f},
            //    {0.110f, 0.533f, 0.569f, 1.0f},
            //    {0.016f, 0.839f, 0.059f, 1.0f},
            //    {0.667f, 0.949f, 0.000f, 1.0f},
            //    {0.988f, 0.975f, 0.012f, 1.0f},
            //    {0.961f, 0.808f, 0.039f, 1.0f},
            //    //{0.890f, 0.533f, 0.125f, 1.0f},
            //    {0.820f, 0.408f, 0.188f, 1.0f},
            //    {0.761f, 0.322f, 0.235f, 1.0f},
            //    {0.581f, 0.149f, 0.087f, 1.0f} // reddish
            //}};
            const Color BaseRenderer::Wipe_Color = { 1.0f, 1.0f, 0.0f, 1.0f };
            const Color BaseRenderer::Neutral_Color = { 0.25f, 0.25f, 0.25f, 1.0f };

            BaseRenderer::BaseRenderer()
            {
                m_moves_slider = new IMSlider(0, 0, 0, 100, wxSL_HORIZONTAL);
                m_layers_slider = new IMSlider(0, 0, 0, 100, wxSL_VERTICAL);
                m_p_extrusions = std::make_shared<Extrusions>();
                m_p_extrusions->reset_role_visibility_flags();
                if (GUI::wxGetApp().app_config->get_bool("enable_record_gcodeviewer_option_item")) {
                    auto back_gcodeviewer_option_item = wxGetApp().app_config->get("gcodeviewer_option_item");
                    if (!back_gcodeviewer_option_item.empty()) {
                        m_last_non_helio_option_item = std::atoi(back_gcodeviewer_option_item.c_str());
                    }
                }
                //    m_sequential_view.skip_invisible_moves = true;
            }

            BaseRenderer::~BaseRenderer()
            {
                if (m_moves_slider) {
                    delete m_moves_slider;
                    m_moves_slider = nullptr;
                }
                if (m_layers_slider) {
                    delete m_layers_slider;
                    m_layers_slider = nullptr;
                }
            }

            void BaseRenderer::init(ConfigOptionMode mode, PresetBundle* preset_bundle)
            {
                // BBS initialzed view_type items
                m_user_mode = mode;
                update_by_mode(m_user_mode);
                // initializes tool marker
                init_tool_maker(preset_bundle);
                m_layers_slider->init_texture();

                if (preset_bundle)
                    m_nozzle_nums = preset_bundle->get_printer_extruder_count();
                init_thermal_icons();
                // set to color print by default if use multi extruders
                update_default_view_type();
            }

            bool BaseRenderer::is_legend_enabled() const
            {
                return m_legend_enabled;
            }

            void BaseRenderer::enable_legend(bool enable)
            {
                m_legend_enabled = enable;
            }

            float BaseRenderer::get_legend_height() const
            {
                return m_legend_height;
            }

            const Shells& BaseRenderer::get_shells() const
            {
                return m_shells;
            }

            const GCodeCheckResult& BaseRenderer::get_gcode_check_result() const
            {
                return m_gcode_check_result;
            }

            const FilamentPrintableResult& BaseRenderer::get_filament_printable_result() const
            {
                return filament_printable_reuslt;
            }

            const ConflictResultOpt& BaseRenderer::get_conflict_result() const
            {
                return m_conflict_result;
            }

            size_t BaseRenderer::get_extruders_count() const
            {
                return m_extruders_count;
            }

            bool BaseRenderer::has_data() const
            {
                return !m_roles.empty();
            }

            const float BaseRenderer::get_max_print_height() const
            {
                return m_max_print_height;
            }

            const BoundingBoxf3& BaseRenderer::get_paths_bounding_box() const
            {
                return m_paths_bounding_box;
            }

            const BoundingBoxf3& BaseRenderer::get_max_bounding_box() const
            {
                return m_max_bounding_box;
            }

            const BoundingBoxf3& BaseRenderer::get_shell_bounding_box() const
            {
                return m_shell_bounding_box;
            }

            bool BaseRenderer::is_contained_in_bed() const
            {
                return m_contained_in_bed;
            }

            EViewType BaseRenderer::get_view_type() const
            {
                return m_view_type;
            }

            std::vector<CustomGCode::Item>& BaseRenderer::get_custom_gcode_per_print_z()
            {
                return m_custom_gcode_per_print_z;
            }

            void BaseRenderer::update_shells_color_by_extruder(const DynamicPrintConfig* config)
            {
                if (config != nullptr) {
                    m_config = config;
                    m_shells.volumes.update_colors_by_extruder(config, false);
                }
            }

            void BaseRenderer::load_shells(const Print& print, bool initialized, bool force_previewing)
            {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": initialized=%1%, force_previewing=%2%") % initialized % force_previewing;
                if ((print.id().id == m_shells.print_id) && (print.get_modified_count() == m_shells.print_modify_count)) {
                    //BBS: update force previewing logic
                    if (force_previewing)
                        m_shells.previewing = force_previewing;
                    //already loaded
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": already loaded, print=%1% print_id=%2%, print_modify_count=%3%, force_previewing %4%") % (&print) % m_shells.print_id % m_shells.print_modify_count % force_previewing;
                    return;
                }
                //reset shell firstly
                reset_shell();
                //BBS: move behind of reset_shell, to clear previous shell for empty plate
                if (print.objects().empty()) {
                    // no shells, return
                    return;
                }
                // adds objects' volumes
                // BBS: fix the issue that object_idx is not assigned as index of Model.objects array
                int object_count = 0;
                const ModelObjectPtrs& model_objs = wxGetApp().model().objects;
                bool enable_lod = GUI::wxGetApp().app_config->get("enable_lod") == "true";
                for (const PrintObject* obj : print.objects()) {
                    const ModelObject* model_obj = obj->model_object();
                    int object_idx = -1;
                    for (int idx = 0; idx < model_objs.size(); idx++) {
                        if (model_objs[idx]->id() == model_obj->id()) {
                            object_idx = idx;
                            break;
                        }
                    }
                    // BBS: object may be deleted when this method is called when deleting an object
                    if (object_idx == -1)
                        continue;
                    std::vector<int> instance_ids(model_obj->instances.size());
                    //BBS: only add the printable instance
                    int instance_index = 0;
                    for (int i = 0; i < (int)model_obj->instances.size(); ++i) {
                        //BBS: only add the printable instance
                        if (model_obj->instances[i]->is_printable())
                            instance_ids[instance_index++] = i;
                    }
                    instance_ids.resize(instance_index);
                    size_t current_volumes_count = m_shells.volumes.volumes.size();
                    m_shells.volumes.load_object(model_obj, object_idx, instance_ids, "object", initialized, enable_lod);
                    // adjust shells' z if raft is present
                    const SlicingParameters& slicing_parameters = obj->slicing_parameters();
                    if (slicing_parameters.object_print_z_min != 0.0) {
                        const Vec3d z_offset = slicing_parameters.object_print_z_min * Vec3d::UnitZ();
                        for (size_t i = current_volumes_count; i < m_shells.volumes.volumes.size(); ++i) {
                            GLVolume* v = m_shells.volumes.volumes[i];
                            auto offset = v->get_instance_transformation().get_matrix_no_offset().inverse() * z_offset;
                            v->set_volume_offset(v->get_volume_offset() + offset);
                        }
                    }
                    object_count++;
                }
                if (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF) {
                    // BBS: adds wipe tower's volume
                    std::vector<unsigned int> print_extruders = print.extruders(true);
                    int extruders_count = print_extruders.size();
                    const double max_z = print.objects()[0]->model_object()->get_model()->bounding_box().max(2);
                    const PrintConfig& config = print.config();
                    if (config.enable_prime_tower &&
                        (print.enable_timelapse_print() || (extruders_count > 1 && (config.print_sequence == PrintSequence::ByLayer)))) {
                        const float depth = print.wipe_tower_data(extruders_count).depth;
                        const float brim_width = print.wipe_tower_data(extruders_count).brim_width;
                        int plate_idx = print.get_plate_index();
                        Vec3d plate_origin = print.get_plate_origin();
                        double wipe_tower_x = config.wipe_tower_x.get_at(plate_idx) + plate_origin(0);
                        double wipe_tower_y = config.wipe_tower_y.get_at(plate_idx) + plate_origin(1);
                        m_shells.volumes.load_wipe_tower_preview(1000, wipe_tower_x, wipe_tower_y, config.prime_tower_width, depth, max_z, config.wipe_tower_rotation_angle,
                            !print.is_step_done(psWipeTower), brim_width, initialized);
                    }
                }
                // remove modifiers
                while (true) {
                    GLVolumePtrs::iterator it = std::find_if(m_shells.volumes.volumes.begin(), m_shells.volumes.volumes.end(), [](GLVolume* volume) { return volume->is_modifier; });
                    if (it != m_shells.volumes.volumes.end()) {
                        m_shells.volumes.release_volume(*it);
                        delete (*it);
                        m_shells.volumes.volumes.erase(it);
                    }
                    else
                        break;
                }
                for (GLVolume* volume : m_shells.volumes.volumes) {
                    volume->zoom_to_volumes = false;
                    volume->color[3] = 0.5f;
                    volume->force_native_color = true;
                    volume->set_render_color();
                    //BBS: add shell bounding box logic
                    m_shell_bounding_box.merge(volume->transformed_bounding_box());
                }
                //BBS: always load shell when preview
                m_shells.print_id = print.id().id;
                m_shells.print_modify_count = print.get_modified_count();
                m_shells.previewing = true;
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": shell loaded, id change to %1%, modify_count %2%, object count %3%, glvolume count %4%")
                    % m_shells.print_id % m_shells.print_modify_count % object_count % m_shells.volumes.volumes.size();
            }

            void BaseRenderer::set_shells_on_preview(bool is_previewing)
            {
                if (is_previewing) {
                    delete_wipe_tower();
                }
                m_shells.previewing = is_previewing;
            }

            const std::array<unsigned int, 2>& BaseRenderer::get_layers_z_range() const
            {
                return m_layers_z_range;
            }

            void BaseRenderer::toggle_gcode_window_visibility()
            {
                const auto& p_sequential_view = get_sequential_view();
                if (p_sequential_view) {
                    p_sequential_view->gcode_window.toggle_visibility();
                }
            }

            const std::shared_ptr<SequentialView>& BaseRenderer::get_sequential_view() const
            {
                if (!m_p_sequential_view) {
                    m_p_sequential_view = std::make_shared<SequentialView>();
                }
                return m_p_sequential_view;
            }

            void BaseRenderer::on_change_color_mode(bool is_dark)
            {
                m_is_dark = is_dark;
                const auto& p_sequential_view = get_sequential_view();
                if (p_sequential_view) {
                    p_sequential_view->marker.on_change_color_mode(m_is_dark);
                    p_sequential_view->gcode_window.on_change_color_mode(m_is_dark);
                }
            }

            void BaseRenderer::set_scale(float scale)
            {
                if (m_scale != scale)m_scale = scale;
                const auto& p_sequential_view = get_sequential_view();
                if (p_sequential_view && p_sequential_view->m_scale != scale) {
                    p_sequential_view->m_scale = scale;
                    p_sequential_view->marker.m_scale = scale;
                }
            }

            void BaseRenderer::update_marker_curr_move()
            {
                const auto& p_sequential_view = get_sequential_view();
                if (!p_sequential_view) {
                    return;
                }
                if ((int)m_last_result_id != -1) {
                    auto it = std::find_if(m_gcode_result->moves.begin(), m_gcode_result->moves.end(), [this, &p_sequential_view](auto move) {
                        if (p_sequential_view->current.last < p_sequential_view->gcode_ids.size() && p_sequential_view->current.last >= 0) {
                            return move.gcode_id == static_cast<uint64_t>(p_sequential_view->gcode_ids[p_sequential_view->current.last]);
                        }
                        return false;
                        });
                    if (it != m_gcode_result->moves.end())
                        p_sequential_view->marker.update_curr_move(*it);
                }
            }

            void BaseRenderer::load(const GCodeProcessorResult& gcode_result, const Print& print, const BuildVolume& build_volume, const std::vector<BoundingBoxf3>& exclude_bounding_box, bool initialized, ConfigOptionMode mode, bool only_gcode)
            {
                // avoid processing if called with the same gcode_result
                if (m_last_result_id == gcode_result.id) {
                    //BBS: add logs
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": the same id %1%, return directly, result %2% ") % m_last_result_id % (&gcode_result);
                    return;
                }
                //BBS: add logs
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": gcode result %1%, new id %2%, gcode file %3% ") % (&gcode_result) % m_last_result_id % PathSanitizer::sanitize(gcode_result.filename);
                // release gpu memory, if used
                reset();
                //BBS: add mutex for protection of gcode result
                wxGetApp().plater()->suppress_background_process(true);
                gcode_result.lock();
                //BBS: add safe check
                if (gcode_result.moves.size() == 0) {
                    //result cleaned before slicing ,should return here
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": gcode result reset before, return directly!");
                    gcode_result.unlock();
                    wxGetApp().plater()->schedule_background_process();
                    return;
                }
                //BBS: move the id to the end of reset
                m_last_result_id = gcode_result.id;
                m_gcode_result = &gcode_result;
                m_only_gcode_in_preview = only_gcode;
                const auto& p_sequential_view = get_sequential_view();
                if (p_sequential_view) {
                    p_sequential_view->gcode_window.load_gcode(gcode_result.filename, gcode_result.lines_ends);
                }
                //BBS: add only gcode mode
                //if (wxGetApp().is_gcode_viewer())
                if (m_only_gcode_in_preview)
                    m_custom_gcode_per_print_z = gcode_result.custom_gcode_per_print_z;
                m_max_print_height = gcode_result.printable_height;
                bool rt = load_toolpaths(gcode_result, build_volume, exclude_bounding_box);
                //BBS: add mutex for protection of gcode result
                if (!rt) {
                    gcode_result.unlock();
                    wxGetApp().plater()->schedule_background_process();
                    return;
                }
                // BBS: data for rendering color arrangement recommendation
                m_nozzle_nums = print.config().option<ConfigOptionFloatsNullable>("nozzle_diameter")->values.size();
                std::vector<int>         filament_maps = print.get_filament_maps();
                std::vector<std::string> color_opt = print.config().option<ConfigOptionStrings>("filament_colour")->values;
                std::vector<std::string> type_opt = print.config().option<ConfigOptionStrings>("filament_type")->values;
                std::vector<unsigned char> support_filament_opt = print.config().option<ConfigOptionBools>("filament_is_support")->values;
                for (auto extruder_id : m_extruder_ids) {
                    if (filament_maps[extruder_id] == 1) {
                        m_left_extruder_filament.push_back({ type_opt[extruder_id], color_opt[extruder_id], extruder_id, (bool)(support_filament_opt[extruder_id]) });
                    }
                    else {
                        m_right_extruder_filament.push_back({ type_opt[extruder_id], color_opt[extruder_id], extruder_id, (bool)(support_filament_opt[extruder_id]) });
                    }
                }
                m_settings_ids = gcode_result.settings_ids;
                m_filament_diameters = gcode_result.filament_diameters;
                m_filament_densities = gcode_result.filament_densities;

                //BBS: add only gcode mode
                if (m_only_gcode_in_preview) {
                    Pointfs printable_area;
                    //BBS: add bed exclude area
                    Pointfs bed_exclude_area = Pointfs();
                    Pointfs wrapping_exclude_area = Pointfs();
                    std::vector<Pointfs> extruder_areas;
                    std::vector<double> extruder_heights;
                    std::string texture;
                    std::string model;
                    if (!gcode_result.printable_area.empty()) {
                        // bed shape detected in the gcode
                        printable_area = gcode_result.printable_area;
                        const auto bundle = wxGetApp().preset_bundle;
                        if (bundle != nullptr && !m_settings_ids.printer.empty()) {
                            const Preset* preset = bundle->printers.find_preset(m_settings_ids.printer);
                            if (preset != nullptr) {
                                model = PresetUtils::system_printer_bed_model(*preset);
                                texture = PresetUtils::system_printer_bed_texture(*preset);
                            }
                        }
                        //BBS: add bed exclude area
                        if (!gcode_result.bed_exclude_area.empty())
                            bed_exclude_area = gcode_result.bed_exclude_area;
                        if (!gcode_result.wrapping_exclude_area.empty())
                            wrapping_exclude_area = gcode_result.wrapping_exclude_area;
                        if (!gcode_result.extruder_areas.empty())
                            extruder_areas = gcode_result.extruder_areas;
                        if (!gcode_result.extruder_heights.empty())
                            extruder_heights = gcode_result.extruder_heights;
                        wxGetApp().plater()->set_bed_shape(printable_area, bed_exclude_area, wrapping_exclude_area, gcode_result.printable_height, extruder_areas, extruder_heights, texture, model, gcode_result.printable_area.empty());
                    }
                    /*else {
                        // adjust printbed size in dependence of toolpaths bbox
                        const double margin = 10.0;
                        const Vec2d min(m_paths_bounding_box.min.x() - margin, m_paths_bounding_box.min.y() - margin);
                        const Vec2d max(m_paths_bounding_box.max.x() + margin, m_paths_bounding_box.max.y() + margin);
                        const Vec2d size = max - min;
                        printable_area = {
                            { min.x(), min.y() },
                            { max.x(), min.y() },
                            { max.x(), min.y() + 0.442265 * size.y()},
                            { max.x() - 10.0, min.y() + 0.4711325 * size.y()},
                            { max.x() + 10.0, min.y() + 0.5288675 * size.y()},
                            { max.x(), min.y() + 0.557735 * size.y()},
                            { max.x(), max.y() },
                            { min.x() + 0.557735 * size.x(), max.y()},
                            { min.x() + 0.5288675 * size.x(), max.y() - 10.0},
                            { min.x() + 0.4711325 * size.x(), max.y() + 10.0},
                            { min.x() + 0.442265 * size.x(), max.y()},
                            { min.x(), max.y() } };
                    }*/
                }
                else if (&wxGetApp() && wxGetApp().app_config->get_bool("show_shells_in_preview")) { // BBS: load shell at helio_gcode
                    load_shells(print, initialized,true);
                    update_shells_color_by_extruder(m_config);
                }
                m_print_statistics = gcode_result.print_statistics;
                if (m_time_estimate_mode != PrintEstimatedStatistics::ETimeMode::Normal) {
                    const float time = m_print_statistics.modes[static_cast<size_t>(m_time_estimate_mode)].time;
                    if (time == 0.0f ||
                        short_time(get_time_dhms(time)) == short_time(get_time_dhms(m_print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time)))
                        m_time_estimate_mode = PrintEstimatedStatistics::ETimeMode::Normal;
                }
                if (&wxGetApp() && !wxGetApp().app_config->get_bool("use_last_fold_state_gcodeview_option_panel")) {
                    m_fold = false;
                }

                bool only_gcode_3mf = false;
                PartPlate* current_plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
                bool current_has_print_instances = current_plate->has_printable_instances();
                if (current_plate->is_slice_result_valid() && wxGetApp().model().objects.empty() && !current_has_print_instances)
                    only_gcode_3mf = true;
                m_layers_slider->set_menu_enable(!(only_gcode || only_gcode_3mf));
                m_layers_slider->set_as_dirty();
                m_moves_slider->set_as_dirty();
                //BBS
                m_gcode_check_result = gcode_result.gcode_check_result;
                filament_printable_reuslt = gcode_result.filament_printable_reuslt;
                //BBS: add mutex for protection of gcode result
                gcode_result.unlock();
                wxGetApp().plater()->schedule_background_process();
            }

            void BaseRenderer::set_view_type(EViewType type, bool reset_feature_type_visible)
            {
                if (type == EViewType::Count)
                    type = EViewType::FeatureType;
                do_set_view_type(type);
                if (reset_feature_type_visible && type == EViewType::ColorPrint) {
                    reset_visible(EViewType::FeatureType);
                }
            }

            void BaseRenderer::reset_visible(EViewType type)
            {
                if (!m_p_extrusions) {
                    return;
                }
                if (type == EViewType::FeatureType) {
                    for (size_t i = 0; i < m_roles.size(); ++i) {
                        set_extrusion_role_visible(m_roles[i], true);
                    }
                }
                else if (type == EViewType::ColorPrint) {
                    for (auto item : m_tools.m_tool_visibles) item = true;
                }
            }

            bool BaseRenderer::is_only_gcode_in_preview() const
            {
                return m_only_gcode_in_preview;
            }

            IMSlider* BaseRenderer::get_moves_slider()
            {
                return m_moves_slider;
            }

            IMSlider* BaseRenderer::get_layers_slider()
            {
                return m_layers_slider;
            }

            void BaseRenderer::reset_shell()
            {
                m_shells.reset();
                m_shell_bounding_box.reset();
            }

            //BBS
            void BaseRenderer::render_all_plates_stats(const std::vector<const GCodeProcessorResult*>& gcode_result_list, bool show) const
            {
                if (!show)
                    return;
                for (auto gcode_result : gcode_result_list) {
                    if (gcode_result->moves.size() == 0)
                        return;
                }
                ImGuiWrapper& imgui = *wxGetApp().imgui();
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0, 10.0 * m_scale));
                ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.00f, 0.68f, 0.26f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.68f, 0.26f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.42f, 0.42f, 0.42f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(340.f * m_scale * imgui.scaled(1.0f / 15.0f), 0));
                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), 0, ImVec2(0.5f, 0.5f));
                ImGui::Begin(_L("Statistics of All Plates").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                std::vector<float> filament_diameters = gcode_result_list.front()->filament_diameters;
                std::vector<float> filament_densities = gcode_result_list.front()->filament_densities;
                std::vector<Color> filament_colors = ::decode_colors(wxGetApp().plater()->get_extruder_colors_from_plater_config(gcode_result_list.back()));
                for (int i = 0; i < filament_colors.size(); i++) {
                    filament_colors[i] = adjust_color_for_rendering(filament_colors[i]);
                }
                bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
                float window_padding = 4.0f * m_scale;
                const float icon_size = ImGui::GetTextLineHeight() * 0.7;
                std::map<std::string, float> fil_table_offsets;
                std::map<std::string, float> time_est_table_offsets;
                std::map<int, double> model_volume_of_extruders_all_plates; // map<extruder_idx, volume>
                std::map<int, double> flushed_volume_of_extruders_all_plates; // map<extruder_idx, flushed volume>
                std::map<int, double> wipe_tower_volume_of_extruders_all_plates; // map<extruder_idx, flushed volume>
                std::map<int, double> support_volume_of_extruders_all_plates; // map<extruder_idx, flushed volume>
                std::map<int, double> plate_time; // map<plate_idx, time>
                std::vector<double> model_used_filaments_m_all_plates;
                std::vector<double> model_used_filaments_g_all_plates;
                std::vector<double> flushed_filaments_m_all_plates;
                std::vector<double> flushed_filaments_g_all_plates;
                std::vector<double> wipe_tower_used_filaments_m_all_plates;
                std::vector<double> wipe_tower_used_filaments_g_all_plates;
                std::vector<double> support_used_filaments_m_all_plates;
                std::vector<double> support_used_filaments_g_all_plates;
                float total_time_all_plates = 0.0f;
                float total_cost_all_plates = 0.0f;
                double unit_conver = imperial_units ? GizmoObjectManipulation::oz_to_g : 1.0;
                struct ColumnData {
                    enum {
                        Model = 1,
                        Flushed = 2,
                        WipeTower = 4,
                        Support = 1 << 3,
                    };
                };
                int displayed_columns = 0;
                auto max_width = [](const std::vector<std::string>& items, const std::string& title, float extra_size = 0.0f) {
                    float ret = ImGui::CalcTextSize(title.c_str()).x;
                    for (const std::string& item : items) {
                        ret = std::max(ret, extra_size + ImGui::CalcTextSize(item.c_str()).x);
                    }
                    return ret;
                    };
                auto calculate_offsets = [max_width, window_padding](const std::vector<std::pair<std::string, std::vector<::string>>>& title_columns, float extra_size = 0.0f) {
                    const ImGuiStyle& style = ImGui::GetStyle();
                    std::vector<float> offsets;
                    offsets.push_back(max_width(title_columns[0].second, title_columns[0].first, extra_size) + 3.0f * style.ItemSpacing.x + style.WindowPadding.x);
                    for (size_t i = 1; i < title_columns.size() - 1; i++)
                        offsets.push_back(offsets.back() + max_width(title_columns[i].second, title_columns[i].first) + style.ItemSpacing.x);
                    if (title_columns.back().first == _u8L("Display"))
                        offsets.back() = ImGui::GetWindowWidth() - ImGui::CalcTextSize(_u8L("Display").c_str()).x - ImGui::GetFrameHeight() / 2 - 2 * window_padding;
                    float average_col_width = ImGui::GetWindowWidth() / static_cast<float>(title_columns.size());
                    std::vector<float> ret;
                    ret.push_back(0);
                    for (size_t i = 1; i < title_columns.size(); i++) {
                        ret.push_back(std::max(offsets[i - 1], i * average_col_width));
                    }
                    return ret;
                    };
                auto append_item = [icon_size, &imgui, imperial_units, &window_padding, &draw_list, this](bool draw_icon, const Color& color, const std::vector<std::pair<std::string, float>>& columns_offsets)
                    {
                        // render icon
                        ImVec2 pos = ImVec2(ImGui::GetCursorScreenPos().x + window_padding * 3, ImGui::GetCursorScreenPos().y);
                        if (draw_icon)
                            draw_list->AddRectFilled({ pos.x + 1.0f * m_scale, pos.y + 3.0f * m_scale }, { pos.x + icon_size - 1.0f * m_scale, pos.y + icon_size + 1.0f * m_scale },
                                ImGui::GetColorU32({ color[0], color[1], color[2], 1.0f }));
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(20.0 * m_scale, 6.0 * m_scale));
                        // render selectable
                        ImGui::Dummy({ 0.0, 0.0 });
                        ImGui::SameLine();
                        // render column item
                        {
                            float dummy_size = draw_icon ? ImGui::GetStyle().ItemSpacing.x + icon_size : window_padding * 3;
                            ImGui::SameLine(dummy_size);
                            imgui.text(columns_offsets[0].first);
                            for (auto i = 1; i < columns_offsets.size(); i++) {
                                ImGui::SameLine(columns_offsets[i].second);
                                imgui.text(columns_offsets[i].first);
                            }
                        }
                        ImGui::PopStyleVar(1);
                    };
                auto append_headers = [&imgui](const std::vector<std::pair<std::string, float>>& title_offsets) {
                    for (size_t i = 0; i < title_offsets.size(); i++) {
                        ImGui::SameLine(title_offsets[i].second);
                        imgui.bold_text(title_offsets[i].first);
                    }
                    ImGui::Separator();
                    };
                auto get_used_filament_from_volume = [this, imperial_units, &filament_diameters, &filament_densities](double volume, int extruder_id) {
                    double koef = imperial_units ? 1.0 / GizmoObjectManipulation::in_to_mm : 0.001;
                    std::pair<double, double> ret = { koef * volume / (PI * sqr(0.5 * filament_diameters[extruder_id])),
                                                        volume * filament_densities[extruder_id] * 0.001 };
                    return ret;
                    };
                ImGui::Dummy({ window_padding, window_padding });
                ImGui::SameLine();
                // title and item data
                {
                    PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();
                    for (auto plate : plate_list.get_nonempty_plate_list())
                    {
                        auto plate_print_statistics = plate->get_slice_result()->print_statistics;
                        auto plate_extruders = plate->get_extruders(true);
                        auto max_extruders_colors   = wxGetApp().plater()->get_extruders_colors().size();
                        for (size_t extruder_id : plate_extruders) {
                            extruder_id -= 1;
                            if (extruder_id >= max_extruders_colors) {
                                continue;
                            }
                            if (plate_print_statistics.model_volumes_per_extruder.find(extruder_id) == plate_print_statistics.model_volumes_per_extruder.end())
                                model_volume_of_extruders_all_plates[extruder_id] += 0;
                            else {
                                double model_volume = plate_print_statistics.model_volumes_per_extruder.at(extruder_id);
                                model_volume_of_extruders_all_plates[extruder_id] += model_volume;
                            }
                            if (plate_print_statistics.flush_per_filament.find(extruder_id) == plate_print_statistics.flush_per_filament.end())
                                flushed_volume_of_extruders_all_plates[extruder_id] += 0;
                            else {
                                double flushed_volume = plate_print_statistics.flush_per_filament.at(extruder_id);
                                flushed_volume_of_extruders_all_plates[extruder_id] += flushed_volume;
                            }
                            if (plate_print_statistics.wipe_tower_volumes_per_extruder.find(extruder_id) == plate_print_statistics.wipe_tower_volumes_per_extruder.end())
                                wipe_tower_volume_of_extruders_all_plates[extruder_id] += 0;
                            else {
                                double wipe_tower_volume = plate_print_statistics.wipe_tower_volumes_per_extruder.at(extruder_id);
                                wipe_tower_volume_of_extruders_all_plates[extruder_id] += wipe_tower_volume;
                            }
                            if (plate_print_statistics.support_volumes_per_extruder.find(extruder_id) == plate_print_statistics.support_volumes_per_extruder.end())
                                support_volume_of_extruders_all_plates[extruder_id] += 0;
                            else {
                                double support_volume = plate_print_statistics.support_volumes_per_extruder.at(extruder_id);
                                support_volume_of_extruders_all_plates[extruder_id] += support_volume;
                            }
                        }
                        const PrintEstimatedStatistics::Mode& plate_time_mode = plate_print_statistics.modes[static_cast<size_t>(m_time_estimate_mode)];
                        plate_time.insert_or_assign(plate->get_index(), plate_time_mode.time);
                        total_time_all_plates += plate_time_mode.time;
                        Print* print;
                        plate->get_print((PrintBase**)&print, nullptr, nullptr);
                        total_cost_all_plates += print->print_statistics().total_cost;
                    }
                    for (auto it = model_volume_of_extruders_all_plates.begin(); it != model_volume_of_extruders_all_plates.end(); it++) {
                        auto [model_used_filament_m, model_used_filament_g] = get_used_filament_from_volume(it->second, it->first);
                        if (model_used_filament_m != 0.0 || model_used_filament_g != 0.0)
                            displayed_columns |= ColumnData::Model;
                        model_used_filaments_m_all_plates.push_back(model_used_filament_m);
                        model_used_filaments_g_all_plates.push_back(model_used_filament_g);
                    }
                    for (auto it = flushed_volume_of_extruders_all_plates.begin(); it != flushed_volume_of_extruders_all_plates.end(); it++) {
                        auto [flushed_filament_m, flushed_filament_g] = get_used_filament_from_volume(it->second, it->first);
                        if (flushed_filament_m != 0.0 || flushed_filament_g != 0.0)
                            displayed_columns |= ColumnData::Flushed;
                        flushed_filaments_m_all_plates.push_back(flushed_filament_m);
                        flushed_filaments_g_all_plates.push_back(flushed_filament_g);
                    }
                    for (auto it = wipe_tower_volume_of_extruders_all_plates.begin(); it != wipe_tower_volume_of_extruders_all_plates.end(); it++) {
                        auto [wipe_tower_filament_m, wipe_tower_filament_g] = get_used_filament_from_volume(it->second, it->first);
                        if (wipe_tower_filament_m != 0.0 || wipe_tower_filament_g != 0.0)
                            displayed_columns |= ColumnData::WipeTower;
                        wipe_tower_used_filaments_m_all_plates.push_back(wipe_tower_filament_m);
                        wipe_tower_used_filaments_g_all_plates.push_back(wipe_tower_filament_g);
                    }
                    for (auto it = support_volume_of_extruders_all_plates.begin(); it != support_volume_of_extruders_all_plates.end(); it++) {
                        auto [support_filament_m, support_filament_g] = get_used_filament_from_volume(it->second, it->first);
                        if (support_filament_m != 0.0 || support_filament_g != 0.0)
                            displayed_columns |= ColumnData::Support;
                        support_used_filaments_m_all_plates.push_back(support_filament_m);
                        support_used_filaments_g_all_plates.push_back(support_filament_g);
                    }
                    char buff[64];
                    double longest_str = 0.0;
                    for (auto i : model_used_filaments_g_all_plates) {
                        longest_str += i;
                    }
                    ::sprintf(buff, imperial_units ? "%.2f oz" : "%.2f g", longest_str / unit_conver);
                    std::vector<std::pair<std::string, std::vector<::string>>> title_columns;
                    if (displayed_columns & ColumnData::Model) {
                        title_columns.push_back({ _u8L("Filament"), {""} });
                        title_columns.push_back({ _u8L("Model"), {buff} });
                    }
                    if (displayed_columns & ColumnData::Support) {
                        title_columns.push_back({ _u8L("Support"), {buff} });
                    }
                    if (displayed_columns & ColumnData::Flushed) {
                        title_columns.push_back({ _u8L("Flushed"), {buff} });
                    }
                    if (displayed_columns & ColumnData::WipeTower) {
                        title_columns.push_back({ _u8L("Tower"), {buff} });
                    }
                    if ((displayed_columns & ~ColumnData::Model) > 0) {
                        title_columns.push_back({ _u8L("Total"), {buff} });
                    }
                    auto offsets_ = calculate_offsets(title_columns, icon_size);
                    std::vector<std::pair<std::string, float>> title_offsets;
                    for (int i = 0; i < offsets_.size(); i++) {
                        title_offsets.push_back({ title_columns[i].first, offsets_[i] });
                        fil_table_offsets[title_columns[i].first] = offsets_[i];
                    }
                    append_headers(title_offsets);
                }
                // item
                {
                    size_t i = 0;
                    for (auto it = model_volume_of_extruders_all_plates.begin(); it != model_volume_of_extruders_all_plates.end(); it++) {
                        if (i < model_used_filaments_m_all_plates.size() && i < model_used_filaments_g_all_plates.size()) {
                            std::vector<std::pair<std::string, float>> columns_offsets;
                            columns_offsets.push_back({ std::to_string(it->first + 1), fil_table_offsets[_u8L("Filament")] });
                            char buf[64];
                            float column_sum_m = 0.0f;
                            float column_sum_g = 0.0f;
                            if (displayed_columns & ColumnData::Model) {
                                if ((displayed_columns & ~ColumnData::Model) > 0)
                                    ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", model_used_filaments_m_all_plates[i], model_used_filaments_g_all_plates[i] / unit_conver);
                                else
                                    ::sprintf(buf, imperial_units ? "%.2f in    %.2f oz" : "%.2f m    %.2f g", model_used_filaments_m_all_plates[i], model_used_filaments_g_all_plates[i] / unit_conver);
                                columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Model")] });
                                column_sum_m += model_used_filaments_m_all_plates[i];
                                column_sum_g += model_used_filaments_g_all_plates[i];
                            }
                            if (displayed_columns & ColumnData::Support) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", support_used_filaments_m_all_plates[i], support_used_filaments_g_all_plates[i] / unit_conver);
                                columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Support")] });
                                column_sum_m += support_used_filaments_m_all_plates[i];
                                column_sum_g += support_used_filaments_g_all_plates[i];
                            }
                            if (displayed_columns & ColumnData::Flushed) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", flushed_filaments_m_all_plates[i], flushed_filaments_g_all_plates[i] / unit_conver);
                                columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Flushed")] });
                                column_sum_m += flushed_filaments_m_all_plates[i];
                                column_sum_g += flushed_filaments_g_all_plates[i];
                            }
                            if (displayed_columns & ColumnData::WipeTower) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", wipe_tower_used_filaments_m_all_plates[i], wipe_tower_used_filaments_g_all_plates[i] / unit_conver);
                                columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Tower")] });
                                column_sum_m += wipe_tower_used_filaments_m_all_plates[i];
                                column_sum_g += wipe_tower_used_filaments_g_all_plates[i];
                            }
                            if ((displayed_columns & ~ColumnData::Model) > 0) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", column_sum_m, column_sum_g / unit_conver);
                                columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Total")] });
                            }
                            append_item(true, filament_colors[it->first], columns_offsets);
                        }
                        i++;
                    }
                    // Sum of all rows
                    char buf[64];
                    if (model_volume_of_extruders_all_plates.size() > 1) {
                        // Separator
                        ImGuiWindow* window = ImGui::GetCurrentWindow();
                        const ImRect separator(ImVec2(window->Pos.x + window_padding * 3, window->DC.CursorPos.y),
                            ImVec2(window->Pos.x + window->Size.x - window_padding * 3, window->DC.CursorPos.y + 1.0f));
                        ImGui::ItemSize(ImVec2(0.0f, 0.0f));
                        const bool item_visible = ImGui::ItemAdd(separator, 0);
                        window->DrawList->AddLine(separator.Min, ImVec2(separator.Max.x, separator.Min.y), ImGui::GetColorU32(ImGuiCol_Separator));
                        std::vector<std::pair<std::string, float>> columns_offsets;
                        columns_offsets.push_back({ _u8L("Total"), fil_table_offsets[_u8L("Filament")] });
                        double total_model_used_filament_m = 0;
                        double total_model_used_filament_g = 0;
                        double total_support_used_filament_m = 0;
                        double total_support_used_filament_g = 0;
                        double total_flushed_filament_m = 0;
                        double total_flushed_filament_g = 0;
                        double total_wipe_tower_used_filament_m = 0;
                        double total_wipe_tower_used_filament_g = 0;
                        if (displayed_columns & ColumnData::Model) {
                            std::for_each(model_used_filaments_m_all_plates.begin(), model_used_filaments_m_all_plates.end(), [&total_model_used_filament_m](double value) {
                                total_model_used_filament_m += value;
                                });
                            std::for_each(model_used_filaments_g_all_plates.begin(), model_used_filaments_g_all_plates.end(), [&total_model_used_filament_g](double value) {
                                total_model_used_filament_g += value;
                                });
                            if ((displayed_columns & ~ColumnData::Model) > 0)
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_model_used_filament_m, total_model_used_filament_g / unit_conver);
                            else
                                ::sprintf(buf, imperial_units ? "%.2f in    %.2f oz" : "%.2f m    %.2f g", total_model_used_filament_m, total_model_used_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Model")] });
                        }
                        if (displayed_columns & ColumnData::Support) {
                            std::for_each(support_used_filaments_m_all_plates.begin(), support_used_filaments_m_all_plates.end(), [&total_support_used_filament_m](double value) {
                                total_support_used_filament_m += value;
                                });
                            std::for_each(support_used_filaments_g_all_plates.begin(), support_used_filaments_g_all_plates.end(), [&total_support_used_filament_g](double value) {
                                total_support_used_filament_g += value;
                                });
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_support_used_filament_m, total_support_used_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Support")] });
                        }
                        if (displayed_columns & ColumnData::Flushed) {
                            std::for_each(flushed_filaments_m_all_plates.begin(), flushed_filaments_m_all_plates.end(), [&total_flushed_filament_m](double value) {
                                total_flushed_filament_m += value;
                                });
                            std::for_each(flushed_filaments_g_all_plates.begin(), flushed_filaments_g_all_plates.end(), [&total_flushed_filament_g](double value) {
                                total_flushed_filament_g += value;
                                });
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_flushed_filament_m, total_flushed_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Flushed")] });
                        }
                        if (displayed_columns & ColumnData::WipeTower) {
                            std::for_each(wipe_tower_used_filaments_m_all_plates.begin(), wipe_tower_used_filaments_m_all_plates.end(), [&total_wipe_tower_used_filament_m](double value) {
                                total_wipe_tower_used_filament_m += value;
                                });
                            std::for_each(wipe_tower_used_filaments_g_all_plates.begin(), wipe_tower_used_filaments_g_all_plates.end(), [&total_wipe_tower_used_filament_g](double value) {
                                total_wipe_tower_used_filament_g += value;
                                });
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_wipe_tower_used_filament_m, total_wipe_tower_used_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Tower")] });
                        }
                        if ((displayed_columns & ~ColumnData::Model) > 0) {
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g",
                                total_model_used_filament_m + total_support_used_filament_m + total_flushed_filament_m + total_wipe_tower_used_filament_m,
                                (total_model_used_filament_g + total_support_used_filament_g + total_flushed_filament_g + total_wipe_tower_used_filament_g) / unit_conver);
                            columns_offsets.push_back({ buf, fil_table_offsets[_u8L("Total")] });
                        }
                        append_item(false, m_tools.m_tool_colors[0], columns_offsets);
                    }
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(_u8L("Total cost") + ":");
                    ImGui::SameLine();
                    ::sprintf(buf, "%.2f", total_cost_all_plates);
                    imgui.text(buf);

                    // Calculating Column Offsets for the Time Estimation Table
                    {
                        std::vector<std::string>                                   time_labels;
                        std::vector<std::string>                                   time_values;
                        std::vector<std::pair<std::string, std::vector<::string>>> time_columns;

                        for (auto it = plate_time.begin(); it != plate_time.end(); it++) {
                            time_labels.push_back(_u8L("Plate") + " " + std::to_string(it->first + 1));
                            time_values.push_back(short_time(get_time_dhms(it->second)));
                        }
                        if (plate_time.size() > 1) {
                            time_labels.push_back(_u8L("Total"));
                            time_values.push_back(short_time(get_time_dhms(total_time_all_plates)));
                        }

                        time_columns.push_back({_u8L("Plate"), time_labels});
                        time_columns.push_back({_u8L("Time"), time_values});

                        auto time_offsets = calculate_offsets(time_columns);
                        for (int i = 0; i < time_offsets.size(); i++) { time_est_table_offsets[time_columns[i].first] = time_offsets[i]; }
                    }

                    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.title(_u8L("Time Estimation"));
                    for (auto it = plate_time.begin(); it != plate_time.end(); it++) {
                        std::vector<std::pair<std::string, float>> columns_offsets;
                        columns_offsets.push_back({_u8L("Plate") + " " + std::to_string(it->first + 1), time_est_table_offsets[_u8L("Plate")]});
                        columns_offsets.push_back({short_time(get_time_dhms(it->second)), time_est_table_offsets[_u8L("Time")]});
                        append_item(false, m_tools.m_tool_colors[0], columns_offsets);
                    }
                    if (plate_time.size() > 1) {
                        // Separator
                        ImGuiWindow* window = ImGui::GetCurrentWindow();
                        const ImRect separator(ImVec2(window->Pos.x + window_padding * 3, window->DC.CursorPos.y),
                            ImVec2(window->Pos.x + window->Size.x - window_padding * 3, window->DC.CursorPos.y + 1.0f));
                        ImGui::ItemSize(ImVec2(0.0f, 0.0f));
                        const bool item_visible = ImGui::ItemAdd(separator, 0);
                        window->DrawList->AddLine(separator.Min, ImVec2(separator.Max.x, separator.Min.y), ImGui::GetColorU32(ImGuiCol_Separator));
                        std::vector<std::pair<std::string, float>> columns_offsets;
                        columns_offsets.push_back({_u8L("Total"), time_est_table_offsets[_u8L("Plate")]});
                        columns_offsets.push_back({short_time(get_time_dhms(total_time_all_plates)), time_est_table_offsets[_u8L("Time")]});
                        append_item(false, m_tools.m_tool_colors[0], columns_offsets);
                    }
                }
                ImGui::End();
                ImGui::PopStyleColor(6);
                ImGui::PopStyleVar(3);
                return;
            }

            // helio
            void BaseRenderer::set_show_horizontal_slider(bool flag)
            {
                m_show_horizontal_slider = flag;
            }

            bool BaseRenderer::is_show_horizontal_slider() const
            {
                return m_show_horizontal_slider;
            }

            bool BaseRenderer::is_helio_option() const
            {
                if (m_view_type == EViewType::ThermalIndexMin ||
                    m_view_type == EViewType::ThermalIndexMax ||
                    m_view_type == EViewType::ThermalIndexMean)
                    return true;
                return false;
            }

            bool BaseRenderer::curr_plate_has_ok_helio_slice(int plate_idx) const
            {
                if (m_helio_slice_map_oks.find(plate_idx) != m_helio_slice_map_oks.end() && m_helio_slice_map_oks.at(plate_idx)) {
                    return true;
                }
                return false;
            }

            void BaseRenderer::reset_curr_plate_thermal_options()
            {
                auto curr_plate_index = wxGetApp().plater()->get_partplate_list().get_curr_plate_index();
                reset_curr_plate_thermal_options(curr_plate_index);
            }
            // end helio

            void BaseRenderer::reset()
            {
                //BBS: should also reset the result id
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": current result id %1% ") % m_last_result_id;
                m_last_result_id = -1;
                m_conflict_result.reset();
                m_gcode_check_result.reset();
                filament_printable_reuslt.reset();
                m_legend_enabled = true;
                m_legend_height = 0.0f;
                m_extruders_count = 0;
                m_roles.clear();
                m_max_print_height = 0.0f;
                m_paths_bounding_box.reset();
                m_max_bounding_box.reset();
                m_custom_gcode_per_print_z.clear();
                m_layers_z_range = { 0, 0 };
                m_only_gcode_in_preview = false;
                if (m_p_sequential_view) {
                    m_p_sequential_view->gcode_window.reset();
                    m_p_sequential_view->gcode_ids.clear();
                }
                m_left_extruder_filament.clear();
                m_right_extruder_filament.clear();
                m_nozzle_nums = 0;
                m_extruder_ids.clear();
                m_filament_diameters.clear();
                m_filament_densities.clear();
                m_print_statistics.reset();
                m_ssid_to_moveid_map.clear();
                m_ssid_to_moveid_map.shrink_to_fit();
                m_plater_extruder.clear();
                m_contained_in_bed = true;

                if (m_p_extrusions) {
                    m_p_extrusions->reset_ranges();
                }
                reset_shell();
            }

            void BaseRenderer::refresh(const GCodeProcessorResult& gcode_result, const std::vector<std::string>& str_tool_colors)
            {
#if ENABLE_GCODE_VIEWER_STATISTICS
                auto start_time = std::chrono::high_resolution_clock::now();
#endif // ENABLE_GCODE_VIEWER_STATISTICS
                //BBS: add mutex for protection of gcode result
                gcode_result.lock();
                //BBS: add safe check
                if (gcode_result.moves.size() == 0) {
                    //result cleaned before slicing ,should return here
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": gcode result reset before, return directly!");
                    gcode_result.unlock();
                    return;
                }
                const auto t_move_count = gcode_result.moves.size();
                //BBS: add mutex for protection of gcode result
                if (t_move_count == 0) {
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": gcode result m_moves_count is 0, return directly!");
                    gcode_result.unlock();
                    return;
                }
                wxBusyCursor busy;
                if (m_view_type == EViewType::Tool && !gcode_result.extruder_colors.empty()) {
                    // update tool colors from config stored in the gcode
                    m_tools.m_tool_colors = ::decode_colors(gcode_result.extruder_colors);
                    m_tools.m_tool_visibles = std::vector<bool>(m_tools.m_tool_colors.size());
                    for (auto item : m_tools.m_tool_visibles) item = true;
                }
                else {
                    // update tool colors
                    m_tools.m_tool_colors = ::decode_colors(str_tool_colors);
                    m_tools.m_tool_visibles = std::vector<bool>(m_tools.m_tool_colors.size());
                    for (auto item : m_tools.m_tool_visibles) item = true;
                }
                for (int i = 0; i < m_tools.m_tool_colors.size(); i++) {
                    m_tools.m_tool_colors[i] = adjust_color_for_rendering(m_tools.m_tool_colors[i]);
                }
                // ensure there are enough colors defined
                while (m_tools.m_tool_colors.size() < std::max(size_t(1), gcode_result.filaments_count)) {
                    m_tools.m_tool_colors.push_back(::decode_color("#FF8000"));
                    m_tools.m_tool_visibles.push_back(true);
                }
                // update ranges for coloring / legend
                if (m_p_extrusions) {
                    m_p_extrusions->reset_ranges();
                    for (size_t i = 0; i < t_move_count; ++i) {
                        // skip first vertex
                        if (i == 0)
                            continue;
                        const GCodeProcessorResult::MoveVertex& curr = gcode_result.moves[i];
                        switch (curr.type)
                        {
                        case EMoveType::Extrude:
                        {
                            if (curr.extrusion_role != ExtrusionRole::erCustom) {
                                m_p_extrusions->ranges.height.update_from(round_to_bin(curr.height));
                                m_p_extrusions->ranges.width.update_from(round_to_bin(curr.width));
                            }// prevent the start code extrude extreme height/width and make the range deviate from the normal range
                            m_p_extrusions->ranges.fan_speed.update_from(curr.fan_speed);
                            m_p_extrusions->ranges.temperature.update_from(curr.temperature);
                            if (curr.extrusion_role != erCustom || is_extrusion_role_visible(ExtrusionRole::erCustom))
                                m_p_extrusions->ranges.volumetric_rate.update_from(round_to_bin(curr.volumetric_rate()));
                            if (curr.layer_duration > 0.f) {
                                m_p_extrusions->ranges.layer_duration.update_from(curr.layer_duration);
                            }
                            [[fallthrough]];
                        }
                        case EMoveType::Travel:
                        {
                            if (is_move_type_visible(curr.type))
                                m_p_extrusions->ranges.feedrate.update_from(curr.feedrate);
                            break;
                        }
                        default: { break; }
                        }
                    }
                }
#if ENABLE_GCODE_VIEWER_STATISTICS
                m_statistics.refresh_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time).count();
#endif // ENABLE_GCODE_VIEWER_STATISTICS
                //BBS: add mutex for protection of gcode result
                gcode_result.unlock();
                update_option_item_when_load_gcode();
            }

            void Shells::reset()
            {
                volumes.clear();
                print_id = -1;
            }

            void BaseRenderer::init_tool_maker(PresetBundle* preset_bundle)
            {
                const auto& p_sequential_view = get_sequential_view();
                if (!p_sequential_view) {
                    return;
                }
                // initializes tool marker
                std::string filename;
                if (preset_bundle != nullptr) {
                    const Preset* curr = &preset_bundle->printers.get_selected_preset();
                    if (curr->is_system)
                        filename = PresetUtils::system_printer_hotend_model(*curr);
                    else {
                        auto* printer_model = curr->config.opt<ConfigOptionString>("printer_model");
                        if (printer_model != nullptr && !printer_model->value.empty()) {
                            filename = preset_bundle->get_hotend_model_for_printer_model(printer_model->value);
                        }
                        if (filename.empty()) {
                            filename = preset_bundle->get_hotend_model_for_printer_model(PresetBundle::BBL_DEFAULT_PRINTER_MODEL);
                        }
                    }
                }
                p_sequential_view->marker.init(filename);
            }

            void BaseRenderer::render_sequential_view(uint32_t canvas_width, uint32_t canvas_height, int right_margin)
            {
                const auto& p_sequential_view = get_sequential_view();
                if (!p_sequential_view) {
                    return;
                }
                //BBS fixed bottom_margin for space to render horiz slider
                int bottom_margin = 64;
                if (show_sequential_view()) {
                    p_sequential_view->marker.set_world_position(p_sequential_view->current_position);
                    p_sequential_view->marker.set_world_offset(p_sequential_view->current_offset);
                    //BBS fixed buttom margin. m_moves_slider.pos_y
                    // helio
                    uint8_t length_of_line = 40;
                    if (is_helio_option())
                    {
                        length_of_line = 90;
                    }
                    // end helio
                    p_sequential_view->render(m_legend_height, canvas_width, canvas_height - bottom_margin * m_scale, right_margin * m_scale, m_view_type, [this](size_t& length_of_line)->void {
                        length_of_line = 90;
                        this->set_show_horizontal_slider(true);
                    }, is_show_horizontal_slider(), is_helio_option());
                }
            }

            void BaseRenderer::render_shells()
            {
                //BBS: add shell previewing logic
                if ((!m_shells.previewing && !m_shells.visible) || m_shells.volumes.empty())
                    //if (!m_shells.visible || m_shells.volumes.empty())
                    return;
                const auto& shader = wxGetApp().get_shader("gouraud_light");
                if (shader == nullptr)
                    return;
                // when the background processing is enabled, it may happen that the shells data have been loaded
                // before opengl has been initialized for the preview canvas.
                // when this happens, the volumes' data have not been sent to gpu yet.
                for (GLVolume* v : m_shells.volumes.volumes) {
                    if (!v->indexed_vertex_array->has_VBOs())
                        v->finalize_geometry(true);
                }
                glsafe(::glDepthMask(GL_FALSE));
                wxGetApp().bind_shader(shader);
                //BBS: reopen cul faces
                auto& camera = wxGetApp().plater()->get_camera();
                std::vector<std::array<float, 4>> colors = wxGetApp().plater()->get_extruders_colors();
                m_shells.volumes.render(GUI::ERenderPipelineStage::Normal, GLVolumeCollection::ERenderType::Transparent, false, camera, colors, wxGetApp().plater()->model());
                wxGetApp().unbind_shader();
                glsafe(::glDepthMask(GL_TRUE));
            }

            void BaseRenderer::render_slider(int canvas_width, int canvas_height)
            {
                if (m_moves_slider) {
                    m_moves_slider->render(canvas_width, canvas_height);
                }
                if (m_layers_slider) {
                    m_layers_slider->render(canvas_width, canvas_height);
                }
            }

            void BaseRenderer::render_legend(float& legend_height, int canvas_width, int canvas_height, int right_margin)
            {
                if (!m_legend_enabled)
                    return;
                const Size cnv_size = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size();
                ImGuiWrapper& imgui = *wxGetApp().imgui();
                //BBS: GUI refactor: move to the right
                imgui.set_next_window_pos(float(canvas_width - right_margin * m_scale), 0.0f, ImGuiCond_Always, 1.0f, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0, 0.0));
                ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.00f, 0.68f, 0.26f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.68f, 0.26f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.42f, 0.42f, 0.42f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.93f, 0.93f, 0.93f, 1.00f));
                //ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.f, 1.f, 1.f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Border, { 1, 0, 0, 0 });
                ImGui::SetNextWindowBgAlpha(0.8f);
                const float max_height = 0.75f * static_cast<float>(cnv_size.get_height());
                const float child_height = 0.3333f * max_height;
                ImGui::SetNextWindowSizeConstraints({ 0.0f, 0.0f }, { -1.0f, max_height });
                imgui.begin(std::string("Legend"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
                enum class EItemType : unsigned char
                {
                    Rect,
                    Circle,
                    Hexagon,
                    Line,
                    None
                };
                const PrintEstimatedStatistics::Mode& time_mode = m_print_statistics.modes[static_cast<size_t>(m_time_estimate_mode)];
                //BBS
                /*bool show_estimated_time = time_mode.time > 0.0f && (m_view_type == EViewType::FeatureType ||
                    (m_view_type == EViewType::ColorPrint && !time_mode.custom_gcode_times.empty()));*/
                bool show_estimated = time_mode.time > 0.0f && (m_view_type == EViewType::FeatureType || m_view_type == EViewType::ColorPrint);
                const float icon_size = ImGui::GetTextLineHeight() * 0.7;
                //BBS GUI refactor
                //const float percent_bar_size = 2.0f * ImGui::GetTextLineHeight();
                const float percent_bar_size = 0;
                bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                ImVec2 pos_rect = ImGui::GetCursorScreenPos();
                float window_padding = 4.0f * m_scale;
                float checkbox_offset = 0.0f;
                draw_list->AddRectFilled(ImVec2(pos_rect.x, pos_rect.y - ImGui::GetStyle().WindowPadding.y),
                    ImVec2(pos_rect.x + ImGui::GetWindowWidth() + ImGui::GetFrameHeight(), pos_rect.y + ImGui::GetFrameHeight() + window_padding * 2.5),
                    ImGui::GetColorU32(ImVec4(0, 0, 0, 0.3)));
                auto append_item = [icon_size, &imgui, imperial_units, &window_padding, &draw_list, &checkbox_offset, this](
                    EItemType type,
                    const Color& color,
                    const std::vector<std::pair<std::string, float>>& columns_offsets,
                    bool checkbox = true,
                    bool visible = true,
                    std::function<void()> callback = nullptr)
                    {
                        // render icon
                        ImVec2 pos = ImVec2(ImGui::GetCursorScreenPos().x + window_padding * 3, ImGui::GetCursorScreenPos().y);
                        switch (type) {
                        default:
                        case EItemType::Rect: {
                            draw_list->AddRectFilled({ pos.x + 1.0f * m_scale, pos.y + 3.0f * m_scale }, { pos.x + icon_size - 1.0f * m_scale, pos.y + icon_size + 1.0f * m_scale },
                                ImGui::GetColorU32({ color[0], color[1], color[2], color[3] }));
                            break;
                        }
                        case EItemType::Circle: {
                            ImVec2 center(0.5f * (pos.x + pos.x + icon_size), 0.5f * (pos.y + pos.y + icon_size + 5.0f));
                            draw_list->AddCircleFilled(center, 0.5f * icon_size, ImGui::GetColorU32({ color[0], color[1], color[2], color[3] }), 16);
                            break;
                        }
                        case EItemType::Hexagon: {
                            ImVec2 center(0.5f * (pos.x + pos.x + icon_size), 0.5f * (pos.y + pos.y + icon_size + 5.0f));
                            draw_list->AddNgonFilled(center, 0.5f * icon_size, ImGui::GetColorU32({ color[0], color[1], color[2], color[3] }), 6);
                            break;
                        }
                        case EItemType::Line: {
                            draw_list->AddLine({ pos.x + 1, pos.y + icon_size + 2 }, { pos.x + icon_size - 1, pos.y + 4 }, ImGui::GetColorU32({ color[0], color[1], color[2], color[3] }), 3.0f);
                            break;
                        case EItemType::None:
                            break;
                        }
                        }
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(20.0 * m_scale, 6.0 * m_scale));
                        // BBS render selectable
                        ImGui::Dummy({ 0.0, 0.0 });
                        ImGui::SameLine();
                        if (callback) {
                            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * m_scale);
                            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0 * m_scale, 0.0));
                            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.00f, 0.68f, 0.26f, 0.0f));
                            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1.00f, 0.68f, 0.26f, 0.0f));
                            ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(0.00f, 0.68f, 0.26f, 1.00f));
                            float max_height = 0.f;
                            for (auto column_offset : columns_offsets) {
                                if (ImGui::CalcTextSize(column_offset.first.c_str()).y > max_height)
                                    max_height = ImGui::CalcTextSize(column_offset.first.c_str()).y;
                            }
                            bool b_menu_item = ImGui::BBLMenuItem(("##" + columns_offsets[0].first).c_str(), nullptr, false, true, max_height);
                            ImGui::PopStyleVar(2);
                            ImGui::PopStyleColor(3);
                            if (b_menu_item)
                                callback();
                            if (checkbox) {
                                ImGui::SameLine(checkbox_offset);
                                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0, 0.0));
                                ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.00f, 0.68f, 0.26f, 1.00f));
                                ImGui::Checkbox(("##" + columns_offsets[0].first).c_str(), &visible);
                                ImGui::PopStyleColor(1);
                                ImGui::PopStyleVar(1);
                            }
                        }
                        // BBS render column item
                        {
                            if (callback && !checkbox && !visible)
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(172 / 255.0f, 172 / 255.0f, 172 / 255.0f, 1.00f));
                            float dummy_size = type == EItemType::None ? window_padding * 3 : ImGui::GetStyle().ItemSpacing.x + icon_size;
                            ImGui::SameLine(dummy_size);
                            imgui.text(columns_offsets[0].first);
                            for (auto i = 1; i < columns_offsets.size(); i++) {
                                ImGui::SameLine(columns_offsets[i].second);
                                imgui.text(columns_offsets[i].first);
                            }
                            if (callback && !checkbox && !visible)
                                ImGui::PopStyleColor(1);
                        }
                        ImGui::PopStyleVar(1);
                    };
                auto append_range = [append_item](const Range& range, unsigned int decimals) {
                    auto append_range_item = [append_item, range](int i, float value, unsigned int decimals) {
                        char buf[1024];
                        ::sprintf(buf, "%.*f", decimals, value);
                        append_item(EItemType::Rect, range.range_colors[i].get_data(), { {buf, 0} });
                        };
                    if (range.count == 1)
                        // single item use case
                        append_range_item(0, range.min, decimals);
                    else if (range.count == 2) {
                        append_range_item(static_cast<int>(range.range_colors.size()) - 1, range.max, decimals);
                        append_range_item(0, range.min, decimals);
                    }
                    else {
                        const float step_size = range.step_size();
                        for (int i = static_cast<int>(range.range_colors.size()) - 1; i >= 0; --i) {
                            append_range_item(i, range.get_value_at_step(i), decimals);
                        }
                    }
                    };
                auto append_headers = [&imgui, &window_padding](const std::vector<std::pair<std::string, float>>& title_offsets) {
                    for (size_t i = 0; i < title_offsets.size(); i++) {
                        ImGui::SameLine(title_offsets[i].second);
                        imgui.bold_text(title_offsets[i].first);
                    }
                    ImGui::SameLine();
                    ImGui::Dummy({ window_padding, 0 });
                    ImGui::Separator();
                    };
                auto max_width = [](const std::vector<std::string>& items, const std::string& title, float extra_size = 0.0f) {
                    float ret = ImGui::CalcTextSize(title.c_str()).x;
                    for (const std::string& item : items) {
                        ret = std::max(ret, extra_size + ImGui::CalcTextSize(item.c_str()).x);
                    }
                    return ret;
                    };
                auto calculate_offsets = [max_width, window_padding, &checkbox_offset](const std::vector<std::pair<std::string, std::vector<::string>>>& title_columns, float extra_size = 0.0f) {
                    const ImGuiStyle& style = ImGui::GetStyle();
                    std::vector<float> offsets;
                    offsets.push_back(max_width(title_columns[0].second, title_columns[0].first, extra_size) + 3.0f * style.ItemSpacing.x);
                    for (size_t i = 2; i < title_columns.size(); i++) {
                        if (title_columns[i].first == "") {
                            offsets.push_back(offsets.back() + max_width(title_columns[i - 1].second, "") + style.ItemSpacing.x);
                        }
                        else if (title_columns[i].first == _u8L("Display")) {
                            float length = ImGui::CalcTextSize(title_columns[i - 2].first.c_str()).x;
                            float offset = offsets.back() + max_width(title_columns[i - 1].second, title_columns[i - 1].first);
                            size_t index = offsets.size() - 2;
                            if (index >= 0) {
                                offset = std::max(offset, length + offsets[index]);
                            }
                            offsets.push_back(offset + 2.0f * style.ItemSpacing.x);
                        }
                        else {
                            offsets.push_back(offsets.back() + max_width(title_columns[i - 1].second, title_columns[i - 1].first) + 2.0f * style.ItemSpacing.x);
                        }
                    }
                    float average_col_width = ImGui::GetWindowWidth() / static_cast<float>(title_columns.size());
                    std::vector<float> ret;
                    ret.push_back(0);
                    for (size_t i = 1; i < title_columns.size(); i++) {
                        ret.push_back(std::max(offsets[i - 1], i * average_col_width));
                    }
                    if (title_columns.back().first == _u8L("Display")) {
                        checkbox_offset = ret.back() + window_padding;
                    }
                    return ret;
                    };
                // BBS: no ColorChange type, use ToolChange
                //auto color_print_ranges = [this](unsigned char extruder_id, const std::vector<CustomGCode::Item>& custom_gcode_per_print_z) {
                //    std::vector<std::pair<Color, std::pair<double, double>>> ret;
                //    ret.reserve(custom_gcode_per_print_z.size());
                //    for (const auto& item : custom_gcode_per_print_z) {
                //        if (extruder_id + 1 != static_cast<unsigned char>(item.extruder))
                //            continue;
                //        if (item.type != ColorChange)
                //            continue;
                //        const std::vector<double> zs = m_layers.get_zs();
                //        auto lower_b = std::lower_bound(zs.begin(), zs.end(), item.print_z - epsilon());
                //        if (lower_b == zs.end())
                //            continue;
                //        const double current_z = *lower_b;
                //        const double previous_z = (lower_b == zs.begin()) ? 0.0 : *(--lower_b);
                //        // to avoid duplicate values, check adding values
                //        if (ret.empty() || !(ret.back().second.first == previous_z && ret.back().second.second == current_z))
                //            ret.push_back({ decode_color(item.color), { previous_z, current_z } });
                //    }
                //    return ret;
                //};
                auto upto_label = [](double z) {
                    char buf[64];
                    ::sprintf(buf, "%.2f", z);
                    return _u8L("up to") + " " + std::string(buf) + " " + _u8L("mm");
                    };
                auto above_label = [](double z) {
                    char buf[64];
                    ::sprintf(buf, "%.2f", z);
                    return _u8L("above") + " " + std::string(buf) + " " + _u8L("mm");
                    };
                auto fromto_label = [](double z1, double z2) {
                    char buf1[64];
                    ::sprintf(buf1, "%.2f", z1);
                    char buf2[64];
                    ::sprintf(buf2, "%.2f", z2);
                    return _u8L("from") + " " + std::string(buf1) + " " + _u8L("to") + " " + std::string(buf2) + " " + _u8L("mm");
                    };
                auto role_time_and_percent = [time_mode](ExtrusionRole role) {
                    auto it = std::find_if(time_mode.roles_times.begin(), time_mode.roles_times.end(), [role](const std::pair<ExtrusionRole, float>& item) { return role == item.first; });
                    return (it != time_mode.roles_times.end()) ? std::make_pair(it->second, it->second / time_mode.time) : std::make_pair(0.0f, 0.0f);
                    };
                auto move_time_and_percent = [time_mode](EMoveType move_type) {
                    auto it = std::find_if(time_mode.moves_times.begin(), time_mode.moves_times.end(), [move_type](const std::pair<EMoveType, float>& item) { return move_type == item.first; });
                    return (it != time_mode.moves_times.end()) ? std::make_pair(it->second, it->second / time_mode.time) : std::make_pair(0.0f, 0.0f);
                    };
                auto used_filament_per_role = [this, imperial_units](ExtrusionRole role) {
                    auto it = m_print_statistics.used_filaments_per_role.find(role);
                    if (it == m_print_statistics.used_filaments_per_role.end())
                        return std::make_pair(0.0, 0.0);
                    double koef = imperial_units ? 1000.0 / GizmoObjectManipulation::in_to_mm : 1.0;
                    return std::make_pair(it->second.first * koef, it->second.second);
                    };
                // get used filament (meters and grams) from used volume in respect to the active extruder
                auto get_used_filament_from_volume = [this, imperial_units](double volume, int extruder_id) {
                    double koef = imperial_units ? 1.0 / GizmoObjectManipulation::in_to_mm : 0.001;
                    std::pair<double, double> ret = { koef * volume / (PI * sqr(0.5 * m_filament_diameters[extruder_id])),
                                                      volume * m_filament_densities[extruder_id] * 0.001 };
                    return ret;
                    };
                // BBS Slicing Result title
                ImGui::Dummy({ window_padding, window_padding });
                ImGui::Dummy({ window_padding, window_padding });
                ImGui::SameLine();
                ImVec2      title_start_pos = ImGui::GetCursorPos();
                std::string title = _u8L("Slicing Result");
                imgui.bold_text(title);

                // BBS Set the width of the 8 "ABCD" words minus the "sliced result" to the spacing between the buttons and the title
                float single_word_width = imgui.calc_text_size("ABCD").x;
                float title_width = imgui.calc_text_size(title).x;
                float spacing = 18.0f * m_scale;
                ImGui::SameLine(0, (single_word_width + spacing) * 8.0f - title_width);
                // BBS support helio
                std::wstring btn_name;
                if (m_fold)
                    btn_name = ImGui::UnfoldButtonIcon + boost::nowide::widen(std::string(""));
                else
                    btn_name = ImGui::FoldButtonIcon + boost::nowide::widen(std::string(""));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.68f, 0.26f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.68f, 0.26f, 0.78f));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
                float button_width = ImGui::CalcTextSize(into_u8(btn_name).c_str()).x;
                ImGui::SetCursorPosY(8.f);
                if (ImGui::Button(into_u8(btn_name).c_str(), ImVec2(button_width, 0))) { m_fold = !m_fold; }
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(1);
                if (m_fold) {
                    legend_height = ImGui::GetStyle().WindowPadding.y + ImGui::GetFrameHeight() + window_padding * 2.5;
                    imgui.end();
                    ImGui::PopStyleColor(7);
                    ImGui::PopStyleVar(2);
                    return;
                }
                //BBS display Color Scheme
                ImGui::Dummy({ window_padding, window_padding });
                ImGui::Dummy({ window_padding, window_padding });
                ImGui::SameLine();
                imgui.bold_text(_u8L("Color Scheme"));
                ImGui::SameLine();
                auto curr_plate_index = wxGetApp().plater()->get_partplate_list().get_curr_plate_index();
                if (wxGetApp().plater()->get_helio_process_status() != m_last_helio_process_status || m_gcode_result->update_imgui_flag) {
                    auto load_only_gcode = wxGetApp().plater()->only_gcode_mode();
                    auto load_gcode3mf = wxGetApp().plater()->is_gcode_3mf();
                    if (load_only_gcode || load_gcode3mf) {
                        wxGetApp().plater()->clear_helio_process_status();
                    }
                    m_last_helio_process_status = wxGetApp().plater()->get_helio_process_status();
                    m_show_horizontal_slider = false;
                    if ((int)Slic3r::HelioBackgroundProcess::State::STATE_FINISHED == m_last_helio_process_status || (m_gcode_result->update_imgui_flag && m_gcode_result->is_helio_gcode)) {
                        update_thermal_options(true);
                        for (int i = 0; i < view_type_items.size(); i++) {
                            if (view_type_items[i] == EViewType::ThermalIndexMean) {
                                m_view_type_sel = i;
                                break;
                            }
                        }
                        set_view_type(EViewType::ThermalIndexMean);
                        wxGetApp().plater()->get_notification_manager()->close_notification_of_type(NotificationType::HelioSlicingError);
                        m_helio_slice_map_oks[curr_plate_index] = true;
                    }
                    else if ((int)Slic3r::HelioBackgroundProcess::State::STATE_CANCELED == m_last_helio_process_status ||
                        (m_gcode_result->update_imgui_flag && !m_gcode_result->is_helio_gcode && curr_plate_has_ok_helio_slice(curr_plate_index))) {
                        reset_curr_plate_thermal_options(curr_plate_index);
                    }
                    const_cast<GCodeProcessorResult*>(m_gcode_result)->update_imgui_flag = false;
                }
                push_combo_style();
                ImGuiComboFlags flags = 0;
                const char* view_type_value = view_type_image_names[m_view_type_sel].option_name.c_str();
                if (ImGui::BBLBeginCombo("", view_type_value, flags)) {
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
                    for (int i = 0; i < view_type_image_names.size(); i++) {
                        const bool is_selected = (m_view_type_sel == i);
                        if (ImGui::BBLSelectable_LeftImage(view_type_image_names[i].option_name.c_str(), is_selected, view_type_image_names[i].texture_id)) {
                            m_fold = false;
                            m_view_type_sel = i;
                            if (!is_helio_option()) {
                                m_last_non_helio_option_item = i;
                                record_gcodeviewer_option_item();
                            }
                            set_view_type(view_type_items[m_view_type_sel]);
                            reset_visible(view_type_items[m_view_type_sel]);
                            // update buffers' render paths
                            refresh_render_paths();
                            update_moves_slider();
                            wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
                        }
                        if (is_selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::PopStyleVar(1);
                    ImGui::EndCombo();
                }
                pop_combo_style();
                ImGui::SameLine();
                ImGui::Dummy({ window_padding, window_padding });
                // data used to properly align items in columns when showing time
                std::vector<float> offsets;
                std::vector<std::string> labels;
                std::vector<std::string> times;
                std::string travel_time;
                std::vector<std::string> percents;
                std::string travel_percent;
                std::vector<double> model_used_filaments_m;
                std::vector<double> model_used_filaments_g;
                std::vector<std::string> used_filaments_m;
                std::vector<std::string> used_filaments_g;
                double total_model_used_filament_m = 0, total_model_used_filament_g = 0;
                std::vector<double> flushed_filaments_m;
                std::vector<double> flushed_filaments_g;
                double total_flushed_filament_m = 0, total_flushed_filament_g = 0;
                std::vector<double> wipe_tower_used_filaments_m;
                std::vector<double> wipe_tower_used_filaments_g;
                double total_wipe_tower_used_filament_m = 0, total_wipe_tower_used_filament_g = 0;
                std::vector<double> support_used_filaments_m;
                std::vector<double> support_used_filaments_g;
                double total_support_used_filament_m = 0, total_support_used_filament_g = 0;
                struct ColumnData {
                    enum {
                        Model = 1,
                        Flushed = 2,
                        WipeTower = 4,
                        Support = 1 << 3,
                    };
                };
                int displayed_columns = 0;
                std::map<std::string, float> color_print_offsets;
                const PrintStatistics& ps = wxGetApp().plater()->get_partplate_list().get_current_fff_print().print_statistics();
                double koef = imperial_units ? GizmoObjectManipulation::in_to_mm : 1000.0;
                double unit_conver = imperial_units ? GizmoObjectManipulation::oz_to_g : 1;
                // used filament statistics
                for (size_t extruder_id : m_extruder_ids) {
                    if (m_print_statistics.model_volumes_per_extruder.find(extruder_id) == m_print_statistics.model_volumes_per_extruder.end()) {
                        model_used_filaments_m.push_back(0.0);
                        model_used_filaments_g.push_back(0.0);
                    }
                    else {
                        double volume = m_print_statistics.model_volumes_per_extruder.at(extruder_id);
                        auto [model_used_filament_m, model_used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
                        model_used_filaments_m.push_back(model_used_filament_m);
                        model_used_filaments_g.push_back(model_used_filament_g);
                        total_model_used_filament_m += model_used_filament_m;
                        total_model_used_filament_g += model_used_filament_g;
                        displayed_columns |= ColumnData::Model;
                    }
                }
                for (size_t extruder_id : m_extruder_ids) {
                    if (m_print_statistics.wipe_tower_volumes_per_extruder.find(extruder_id) == m_print_statistics.wipe_tower_volumes_per_extruder.end()) {
                        wipe_tower_used_filaments_m.push_back(0.0);
                        wipe_tower_used_filaments_g.push_back(0.0);
                    }
                    else {
                        double volume = m_print_statistics.wipe_tower_volumes_per_extruder.at(extruder_id);
                        auto [wipe_tower_used_filament_m, wipe_tower_used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
                        wipe_tower_used_filaments_m.push_back(wipe_tower_used_filament_m);
                        wipe_tower_used_filaments_g.push_back(wipe_tower_used_filament_g);
                        total_wipe_tower_used_filament_m += wipe_tower_used_filament_m;
                        total_wipe_tower_used_filament_g += wipe_tower_used_filament_g;
                        displayed_columns |= ColumnData::WipeTower;
                    }
                }
                for (size_t extruder_id : m_extruder_ids) {
                    if (m_print_statistics.flush_per_filament.find(extruder_id) == m_print_statistics.flush_per_filament.end()) {
                        flushed_filaments_m.push_back(0.0);
                        flushed_filaments_g.push_back(0.0);
                    }
                    else {
                        double volume = m_print_statistics.flush_per_filament.at(extruder_id);
                        auto [flushed_filament_m, flushed_filament_g] = get_used_filament_from_volume(volume, extruder_id);
                        flushed_filaments_m.push_back(flushed_filament_m);
                        flushed_filaments_g.push_back(flushed_filament_g);
                        total_flushed_filament_m += flushed_filament_m;
                        total_flushed_filament_g += flushed_filament_g;
                        displayed_columns |= ColumnData::Flushed;
                    }
                }
                for (size_t extruder_id : m_extruder_ids) {
                    if (m_print_statistics.support_volumes_per_extruder.find(extruder_id) == m_print_statistics.support_volumes_per_extruder.end()) {
                        support_used_filaments_m.push_back(0.0);
                        support_used_filaments_g.push_back(0.0);
                    }
                    else {
                        double volume = m_print_statistics.support_volumes_per_extruder.at(extruder_id);
                        auto [used_filament_m, used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
                        support_used_filaments_m.push_back(used_filament_m);
                        support_used_filaments_g.push_back(used_filament_g);
                        total_support_used_filament_m += used_filament_m;
                        total_support_used_filament_g += used_filament_g;
                        displayed_columns |= ColumnData::Support;
                    }
                }
                // extrusion paths section -> title
                ImGui::Dummy({ window_padding, window_padding });
                ImGui::SameLine();
                switch (m_view_type)
                {
                case EViewType::FeatureType:
                {
                    // calculate offsets to align time/percentage data
                    char buffer[64];
                    for (size_t i = 0; i < m_roles.size(); ++i) {
                        ExtrusionRole role = m_roles[i];
                        if (role < erCount) {
                            labels.push_back(_u8L(ExtrusionEntity::role_to_string(role)));
                            auto [time, percent] = role_time_and_percent(role);
                            times.push_back((time > 0.0f) ? short_time(get_time_dhms(time)) : "");
                            if (percent == 0)
                                ::sprintf(buffer, "0%%");
                            else
                                percent > 0.001 ? ::sprintf(buffer, "%.1f%%", percent * 100) : ::sprintf(buffer, "<0.1%%");
                            percents.push_back(buffer);
                            auto [model_used_filament_m, model_used_filament_g] = used_filament_per_role(role);
                            //model_used_filaments_m.push_back(model_used_filament_m);
                            //model_used_filaments_g.push_back(model_used_filament_g);
                            memset(&buffer, 0, sizeof(buffer));
                            ::sprintf(buffer, imperial_units ? "%.2f in" : "%.2f m", model_used_filament_m);
                            used_filaments_m.push_back(buffer);
                            memset(&buffer, 0, sizeof(buffer));
                            ::sprintf(buffer, "%.2f g", model_used_filament_g);
                            used_filaments_g.push_back(buffer);
                        }
                    }
                    //BBS: get travel time and percent
                    {
                        auto [time, percent] = move_time_and_percent(EMoveType::Travel);
                        travel_time = (time > 0.0f) ? short_time(get_time_dhms(time)) : "";
                        if (percent == 0)
                            ::sprintf(buffer, "0%%");
                        else
                            percent > 0.001 ? ::sprintf(buffer, "%.1f%%", percent * 100) : ::sprintf(buffer, "<0.1%%");
                        travel_percent = buffer;
                    }
                    offsets = calculate_offsets({ {_u8L("Line Type"), labels}, {_u8L("Time"), times}, {_u8L("Percent"), percents}, {_u8L("Used filament"), used_filaments_m}, {"", used_filaments_g}, {_u8L("Display"), {""}} }, icon_size);
                    append_headers({ {_u8L("Line Type"), offsets[0]}, {_u8L("Time"), offsets[1]}, {_u8L("Percent"), offsets[2]}, {_u8L("Used filament"), offsets[3]}, {"", offsets[4]}, {_u8L("Display"), offsets[5]} });
                    break;
                }
                case EViewType::Height: { imgui.title(_u8L("Layer Height (mm)")); break; }
                case EViewType::Width: { imgui.title(_u8L("Line Width (mm)")); break; }
                case EViewType::Feedrate: { imgui.title(_u8L("Speed (mm/s)")); break; }
                case EViewType::FanSpeed: { imgui.title(_u8L("Fan Speed (%)")); break; }
                case EViewType::Temperature: { imgui.title(_u8L("Temperature (°C)")); break; }
                case EViewType::VolumetricRate: { imgui.title(_u8L("Volumetric flow rate (mm³/s)")); break; }
                case EViewType::LayerTime: { imgui.title(_u8L("Layer Time")); break; }
                case EViewType::Tool:
                {
                    // calculate used filaments data
                    for (size_t extruder_id : m_extruder_ids) {
                        if (m_print_statistics.model_volumes_per_extruder.find(extruder_id) == m_print_statistics.model_volumes_per_extruder.end())
                            continue;
                        double volume = m_print_statistics.model_volumes_per_extruder.at(extruder_id);
                        auto [model_used_filament_m, model_used_filament_g] = get_used_filament_from_volume(volume, extruder_id);
                        model_used_filaments_m.push_back(model_used_filament_m);
                        model_used_filaments_g.push_back(model_used_filament_g);
                    }
                    offsets = calculate_offsets({ { "Extruder NNN", {""}} }, icon_size);
                    append_headers({ {_u8L("Filament"), offsets[0]}, {_u8L("Used filament"), offsets[1]} });
                    break;
                }
                case EViewType::ColorPrint:
                {
                    std::vector<std::string> total_filaments;
                    char buffer[64];
                    ::sprintf(buffer, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", ps.total_used_filament / /*1000*/koef, ps.total_weight / unit_conver);
                    total_filaments.push_back(buffer);
                    std::vector<std::pair<std::string, std::vector<::string>>> title_columns;
                    if (displayed_columns & ColumnData::Model) {
                        title_columns.push_back({ _u8L("Filament"), {""} });
                        title_columns.push_back({ _u8L("Model"), total_filaments });
                    }
                    if (displayed_columns & ColumnData::Support) {
                        title_columns.push_back({ _u8L("Support"), total_filaments });
                    }
                    if (displayed_columns & ColumnData::Flushed) {
                        title_columns.push_back({ _u8L("Flushed"), total_filaments });
                    }
                    if (displayed_columns & ColumnData::WipeTower) {
                        title_columns.push_back({ _u8L("Tower"), total_filaments });
                    }
                    if ((displayed_columns & ~ColumnData::Model) > 0) {
                        title_columns.push_back({ _u8L("Total"), total_filaments });
                    }
                    auto offsets_ = calculate_offsets(title_columns, icon_size);
                    std::vector<std::pair<std::string, float>> title_offsets;
                    for (int i = 0; i < offsets_.size(); i++) {
                        title_offsets.push_back({ title_columns[i].first, offsets_[i] });
                        color_print_offsets[title_columns[i].first] = offsets_[i];
                    }
                    append_headers(title_offsets);
                    break;
                }
                // helio
                case EViewType::ThermalIndexMin: { imgui.title(_u8L("Thermal Index (min)")); break; }
                case EViewType::ThermalIndexMax: { imgui.title(_u8L("Thermal Index (max)")); break; }
                case EViewType::ThermalIndexMean: { imgui.title(_u8L("Thermal Index (mean)")); break; }
                // end helio
                default: { break; }
                }
                auto append_option_item = [this, append_item](EMoveType type, std::vector<float> offsets) {
                    auto append_option_item_with_type = [this, offsets, append_item](EMoveType type, const Color& color, const std::string& label, bool visible) {
                        append_item(EItemType::Rect, color, { { label , offsets[0] } }, true, visible, [this, type, visible]() {
                            set_move_type_visible(type, !is_move_type_visible(type));
                            on_visibility_changed();
                            });
                        };
                    const bool visible = is_move_type_visible(type);
                    if (type == EMoveType::Travel) {
                        //BBS: only display travel time in FeatureType view
                        append_option_item_with_type(type, Travel_Colors[0], _u8L("Travel"), visible);
                    }
                    else if (type == EMoveType::Seam)
                        append_option_item_with_type(type, Options_Colors[(int)EOptionsColors::Seams], _u8L("Seams"), visible);
                    else if (type == EMoveType::Retract)
                        append_option_item_with_type(type, Options_Colors[(int)EOptionsColors::Retractions], _u8L("Retract"), visible);
                    else if (type == EMoveType::Unretract)
                        append_option_item_with_type(type, Options_Colors[(int)EOptionsColors::Unretractions], _u8L("Unretract"), visible);
                    else if (type == EMoveType::Tool_change)
                        append_option_item_with_type(type, Options_Colors[(int)EOptionsColors::ToolChanges], _u8L("Filament Changes"), visible);
                    else if (type == EMoveType::Wipe)
                        append_option_item_with_type(type, Wipe_Color, _u8L("Wipe"), visible);
                    };
                // extrusion paths section -> items
                switch (m_view_type)
                {
                case EViewType::FeatureType:
                {
                    for (size_t i = 0; i < m_roles.size(); ++i) {
                        ExtrusionRole role = m_roles[i];
                        if (role >= erCount)
                            continue;
                        const bool visible = is_extrusion_role_visible(role);
                        std::vector<std::pair<std::string, float>> columns_offsets;
                        columns_offsets.push_back({ labels[i], offsets[0] });
                        columns_offsets.push_back({ times[i], offsets[1] });
                        columns_offsets.push_back({ percents[i], offsets[2] });
                        columns_offsets.push_back({ used_filaments_m[i], offsets[3] });
                        columns_offsets.push_back({ used_filaments_g[i], offsets[4] });
                        append_item(EItemType::Rect, Extrusion_Role_Colors[static_cast<unsigned int>(role)], columns_offsets,
                            true, visible, [this, role, visible]() {
                                set_extrusion_role_visible(role, !visible);
                                on_visibility_changed();
                            });
                    }
                    for (auto item : options_items) {
                        if (item != EMoveType::Travel) {
                            append_option_item(item, offsets);
                        }
                        else {
                            //BBS: show travel time in FeatureType view
                            const bool visible = is_move_type_visible(item);
                            std::vector<std::pair<std::string, float>> columns_offsets;
                            columns_offsets.push_back({ _u8L("Travel"), offsets[0] });
                            columns_offsets.push_back({ travel_time, offsets[1] });
                            columns_offsets.push_back({ travel_percent, offsets[2] });
                            append_item(EItemType::Rect, Travel_Colors[0], columns_offsets, true, visible, [this, item, visible]() {
                                set_move_type_visible(item, !visible);
                                on_visibility_changed();
                                });
                        }
                    }
                    break;
                }
                case EViewType::Height: { append_range(m_p_extrusions->ranges.height, 2); break; }
                case EViewType::Width: { append_range(m_p_extrusions->ranges.width, 2); break; }
                case EViewType::Feedrate: {
                    append_range(m_p_extrusions->ranges.feedrate, 0);
                    ImGui::Spacing();
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    offsets = calculate_offsets({ { _u8L("Options"), { _u8L("Travel")}}, { _u8L("Display"), {""}} }, icon_size);
                    append_headers({ {_u8L("Options"), offsets[0] }, { _u8L("Display"), offsets[1]} });
                    const bool travel_visible = is_move_type_visible(EMoveType::Travel);
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 3.0f));
                    append_item(EItemType::None, Travel_Colors[0], { {_u8L("travel"), offsets[0] } }, true, travel_visible, [this, travel_visible]() {
                        set_move_type_visible(EMoveType::Travel, !travel_visible);
                        // update buffers' render paths, and update m_tools.m_tool_colors and m_extrusions.ranges
                        refresh(*m_gcode_result, wxGetApp().plater()->get_extruder_colors_from_plater_config(m_gcode_result));
                        on_visibility_changed();
                        });
                    ImGui::PopStyleVar(1);
                    break;
                }
                case EViewType::FanSpeed: { append_range(m_p_extrusions->ranges.fan_speed, 0); break; }
                case EViewType::Temperature: { append_range(m_p_extrusions->ranges.temperature, 0); break; }
                case EViewType::LayerTime: { append_range(m_p_extrusions->ranges.layer_duration, 1); break; }
                case EViewType::VolumetricRate: { append_range(m_p_extrusions->ranges.volumetric_rate, 2); break; }
                case EViewType::Tool:
                {
                    // shows only extruders actually used
                    char buf[64];
                    size_t i = 0;
                    for (unsigned char extruder_id : m_extruder_ids) {
                        ::sprintf(buf, imperial_units ? "%.2f in    %.2f g" : "%.2f m    %.2f g", model_used_filaments_m[i], model_used_filaments_g[i]);
                        append_item(EItemType::Rect, m_tools.m_tool_colors[extruder_id], { { _u8L("Extruder") + " " + std::to_string(extruder_id + 1), offsets[0]}, {buf, offsets[1]} });
                        i++;
                    }
                    break;
                }
                case EViewType::Summary:
                {
                    char buf[64];
                    imgui.text(_u8L("Total") + ":");
                    ImGui::SameLine();
                    ::sprintf(buf, imperial_units ? "%.2f in / %.2f oz" : "%.2f m / %.2f g", ps.total_used_filament / koef, ps.total_weight / unit_conver);
                    imgui.text(buf);
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(_u8L("Cost") + ":");
                    ImGui::SameLine();
                    ::sprintf(buf, "%.2f", ps.total_cost);
                    imgui.text(buf);
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(_u8L("Total time") + ":");
                    ImGui::SameLine();
                    imgui.text(short_time(get_time_dhms(time_mode.time)));
                    break;
                }
                case EViewType::ColorPrint:
                {
                    //BBS: replace model custom gcode with current plate custom gcode
                    const std::vector<CustomGCode::Item>& custom_gcode_per_print_z = wxGetApp().is_editor() ? wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes : m_custom_gcode_per_print_z;
                    size_t total_items = 1;
                    // BBS: no ColorChange type, use ToolChange
                    //for (size_t extruder_id : m_extruder_ids) {
                    //    total_items += color_print_ranges(extruder_id, custom_gcode_per_print_z).size();
                    //}
                    const bool need_scrollable = static_cast<float>(total_items) * (icon_size + ImGui::GetStyle().ItemSpacing.y) > child_height;
                    // add scrollable region, if needed
                    if (need_scrollable)
                        ImGui::BeginChild("color_prints", { -1.0f, child_height }, false);
                    // shows only extruders actually used
                    size_t i = 0;
                    for (auto extruder_idx : m_extruder_ids) {
                        const bool filament_visible = m_tools.m_tool_visibles[extruder_idx];
                        if (i < model_used_filaments_m.size() && i < model_used_filaments_g.size()) {
                            std::vector<std::pair<std::string, float>> columns_offsets;
                            columns_offsets.push_back({ std::to_string(extruder_idx + 1), color_print_offsets[_u8L("Filament")] });
                            char buf[64];
                            float column_sum_m = 0.0f;
                            float column_sum_g = 0.0f;
                            if (displayed_columns & ColumnData::Model) {
                                if ((displayed_columns & ~ColumnData::Model) > 0)
                                    ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", model_used_filaments_m[i], model_used_filaments_g[i] / unit_conver);
                                else
                                    ::sprintf(buf, imperial_units ? "%.2f in    %.2f oz" : "%.2f m    %.2f g", model_used_filaments_m[i], model_used_filaments_g[i] / unit_conver);
                                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Model")] });
                                column_sum_m += model_used_filaments_m[i];
                                column_sum_g += model_used_filaments_g[i];
                            }
                            if (displayed_columns & ColumnData::Support) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", support_used_filaments_m[i], support_used_filaments_g[i] / unit_conver);
                                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Support")] });
                                column_sum_m += support_used_filaments_m[i];
                                column_sum_g += support_used_filaments_g[i];
                            }
                            if (displayed_columns & ColumnData::Flushed) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", flushed_filaments_m[i], flushed_filaments_g[i] / unit_conver);
                                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Flushed")] });
                                column_sum_m += flushed_filaments_m[i];
                                column_sum_g += flushed_filaments_g[i];
                            }
                            if (displayed_columns & ColumnData::WipeTower) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", wipe_tower_used_filaments_m[i], wipe_tower_used_filaments_g[i] / unit_conver);
                                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Tower")] });
                                column_sum_m += wipe_tower_used_filaments_m[i];
                                column_sum_g += wipe_tower_used_filaments_g[i];
                            }
                            if ((displayed_columns & ~ColumnData::Model) > 0) {
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", column_sum_m, column_sum_g / unit_conver);
                                columns_offsets.push_back({ buf, color_print_offsets[_u8L("Total")] });
                            }
                            append_item(EItemType::Rect, m_tools.m_tool_colors[extruder_idx], columns_offsets, false, filament_visible, [this, extruder_idx]() {
                                m_tools.m_tool_visibles[extruder_idx] = !m_tools.m_tool_visibles[extruder_idx];
                                on_visibility_changed();
                                });
                        }
                        i++;
                    }
                    if (need_scrollable)
                        ImGui::EndChild();
                    // Sum of all rows
                    char buf[64];
                    if (m_extruder_ids.size() > 1) {
                        // Separator
                        ImGuiWindow* window = ImGui::GetCurrentWindow();
                        const ImRect separator(ImVec2(window->Pos.x + window_padding * 3, window->DC.CursorPos.y), ImVec2(window->Pos.x + window->Size.x - window_padding * 3, window->DC.CursorPos.y + 1.0f));
                        ImGui::ItemSize(ImVec2(0.0f, 0.0f));
                        const bool item_visible = ImGui::ItemAdd(separator, 0);
                        window->DrawList->AddLine(separator.Min, ImVec2(separator.Max.x, separator.Min.y), ImGui::GetColorU32(ImGuiCol_Separator));
                        std::vector<std::pair<std::string, float>> columns_offsets;
                        columns_offsets.push_back({ _u8L("Total"), color_print_offsets[_u8L("Filament")] });
                        if (displayed_columns & ColumnData::Model) {
                            if ((displayed_columns & ~ColumnData::Model) > 0)
                                ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_model_used_filament_m, total_model_used_filament_g / unit_conver);
                            else
                                ::sprintf(buf, imperial_units ? "%.2f in    %.2f oz" : "%.2f m    %.2f g", total_model_used_filament_m, total_model_used_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, color_print_offsets[_u8L("Model")] });
                        }
                        if (displayed_columns & ColumnData::Support) {
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_support_used_filament_m, total_support_used_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, color_print_offsets[_u8L("Support")] });
                        }
                        if (displayed_columns & ColumnData::Flushed) {
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_flushed_filament_m, total_flushed_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, color_print_offsets[_u8L("Flushed")] });
                        }
                        if (displayed_columns & ColumnData::WipeTower) {
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_wipe_tower_used_filament_m, total_wipe_tower_used_filament_g / unit_conver);
                            columns_offsets.push_back({ buf, color_print_offsets[_u8L("Tower")] });
                        }
                        if ((displayed_columns & ~ColumnData::Model) > 0) {
                            ::sprintf(buf, imperial_units ? "%.2f in\n%.2f oz" : "%.2f m\n%.2f g", total_model_used_filament_m + total_support_used_filament_m + total_flushed_filament_m + total_wipe_tower_used_filament_m,
                                (total_model_used_filament_g + total_support_used_filament_g + total_flushed_filament_g + total_wipe_tower_used_filament_g) / unit_conver);
                            columns_offsets.push_back({ buf, color_print_offsets[_u8L("Total")] });
                        }
                        append_item(EItemType::None, m_tools.m_tool_colors[0], columns_offsets);
                    }
                    //BBS display filament change times
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(_u8L("Filament change times") + ":");
                    ImGui::SameLine();
                    ::sprintf(buf, "%d", m_print_statistics.total_filament_changes + m_print_statistics.total_extruder_changes + m_print_statistics.total_nozzle_changes);
                    imgui.text(buf);
                    //BBS display cost
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(_u8L("Cost") + ":");
                    ImGui::SameLine();
                    ::sprintf(buf, "%.2f", ps.total_cost);
                    imgui.text(buf);
                    break;
                }
                // helio
                case EViewType::ThermalIndexMin: { append_range(m_p_extrusions->ranges.thermal_index_min, 0); break; }
                case EViewType::ThermalIndexMax: { append_range(m_p_extrusions->ranges.thermal_index_max, 0); break; }
                case EViewType::ThermalIndexMean: { append_range(m_p_extrusions->ranges.thermal_index_mean, 0); break; }
                // end helio
                default: { break; }
                }
                // partial estimated printing time section
                if (m_view_type == EViewType::ColorPrint) {
                    using Times = std::pair<float, float>;
                    using TimesList = std::vector<std::pair<CustomGCode::Type, Times>>;
                    // helper structure containig the data needed to render the time items
                    struct PartialTime
                    {
                        enum class EType : unsigned char
                        {
                            Print,
                            ColorChange,
                            Pause
                        };
                        EType type;
                        int extruder_id;
                        Color color1;
                        Color color2;
                        Times times;
                        std::pair<double, double> used_filament{ 0.0f, 0.0f };
                    };
                    using PartialTimes = std::vector<PartialTime>;
                    auto generate_partial_times = [this, get_used_filament_from_volume](const TimesList& times, const std::vector<double>& used_filaments) {
                        PartialTimes items;
                        //BBS: replace model custom gcode with current plate custom gcode
                        std::vector<CustomGCode::Item> custom_gcode_per_print_z = wxGetApp().is_editor() ? wxGetApp().plater()->model().get_curr_plate_custom_gcodes().gcodes : m_custom_gcode_per_print_z;
                        std::vector<Color> last_color(m_extruders_count);
                        for (size_t i = 0; i < m_extruders_count; ++i) {
                            last_color[i] = m_tools.m_tool_colors[i];
                        }
                        int last_extruder_id = 1;
                        int color_change_idx = 0;
                        for (const auto& time_rec : times) {
                            switch (time_rec.first)
                            {
                            case CustomGCode::PausePrint: {
                                auto it = std::find_if(custom_gcode_per_print_z.begin(), custom_gcode_per_print_z.end(), [time_rec](const CustomGCode::Item& item) { return item.type == time_rec.first; });
                                if (it != custom_gcode_per_print_z.end()) {
                                    items.push_back({ PartialTime::EType::Print, it->extruder, last_color[it->extruder - 1], Color(), time_rec.second });
                                    items.push_back({ PartialTime::EType::Pause, it->extruder, Color(), Color(), time_rec.second });
                                    custom_gcode_per_print_z.erase(it);
                                }
                                break;
                            }
                            case CustomGCode::ColorChange: {
                                auto it = std::find_if(custom_gcode_per_print_z.begin(), custom_gcode_per_print_z.end(), [time_rec](const CustomGCode::Item& item) { return item.type == time_rec.first; });
                                if (it != custom_gcode_per_print_z.end()) {
                                    items.push_back({ PartialTime::EType::Print, it->extruder, last_color[it->extruder - 1], Color(), time_rec.second, get_used_filament_from_volume(used_filaments[color_change_idx++], it->extruder - 1) });
                                    items.push_back({ PartialTime::EType::ColorChange, it->extruder, last_color[it->extruder - 1], ::decode_color(it->color), time_rec.second });
                                    last_color[it->extruder - 1] = ::decode_color(it->color);
                                    last_extruder_id = it->extruder;
                                    custom_gcode_per_print_z.erase(it);
                                }
                                else
                                    items.push_back({ PartialTime::EType::Print, last_extruder_id, last_color[last_extruder_id - 1], Color(), time_rec.second, get_used_filament_from_volume(used_filaments[color_change_idx++], last_extruder_id - 1) });
                                break;
                            }
                            default: { break; }
                            }
                        }
                        return items;
                        };
                    auto append_color_change = [&imgui](const Color& color1, const Color& color2, const std::array<float, 4>& offsets, const Times& times) {
                        imgui.text(_u8L("Color change"));
                        ImGui::SameLine();
                        float icon_size = ImGui::GetTextLineHeight();
                        ImDrawList* draw_list = ImGui::GetWindowDrawList();
                        ImVec2 pos = ImGui::GetCursorScreenPos();
                        pos.x -= 0.5f * ImGui::GetStyle().ItemSpacing.x;
                        draw_list->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { pos.x + icon_size - 1.0f, pos.y + icon_size - 1.0f },
                            ImGui::GetColorU32({ color1[0], color1[1], color1[2], 1.0f }));
                        pos.x += icon_size;
                        draw_list->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { pos.x + icon_size - 1.0f, pos.y + icon_size - 1.0f },
                            ImGui::GetColorU32({ color2[0], color2[1], color2[2], 1.0f }));
                        ImGui::SameLine(offsets[0]);
                        imgui.text(short_time(get_time_dhms(times.second - times.first)));
                        };
                    auto append_print = [&imgui, imperial_units](const Color& color, const std::array<float, 4>& offsets, const Times& times, std::pair<double, double> used_filament) {
                        imgui.text(_u8L("Print"));
                        ImGui::SameLine();
                        float icon_size = ImGui::GetTextLineHeight();
                        ImDrawList* draw_list = ImGui::GetWindowDrawList();
                        ImVec2 pos = ImGui::GetCursorScreenPos();
                        pos.x -= 0.5f * ImGui::GetStyle().ItemSpacing.x;
                        draw_list->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { pos.x + icon_size - 1.0f, pos.y + icon_size - 1.0f },
                            ImGui::GetColorU32({ color[0], color[1], color[2], 1.0f }));
                        ImGui::SameLine(offsets[0]);
                        imgui.text(short_time(get_time_dhms(times.second)));
                        ImGui::SameLine(offsets[1]);
                        imgui.text(short_time(get_time_dhms(times.first)));
                        if (used_filament.first > 0.0f) {
                            char buffer[64];
                            ImGui::SameLine(offsets[2]);
                            ::sprintf(buffer, imperial_units ? "%.2f in" : "%.2f m", used_filament.first);
                            imgui.text(buffer);
                            ImGui::SameLine(offsets[3]);
                            ::sprintf(buffer, "%.2f g", used_filament.second);
                            imgui.text(buffer);
                        }
                        };
                    PartialTimes partial_times = generate_partial_times(time_mode.custom_gcode_times, m_print_statistics.volumes_per_color_change);
                    if (!partial_times.empty()) {
                        labels.clear();
                        times.clear();
                        for (const PartialTime& item : partial_times) {
                            switch (item.type)
                            {
                            case PartialTime::EType::Print: { labels.push_back(_u8L("Print")); break; }
                            case PartialTime::EType::Pause: { labels.push_back(_u8L("Pause")); break; }
                            case PartialTime::EType::ColorChange: { labels.push_back(_u8L("Color change")); break; }
                            }
                            times.push_back(short_time(get_time_dhms(item.times.second)));
                        }
                        std::string longest_used_filament_string;
                        for (const PartialTime& item : partial_times) {
                            if (item.used_filament.first > 0.0f) {
                                char buffer[64];
                                ::sprintf(buffer, imperial_units ? "%.2f in" : "%.2f m", item.used_filament.first);
                                if (::strlen(buffer) > longest_used_filament_string.length())
                                    longest_used_filament_string = buffer;
                            }
                        }
                        //offsets = calculate_offsets(labels, times, { _u8L("Event"), _u8L("Remaining time"), _u8L("Duration"), longest_used_filament_string }, 2.0f * icon_size);
                        //ImGui::Spacing();
                        //append_headers({ _u8L("Event"), _u8L("Remaining time"), _u8L("Duration"), _u8L("Used filament") }, offsets);
                        //const bool need_scrollable = static_cast<float>(partial_times.size()) * (icon_size + ImGui::GetStyle().ItemSpacing.y) > child_height;
                        //if (need_scrollable)
                        //    // add scrollable region
                        //    ImGui::BeginChild("events", { -1.0f, child_height }, false);
                        //for (const PartialTime& item : partial_times) {
                        //    switch (item.type)
                        //    {
                        //    case PartialTime::EType::Print: {
                        //        append_print(item.color1, offsets, item.times, item.used_filament);
                        //        break;
                        //    }
                        //    case PartialTime::EType::Pause: {
                        //        imgui.text(_u8L("Pause"));
                        //        ImGui::SameLine(offsets[0]);
                        //        imgui.text(short_time(get_time_dhms(item.times.second - item.times.first)));
                        //        break;
                        //    }
                        //    case PartialTime::EType::ColorChange: {
                        //        append_color_change(item.color1, item.color2, offsets, item.times);
                        //        break;
                        //    }
                        //    }
                        //}
                        //if (need_scrollable)
                        //    ImGui::EndChild();
                    }
                }
                // travel paths section
                if (is_move_type_visible(EMoveType::Travel)) {
                    switch (m_view_type)
                    {
                    case EViewType::Feedrate:
                    case EViewType::Tool:
                    case EViewType::ColorPrint: {
                        break;
                    }
                    default: {
                        // BBS GUI:refactor
                        // title
                        //ImGui::Spacing();
                        //imgui.title(_u8L("Travel"));
                        //// items
                        //append_item(EItemType::Line, Travel_Colors[0], _u8L("Movement"));
                        //append_item(EItemType::Line, Travel_Colors[1], _u8L("Extrusion"));
                        //append_item(EItemType::Line, Travel_Colors[2], _u8L("Retraction"));
                        break;
                    }
                    }
                }
                // wipe paths section
                //if (m_buffers[buffer_id(EMoveType::Wipe)].visible) {
                //    switch (m_view_type)
                //    {
                //    case EViewType::Feedrate:
                //    case EViewType::Tool:
                //    case EViewType::ColorPrint: { break; }
                //    default: {
                //        // title
                //        ImGui::Spacing();
                //        ImGui::Dummy({ window_padding, window_padding });
                //        ImGui::SameLine();
                //        imgui.title(_u8L("Wipe"));
                //        // items
                //        append_item(EItemType::Line, Wipe_Color, { {_u8L("Wipe"), 0} });
                //        break;
                //    }
                //    }
                //}
                //auto any_option_available = [this]() {
                //    auto available = [this](EMoveType type) {
                //        const TBuffer& buffer = m_buffers[buffer_id(type)];
                //        return buffer.visible && buffer.has_data();
                //        };
                //    return available(EMoveType::Color_change) ||
                //        available(EMoveType::Custom_GCode) ||
                //        available(EMoveType::Pause_Print) ||
                //        available(EMoveType::Retract) ||
                //        available(EMoveType::Tool_change) ||
                //        available(EMoveType::Unretract) ||
                //        available(EMoveType::Seam);
                //    };
                //auto add_option = [this, append_item](EMoveType move_type, EOptionsColors color, const std::string& text) {
                //    const TBuffer& buffer = m_buffers[buffer_id(move_type)];
                //    if (buffer.visible && buffer.has_data())
                //        append_item(EItemType::Circle, Options_Colors[static_cast<unsigned int>(color)], text);
                //};
                /* BBS GUI refactor */
                // options section
                //if (any_option_available()) {
                //    // title
                //    ImGui::Spacing();
                //    imgui.title(_u8L("Options"));
                //    // items
                //    add_option(EMoveType::Retract, EOptionsColors::Retractions, _u8L("Retractions"));
                //    add_option(EMoveType::Unretract, EOptionsColors::Unretractions, _u8L("Deretractions"));
                //    add_option(EMoveType::Seam, EOptionsColors::Seams, _u8L("Seams"));
                //    add_option(EMoveType::Tool_change, EOptionsColors::ToolChanges, _u8L("Tool changes"));
                //    add_option(EMoveType::Color_change, EOptionsColors::ColorChanges, _u8L("Color changes"));
                //    add_option(EMoveType::Pause_Print, EOptionsColors::PausePrints, _u8L("Print pauses"));
                //    add_option(EMoveType::Custom_GCode, EOptionsColors::CustomGCodes, _u8L("Custom G-codes"));
                //}
                // settings section
                bool has_settings = false;
                has_settings |= !m_settings_ids.print.empty();
                has_settings |= !m_settings_ids.printer.empty();
                bool has_filament_settings = true;
                has_filament_settings &= !m_settings_ids.filament.empty();
                for (const std::string& fs : m_settings_ids.filament) {
                    has_filament_settings &= !fs.empty();
                }
                has_settings |= has_filament_settings;
                //BBS: add only gcode mode
                bool show_settings = m_only_gcode_in_preview; //wxGetApp().is_gcode_viewer();
                show_settings &= (m_view_type == EViewType::FeatureType || m_view_type == EViewType::Tool);
                show_settings &= has_settings;
                if (show_settings) {
                    auto calc_offset = [this]() {
                        float ret = 0.0f;
                        if (!m_settings_ids.printer.empty())
                            ret = std::max(ret, ImGui::CalcTextSize((_u8L("Printer") + std::string(":")).c_str()).x);
                        if (!m_settings_ids.print.empty())
                            ret = std::max(ret, ImGui::CalcTextSize((_u8L("Print settings") + std::string(":")).c_str()).x);
                        if (!m_settings_ids.filament.empty()) {
                            for (unsigned char i : m_extruder_ids) {
                                ret = std::max(ret, ImGui::CalcTextSize((_u8L("Filament") + " " + std::to_string(i + 1) + ":").c_str()).x);
                            }
                        }
                        if (ret > 0.0f)
                            ret += 2.0f * ImGui::GetStyle().ItemSpacing.x;
                        return ret;
                        };
                    ImGui::Spacing();
                    imgui.title(_u8L("Settings"));
                    float offset = calc_offset();
                    if (!m_settings_ids.printer.empty()) {
                        imgui.text(_u8L("Printer") + ":");
                        ImGui::SameLine(offset);
                        imgui.text(m_settings_ids.printer);
                    }
                    if (!m_settings_ids.print.empty()) {
                        imgui.text(_u8L("Print settings") + ":");
                        ImGui::SameLine(offset);
                        imgui.text(m_settings_ids.print);
                    }
                    if (!m_settings_ids.filament.empty()) {
                        for (unsigned char i : m_extruder_ids) {
                            if (i < static_cast<unsigned char>(m_settings_ids.filament.size()) && !m_settings_ids.filament[i].empty()) {
                                std::string txt = _u8L("Filament");
                                txt += (m_extruder_ids.size() == 1) ? ":" : " " + std::to_string(i + 1);
                                imgui.text(txt);
                                ImGui::SameLine(offset);
                                imgui.text(m_settings_ids.filament[i]);
                            }
                        }
                    }
                }
                // total estimated printing time section
                if (show_estimated) {
                    ImGui::Spacing();
                    std::string time_title = m_view_type == EViewType::FeatureType ? _u8L("Total Estimation") : _u8L("Time Estimation");
                    auto can_show_mode_button = [this](PrintEstimatedStatistics::ETimeMode mode) {
                        bool show = false;
                        if (m_print_statistics.modes.size() > 1 && m_print_statistics.modes[static_cast<size_t>(mode)].roles_times.size() > 0) {
                            for (size_t i = 0; i < m_print_statistics.modes.size(); ++i) {
                                if (i != static_cast<size_t>(mode) &&
                                    m_print_statistics.modes[i].time > 0.0f &&
                                    short_time(get_time_dhms(m_print_statistics.modes[static_cast<size_t>(mode)].time)) != short_time(get_time_dhms(m_print_statistics.modes[i].time))) {
                                    show = true;
                                    break;
                                }
                            }
                        }
                        return show;
                        };
                    if (can_show_mode_button(m_time_estimate_mode)) {
                        switch (m_time_estimate_mode)
                        {
                        case PrintEstimatedStatistics::ETimeMode::Normal: { time_title += " [" + _u8L("Normal mode") + "]"; break; }
                        default: { assert(false); break; }
                        }
                    }
                    double timelapse_time = 0.f;
                    if (auto timelapse_time_iter = m_gcode_result->skippable_part_time.find(SkipType::stTimelapse); timelapse_time_iter != m_gcode_result->skippable_part_time.end()) {
                        timelapse_time = timelapse_time_iter->second;
                    }
                    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.title(time_title);
                    std::string total_filament_str = _u8L("Total Filament");
                    std::string model_filament_str = _u8L("Model Filament");
                    std::string cost_str = _u8L("Cost");
                    std::string prepare_str;
                    if (timelapse_time != 0.0f)
                        prepare_str = _u8L("Prepare and timelapse time");
                    else
                        prepare_str = _u8L("Prepare time");
                    std::string print_str = _u8L("Model printing time");
                    std::string total_str = _u8L("Total time");
                    float max_len = window_padding + 2 * ImGui::GetStyle().ItemSpacing.x;
                    if (time_mode.layers_times.empty())
                        max_len += ImGui::CalcTextSize(total_str.c_str()).x;
                    else {
                        if (m_view_type == EViewType::FeatureType)
                            max_len += std::max(ImGui::CalcTextSize(cost_str.c_str()).x,
                                std::max(ImGui::CalcTextSize(print_str.c_str()).x,
                                    std::max(std::max(ImGui::CalcTextSize(prepare_str.c_str()).x, ImGui::CalcTextSize(total_str.c_str()).x),
                                        std::max(ImGui::CalcTextSize(total_filament_str.c_str()).x, ImGui::CalcTextSize(model_filament_str.c_str()).x))));
                        else
                            max_len += std::max(ImGui::CalcTextSize(print_str.c_str()).x,
                                (std::max(ImGui::CalcTextSize(prepare_str.c_str()).x, ImGui::CalcTextSize(total_str.c_str()).x)));
                    }
                    if (m_view_type == EViewType::FeatureType) {
                        //BBS display filament cost
                        ImGui::Dummy({ window_padding, window_padding });
                        ImGui::SameLine();
                        imgui.text(total_filament_str + ":");
                        ImGui::SameLine(max_len);
                        //BBS: use current plater's print statistics
                        bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
                        char buf[64];
                        ::sprintf(buf, imperial_units ? "%.2f in" : "%.2f m", ps.total_used_filament / koef);
                        imgui.text(buf);
                        ImGui::SameLine();
                        ::sprintf(buf, imperial_units ? "  %.2f oz" : "  %.2f g", ps.total_weight / unit_conver);
                        imgui.text(buf);
                        ImGui::Dummy({ window_padding, window_padding });
                        ImGui::SameLine();
                        imgui.text(model_filament_str + ":");
                        ImGui::SameLine(max_len);
                        auto exlude_m = total_support_used_filament_m + total_flushed_filament_m + total_wipe_tower_used_filament_m;
                        auto exlude_g = total_support_used_filament_g + total_flushed_filament_g + total_wipe_tower_used_filament_g;
                        ::sprintf(buf, imperial_units ? "%.2f in" : "%.2f m", ps.total_used_filament / koef - exlude_m);
                        imgui.text(buf);
                        ImGui::SameLine();
                        ::sprintf(buf, imperial_units ? "  %.2f oz" : "  %.2f g", (ps.total_weight - exlude_g) / unit_conver);
                        imgui.text(buf);
                        //BBS: display cost of filaments
                        ImGui::Dummy({ window_padding, window_padding });
                        ImGui::SameLine();
                        imgui.text(cost_str + ":");
                        ImGui::SameLine(max_len);
                        ::sprintf(buf, "%.2f", ps.total_cost);
                        imgui.text(buf);
                    }
                    auto role_time = [time_mode](ExtrusionRole role) {
                        auto it = std::find_if(time_mode.roles_times.begin(), time_mode.roles_times.end(), [role](const std::pair<ExtrusionRole, float>& item) { return role == item.first; });
                        return (it != time_mode.roles_times.end()) ? it->second : 0.0f;
                        };
                    //BBS: start gcode is mostly same with prepeare time
                    if (time_mode.prepare_time != 0.0f) {
                        ImGui::Dummy({ window_padding, window_padding });
                        ImGui::SameLine();
                        imgui.text(prepare_str + ":");
                        ImGui::SameLine(max_len);
                        if (timelapse_time != 0.0f)
                            imgui.text(short_time(get_time_dhms(time_mode.prepare_time)) + " + " + short_time(get_time_dhms(timelapse_time)));
                        else
                            imgui.text(short_time(get_time_dhms(time_mode.prepare_time)));
                    }
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(print_str + ":");
                    ImGui::SameLine(max_len);
                    imgui.text(short_time(get_time_dhms(time_mode.time - time_mode.prepare_time - timelapse_time)));
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    imgui.text(total_str + ":");
                    ImGui::SameLine(max_len);
                    imgui.text(short_time(get_time_dhms(time_mode.time)));
                    auto show_mode_button = [this, &imgui, can_show_mode_button](const wxString& label, PrintEstimatedStatistics::ETimeMode mode) {
                        if (can_show_mode_button(mode)) {
                            if (imgui.button(label)) {
                                m_time_estimate_mode = mode;
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
                                imgui.set_requires_extra_frame();
#else
                                wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
                                wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
                            }
                        }
                        };
                    switch (m_time_estimate_mode) {
                    case PrintEstimatedStatistics::ETimeMode::Normal: {
                        show_mode_button(_L("Switch to silent mode"), PrintEstimatedStatistics::ETimeMode::Stealth);
                        break;
                    }
                    case PrintEstimatedStatistics::ETimeMode::Stealth: {
                        show_mode_button(_L("Switch to normal mode"), PrintEstimatedStatistics::ETimeMode::Normal);
                        break;
                    }
                    default: { assert(false); break; }
                    }
                }
                if (m_view_type == EViewType::ColorPrint) {
                    ImGui::Spacing();
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::SameLine();
                    offsets = calculate_offsets({ { _u8L("Options"), { ""}}, { _u8L("Display"), {""}} }, icon_size);
                    append_headers({ {_u8L("Options"), offsets[0] }, { _u8L("Display"), offsets[1]} });
                    for (auto item : options_items)
                        append_option_item(item, offsets);
                }
                ImGui::Dummy({ window_padding, window_padding });
                if (m_nozzle_nums > 1)
                    render_legend_color_arr_recommen(window_padding);
                legend_height = ImGui::GetCurrentWindow()->Size.y;
                imgui.end();
                ImGui::PopStyleColor(7);
                ImGui::PopStyleVar(2);
            }

            void BaseRenderer::delete_wipe_tower()
            {
                size_t current_volumes_count = m_shells.volumes.volumes.size();
                if (current_volumes_count >= 1) {
                    for (size_t i = current_volumes_count - 1; i > 0; i--) {
                        GLVolume* v = m_shells.volumes.volumes[i];
                        if (v->is_wipe_tower) {
                            m_shells.volumes.release_volume(v);
                            delete v;
                            m_shells.volumes.volumes.erase(m_shells.volumes.volumes.begin() + i);
                            break;
                        }
                    }
                }
            }

            void BaseRenderer::render_legend_color_arr_recommen(float window_padding)
            {
                ImGuiWrapper& imgui = *wxGetApp().imgui();
                auto link_text = [&](const std::string& label) {
                    ImVec2 wiki_part_size = ImGui::CalcTextSize(label.c_str());
                    ImColor HyperColor = ImColor(0, 174, 66, 255).Value;
                    ImGui::PushStyleColor(ImGuiCol_Text, HyperColor.Value);
                    imgui.text(label.c_str());
                    ImGui::PopStyleColor();
                    // underline
                    ImVec2 lineEnd = ImGui::GetItemRectMax();
                    lineEnd.y -= 2.0f;
                    ImVec2 lineStart = lineEnd;
                    lineStart.x = ImGui::GetItemRectMin().x;
                    ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, HyperColor);
                    // click behavior
                    if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true)) {
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            Plater* plater = wxGetApp().plater();
                            wxCommandEvent evt(EVT_OPEN_FILAMENT_MAP_SETTINGS_DIALOG);
                            evt.SetEventObject(plater);
                            evt.SetInt(1); // 1 means from gcode viewer
                            wxPostEvent(plater, evt);
                        }
                    }
                    };
                auto link_text_set_to_optional = [&](const std::string& label) {
                    ImVec2 wiki_part_size = ImGui::CalcTextSize(label.c_str());
                    ImColor HyperColor = ImColor(0, 174, 66, 255).Value;
                    ImGui::PushStyleColor(ImGuiCol_Text, HyperColor.Value);
                    imgui.text(label.c_str());
                    ImGui::PopStyleColor();
                    // underline
                    ImVec2 lineEnd = ImGui::GetItemRectMax();
                    lineEnd.y -= 2.0f;
                    ImVec2 lineStart = lineEnd;
                    lineStart.x = ImGui::GetItemRectMin().x;
                    ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, HyperColor);
                    // click behavior
                    if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true)) {
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            MessageDialog msg_dlg(nullptr, _L("Automatically re-slice according to the optimal filament grouping, and the grouping results will be displayed after slicing."), wxEmptyString, wxOK | wxCANCEL);
                            if (msg_dlg.ShowModal() == wxID_OK) {
                                PartPlateList& partplate_list = wxGetApp().plater()->get_partplate_list();
                                PartPlate* plate = partplate_list.get_curr_plate();
                                plate->set_filament_map_mode(FilamentMapMode::fmmAutoForFlush);
                                Plater* plater = wxGetApp().plater();
                                wxPostEvent(plater, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
                            }
                        }
                    }
                    };
                auto link_filament_group_wiki = [&](const std::string& label) {
                    ImVec2 wiki_part_size = ImGui::CalcTextSize(label.c_str());
                    ImColor HyperColor = ImColor(0, 174, 66, 255).Value;
                    ImGui::PushStyleColor(ImGuiCol_Text, HyperColor.Value);
                    imgui.text(label.c_str());
                    ImGui::PopStyleColor();
                    // click behavior
                    if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true)) {
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            open_filament_group_wiki();
                        }
                    }
                    };
                auto draw_dash_line = [&](ImDrawList* draw_list, int dash_length = 5, int gap_length = 3) {
                    ImVec2 p1 = ImGui::GetCursorScreenPos();
                    ImVec2 p2 = ImVec2(p1.x + ImGui::GetContentRegionAvail().x, p1.y);
                    for (float i = p1.x; i < p2.x; i += (dash_length + gap_length)) {
                        draw_list->AddLine(ImVec2(i, p1.y), ImVec2(i + dash_length, p1.y), IM_COL32(206, 206, 206, 255));
                    }
                    };
                ////BBS Color Arrangement Recommendation
                auto config = wxGetApp().plater()->get_partplate_list().get_current_fff_print().config();
                auto stats_by_extruder = wxGetApp().plater()->get_partplate_list().get_current_fff_print().statistics_by_extruder();
                float delta_weight_to_single_ext = stats_by_extruder.stats_by_single_extruder.filament_flush_weight - stats_by_extruder.stats_by_multi_extruder_curr.filament_flush_weight;
                float delta_weight_to_best = stats_by_extruder.stats_by_multi_extruder_curr.filament_flush_weight - stats_by_extruder.stats_by_multi_extruder_best.filament_flush_weight;
                int   delta_change_to_single_ext = stats_by_extruder.stats_by_single_extruder.filament_change_count - stats_by_extruder.stats_by_multi_extruder_curr.filament_change_count;
                int   delta_change_to_best = stats_by_extruder.stats_by_multi_extruder_curr.filament_change_count - stats_by_extruder.stats_by_multi_extruder_best.filament_change_count;
                bool any_less_to_single_ext = delta_weight_to_single_ext > EPSILON || delta_change_to_single_ext > 0;
                bool any_more_to_best = delta_weight_to_best > EPSILON || delta_change_to_best > 0;
                bool all_less_to_single_ext = delta_weight_to_single_ext > EPSILON && delta_change_to_single_ext > 0;
                bool all_more_to_best = delta_weight_to_best > EPSILON && delta_change_to_best > 0;
                auto get_filament_display_type = [](const ExtruderFilament& filament) {
                    if (filament.is_support_filament && (filament.type == "PLA" || filament.type == "PA" || filament.type == "ABS"))
                        return "Sup." + filament.type;
                    return filament.type;
                    };
                // BBS AMS containers
                float line_height = ImGui::GetFrameHeight();
                float ams_item_height = 0;
                float filament_group_item_align_width = 0;
                {
                    float three_words_width = imgui.calc_text_size("ABC").x;
                    const int line_capacity = 4;
                    for (const auto& extruder_filaments : { m_left_extruder_filament,m_right_extruder_filament })
                    {
                        float container_height = 0.f;
                        for (size_t idx = 0; idx < extruder_filaments.size(); idx += line_capacity) {
                            float text_line_height = 0;
                            for (int j = idx; j < extruder_filaments.size() && j < idx + line_capacity; ++j) {
                                auto text_info = imgui.calculate_filament_group_text_size(get_filament_display_type(extruder_filaments[j]));
                                auto text_size = std::get<0>(text_info);
                                filament_group_item_align_width = max(filament_group_item_align_width, text_size.x);
                                text_line_height = max(text_line_height, text_size.y);
                            }
                            container_height += (three_words_width * 1.3f + text_line_height);
                        }
                        container_height += 2 * line_height;
                        ams_item_height = std::max(ams_item_height, container_height);
                    }
                }
                int tips_count = 8;
                if (any_more_to_best) {
                    tips_count = 8;
                    if (wxGetApp().app_config->get("language") != "zh_CN")
                        tips_count += 1;
                }
                else if (any_less_to_single_ext) {
                    tips_count = 6;
                    if (wxGetApp().app_config->get("language") != "zh_CN")
                        tips_count += 1;
                }
                else
                    tips_count = 5;
                float AMS_container_height = ams_item_height + line_height * tips_count + line_height / 2;
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.f, 1.f, 1.f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.15f, .18f, .19f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(window_padding * 3, 0));
                // ImGui::Dummy({window_padding, window_padding});
                ImGui::BeginChild("#AMS", ImVec2(0, AMS_container_height), true, ImGuiWindowFlags_AlwaysUseWindowPadding);
                {
                    float available_width = ImGui::GetContentRegionAvail().x;
                    float half_width = available_width * 0.49f;
                    float spacing = 18.0f * m_scale;
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(.8f, .8f, .8f, 1.0f));
                    imgui.bold_text(_u8L("Filament Grouping"));
                    ImGui::SameLine();
                    std::string tip_str = _u8L("Why this grouping");
                    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionWidth() - window_padding - ImGui::CalcTextSize(tip_str.c_str()).x);
                    link_filament_group_wiki(tip_str);
                    ImGui::Separator();
                    ImGui::PopStyleColor();
                    ImGui::Dummy({ window_padding, window_padding });
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.00f, 0.00f, 0.00f, 0.1f));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(window_padding * 2, window_padding));
                    ImDrawList* child_begin_draw_list = ImGui::GetWindowDrawList();
                    ImVec2      cursor_pos = ImGui::GetCursorScreenPos();
                    child_begin_draw_list->AddRectFilled(cursor_pos, ImVec2(cursor_pos.x + half_width, cursor_pos.y + line_height), IM_COL32(0, 0, 0, 20));
                    ImGui::BeginChild("#LeftAMS", ImVec2(half_width, ams_item_height), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
                    {
                        imgui.text(_u8L("Left nozzle"));
                        ImGui::Dummy({ window_padding, window_padding });
                        int index = 1;
                        for (const auto& extruder_filament : m_left_extruder_filament) {
                            imgui.filament_group(get_filament_display_type(extruder_filament), extruder_filament.hex_color.c_str(), extruder_filament.filament_id, filament_group_item_align_width);
                            if (index % 4 != 0) { ImGui::SameLine(0, spacing); }
                            index++;
                        }
                        ImGui::EndChild();
                    }
                    ImGui::SameLine();
                    cursor_pos = ImGui::GetCursorScreenPos();
                    child_begin_draw_list->AddRectFilled(cursor_pos, ImVec2(cursor_pos.x + half_width, cursor_pos.y + line_height), IM_COL32(0, 0, 0, 20));
                    ImGui::BeginChild("#RightAMS", ImVec2(half_width, ams_item_height), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
                    {
                        imgui.text(_u8L("Right nozzle"));
                        ImGui::Dummy({ window_padding, window_padding });
                        int index = 1;
                        for (const auto& extruder_filament : m_right_extruder_filament) {
                            imgui.filament_group(get_filament_display_type(extruder_filament), extruder_filament.hex_color.c_str(), extruder_filament.filament_id, filament_group_item_align_width);
                            if (index % 4 != 0) { ImGui::SameLine(0, spacing); }
                            index++;
                        }
                        ImGui::EndChild();
                    }
                    ImGui::PopStyleColor(1);
                    ImGui::PopStyleVar(1);
                    ImGui::Dummy({ window_padding, window_padding });
                    imgui.text_wrapped(from_u8(_u8L("Please place filaments on the printer based on grouping result.")), ImGui::GetContentRegionAvail().x);
                    ImGui::Dummy({ window_padding, window_padding });
                    {
                        ImDrawList* draw_list = ImGui::GetWindowDrawList();
                        draw_dash_line(draw_list);
                    }
                    ImGui::Dummy({ window_padding, window_padding });
                    bool is_optimal_group = true;
                    float parent_width = ImGui::GetContentRegionAvail().x;
                    auto number_format = [](float num) {
                        if (num > 1000) {
                            std::string number_str = std::to_string(num);
                            std::string first_three_digits = number_str.substr(0, 3);
                            return std::stoi(first_three_digits);
                        }
                        return static_cast<int>(num);
                        };
                    if (any_more_to_best) {
                        is_optimal_group = false;
                        ImVec4 orangeColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, orangeColor);
                        imgui.text(_u8L("Tips:"));
                        imgui.text(_u8L("Current grouping of slice result is not optimal."));
                        wxString tip;
                        if (delta_weight_to_best >= 0 && delta_change_to_best >= 0)
                            tip = from_u8((boost::format(_u8L("Increase %1%g filament and %2% changes compared to optimal grouping."))
                                % number_format(delta_weight_to_best)
                                % delta_change_to_best).str());
                        else if (delta_weight_to_best >= 0 && delta_change_to_best < 0)
                            tip = from_u8((boost::format(_u8L("Increase %1%g filament and save %2% changes compared to optimal grouping."))
                                % number_format(delta_weight_to_best)
                                % std::abs(delta_change_to_best)).str());
                        else if (delta_weight_to_best < 0 && delta_change_to_best >= 0)
                            tip = from_u8((boost::format(_u8L("Save %1%g filament and increase %2% changes compared to optimal grouping."))
                                % number_format(std::abs(delta_weight_to_best))
                                % delta_change_to_best).str());
                        imgui.text_wrapped(tip, parent_width);
                        ImGui::PopStyleColor(1);
                    }
                    else if (any_less_to_single_ext) {
                        wxString tip;
                        if (delta_weight_to_single_ext >= 0 && delta_change_to_single_ext >= 0)
                            tip = from_u8((boost::format(_u8L("Save %1%g filament and %2% changes compared to a printer with one nozzle."))
                                % number_format(delta_weight_to_single_ext)
                                % delta_change_to_single_ext).str());
                        else if (delta_weight_to_single_ext >= 0 && delta_change_to_single_ext < 0)
                            tip = from_u8((boost::format(_u8L("Save %1%g filament and increase %2% changes compared to a printer with one nozzle."))
                                % number_format(delta_weight_to_single_ext)
                                % std::abs(delta_change_to_single_ext)).str());
                        else if (delta_weight_to_single_ext < 0 && delta_change_to_single_ext >= 0)
                            tip = from_u8((boost::format(_u8L("Increase %1%g filament and save %2% changes compared to a printer with one nozzle."))
                                % number_format(std::abs(delta_weight_to_single_ext))
                                % delta_change_to_single_ext).str());
                        imgui.text_wrapped(tip, parent_width);
                    }
                    ImGui::Dummy({ window_padding, window_padding });
                    if (!is_optimal_group) {
                        link_text_set_to_optional(_u8L("Set to Optimal"));
                        ImGui::SameLine();
                        ImGui::Dummy({ window_padding, window_padding });
                        ImGui::SameLine();
                    }
                    link_text(_u8L("Regroup filament"));
                    ImGui::EndChild();
                }
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(1);
            }

            void BaseRenderer::update_moves_slider(bool set_to_max)
            {
                const auto& p_sequential_view = get_sequential_view();
                if (!p_sequential_view) {
                    return;
                }
                // this should not be needed, but it is here to try to prevent rambling crashes on Mac Asan
                if (p_sequential_view->endpoints.last < p_sequential_view->endpoints.first) return;
                std::vector<double> values(p_sequential_view->endpoints.last - p_sequential_view->endpoints.first + 1);
                unsigned int        count = 0;
                for (unsigned int i = p_sequential_view->endpoints.first; i <= p_sequential_view->endpoints.last; ++i) {
                    values[count] = static_cast<double>(i + 1);
                    ++count;
                }
                m_moves_slider->SetSliderValues(values);
                m_moves_slider->SetMaxValue(p_sequential_view->endpoints.last - p_sequential_view->endpoints.first);
                m_moves_slider->SetSelectionSpan(p_sequential_view->current.first - p_sequential_view->endpoints.first, p_sequential_view->current.last - p_sequential_view->endpoints.first);
                if (set_to_max)
                    m_moves_slider->SetHigherValue(m_moves_slider->GetMaxValue());
            }

            bool BaseRenderer::show_sequential_view() const
            {
                return false;
            }

            void BaseRenderer::on_visibility_changed()
            {
                wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
            }

            void BaseRenderer::do_set_view_type(EViewType type)
            {
                m_view_type = type;
            }

            // helio
            void BaseRenderer::update_option_item_when_load_gcode()
            {
                auto curr_plate_index = wxGetApp().plater()->get_partplate_list().get_curr_plate_index();
                if (curr_plate_has_ok_helio_slice(curr_plate_index)) {
                    update_thermal_options(true);
                }
                else {
                    update_thermal_options(false);
                    update_default_view_type();
                }
            }

            void BaseRenderer::update_default_view_type()
            {
                if (view_type_items.empty()) { return; }
                if (m_last_non_helio_option_item >= (int)view_type_items.size()) { return; }
                EViewType cur_type = m_nozzle_nums > 1 ? EViewType::Summary : EViewType::ColorPrint;
                if (m_last_non_helio_option_item < 0) {//not set
                    m_view_type_sel = std::distance(view_type_items.begin(), std::find(view_type_items.begin(), view_type_items.end(), cur_type));
                    m_last_non_helio_option_item = m_view_type_sel;
                    record_gcodeviewer_option_item();
                }
                else {
                    m_view_type_sel = m_last_non_helio_option_item;
                    cur_type = view_type_items[m_view_type_sel];
                }
                set_view_type(cur_type);
            }

            void BaseRenderer::record_gcodeviewer_option_item() {
                if (GUI::wxGetApp().app_config->get_bool("enable_record_gcodeviewer_option_item")) {
                    wxGetApp().app_config->set("gcodeviewer_option_item", std::to_string(m_last_non_helio_option_item));
                    wxGetApp().app_config->save();
                }
            }

            bool BaseRenderer::get_min_max_value_of_option(int index, float& _min, float& _max) const
            {
                switch ((EViewType)index) {
                case EViewType::Summary: break;
                case EViewType::FeatureType: break;
                case EViewType::Height: {
                    _min = m_p_extrusions->ranges.height.min;
                    _max = m_p_extrusions->ranges.height.max;
                    return true;
                }
                case EViewType::Width: {
                    _min = m_p_extrusions->ranges.width.min;
                    _max = m_p_extrusions->ranges.width.max;
                    return true;
                }
                case EViewType::Feedrate: {
                    _min = m_p_extrusions->ranges.feedrate.min;
                    _max = m_p_extrusions->ranges.feedrate.max;
                    return true;
                }
                case EViewType::FanSpeed: {
                    _min = m_p_extrusions->ranges.fan_speed.min;
                    _max = m_p_extrusions->ranges.fan_speed.max;
                    return true;
                }
                case EViewType::Temperature: {
                    _min = m_p_extrusions->ranges.temperature.min;
                    _max = m_p_extrusions->ranges.temperature.max;
                    return true;
                }
                case EViewType::ThermalIndexMin: {
                    _min = m_p_extrusions->ranges.thermal_index_min.min;
                    _max = m_p_extrusions->ranges.thermal_index_min.max;
                    return true;
                }
                case EViewType::ThermalIndexMax: {
                    _min = m_p_extrusions->ranges.thermal_index_max.min;
                    _max = m_p_extrusions->ranges.thermal_index_max.max;
                    return true;
                }
                case EViewType::ThermalIndexMean: {
                    _min = m_p_extrusions->ranges.thermal_index_mean.min;
                    _max = m_p_extrusions->ranges.thermal_index_mean.max;
                    return true;
                }
                case EViewType::VolumetricRate: {
                    _min = m_p_extrusions->ranges.volumetric_rate.min;
                    _max = m_p_extrusions->ranges.volumetric_rate.max;
                    return true;
                }
                case EViewType::Tool: break;
                case EViewType::ColorPrint: break;
                case EViewType::FilamentId: break;
                case EViewType::LayerTime: {
                    _min = m_p_extrusions->ranges.layer_duration.min;
                    _max = m_p_extrusions->ranges.layer_duration.max;
                    return true;
                }
                case EViewType::Count:
                default: break;
                }
                return false;
            }

            void BaseRenderer::reset_curr_plate_thermal_options(int plate_idx)
            {
                update_thermal_options(false);
                update_default_view_type();
                m_helio_slice_map_oks[plate_idx] = false;
            }

            void  BaseRenderer::init_thermal_icons() {
                if (!m_helio_icon_dark_texture) {
                    ImVec2 icon_size(16, 16);
                    IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/helio_icon_dark.svg", icon_size.x, icon_size.y, m_helio_icon_dark_texture);
                    IMTexture::load_from_svg_file(Slic3r::resources_dir() + "/images/helio_icon.svg", icon_size.x, icon_size.y, m_helio_icon_texture);
                }
            }
            // end helio

            void BaseRenderer::push_combo_style()
            {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0, 8.0));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.0f, 0.0f, 0.0f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(0.00f, 0.68f, 0.26f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.00f, 0.68f, 0.26f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.00f, 0.68f, 0.26f, 1.0f));
            }
            void BaseRenderer::pop_combo_style()
            {
                ImGui::PopStyleVar(3);
                ImGui::PopStyleColor(8);
            }

            void BaseRenderer::update_by_mode(ConfigOptionMode mode)
            {
                view_type_items.clear();
                view_type_image_names.clear();
                options_items.clear();
                // BBS initialzed view_type items
                view_type_items.push_back(EViewType::Summary);
                view_type_items.push_back(EViewType::FeatureType);
                view_type_items.push_back(EViewType::ColorPrint);
                view_type_items.push_back(EViewType::Feedrate);
                view_type_items.push_back(EViewType::Height);
                view_type_items.push_back(EViewType::Width);
                view_type_items.push_back(EViewType::VolumetricRate);
                view_type_items.push_back(EViewType::LayerTime);
                view_type_items.push_back(EViewType::FanSpeed);
                view_type_items.push_back(EViewType::Temperature);
                for (int i = 0; i < view_type_items.size(); i++) {
                    if (view_type_items[i] == EViewType::FilamentId) {
                        continue;
                    }
                    ImageName temp = { get_view_type_string(view_type_items[i]), nullptr, nullptr };
                    view_type_image_names.push_back(temp);
                }
                view_type_items.push_back(EViewType::FilamentId); // BBS for first layer inspection
                options_items.push_back(EMoveType::Travel);
                options_items.push_back(EMoveType::Retract);
                options_items.push_back(EMoveType::Unretract);
                options_items.push_back(EMoveType::Wipe);
                //if (mode == ConfigOptionMode::comDevelop) {
                //    options_items.push_back(EMoveType::Tool_change);
                //}
                //BBS: seam is not real move and extrusion, put at last line
                options_items.push_back(EMoveType::Seam);
            }

            void BaseRenderer::update_thermal_options(bool add) {

                if (add) {
                    for (int i = view_type_items.size() - 1; i >= 0; i--) {
                        if (view_type_items[i] == EViewType::ThermalIndexMean) {
                            return;
                        }
                    }
                    view_type_items.pop_back();//delete EViewType::FilamentId
                    auto index = view_type_items.size();

                    view_type_items.push_back(EViewType::ThermalIndexMin);
                    view_type_items.push_back(EViewType::ThermalIndexMax);
                    view_type_items.push_back(EViewType::ThermalIndexMean);
                    for (int i = index; i < view_type_items.size(); i++) {
                        ImageName temp = { get_view_type_string(view_type_items[i]), m_helio_icon_texture, m_helio_icon_dark_texture };
                        view_type_image_names.push_back(temp);
                    }
                    view_type_items.push_back(EViewType::FilamentId);
                }
                else {
                    for (int i = view_type_items.size() - 1; i >= 0; i--) {
                        if (view_type_items[i] == EViewType::ThermalIndexMean || view_type_items[i] == EViewType::ThermalIndexMin || view_type_items[i] == EViewType::ThermalIndexMax) {
                            view_type_items.erase(view_type_items.begin() + i);
                            view_type_image_names.erase(view_type_image_names.begin() + i);
                        }
                    }
                }
            }

            GCodeWindow::GCodeWindow()
            {

            }

            GCodeWindow::~GCodeWindow()
            {
                stop_mapping_file();
            }

            void GCodeWindow::reset() {
                stop_mapping_file();
                m_lines_ends.clear();
                m_lines_ends.shrink_to_fit();
                m_lines.clear();
                m_lines.shrink_to_fit();
                m_filename.clear();
                m_filename.shrink_to_fit();
            }

            void GCodeWindow::toggle_visibility()
            {
                m_visible = !m_visible;
            }

            void GCodeWindow::load_gcode(const std::string& filename, const std::vector<size_t>& lines_ends)
            {
                assert(!m_file.is_open());
                if (m_file.is_open())
                    return;
                m_filename = filename;
                m_lines_ends = lines_ends;
                m_selected_line_id = 0;
                m_last_lines_size = 0;
                try
                {
                    m_file.open(boost::filesystem::path(m_filename));
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": mapping file " << PathSanitizer::sanitize(m_filename);
                }
                catch (...)
                {
                    BOOST_LOG_TRIVIAL(error) << "Unable to map file " << PathSanitizer::sanitize(m_filename) << ". Cannot show G-code window.";
                    reset();
                }
            }

            // helio
            void GCodeWindow::render_thermal_index_windows(
                std::vector<GCodeProcessor::ThermalIndex> thermal_indexes, float top, float right, float wnd_height, float f_lines_count, uint64_t start_id, uint64_t end_id) const
            {
                const float         text_height = ImGui::CalcTextSize("0").y;
                static const ImVec4 LINE_NUMBER_COLOR = ImGuiWrapper::COL_ORANGE_LIGHT;

                float previousWindowWidth = right;

                auto place_window = [text_height, thermal_indexes, top, wnd_height, f_lines_count, start_id, end_id](std::string heading, size_t index_id, float right) {
                    ImGuiWrapper& imgui = *wxGetApp().imgui();
                    const ImGuiStyle& style = ImGui::GetStyle();
                    imgui.set_next_window_pos(right - 0.4f, top, ImGuiCond_Always, 1.0f, 0.0f);
                    imgui.set_next_window_size(0.0f, wnd_height, ImGuiCond_Always);
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
                    ImGui::SetNextWindowBgAlpha(0.8f);
                    imgui.begin(std::string("Thermal-Index-" + heading), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);

                    ImDrawList* draw_list = ImGui::GetWindowDrawList();
                    ImVec2      pos_rect = ImGui::GetCursorScreenPos();
                    ImVec2      windowPadding = ImGui::GetStyle().WindowPadding;
                    ImVec2      framePadding = ImGui::GetStyle().FramePadding;

                    float textHeight = ImGui::GetTextLineHeight() + 4.0f;

                    ImVec2 rectMin = ImVec2(pos_rect.x - windowPadding.x - framePadding.x, pos_rect.y - windowPadding.y - framePadding.y);

                    ImVec2 rectMax = ImVec2(pos_rect.x + ImGui::GetContentRegionAvail().x, pos_rect.y + textHeight);

                    draw_list->AddRectFilled(rectMin, rectMax, ImGui::GetColorU32(ImVec4(0, 0, 0, 0.3)));
                    ImGui::SetCursorPosY(0.5f * (wnd_height - f_lines_count * text_height - (f_lines_count - 1.0f) * style.ItemSpacing.y));

                    const float item_size = imgui.calc_text_size_new(std::string_view{ "X: 000.000  " }).x;
                    const float item_spacing = imgui.get_item_spacing().x;

                    ImGui::SameLine(0.0f, 0.0f);

                    // render text lines
                    imgui.bold_text(" " + heading);

                    char buf[1024];
                    for (uint64_t id = start_id; id <= end_id; ++id) {
                        auto thermal_index = thermal_indexes[id - start_id];

                        ImGui::PushStyleColor(ImGuiCol_Text, LINE_NUMBER_COLOR);

                        float ti_value;

                        switch (index_id) {
                        case 0: ti_value = thermal_index.min; break;
                        case 1: ti_value = thermal_index.max; break;
                        case 2: ti_value = thermal_index.mean; break;
                        };

                        if (thermal_index.isNull)
                            sprintf(buf, "%s  ", "  null  ");
                        else
                            sprintf(buf, "%8.2f  ", ti_value);

                        imgui.text(buf);
                        ImGui::PopStyleColor();
                    }

                    float previousWindowWidth = ImGui::GetCurrentWindow()->Pos.x;
                    imgui.end();
                    ImGui::PopStyleVar();

                    return previousWindowWidth;
                    };

                previousWindowWidth = place_window("Mean", 2, previousWindowWidth);
                previousWindowWidth = place_window("Max", 1, previousWindowWidth);
                previousWindowWidth = place_window("Min", 0, previousWindowWidth);
            }
            // end helio

            //BBS: GUI refactor: move to right
            void GCodeWindow::render(float top, float bottom, float right, uint64_t curr_line_id, const OnAttachingHelio& cb, bool b_show_horizon_slider, bool b_helio_option) const
                //void LegacyRenderer::SequentialView::GCodeWindow::render(float top, float bottom, uint64_t curr_line_id) const
            {
                auto update_lines = [this, &cb](uint64_t start_id, uint64_t end_id) {
                    std::vector<Line> ret;
                    ret.reserve(end_id - start_id + 1);
                    size_t length_of_line = 40;
                    for (uint64_t id = start_id; id <= end_id; ++id) {
                        // read line from file
                        const size_t start = id == 1 ? 0 : m_lines_ends[id - 2];
                        const size_t original_len = m_lines_ends[id - 1] - start;
                        const size_t len = std::min(original_len, (size_t)length_of_line);
                        std::string gline(m_file.data() + start, len);
                        if (boost::contains(gline, "helio")) {
                            if (cb)
                            {
                                cb(length_of_line);
                            }
                        }
                        std::string command;
                        std::string parameters;
                        std::string comment;

                        // If original line is longer than 55 characters, truncate and append "..."
                        if (original_len > length_of_line)
                            gline = gline.substr(0, length_of_line - 3) + "...";

                        // extract comment
                        std::vector<std::string> tokens;
                        boost::split(tokens, gline, boost::is_any_of(";"), boost::token_compress_on);
                        command = tokens.front();
                        if (tokens.size() > 1) {
                            comment = ";" + tokens.back();
                        }

                        // extract gcode command and parameters
                        if (!command.empty()) {
                            boost::split(tokens, command, boost::is_any_of(" "), boost::token_compress_on);
                            command = tokens.front();
                            if (tokens.size() > 1) {
                                for (size_t i = 1; i < tokens.size(); ++i) {
                                    parameters += " " + tokens[i];
                                }
                            }
                        }

                        boost::trim(command);
                        boost::trim(parameters);
                        boost::trim(comment);
                        ret.push_back({ command, parameters, comment });
                    }
                    return ret;
                };
                static const ImVec4 LINE_NUMBER_COLOR = { 0, 174.0f / 255.0f, 66.0f / 255.0f, 1.0f };
                static const ImVec4 SELECTION_RECT_COLOR = { 0, 174.0f / 255.0f, 66.0f / 255.0f, 1.0f };
                static const ImVec4 COMMAND_COLOR = m_is_dark ? ImVec4(240.0f / 255.0f, 240.0f / 255.0f, 240.0f / 255.0f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                static const ImVec4 PARAMETERS_COLOR = m_is_dark ? ImVec4(179.0f / 255.0f, 179.0f / 255.0f, 179.0f / 255.0f, 1.0f) : ImVec4(206.0f / 255.0f, 206.0f / 255.0f, 206.0f / 255.0f, 1.0f);
                static const ImVec4 COMMENT_COLOR = m_is_dark ? ImVec4(129.0f / 255.0f, 129.0f / 255.0f, 129.0f / 255.0f, 1.0f) : ImVec4(172.0f / 255.0f, 172.0f / 255.0f, 172.0f / 255.0f, 1.0f);
                if (!m_visible || m_filename.empty() || m_lines_ends.empty() || curr_line_id == 0)
                    return;
                // window height
                const float wnd_height = bottom - top;
                // number of visible lines
                const float text_height = ImGui::CalcTextSize("0").y;
                const ImGuiStyle& style = ImGui::GetStyle();
                const uint64_t lines_count = static_cast<uint64_t>((wnd_height - 2.0f * style.WindowPadding.y + style.ItemSpacing.y) / (text_height + style.ItemSpacing.y));
                if (lines_count == 0)
                    return;
                // visible range
                const uint64_t half_lines_count = lines_count / 2;
                uint64_t start_id = (curr_line_id >= half_lines_count) ? curr_line_id - half_lines_count : 0;
                uint64_t end_id = start_id + lines_count - 1;
                if (end_id >= static_cast<uint64_t>(m_lines_ends.size())) {
                    end_id = static_cast<uint64_t>(m_lines_ends.size()) - 1;
                    start_id = end_id - lines_count + 1;
                }
                // updates list of lines to show, if needed
                if (m_selected_line_id != curr_line_id || m_last_lines_size != end_id - start_id + 1) {
                    try
                    {
                        *const_cast<std::vector<Line>*>(&m_lines) = update_lines(start_id, end_id);
                    }
                    catch (...)
                    {
                        BOOST_LOG_TRIVIAL(error) << "Error while loading from file " << PathSanitizer::sanitize(m_filename) << ". Cannot show G-code window.";
                        return;
                    }
                    *const_cast<uint64_t*>(&m_selected_line_id) = curr_line_id;
                    *const_cast<size_t*>(&m_last_lines_size) = m_lines.size();
                }
                // line number's column width
                const float id_width = ImGui::CalcTextSize(std::to_string(end_id).c_str()).x;
                ImGuiWrapper& imgui = *wxGetApp().imgui();
                //BBS: GUI refactor: move to right
                //imgui.set_next_window_pos(0.0f, top, ImGuiCond_Always, 0.0f, 0.0f);
                imgui.set_next_window_pos(right, top, ImGuiCond_Always, 1.0f, 0.0f);
                if (b_show_horizon_slider) {
                    auto imgui_window_width = ImGui::CalcTextSize("10000 G1 X191.55 Y166.478 E.07946; helio").x;
                    imgui.set_next_window_size(imgui_window_width, wnd_height, ImGuiCond_Always);
                }
                else {
                    imgui.set_next_window_size(0.0f, wnd_height, ImGuiCond_Always);
                }
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
                ImGui::SetNextWindowBgAlpha(0.8f);
                if (b_show_horizon_slider) {
                    imgui.begin(std::string("G-code"), ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysHorizontalScrollbar);
                }
                else {
                    imgui.begin(std::string("G-code"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
                }
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                ImVec2      pos_rect = ImGui::GetCursorScreenPos();
                ImVec2      windowPadding = ImGui::GetStyle().WindowPadding;
                ImVec2      framePadding = ImGui::GetStyle().FramePadding;

                float textHeight = ImGui::GetTextLineHeight() + 4.0f;

                ImVec2 rectMin = ImVec2(pos_rect.x - windowPadding.x - framePadding.x, pos_rect.y - windowPadding.y - framePadding.y);

                ImVec2 rectMax = ImVec2(pos_rect.x + ImGui::GetContentRegionAvail().x, pos_rect.y + textHeight);

                draw_list->AddRectFilled(rectMin, rectMax, ImGui::GetColorU32(ImVec4(0, 0, 0, 0.3)));

                // center the text in the window by pushing down the first line
                const float f_lines_count = static_cast<float>(lines_count);
                ImGui::SetCursorPosY(0.5f * (wnd_height - f_lines_count * text_height - (f_lines_count - 1.0f) * style.ItemSpacing.y));

                const float window_padding = ImGui::GetStyle().WindowPadding.x;

                ImGui::SameLine(0.0f, 0.0f);

                // render text lines
                imgui.text("GCode");
                for (uint64_t id = start_id; id <= end_id; ++id) {
                    const Line& line = m_lines[id - start_id];
                    // rect around the current selected line
                    if (id == curr_line_id) {
                        //BBS: GUI refactor: move to right
                        const float pos_y = ImGui::GetCursorScreenPos().y;
                        const float pos_x = ImGui::GetCursorScreenPos().x;
                        const float half_ItemSpacing_y = 0.5f * style.ItemSpacing.y;
                        const float half_ItemSpacing_x = 0.5f * style.ItemSpacing.x;
                        //ImGui::GetWindowDrawList()->AddRect({ half_padding_x, pos_y - half_ItemSpacing_y },
                        //    { ImGui::GetCurrentWindow()->Size.x - half_padding_x, pos_y + text_height + half_ItemSpacing_y },
                        //    ImGui::GetColorU32(SELECTION_RECT_COLOR));
                        ImGui::GetWindowDrawList()->AddRect({ pos_x - half_ItemSpacing_x, pos_y - half_ItemSpacing_y },
                            { right - half_ItemSpacing_x, pos_y + text_height + half_ItemSpacing_y },
                            ImGui::GetColorU32(SELECTION_RECT_COLOR));
                    }
                    // render line number
                    const std::string id_str = std::to_string(id);
                    // spacer to right align text
                    ImGui::Dummy({ id_width - ImGui::CalcTextSize(id_str.c_str()).x, text_height });
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, LINE_NUMBER_COLOR);
                    imgui.text(id_str);
                    ImGui::PopStyleColor();
                    if (!line.command.empty() || !line.comment.empty())
                        ImGui::SameLine();
                    // render command
                    if (!line.command.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, COMMAND_COLOR);
                        imgui.text(line.command);
                        ImGui::PopStyleColor();
                    }
                    // render parameters
                    if (!line.parameters.empty()) {
                        ImGui::SameLine(0.0f, 0.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, PARAMETERS_COLOR);
                        imgui.text(line.parameters);
                        ImGui::PopStyleColor();
                    }
                    // render comment
                    if (!line.comment.empty()) {
                        if (!line.command.empty())
                            ImGui::SameLine(0.0f, 0.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, COMMENT_COLOR);
                        imgui.text(line.comment);
                        ImGui::PopStyleColor();
                    }
                }

                // helio
                float gcodeWindowWidth = ImGui::GetCurrentWindow()->Pos.x;
                // end helio

                imgui.end();
                ImGui::PopStyleVar();

                // helio
                if (b_helio_option) {
                    auto get_thermal_index = [this](uint64_t start_id, uint64_t end_id) {
                        std::vector<GCodeProcessor::ThermalIndex> ret;
                        ret.reserve(end_id - start_id + 1);
                        for (uint64_t id = start_id; id <= end_id; ++id) {
                            // read line from file
                            const size_t start = id == 1 ? 0 : m_lines_ends[id - 2];
                            const size_t len = m_lines_ends[id - 1] - start;
                            std::string  gline(m_file.data() + start, len);

                            std::string command, comment;
                            // extract comment
                            std::vector<std::string> tokens;
                            boost::split(tokens, gline, boost::is_any_of(";"), boost::token_compress_on);
                            command = tokens.front();
                            if (tokens.size() > 1) comment = ";" + tokens.back();
                            bool is_helio_gcode{ false };
                            ret.push_back(GCodeProcessor::parse_helioadditive_comment(comment, is_helio_gcode));
                        }
                        return ret;
                        };

                    std::vector<GCodeProcessor::ThermalIndex> thermal_indexes = get_thermal_index(start_id, end_id);

                    render_thermal_index_windows(thermal_indexes, top, gcodeWindowWidth, wnd_height, f_lines_count, start_id, end_id);
                }
                // end helio
            }

            void GCodeWindow::stop_mapping_file()
            {
                //BBS: add log to trace the gcode file issue
                if (m_file.is_open()) {
                    m_file.close();
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": finished mapping file " << PathSanitizer::sanitize(m_filename);
                }
            }

            void SequentialView::Marker::init(std::string filename)
            {
                if (filename.empty()) {
                    m_model.init_from(stilized_arrow(16, 1.5f, 3.0f, 0.8f, 3.0f));
                }
                else {
                    m_model.init_from_file(filename);
                }
                m_model.set_color(-1, { 1.0f, 1.0f, 1.0f, 0.5f });
            }
            void SequentialView::Marker::set_world_position(const Vec3f& position)
            {
                m_world_position = position;
                m_world_transform = (Geometry::assemble_transform((position + m_z_offset * Vec3f::UnitZ()).cast<double>()) * Geometry::assemble_transform(m_model.get_bounding_box().size().z() * Vec3d::UnitZ(), { M_PI, 0.0, 0.0 })).cast<float>();
            }
            void SequentialView::Marker::update_curr_move(const GCodeProcessorResult::MoveVertex move) {
                m_curr_move = move;
            }
            //BBS: GUI refactor: add canvas size from parameters
            void SequentialView::Marker::render(int canvas_width, int canvas_height, const EViewType& view_type) const
            {
                if (!m_visible)
                    return;
                const auto& shader = wxGetApp().get_shader("gouraud_light");
                if (shader == nullptr)
                    return;
                glsafe(::glEnable(GL_BLEND));
                glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
                wxGetApp().bind_shader(shader);
                shader->set_uniform("emission_factor", 0.0f);
                const Camera& camera = wxGetApp().plater()->get_camera();
                const Transform3d matrix = camera.get_view_matrix() * m_world_transform.cast<double>();
                shader->set_uniform("view_model_matrix", matrix);
                shader->set_uniform("projection_matrix", camera.get_projection_matrix());
                shader->set_uniform("normal_matrix", (Matrix3d)matrix.matrix().block(0, 0, 3, 3).inverse().transpose());
                m_model.render_geometry();
                wxGetApp().unbind_shader();
                glsafe(::glDisable(GL_BLEND));
                static float last_window_width = 0.0f;
                size_t text_line = 0;
                static size_t last_text_line = 0;
                const ImU32 text_name_clr = m_is_dark ? IM_COL32(255, 255, 255, 0.88 * 255) : IM_COL32(38, 46, 48, 255);
                const ImU32 text_value_clr = m_is_dark ? IM_COL32(255, 255, 255, 0.4 * 255) : IM_COL32(144, 144, 144, 255);
                ImGuiWrapper& imgui = *wxGetApp().imgui();
                //BBS: GUI refactor: add canvas size from parameters
                imgui.set_next_window_pos(0.5f * static_cast<float>(canvas_width), static_cast<float>(canvas_height), ImGuiCond_Always, 0.5f, 1.0f);
                imgui.push_toolbar_style(m_scale);
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0, 4.0 * m_scale));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0 * m_scale, 6.0 * m_scale));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, text_name_clr);
                ImGui::PushStyleColor(ImGuiCol_Text, text_value_clr);
                imgui.begin(std::string("ExtruderPosition"), ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
                ImGui::AlignTextToFramePadding();
                //BBS: minus the plate offset when show tool position
                PartPlateList& partplate_list = wxGetApp().plater()->get_partplate_list();
                PartPlate* plate = partplate_list.get_curr_plate();
                const Vec3f position = m_world_position + m_world_offset;
                std::string x = ImGui::ColorMarkerStart + std::string("X: ") + ImGui::ColorMarkerEnd;
                std::string y = ImGui::ColorMarkerStart + std::string("Y: ") + ImGui::ColorMarkerEnd;
                std::string z = ImGui::ColorMarkerStart + std::string("Z: ") + ImGui::ColorMarkerEnd;
                std::string height = ImGui::ColorMarkerStart + _u8L("Height: ") + ImGui::ColorMarkerEnd;
                std::string width = ImGui::ColorMarkerStart + _u8L("Width: ") + ImGui::ColorMarkerEnd;
                std::string speed = ImGui::ColorMarkerStart + _u8L("Speed: ") + ImGui::ColorMarkerEnd;
                std::string flow = ImGui::ColorMarkerStart + _u8L("Flow: ") + ImGui::ColorMarkerEnd;
                std::string layer_time = ImGui::ColorMarkerStart + _u8L("Layer Time: ") + ImGui::ColorMarkerEnd;
                std::string fanspeed = ImGui::ColorMarkerStart + _u8L("Fan Speed: ") + ImGui::ColorMarkerEnd;
                std::string temperature = ImGui::ColorMarkerStart + _u8L("Temperature: ") + ImGui::ColorMarkerEnd;
                std::string    thermal_index = ImGui::ColorMarkerStart + _u8L("Thermal Index") + ImGui::ColorMarkerEnd;
                // helio
                std::string    min = ImGui::ColorMarkerStart + _u8L("Min: ") + ImGui::ColorMarkerEnd;
                std::string    max = ImGui::ColorMarkerStart + _u8L("Max: ") + ImGui::ColorMarkerEnd;
                std::string    mean = ImGui::ColorMarkerStart + _u8L("Mean: ") + ImGui::ColorMarkerEnd;
                // end helio
                const float item_size = imgui.calc_text_size("X: 000.000  ").x;
                const float item_spacing = imgui.get_item_spacing().x;
                const float window_padding = ImGui::GetStyle().WindowPadding.x;
                char buf[1024];
                // extra text depends on whether current move is extrude type
                bool show_extra_text = m_curr_move.type == EMoveType::Extrude;
                // FeatureType and ColorPrint shall only show x,y,z
                if (view_type == EViewType::FeatureType || view_type == EViewType::ColorPrint)
                    show_extra_text = false;
                // Feedrate and LayerTime shall always show extra text
                else if (view_type == EViewType::Feedrate || view_type == EViewType::LayerTime)
                    show_extra_text = true;
                if (show_extra_text)
                {
                    // helio
                    float startx2 = window_padding + item_size + item_spacing;
                    float startx3 = window_padding + 2 * (item_size + item_spacing);
                    // end helio
                    sprintf(buf, "%s%.3f", x.c_str(), position.x() - plate->get_origin().x());
                    ImGui::PushItemWidth(item_size);
                    imgui.text(buf);
                    ImGui::SameLine(window_padding + item_size + item_spacing);
                    sprintf(buf, "%s%.3f", y.c_str(), position.y() - plate->get_origin().y());
                    ImGui::PushItemWidth(item_size);
                    imgui.text(buf);
                    sprintf(buf, "%s%.3f", z.c_str(), position.z());
                    ImGui::PushItemWidth(item_size);
                    imgui.text(buf);
                    // helio
                    if (view_type == EViewType::ThermalIndexMin || view_type == EViewType::ThermalIndexMax || view_type == EViewType::ThermalIndexMean) {
                        sprintf(buf, "%s", thermal_index.c_str());
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                    }
                    // end helio
                    switch (view_type) {
                    case EViewType::Height: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.2f", height.c_str(), m_curr_move.height);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    case EViewType::Width: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.2f", width.c_str(), m_curr_move.width);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    case EViewType::Feedrate: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.0f", speed.c_str(), m_curr_move.feedrate);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    case EViewType::VolumetricRate: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.2f", flow.c_str(), m_curr_move.volumetric_rate());
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    case EViewType::FanSpeed: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.0f", fanspeed.c_str(), m_curr_move.fan_speed);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    case EViewType::Temperature: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.0f", temperature.c_str(), m_curr_move.temperature);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    case EViewType::LayerTime: {
                        ImGui::SameLine(window_padding + item_size + item_spacing);
                        sprintf(buf, "%s%.1f", layer_time.c_str(), m_curr_move.layer_duration);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    // helio
                    case EViewType::ThermalIndexMin:
                    case EViewType::ThermalIndexMax:
                    case EViewType::ThermalIndexMean:
                    {
                        if (m_curr_move.thermal_index_min < -100)
                            sprintf(buf, "%snull", min.c_str());
                        else
                            sprintf(buf, "%s%.1f", min.c_str(), m_curr_move.thermal_index_min);

                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);

                        ImGui::SameLine(startx2);

                        if (m_curr_move.thermal_index_max < -100)
                            sprintf(buf, "%snull", max.c_str());
                        else
                            sprintf(buf, "%s%.1f", max.c_str(), m_curr_move.thermal_index_max);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);

                        ImGui::SameLine(startx3);

                        if (m_curr_move.thermal_index_mean < -100)
                            sprintf(buf, "%snull", mean.c_str());
                        else
                            sprintf(buf, "%s%.1f", mean.c_str(), m_curr_move.thermal_index_mean);
                        ImGui::PushItemWidth(item_size);
                        imgui.text(buf);
                        break;
                    }
                    // end helio
                    default:
                        break;
                    }
                    text_line = 2;
                }
                else {
                    sprintf(buf, "%s%.3f", x.c_str(), position.x() - plate->get_origin().x());
                    imgui.text(buf);
                    ImGui::SameLine();
                    sprintf(buf, "%s%.3f", y.c_str(), position.y() - plate->get_origin().y());
                    imgui.text(buf);
                    ImGui::SameLine();
                    sprintf(buf, "%s%.3f", z.c_str(), position.z());
                    imgui.text(buf);
                    text_line = 1;
                }
                // force extra frame to automatically update window size
                float window_width = ImGui::GetWindowWidth();
                if (window_width != last_window_width || text_line != last_text_line) {
                    last_window_width = window_width;
                    last_text_line = text_line;
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
                    imgui.set_requires_extra_frame();
#else
                    wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
                    wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
                }
                imgui.end();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
                imgui.pop_toolbar_style();
            }

            //BBS: GUI refactor: move to the right
            void SequentialView::render(float legend_height, int canvas_width, int canvas_height, int right_margin, const EViewType& view_type, const OnAttachingHelio& cb, bool b_show_horizon_slider, bool b_helio_option) const
            {
                marker.render(canvas_width, canvas_height, view_type);
                //float bottom = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size().get_height();
                // BBS
#if 0
                if (wxGetApp().is_editor())
                    bottom -= wxGetApp().plater()->get_view_toolbar().get_height();
#endif
                //gcode_window.render(legend_height, bottom, static_cast<uint64_t>(gcode_ids[current.last]));
                gcode_window.render(legend_height, (float)canvas_height, (float)canvas_width - (float)right_margin, static_cast<uint64_t>(gcode_ids[current.last]), cb, b_show_horizon_slider, b_helio_option);
            }

            ColorRGBA Range::get_color_at(float value, EType type) const
            {
                float       global_t = 0.0f;
                const float step = step_size(type);
                float       _min = min;
                if (log_scale) {
                    value = std::log(value);
                    _min = std::log(min);
                }
                if (value > max) {
                    return range_colors[range_colors.size() - 1];
                }
                if (value < _min) {
                    if (value < _min - 0.01f) {
                        return ColorRGBA::GRAY(); // for helio
                    }
                    else {
                        return range_colors[0];
                    }
                }
                if (step > 0.0f) {
                    switch (type) {
                    default:
                    case EType::Linear: {
                        global_t = (value > min) ? (value - min) / step : 0.0f;
                        break;
                    }
                    case EType::Logarithmic: {
                        global_t = (value > _min  && step != 0.0f) ? std::max(0.0f, value - _min) / step : 0.0f;
                        break;
                    }
                    }
                }
                const size_t color_max_idx = range_colors.size() - 1;

                // Compute the two colors just below (low) and above (high) the input value
                const size_t color_low_idx = std::clamp<size_t>(static_cast<size_t>(global_t), 0, color_max_idx);
                const size_t color_high_idx = std::clamp<size_t>(color_low_idx + 1, 0, color_max_idx);
                // Compute how far the value is between the low and high colors so that they can be interpolated
                const float local_t = std::clamp(global_t - static_cast<float>(color_low_idx), 0.0f, 1.0f);
                // Interpolate between the low and high colors to find exactly which color the input value should get
                return lerp(range_colors[color_low_idx], range_colors[color_high_idx], local_t);
            }

            float Range::step_size(EType type) const {
                switch (type) {
                default:
                case EType::Linear: {
                    return (max > min) ? (max - min) / get_color_size() : 0.0f;
                }
                case EType::Logarithmic: {
                    float min_range = min;
                    if (min_range == 0) min_range = 0.001f;
                    return std::log(max / min_range) / get_color_size();
                }
                }
            }

            float Range::get_value_at_step(int step) const {
                if (!log_scale)
                    return min + static_cast<float>(step) * step_size();
                else
                    // calculate log-average
                {
                    float min_range = min;
                    if (min_range == 0)
                        min_range = 0.0001f;
                    float step_size = std::log(max / min_range) / get_color_size();
                    return std::exp(std::log(min) + static_cast<float>(step) * step_size);
                }
            }

            float Range::get_color_size() const {
                return (static_cast<float>(range_colors.size()) - 1.0f);
            }
} // namespace gcode
    } // namespace GUI
} // namespace Slic3r
