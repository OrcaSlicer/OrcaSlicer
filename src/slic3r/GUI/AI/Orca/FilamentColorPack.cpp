#include "FilamentColorPack.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>

namespace Slic3r::GUI {
namespace {
boost::filesystem::path pack_file() { return boost::filesystem::path(data_dir()) / "filament_color_packs.json"; }

FilamentColorPack current_pack() {
    FilamentColorPack pack;
    pack.name = u8"当前工程色卡";
    const auto* bundle = wxGetApp().preset_bundle;
    if (!bundle) return pack;
    const auto* colors = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    const auto* mixed = bundle->project_config.option<ConfigOptionBools>("filament_is_mixed");
    const auto* multi = bundle->project_config.option<ConfigOptionStrings>("filament_multi_colour");
    const auto* types = bundle->project_config.option<ConfigOptionStrings>("filament_colour_type");
    for (size_t i = 0; colors && i < bundle->filament_presets.size() && i < colors->values.size(); ++i) {
        if (mixed && i < mixed->values.size() && mixed->values[i]) continue;
        pack.colors.push_back(colors->values[i]);
        pack.labels.push_back(bundle->filament_presets[i]);
        pack.multi_colors.push_back(multi && i < multi->values.size() ? multi->values[i] : colors->values[i]);
        pack.color_types.push_back(types && i < types->values.size() ? types->values[i] : "");
        if (pack.colors.size() == 6) break;
    }
    return pack;
}

void save_pack(const FilamentColorPack& pack) {
    nlohmann::json json = nlohmann::json::array();
    if (boost::filesystem::exists(pack_file())) {
        boost::filesystem::ifstream input(pack_file());
        json = nlohmann::json::parse(input);
        if (!json.is_array()) throw std::runtime_error("Invalid color pack library");
    }
    const nlohmann::json saved{{"name", pack.name}, {"colors", pack.colors}, {"labels", pack.labels},
        {"multi_colors", pack.multi_colors}, {"color_types", pack.color_types}};
    bool replaced = false;
    for (auto& item : json) {
        if (item.is_object() && item.contains("name") && item["name"] == pack.name) {
            item = saved; replaced = true; break;
        }
    }
    if (!replaced) json.push_back(saved);
    auto tmp = pack_file(); tmp += ".tmp";
    { boost::filesystem::ofstream out(tmp); out << json.dump(2); out.close(); if (!out) throw std::runtime_error("Could not save color pack"); }
    boost::filesystem::rename(tmp, pack_file());
}
}

std::vector<FilamentColorPack> load_filament_color_packs() {
    std::vector<FilamentColorPack> packs{young_portrait_color_pack()};
    try {
        boost::filesystem::ifstream input(pack_file());
        if (!input) return packs;
        const auto json = nlohmann::json::parse(input);
        if (!json.is_array()) return packs;
        for (const auto& item : json) {
            try {
                FilamentColorPack pack{item.at("name").get<std::string>(), item.at("colors").get<std::vector<std::string>>(),
                    item.at("labels").get<std::vector<std::string>>()};
                pack.multi_colors = item.value("multi_colors", std::vector<std::string>{});
                pack.color_types = item.value("color_types", std::vector<std::string>{});
                if (pack.valid() && pack.name != packs.front().name) packs.push_back(std::move(pack));
            } catch (const std::exception&) { /* One bad card must not hide later valid cards. */ }
        }
    } catch (const std::exception&) { /* Malformed user cards never prevent opening Orca. */ }
    return packs;
}

bool show_filament_color_packs(wxWindow* parent) {
    auto* plater = wxGetApp().plater();
    auto* bundle = wxGetApp().preset_bundle;
    if (!plater || !bundle || plater->using_exported_file()) return false;
    wxDialog dialog(parent, wxID_ANY, _L("耗材色卡包"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto packs = load_filament_color_packs();
    auto* choice = new wxChoice(&dialog, wxID_ANY);
    for (const auto& pack : packs) choice->Append(wxString::FromUTF8(pack.name));
    choice->SetSelection(0);
    layout->Add(choice, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    auto* swatches = new wxPanel(&dialog);
    auto* card_layout = new wxBoxSizer(wxVERTICAL);
    swatches->SetSizer(card_layout);
    layout->Add(swatches, 0, wxEXPAND | wxLEFT | wxRIGHT, dialog.FromDIP(12));
    auto update_card = [&] {
        card_layout->Clear(true);
        const auto& pack = packs.at(choice->GetSelection());
        for (size_t i = 0; i < pack.colors.size(); ++i) {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* color = new wxPanel(swatches, wxID_ANY, wxDefaultPosition, dialog.FromDIP(wxSize(36, 24)));
            color->SetBackgroundColour(wxColour(wxString::FromUTF8(pack.colors[i])));
            row->Add(color, 0, wxRIGHT, dialog.FromDIP(10));
            row->Add(new wxStaticText(swatches, wxID_ANY, wxString::Format("%u  ", unsigned(i + 1)) +
                wxString::FromUTF8(pack.labels[i] + "  " + pack.colors[i])), 0, wxALIGN_CENTER_VERTICAL);
            card_layout->Add(row, 0, wxBOTTOM, dialog.FromDIP(8));
        }
        dialog.Layout();
    };
    choice->Bind(wxEVT_CHOICE, [&](wxCommandEvent&) { update_card(); });
    auto* description = new wxStaticText(&dialog, wxID_ANY,
        _L("按顺序更新前几个实体耗材槽，数量不足时补齐。其他槽位保留。\n"
           "所有使用这些槽位的模型都会换色，局部涂色范围保留。\n"
           "切换前自动保存旧色卡；可再次应用恢复颜色，新增槽位保留。\n"
           "沿用现有材料打印参数；请按真实装料核对槽位。\n"
           "年轻向配色含蓝色、不含绿色；色卡不代表实物色准或叠色标定。"));
    layout->Add(description, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    auto* save = new wxButton(&dialog, wxID_ANY, _L("保存当前工程色卡（前六槽）…"));
    layout->Add(save, 0, wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    save->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        auto pack = current_pack();
        if (!pack.valid()) { wxMessageBox(_L("请选择 1 至 6 个已配置颜色的实体耗材后保存。"), _L("保存色卡"), wxOK, &dialog); return; }
        wxTextEntryDialog naming(&dialog, _L("给这组色卡命名，之后可快速切换。"), _L("保存当前工程色卡"));
        if (naming.ShowModal() != wxID_OK || naming.GetValue().Strip(wxString::both).empty()) return;
        pack.name = naming.GetValue().Strip(wxString::both).ToUTF8().data();
        if (pack.name == young_portrait_color_pack().name || pack.name == u8"上次应用前色卡") { wxMessageBox(_L("请使用其他名称，预制色卡和恢复色卡由系统保留。"), _L("保存色卡"), wxOK, &dialog); return; }
        try {
            save_pack(pack); packs = load_filament_color_packs(); choice->Clear();
            for (const auto& item : packs) choice->Append(wxString::FromUTF8(item.name));
            for (size_t i = 0; i < packs.size(); ++i) if (packs[i].name == pack.name) choice->SetSelection(int(i));
            update_card();
        } catch (const std::exception&) { wxMessageBox(_L("色卡保存失败，请检查本地配置目录是否可写。"), _L("保存色卡"), wxOK, &dialog); }
    });
    auto* buttons = dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL);
    static_cast<wxButton*>(dialog.FindWindow(wxID_OK))->SetLabel(_L("应用到工程"));
    layout->Add(buttons, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    dialog.SetSizer(layout); update_card(); dialog.Fit(); dialog.CentreOnParent();
    if (dialog.ShowModal() != wxID_OK) return false;
    const auto pack = packs.at(choice->GetSelection());
    if (!pack.valid()) return false;
    const size_t needed = pack.colors.size() > bundle->num_physical_filaments()
        ? pack.colors.size() - bundle->num_physical_filaments() : 0;
    if (std::max(bundle->filament_presets.size(), plater->sidebar().combos_filament().size()) + needed > MAXIMUM_EXTRUDER_NUMBER) {
        wxMessageBox(_L("实体与叠色槽位总数不足以容纳这套色卡，本次未修改工程。可先在准备页整理未使用槽位。"),
            _L("耗材槽位不足"), wxOK, parent);
        return false;
    }
    // Orca's model undo stack does not serialize project filament settings.
    // Retain an explicit previous card rather than adding a misleading model
    // undo entry that cannot restore the material configuration.
    auto previous_card = current_pack();
    if (previous_card.valid()) {
        previous_card.name = u8"上次应用前色卡";
        try { save_pack(previous_card); }
        catch (const std::exception&) {
            wxMessageBox(_L("无法保存切换前色卡，本次未修改工程。请检查本地配置目录。"), _L("应用色卡"), wxOK, parent);
            return false;
        }
    }
    while (bundle->num_physical_filaments() < pack.colors.size()) {
        const auto before = bundle->num_physical_filaments();
        plater->sidebar().add_custom_filament(wxColour(wxString::FromUTF8(pack.colors[before])));
        if (bundle->num_physical_filaments() == before) return false;
    }
    DynamicPrintConfig changes;
    const auto* mixed = bundle->project_config.option<ConfigOptionBools>("filament_is_mixed");
    for (const char* key : {"filament_colour", "filament_multi_colour", "filament_colour_type"}) {
        const auto* previous = bundle->project_config.option<ConfigOptionStrings>(key);
        auto* values = previous ? new ConfigOptionStrings(*previous) : new ConfigOptionStrings();
        values->values.resize(bundle->filament_presets.size());
        size_t physical = 0;
        for (size_t i = 0; i < values->values.size() && physical < pack.colors.size(); ++i) {
            if (mixed && i < mixed->values.size() && mixed->values[i]) continue;
            values->values[i] = std::string(key) == "filament_colour_type"
                ? (pack.color_types.empty() ? "" : pack.color_types[physical])
                : std::string(key) == "filament_multi_colour" && !pack.multi_colors.empty()
                    ? pack.multi_colors[physical] : pack.colors[physical];
            ++physical;
        }
        changes.set_key_value(key, values);
    }
    bundle->project_config.apply(changes);
    size_t changed_physical = 0;
    for (size_t i = 0; i < bundle->filament_presets.size() && changed_physical < pack.colors.size(); ++i) {
        if (mixed && i < mixed->values.size() && mixed->values[i]) continue;
        if (i < bundle->ams_multi_color_filment.size()) bundle->ams_multi_color_filment[i].clear();
        ++changed_physical;
    }
    plater->on_config_change(changes);
    // Match the native spool-color action after all six colors are available.
    // Retain the user's explicit choice to calculate purge volumes manually.
    if (wxGetApp().app_config->get("auto_calculate_flush") != "disabled")
        plater->sidebar().auto_calc_flushing_volumes(-1);
    plater->sidebar().update_all_preset_comboboxes();
    plater->sidebar().update_mixed_filament_list();
    plater->update_filament_colors_in_full_config();
    if (auto* objects = wxGetApp().obj_list()) objects->update_filament_colors();
    plater->update_project_dirty_from_presets();
    plater->update();
    return true;
}
} // namespace Slic3r::GUI
