#include "OrcaWorkspaceAdapter.hpp"
#include "OrcaPaletteSnapshotBuilder.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/ObjColorDialog.hpp"
#include "slic3r/GUI/ModelColorImportResult.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Widgets/ProgressDialog.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/Utils/FixModelByCgal.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <utility>

namespace Slic3r::GUI {
namespace {

bool is_nonempty_obj(const boost::filesystem::path& path)
{
    boost::system::error_code ec;
    return boost::filesystem::is_regular_file(path, ec) && !ec && boost::filesystem::file_size(path, ec) > 0 && !ec &&
           path.extension() == ".obj";
}

bool has_open_mesh_edges(const ModelObject& object)
{
    return std::any_of(object.volumes.begin(), object.volumes.end(), [](const ModelVolume* volume) {
        return volume != nullptr && its_num_open_edges(volume->mesh().its) != 0;
    });
}

bool has_mmu_painting(const ModelObject& object)
{
    return std::any_of(object.volumes.begin(), object.volumes.end(), [](const ModelVolume* volume) {
        return volume != nullptr && volume->is_mm_painted();
    });
}

ObjImportColorFn make_obj_color_mapper(const std::vector<std::string>& extruder_colours,
                                       const std::vector<size_t>& allowed_slots, bool& applied,
                                       size_t& source_colour_count, size_t& mapped_colour_count)
{
    struct FilamentColour {
        RGBA          colour;
        unsigned char filament_id;
    };
    std::vector<FilamentColour> decoded_colours;
    decoded_colours.reserve(allowed_slots.size());
    for (const size_t slot : allowed_slots) {
        if (slot >= extruder_colours.size())
            continue;
        const wxColour colour(extruder_colours[slot]);
        if (colour.IsOk() && slot < std::numeric_limits<unsigned char>::max())
            decoded_colours.push_back({convert_to_rgba(colour), static_cast<unsigned char>(slot + 1)});
    }

    return [decoded_colours = std::move(decoded_colours), &applied, &source_colour_count,
            &mapped_colour_count](ObjDialogInOut& in_out) {
        applied = false;
        source_colour_count = 0;
        mapped_colour_count = 0;
        if (in_out.model == nullptr || in_out.input_colors.empty() || decoded_colours.empty())
            return;

        source_colour_count = std::set<RGBA>(in_out.input_colors.begin(), in_out.input_colors.end()).size();

        std::vector<size_t> usage(decoded_colours.size(), 0);
        in_out.filament_ids.clear();
        in_out.filament_ids.reserve(in_out.input_colors.size());
        for (const RGBA& vertex_colour : in_out.input_colors) {
            size_t best_index = 0;
            float best_distance = std::numeric_limits<float>::max();
            for (size_t index = 0; index < decoded_colours.size(); ++index) {
                const float distance = calc_color_distance(vertex_colour, decoded_colours[index].colour);
                if (distance < best_distance) {
                    best_distance = distance;
                    best_index = index;
                }
            }
            in_out.filament_ids.emplace_back(decoded_colours[best_index].filament_id);
            ++usage[best_index];
        }
        mapped_colour_count = std::count_if(usage.begin(), usage.end(), [](size_t count) { return count != 0; });

        const auto dominant = std::max_element(usage.begin(), usage.end());
        in_out.first_extruder_id = decoded_colours[std::distance(usage.begin(), dominant)].filament_id;
        applied = in_out.deal_vertex_color
            ? Model::obj_import_vertex_color_deal(in_out.filament_ids, in_out.first_extruder_id, in_out.model)
            : Model::obj_import_face_color_deal(in_out.filament_ids, in_out.first_extruder_id, in_out.model);
        if (!applied) {
            in_out.filament_ids.clear();
            mapped_colour_count = 0;
        }
    };
}

} // namespace

OrcaWorkspaceAdapter::OrcaWorkspaceAdapter(Plater* plater, ImportSucceededFn on_import_succeeded)
    : m_plater(plater)
    , m_on_import_succeeded(std::move(on_import_succeeded))
{}

AI::PrintablePaletteSnapshot OrcaWorkspaceAdapter::printable_palette() const
{
    if (m_plater == nullptr)
        return {};

    std::vector<std::string> project_colors = m_plater->get_extruder_colors_from_plater_config();
    const PresetBundle* bundle = wxGetApp().preset_bundle;
    const auto* mixed_flags = bundle == nullptr
        ? nullptr : bundle->project_config.option<ConfigOptionBools>("filament_is_mixed");
    const auto* mixed_components = bundle == nullptr
        ? nullptr : bundle->project_config.option<ConfigOptionStrings>("filament_mixed_components");
    const auto* mixed_ratios = bundle == nullptr
        ? nullptr : bundle->project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios");

    std::vector<OrcaPaletteSlotCapability> capabilities;
    capabilities.reserve(project_colors.size());
    for (size_t slot = 0; slot < project_colors.size(); ++slot) {
        std::string color = project_colors[slot];
        std::transform(color.begin(), color.end(), color.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        const bool is_mixed = mixed_flags != nullptr && slot < mixed_flags->values.size() &&
                              mixed_flags->values[slot];
        OrcaPaletteSlotCapability capability {slot, color, {}, is_mixed, true, {}};
        if (is_mixed && mixed_components != nullptr && slot < mixed_components->values.size()) {
            const std::vector<unsigned int> component_ids = parse_mixed_components(mixed_components->values[slot]);
            const std::vector<double> ratios = parse_mixed_ratios(
                mixed_ratios != nullptr && slot < mixed_ratios->values.size() ? mixed_ratios->values[slot] : "",
                component_ids.size());
            for (size_t index = 0; index < component_ids.size() && index < ratios.size(); ++index) {
                if (component_ids[index] == 0) {
                    capability.mixed_components.clear();
                    break;
                }
                capability.mixed_components.push_back({component_ids[index] - 1, ratios[index]});
            }
        }
        capabilities.push_back(std::move(capability));
    }

    const std::vector<size_t> physical_slots = select_model_generation_physical_slots(capabilities);
    struct SlotTemperature {
        std::string type;
        int         temperature { 0 };
        int         range_low { 0 };
        int         range_high { 0 };
    };
    std::vector<SlotTemperature> slot_temperatures(physical_slots.size());
    bool metadata_complete = bundle != nullptr;
    for (size_t index = 0; index < physical_slots.size(); ++index) {
        const size_t slot = physical_slots[index];
        const Preset* preset = bundle != nullptr && slot < bundle->filament_presets.size()
            ? bundle->filaments.find_preset(bundle->filament_presets[slot]) : nullptr;
        if (preset == nullptr) {
            metadata_complete = false;
            continue;
        }
        const auto* types = preset->config.option<ConfigOptionStrings>("filament_type");
        const auto* temperatures = preset->config.option<ConfigOptionInts>("nozzle_temperature");
        const auto* range_lows = preset->config.option<ConfigOptionInts>("nozzle_temperature_range_low");
        const auto* range_highs = preset->config.option<ConfigOptionInts>("nozzle_temperature_range_high");
        auto capability = std::find_if(capabilities.begin(), capabilities.end(), [slot](const auto& candidate) {
            return !candidate.is_mixed && candidate.slot == slot;
        });
        if (types != nullptr && !types->values.empty() && capability != capabilities.end())
            capability->material_type = types->get_at(0);
        if (types == nullptr || types->values.empty() || temperatures == nullptr || temperatures->values.empty() ||
            range_lows == nullptr || range_lows->values.empty() || range_highs == nullptr || range_highs->values.empty()) {
            metadata_complete = false;
            continue;
        }
        slot_temperatures[index] =
            {types->get_at(0), temperatures->get_at(0), range_lows->get_at(0), range_highs->get_at(0)};
    }

    std::vector<size_t> compatible_slots = physical_slots;
    if (physical_slots.size() >= 2 && metadata_complete) {
        std::vector<size_t> best {physical_slots.front()};
        const uint32_t subset_count = uint32_t(1) << physical_slots.size();
        for (uint32_t mask = 1; mask < subset_count; ++mask) {
            std::vector<size_t> slots;
            std::vector<std::string> selected_types;
            std::vector<int> selected_temperatures;
            std::vector<int> selected_lows;
            std::vector<int> selected_highs;
            for (size_t bit = 0; bit < physical_slots.size(); ++bit) {
                if ((mask & (uint32_t(1) << bit)) == 0)
                    continue;
                slots.push_back(physical_slots[bit]);
                selected_types.push_back(slot_temperatures[bit].type);
                selected_temperatures.push_back(slot_temperatures[bit].temperature);
                selected_lows.push_back(slot_temperatures[bit].range_low);
                selected_highs.push_back(slot_temperatures[bit].range_high);
            }
            if (slots.size() > best.size() &&
                Print::check_multi_filaments_compatibility(selected_types, selected_temperatures, selected_lows,
                                                           selected_highs) == FilamentCompatibilityType::Compatible)
                best = std::move(slots);
        }
        compatible_slots = std::move(best);
    }

    for (OrcaPaletteSlotCapability& capability : capabilities) {
        if (!capability.is_mixed)
            capability.compatible = std::find(compatible_slots.begin(), compatible_slots.end(), capability.slot) !=
                                    compatible_slots.end();
    }
    AI::PrintablePaletteSnapshot snapshot = build_orca_palette_snapshot(capabilities, metadata_complete);
    // Preserve the raw all-slot projection for the legacy manual matcher. Typed
    // consumers use physical_channels and mixed_recipes, which remain separated.
    snapshot.project_colors = std::move(project_colors);
    return snapshot;
}

AI::ModelImportResult OrcaWorkspaceAdapter::import_artifact(const AI::ModelImportRequest& request)
{
    AI::ModelImportResult result;
    result.color_mode = request.color_mode;
    if (m_plater == nullptr || !is_nonempty_obj(request.artifact.local_path)) {
        result.outcome = AI::ModelImportOutcome::InvalidArtifact;
        result.error = "The generated OBJ is missing or invalid.";
        return result;
    }

    const boost::filesystem::path& path = request.artifact.local_path;
    Sidebar& workflow = m_plater->sidebar();
    workflow.start_ai_workflow(_L("正在导入 AI 生成模型"));
    workflow.update_ai_workflow_step(Sidebar::AIImportModel, Sidebar::AIWorkflowStatus::Running, _L("读取 OBJ"));

    bool import_cancelled = false;
    auto load_model = [this, &path, &import_cancelled](const char* snapshot_name, AI::ImportColorMode color_mode, bool& colors_applied,
                                    size_t& source_color_count, size_t& mapped_color_count) {
        import_cancelled = false;
        if (color_mode == AI::ImportColorMode::NativeMatch) {
            // An OBJ callback bypasses the native texture matcher for vertex and
            // face colors. Leave it unset so AI imports use the same preview,
            // mixed-filament recipes and undo transaction as regular OBJ imports.
            Plater::TakeSnapshot snapshot(m_plater, snapshot_name);
            ModelColorImportResult color_result;
            auto loaded = m_plater->load_files({path}, LoadStrategy::LoadModel, false, nullptr, &color_result);
            import_cancelled = color_result.cancelled;
            colors_applied = color_result.colors_applied;
            source_color_count = color_result.source_color_count;
            mapped_color_count = color_result.mapped_color_count;
            return loaded;
        }
        ObjImportColorFn color_mapper;
        const AI::PrintablePaletteSnapshot palette = printable_palette();
        if (color_mode == AI::ImportColorMode::AutoMap) {
            color_mapper = make_obj_color_mapper(palette.project_colors, palette.compatible_slots, colors_applied,
                                                 source_color_count, mapped_color_count);
        } else if (color_mode == AI::ImportColorMode::ManualMatch) {
            color_mapper = [extruder_colors = palette.project_colors, &colors_applied, &source_color_count,
                            &mapped_color_count, &import_cancelled](ObjDialogInOut& in_out) {
                colors_applied = false;
                mapped_color_count = 0;
                source_color_count = std::set<RGBA>(in_out.input_colors.begin(), in_out.input_colors.end()).size();
                in_out.preserve_input_colors = true;

                ObjColorDialog color_dialog(nullptr, in_out, extruder_colors, Sidebar::should_show_SEMM_buttons());
                if (color_dialog.ShowModal() != wxID_OK) {
                    in_out.filament_ids.clear();
                    in_out.cancelled = true;
                    import_cancelled = true;
                    return;
                }
                std::vector<unsigned char> used_filaments;
                for (const unsigned char filament_id : in_out.filament_ids) {
                    if (filament_id != 0 &&
                        std::find(used_filaments.begin(), used_filaments.end(), filament_id) == used_filaments.end())
                        used_filaments.emplace_back(filament_id);
                }
                mapped_color_count = used_filaments.size();
                colors_applied = in_out.deal_vertex_color
                    ? Model::obj_import_vertex_color_deal(in_out.filament_ids, in_out.first_extruder_id, in_out.model)
                    : Model::obj_import_face_color_deal(in_out.filament_ids, in_out.first_extruder_id, in_out.model);
                if (!colors_applied)
                    mapped_color_count = 0;
            };
        } else {
            color_mapper = [](ObjDialogInOut&) {};
        }
        Plater::TakeSnapshot snapshot(m_plater, snapshot_name);
        return m_plater->load_files({path}, LoadStrategy::LoadModel, false, std::move(color_mapper));
    };

    const size_t before = m_plater->model().objects.size();
    std::vector<size_t> loaded = load_model("Import AI generated model", request.color_mode, result.colors_applied,
                                            result.source_color_count, result.mapped_color_count);
    if (loaded.empty() || m_plater->model().objects.size() <= before) {
        if (!loaded.empty() && m_plater->model().objects.size() > before)
            m_plater->undo();
        result.outcome = import_cancelled ? AI::ModelImportOutcome::Cancelled : AI::ModelImportOutcome::ImportFailed;
        result.error = import_cancelled ? "OBJ import cancelled." : "OBJ import failed.";
        workflow.update_ai_workflow_step(Sidebar::AIImportModel,
            import_cancelled ? Sidebar::AIWorkflowStatus::Warning : Sidebar::AIWorkflowStatus::Failed,
            import_cancelled ? _L("已取消导入。") : _L("OBJ 导入失败"));
        workflow.finish_ai_workflow(false, import_cancelled ? _L("已取消导入。") : _L("模型导入失败"));
        return result;
    }

    workflow.update_ai_workflow_step(Sidebar::AIImportModel, Sidebar::AIWorkflowStatus::Success);
    workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Running);

    auto update_color_status = [&]() {
        result.color_mapping_collapsed = request.color_mode != AI::ImportColorMode::SingleColor && result.colors_applied &&
                                         result.source_color_count > 1 && result.mapped_color_count < 2;
        result.manual_coloring_required = request.color_mode != AI::ImportColorMode::SingleColor &&
                                          (!result.colors_applied || result.color_mapping_collapsed);
        BOOST_LOG_TRIVIAL(info) << "AI OBJ color import: mode=" << static_cast<int>(request.color_mode)
                                << ", source_colours=" << result.source_color_count
                                << ", mapped_colours=" << result.mapped_color_count
                                << ", applied=" << result.colors_applied
                                << ", collapsed=" << result.color_mapping_collapsed;

        if (request.color_mode == AI::ImportColorMode::ManualMatch ||
            request.color_mode == AI::ImportColorMode::NativeMatch) {
            workflow.update_ai_workflow_step(
                Sidebar::AIProcessColors,
                result.manual_coloring_required ? Sidebar::AIWorkflowStatus::Warning : Sidebar::AIWorkflowStatus::Success,
                result.manual_coloring_required ? _L("颜色匹配未完成") : _L("已确认模型颜色与耗材槽"));
        } else if (request.color_mode == AI::ImportColorMode::SingleColor) {
            workflow.update_ai_workflow_step(Sidebar::AIProcessColors, Sidebar::AIWorkflowStatus::Success,
                                             _L("单色导入"));
        } else if (result.colors_applied) {
            workflow.update_ai_workflow_step(Sidebar::AIProcessColors, Sidebar::AIWorkflowStatus::Success,
                                             _L("已映射耗材颜色"));
        } else {
            workflow.update_ai_workflow_step(Sidebar::AIProcessColors, Sidebar::AIWorkflowStatus::Warning,
                                             _L("需要手动上色"));
        }
    };
    update_color_status();

    bool requires_repair = false;
    for (size_t object_index : loaded) {
        if (object_index < m_plater->model().objects.size()) {
            const ModelObject* object = m_plater->model().objects[object_index];
            requires_repair |= object != nullptr && has_open_mesh_edges(*object);
        }
    }
    if (requires_repair) {
        workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Running,
                                         _L("发现开放边，正在自动修复"));
        ProgressDialog progress_dlg(_L("修复 AI 生成模型"), "", 100, find_toplevel_parent(m_plater),
                                    wxPD_AUTO_HIDE | wxPD_APP_MODAL, true);
        std::string repair_error;
        for (size_t object_index : loaded) {
            if (object_index >= m_plater->model().objects.size())
                continue;
            ModelObject* object = m_plater->model().objects[object_index];
            if (object == nullptr || !has_open_mesh_edges(*object))
                continue;
            const bool had_painting = has_mmu_painting(*object);
            std::string object_error;
            const bool completed = fix_model_with_cgal_gui(
                *object, -1, progress_dlg, _L("正在修复模型对象") + ":\n", object_error, true);
            object->ensure_on_bed();
            m_plater->changed_mesh(static_cast<int>(object_index));
            if (!completed || !object_error.empty() || has_open_mesh_edges(*object) ||
                (had_painting && !has_mmu_painting(*object))) {
                repair_error = object_error.empty() ? "网格仍存在非流体问题，或无法保留耗材颜色。" : object_error;
                break;
            }
        }

        if (!repair_error.empty()) {
            m_plater->undo();
            RichMessageDialog fallback(
                m_plater,
                _L("自动网格修复失败。\n\n"
                   "可以将原始 OBJ 手动导入准备页，再使用准备页中的修复工具处理。"),
                _L("自动修复失败"), wxYES_NO | wxICON_WARNING);
            fallback.SetYesNoLabels(_L("手动导入"), _L("取消"));
            if (fallback.ShowModal() != wxID_YES) {
                result.outcome = AI::ModelImportOutcome::RepairFailed;
                result.error = std::move(repair_error);
                workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Failed,
                                                 _L("自动修复失败"));
                workflow.finish_ai_workflow(false, _L("网格修复失败"));
                return result;
            }

            const size_t manual_before = m_plater->model().objects.size();
            bool manual_colors_applied = false;
            size_t manual_source_color_count = 0;
            size_t manual_mapped_color_count = 0;
            const std::vector<size_t> manual_loaded =
                load_model("Manually import AI generated model", request.color_mode, manual_colors_applied,
                           manual_source_color_count, manual_mapped_color_count);
            if (manual_loaded.empty() || m_plater->model().objects.size() <= manual_before) {
                if (!manual_loaded.empty() && m_plater->model().objects.size() > manual_before)
                    m_plater->undo();
                result.outcome = import_cancelled ? AI::ModelImportOutcome::Cancelled : AI::ModelImportOutcome::ImportFailed;
                result.error = import_cancelled ? "OBJ import cancelled." : "Manual OBJ import failed after automatic repair.";
                workflow.update_ai_workflow_step(Sidebar::AIImportModel,
                    import_cancelled ? Sidebar::AIWorkflowStatus::Warning : Sidebar::AIWorkflowStatus::Failed,
                    import_cancelled ? _L("已取消导入。") : _L("手动导入失败"));
                workflow.finish_ai_workflow(false, import_cancelled ? _L("已取消导入。") : _L("模型导入失败"));
                return result;
            }
            result.colors_applied = manual_colors_applied;
            result.source_color_count = manual_source_color_count;
            result.mapped_color_count = manual_mapped_color_count;
            update_color_status();
            result.manual_repair_required = true;
            workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Warning,
                                             _L("需要手动修复"));
        }
    }

    if (!requires_repair)
        workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Success, _L("封闭网格"));
    else if (!result.manual_repair_required)
        workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Success,
                                         _L("自动修复完成"));

    workflow.update_ai_workflow_step(Sidebar::AIArrange, Sidebar::AIWorkflowStatus::Running,
                                     _L("正在放置到打印板"));
    result.outcome = AI::ModelImportOutcome::Imported;
    if (!m_on_import_succeeded) {
        result.error = "The Orca workspace navigation callback is unavailable.";
        workflow.update_ai_workflow_step(Sidebar::AIArrange, Sidebar::AIWorkflowStatus::Failed,
                                          _L("无法切换工作区"));
        workflow.finish_ai_workflow(false, _L("模型已导入，但无法切换到准备页"));
        return result;
    }

    m_on_import_succeeded();
    workflow.update_ai_workflow_step(Sidebar::AIArrange, Sidebar::AIWorkflowStatus::Success,
                                     _L("已放置到打印板"));
    workflow.update_ai_workflow_step(Sidebar::AISlice, Sidebar::AIWorkflowStatus::Waiting,
                                     result.manual_repair_required
                                         ? _L("修复模型后手动切片")
                                         : result.manual_coloring_required ? _L("完成上色后手动切片")
                                                                           : _L("等待手动切片"));
    workflow.update_ai_workflow_step(Sidebar::AIGCode, Sidebar::AIWorkflowStatus::Waiting,
                                     _L("手动切片后生成"));
    workflow.finish_ai_workflow(true,
                                result.manual_repair_required
                                    ? _L("模型已导入准备页，请先手动修复")
                                    : result.manual_coloring_required
                                        ? _L("模型已导入准备页，请先完成上色")
                                        : _L("模型已导入准备页，可手动调整并切片"));
    return result;
}

} // namespace Slic3r::GUI
