#include "ColorDecomposeSupport.hpp"
#include "MixedFilamentDialog.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include "nlohmann/json.hpp"

#include <fstream>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace Slic3r { namespace GUI {

std::string decompose_normalize_color_hex(std::string color)
{
    if (color.size() >= 7)
        color = color.substr(0, 7);
    std::transform(color.begin(), color.end(), color.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return color;
}

const char* decompose_base_color_en(DecomposeBaseColor color)
{
    switch (color) {
    case DecomposeBaseColor::Cyan:    return "Cyan";
    case DecomposeBaseColor::Magenta: return "Magenta";
    case DecomposeBaseColor::Yellow:  return "Yellow";
    case DecomposeBaseColor::White:   return "White";
    case DecomposeBaseColor::Red:     return "Red";
    case DecomposeBaseColor::Green:   return "Green";
    case DecomposeBaseColor::Blue:    return "Blue";
    default:                          return "";
    }
}

wxString decompose_base_color_display(DecomposeBaseColor color)
{
    switch (color) {
    case DecomposeBaseColor::Cyan:    return _L("Cyan");
    case DecomposeBaseColor::Magenta: return _L("Magenta");
    case DecomposeBaseColor::Yellow:  return _L("Yellow");
    case DecomposeBaseColor::White:   return _L("White");
    case DecomposeBaseColor::Red:     return _L("Red");
    case DecomposeBaseColor::Green:   return _L("Green");
    case DecomposeBaseColor::Blue:    return _L("Blue");
    default:                          return wxString();
    }
}

std::string decompose_basic_type_from_source(size_t source_config_idx,
                                             size_t source_physical_idx,
                                             const std::vector<std::string>& physical_types)
{
    auto& project_config = wxGetApp().preset_bundle->project_config;
    if (auto* filament_id_opt = project_config.option<ConfigOptionStrings>("filament_id")) {
        if (source_config_idx < filament_id_opt->values.size()) {
            const std::string& filament_id = filament_id_opt->values[source_config_idx];
            if (filament_id == kDecomposePetgFilamentId)
                return kDecomposePetgBasicType;
            if (filament_id == kDecomposePlaFilamentId)
                return kDecomposePlaBasicType;
        }
    }

    if (source_physical_idx < physical_types.size()) {
        const std::string& type = physical_types[source_physical_idx];
        if (type == kDecomposePetgShortType || type == kDecomposePetgBasicType)
            return kDecomposePetgBasicType;
        if (type == kDecomposePlaShortType || type == kDecomposePlaBasicType)
            return kDecomposePlaBasicType;
    }
    return kDecomposePlaBasicType;
}

std::string decompose_basic_filament_id(const std::string& basic_type)
{
    if (basic_type == kDecomposePetgBasicType)
        return kDecomposePetgFilamentId;
    return kDecomposePlaFilamentId;
}

void set_created_standard_component_metadata(size_t config_idx, const DecomposeOfficialComponent& component)
{
    auto& project_config = wxGetApp().preset_bundle->project_config;
    if (!component.filament_id.empty()) {
        if (auto* filament_id_opt = project_config.option<ConfigOptionStrings>("filament_id")) {
            while (filament_id_opt->values.size() <= config_idx)
                filament_id_opt->values.push_back("");
            filament_id_opt->values[config_idx] = component.filament_id;
        }
    }

    const std::string type = component.filament_id == kDecomposePetgFilamentId ? kDecomposePetgShortType :
                             component.filament_id == kDecomposePlaFilamentId ? kDecomposePlaShortType : "";
    if (!type.empty()) {
        if (auto* type_opt = project_config.option<ConfigOptionStrings>("filament_type")) {
            while (type_opt->values.size() <= config_idx)
                type_opt->values.push_back("");
            type_opt->values[config_idx] = type;
        }
    }
}

DecomposeOfficialComponent lookup_decompose_official_component(
    const std::string& basic_type,
    DecomposeBaseColor base_color,
    const wxColour& fallback)
{
    DecomposeOfficialComponent result;
    result.base_color  = base_color;
    result.color_hex   = decompose_normalize_color_hex(fallback.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
    result.filament_id = decompose_basic_filament_id(basic_type);

    const char* color_name = decompose_base_color_en(base_color);
    if (color_name[0] == '\0')
        return result;

    // Some materials name a standard base color differently in the color-code
    // table. PETG Basic's RYBW blue base is "Reflex Blue" (deep blue, B00,
    // #001489), not "Blue". Match by an ordered list of exact English names so
    // "Navy Blue" (B01, #0086D6) is never picked up by mistake.
    std::vector<std::string> candidate_names;
    candidate_names.emplace_back(color_name);
    if (base_color == DecomposeBaseColor::Blue && basic_type == kDecomposePetgBasicType)
        candidate_names.emplace_back("Reflex Blue");

    std::ifstream ifs(resources_dir() + "/profiles/BBL/filament/filaments_color_codes.json");
    if (!ifs)
        return result;

    json root = json::parse(ifs, nullptr, false);
    if (root.is_discarded() || !root.contains("data") || !root["data"].is_array())
        return result;

    for (const std::string& candidate : candidate_names) {
        for (const auto& item : root["data"]) {
            if (!item.is_object() || item.value("fila_type", "") != basic_type)
                continue;
            if (!item.contains("fila_color_name"))
                continue;
            const auto& names = item["fila_color_name"];
            if (!names.is_object() || names.value("en", "") != candidate)
                continue;
            if (item.contains("fila_color") && item["fila_color"].is_array() && !item["fila_color"].empty())
                result.color_hex = decompose_normalize_color_hex(item["fila_color"][0].get<std::string>());
            result.filament_id = item.value("fila_id", result.filament_id);
            return result;
        }
    }
    return result;
}

std::string find_decompose_standard_preset_name(size_t source_config_idx, const std::string& basic_type)
{
    const PresetBundle& preset_bundle = *wxGetApp().preset_bundle;
    if (source_config_idx < preset_bundle.filament_presets.size()) {
        const std::string& source_name = preset_bundle.filament_presets[source_config_idx];
        if (source_name.find(std::string(kDecomposeBambuPresetPrefix) + basic_type) != std::string::npos)
            return source_name;
    }

    const std::string prefix = std::string(kDecomposeBambuPresetPrefix) + basic_type + " @BBL ";
    for (const std::string& preset_name : preset_bundle.filament_presets) {
        if (preset_name.find(prefix) == 0)
            return preset_name;
    }

    return {};
}

std::string official_basic_type_from_preset_name(const std::string& preset_name)
{
    if (preset_name.find(std::string(kDecomposeBambuPresetPrefix) + kDecomposePlaBasicType) != std::string::npos)
        return kDecomposePlaBasicType;
    if (preset_name.find(std::string(kDecomposeBambuPresetPrefix) + kDecomposePetgBasicType) != std::string::npos)
        return kDecomposePetgBasicType;
    return {};
}

std::string filament_type_for_color_decompose(Preset* preset)
{
    if (!preset)
        return kDecomposePlaShortType;

    std::string display_type;
    std::string ft = preset->config.get_filament_type(display_type);
    const std::string basic = official_basic_type_from_preset_name(preset->name);
    if (!basic.empty())
        ft = basic;
    if (ft.empty())
        ft = kDecomposePlaShortType;
    return ft;
}

int find_existing_decompose_component(
    const DecomposeOfficialComponent& component,
    const std::vector<std::string>& physical_colors,
    const std::vector<size_t>& physical_config_indices,
    size_t source_config_idx)
{
    auto& project_config = wxGetApp().preset_bundle->project_config;
    auto* filament_id_opt = project_config.option<ConfigOptionStrings>("filament_id");
    auto* type_opt = project_config.option<ConfigOptionStrings>("filament_type");
    const PresetBundle& preset_bundle = *wxGetApp().preset_bundle;
    const size_t num_physical = physical_colors.size();
    const std::string expected_basic_type = component.filament_id == kDecomposePetgFilamentId ? kDecomposePetgBasicType :
                                            component.filament_id == kDecomposePlaFilamentId ? kDecomposePlaBasicType : "";
    const std::string expected_short_type = expected_basic_type == kDecomposePetgBasicType ? kDecomposePetgShortType :
                                            expected_basic_type == kDecomposePlaBasicType ? kDecomposePlaShortType : "";
    const std::string expected_preset_part = expected_basic_type.empty() ? "" : std::string(kDecomposeBambuPresetPrefix) + expected_basic_type;
    for (size_t i = 0; i < num_physical && i < physical_config_indices.size(); ++i) {
        const size_t config_idx = physical_config_indices[i];
        const std::string slot_color = decompose_normalize_color_hex(physical_colors[i]);
        const std::string slot_filament_id = (filament_id_opt && config_idx < filament_id_opt->values.size()) ? filament_id_opt->values[config_idx] : "";
        const std::string slot_type = (type_opt && config_idx < type_opt->values.size()) ? type_opt->values[config_idx] : "";
        const std::string preset_name = config_idx < preset_bundle.filament_presets.size() ? preset_bundle.filament_presets[config_idx] : "";
        if (config_idx == source_config_idx) {
            continue;
        }
        if (slot_color != component.color_hex) {
            continue;
        }

        if (!component.filament_id.empty() && slot_filament_id == component.filament_id) {
            return static_cast<int>(config_idx + 1);
        }

        if (!expected_basic_type.empty() && (slot_type == expected_basic_type || slot_type == expected_short_type)) {
            return static_cast<int>(config_idx + 1);
        }

        if (!expected_preset_part.empty() && preset_name.find(expected_preset_part) != std::string::npos) {
            return static_cast<int>(config_idx + 1);
        }

        const bool has_material_hint = !slot_filament_id.empty() || !slot_type.empty() || !preset_name.empty();
        if (!expected_basic_type.empty() && has_material_hint)
            continue;

        return static_cast<int>(config_idx + 1);
    }
    return -1;
}

bool prepare_decompose_mixed_result(
    const ColorDecomposeResult& result,
    size_t source_config_idx,
    size_t source_physical_idx,
    const std::vector<std::string>& physical_colors,
    const std::vector<std::string>& physical_types,
    const std::vector<size_t>& physical_config_indices,
    MixedFilamentResult& out_result,
    std::vector<DecomposeMissingComponent>& missing)
{
    out_result = {};
    missing.clear();
    if (result.components.size() < 2) {
        return false;
    }

    const bool standard_mode = result.mode == DecomposeMode::CMYW || result.mode == DecomposeMode::RYBW;
    std::string basic_type;
    std::string preset_name;
    if (standard_mode) {
        basic_type = decompose_basic_type_from_source(source_config_idx, source_physical_idx, physical_types);
        preset_name = find_decompose_standard_preset_name(source_config_idx, basic_type);
    }

    for (size_t i = 0; i < result.components.size(); ++i) {
        const DecomposeComponent& comp = result.components[i];
        out_result.ratios.push_back(comp.ratio);
        if (!standard_mode) {
            if (comp.filament_index <= 0) {
                return false;
            }
            const size_t physical_idx = static_cast<size_t>(comp.filament_index - 1);
            if (physical_idx >= physical_config_indices.size()) {
                return false;
            }
            out_result.components.push_back(static_cast<unsigned int>(physical_config_indices[physical_idx] + 1));
            continue;
        }

        if (comp.base_color == DecomposeBaseColor::None) {
            return false;
        }
        DecomposeOfficialComponent official_component =
            lookup_decompose_official_component(basic_type, comp.base_color, comp.colour);
        int existing_idx = find_existing_decompose_component(official_component, physical_colors,
                                                             physical_config_indices, source_config_idx);
        if (existing_idx > 0) {
            out_result.components.push_back(static_cast<unsigned int>(existing_idx));
            continue;
        }

        DecomposeMissingComponent missing_comp;
        missing_comp.component_idx = out_result.components.size();
        missing_comp.official_component = official_component;
        missing_comp.preset_name = preset_name;
        missing_comp.display_name = decompose_base_color_display(comp.base_color) +
            wxString::FromUTF8(" ") + wxString::FromUTF8(basic_type);
        missing.push_back(std::move(missing_comp));
        out_result.components.push_back(0);
    }

    const bool ok = out_result.components.size() == out_result.ratios.size() && out_result.components.size() >= 2;
    return ok;
}

size_t count_decompose_new_physical_filaments(
    const ColorDecomposeResult& result,
    const std::vector<std::string>& physical_colors,
    const std::vector<std::string>& physical_types,
    size_t source_physical_idx,
    const std::vector<size_t>* physical_config_indices)
{
    if (result.mode != DecomposeMode::CMYW && result.mode != DecomposeMode::RYBW)
        return 0;

    std::vector<size_t> fallback_indices;
    const std::vector<size_t>* indices = physical_config_indices;
    if (!indices) {
        fallback_indices.resize(physical_colors.size());
        for (size_t i = 0; i < fallback_indices.size(); ++i)
            fallback_indices[i] = i;
        indices = &fallback_indices;
    }

    size_t source_config_idx = size_t(-1);
    if (source_physical_idx < indices->size())
        source_config_idx = (*indices)[source_physical_idx];

    const std::string basic_type =
        decompose_basic_type_from_source(source_config_idx, source_physical_idx, physical_types);

    size_t missing_count = 0;
    for (const DecomposeComponent& comp : result.components) {
        if (comp.base_color == DecomposeBaseColor::None)
            continue;
        DecomposeOfficialComponent official_component =
            lookup_decompose_official_component(basic_type, comp.base_color, comp.colour);
        int existing_idx = find_existing_decompose_component(official_component, physical_colors,
                                                             *indices, source_config_idx);
        if (existing_idx <= 0)
            ++missing_count;
    }
    return missing_count;
}

bool confirm_create_decompose_missing_components(wxWindow* parent, const std::vector<DecomposeMissingComponent>& missing)
{
    if (missing.empty())
        return true;

    static const char* config_key = "not_show_color_decompose_missing_component_tip";
    if (wxGetApp().app_config->get(config_key) == "1") {
        return true;
    }

    wxString missing_text;
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i > 0)
            missing_text += _L(", ");
        missing_text += missing[i].display_name;
    }

    wxString message = _L("The current filament list does not contain ") + missing_text +
        _L(". A project filament required by the mixed filament will be created automatically after decomposition.");

    MessageDialog dlg(parent, message, _L("Tip"), wxOK | wxCANCEL | wxICON_INFORMATION);
    dlg.show_dsa_button();
    int res = dlg.ShowModal();
    if (res == wxID_OK && dlg.get_checkbox_state())
        wxGetApp().app_config->set(config_key, "1");
    return res == wxID_OK;
}

}} // namespace Slic3r::GUI
