#pragma once

#include "ModelPreviewPalette.hpp"
#include "../Model/ColorTrialState.hpp"
#include "PortraitColorPackMapping.hpp"
#include "slic3r/GUI/AI/Orca/FilamentColorPack.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/PresetBundle.hpp"
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/colordlg.h>
#include <wx/panel.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <chrono>
#include <functional>
#include <memory>
#include <boost/log/trivial.hpp>

namespace Slic3r::GUI {
// Trial colors do not modify the project. The separate color-pack action
// explicitly applies a physical-slot card through the Orca adapter.
class ModelPreviewColorControls final : public wxPanel {
public:
    using Color = PreviewPalette::Color;
    explicit ModelPreviewColorControls(wxWindow* parent) : wxPanel(parent) {
        auto* box = new wxBoxSizer(wxVERTICAL);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        m_toggle = new wxButton(this, wxID_ANY, _L("一键试色"));
        row->Add(m_toggle, 0, wxRIGHT, FromDIP(6));
        m_source = new wxChoice(this, wxID_ANY);
        m_source->Append(_L("自动建议"));
        m_source->Append(_L("当前工程耗材"));
        m_source->Append(_L("手动试色"));
        m_packs = load_filament_color_packs();
        for (const auto& pack : m_packs) m_source->Append(wxString::FromUTF8(pack.name));
        m_source->SetSelection(0);
        row->Add(m_source, 1, wxRIGHT, FromDIP(6));
        m_count = new wxSpinCtrl(this, wxID_ANY, "6", wxDefaultPosition, wxSize(FromDIP(60), -1), wxSP_ARROW_KEYS, 1, 6, 6);
        m_count->SetName(_L("试色数量，最多六色"));
        row->Add(m_count, 0, wxRIGHT, FromDIP(4));
        row->Add(new wxStaticText(this, wxID_ANY, _L("色")), 0, wxALIGN_CENTER_VERTICAL);
        box->Add(row, 0, wxEXPAND | wxALL, FromDIP(6));
        auto* options = new wxBoxSizer(wxHORIZONTAL);
        m_fidelity = new wxCheckBox(this, wxID_ANY, _L("尽量保留小面积彩色"));
        m_fidelity->SetValue(true);
        m_fidelity->SetToolTip(_L("缩减颜色时，尽量保留有一定面积的少数彩色，例如嘴唇红、衣服绿。不是自动识别人脸或衣物；取消后更偏向大面积颜色。"));
        options->Add(m_fidelity, 1, wxALIGN_CENTER_VERTICAL);
        auto* reset = new wxButton(this, wxID_ANY, _L("重置试色"));
        options->Add(reset, 0);
        box->Add(options, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
        auto* pack_button = new wxButton(this, wxID_ANY, _L("应用 / 保存耗材包…"));
        box->Add(pack_button, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
        pack_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            const bool applied = show_filament_color_packs(this);
            const wxString selected = m_source->GetStringSelection();
            while (m_source->GetCount() > 3) m_source->Delete(3);
            m_packs = load_filament_color_packs();
            for (const auto& pack : m_packs) m_source->Append(wxString::FromUTF8(pack.name));
            m_source->SetStringSelection(selected);
            if (!applied) return;
            read_project(); m_source->SetSelection(1);
            m_notice = _L("已应用工程色卡；原有涂色范围保留，当前显示实际耗材配色。");
            recompute();
        });
        m_lighting = new wxCheckBox(this, wxID_ANY, _L("光照立体展示"));
        m_lighting->SetValue(false);
        m_lighting->SetToolTip(_L("默认用纯色色块检查分色。开启后添加光照明暗，屏幕上会看到更多深浅，但基础颜色数量不变。"));
        box->Add(m_lighting, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
        auto* swatches = new wxBoxSizer(wxHORIZONTAL);
        for (size_t i = 0; i < 6; ++i) {
            auto* col = new wxBoxSizer(wxVERTICAL);
            m_swatches[i] = new wxButton(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(64), FromDIP(26)), wxBU_EXACTFIT);
            m_swatches[i]->SetName("ai_content_color");
            m_locks[i] = new wxCheckBox(this, wxID_ANY, _L("保留"));
            col->Add(m_swatches[i], 0, wxEXPAND);
            col->Add(m_locks[i], 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(3));
            swatches->Add(col, 1, wxRIGHT, FromDIP(4));
            m_swatches[i]->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { edit_color(i); });
            m_locks[i]->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { recompute(); });
        }
        box->Add(swatches, 0, wxEXPAND | wxALL, FromDIP(6));
        m_status = new wxStaticText(this, wxID_ANY, "");
        box->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
        SetSizer(box);
        m_toggle->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_source->GetSelection() == 1 && read_project()) {
                m_notice = _L("工程耗材已变化，已恢复原色，请重新对照。");
                recompute(false); return;
            }
            if (m_colors.empty()) return;
            m_enabled = !m_enabled; changed();
        });
        m_source->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            for (auto* lock : m_locks) lock->SetValue(false);
            m_notice.clear();
            if (m_source->GetSelection() == 1) read_project();
            recompute();
        });
        m_count->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { recompute(); });
        m_fidelity->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { recompute(); });
        m_lighting->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { changed(); });
        reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            m_source->SetSelection(0); m_count->SetValue(6); m_fidelity->SetValue(true);
            m_lighting->SetValue(false);
            for (auto* lock : m_locks) lock->SetValue(false);
            m_notice.clear(); recompute();
        });
        Bind(wxEVT_IDLE, [this](wxIdleEvent& event) {
            const auto now = std::chrono::steady_clock::now();
            if (IsShownOnScreen() && m_histogram && m_source->GetSelection() == 1 &&
                now - m_last_check > std::chrono::seconds(1)) {
                m_last_check = now;
                if (read_project()) {
                    m_enabled = false;
                    m_notice = _L("工程耗材已变化，已恢复原色，请重新对照。");
                    recompute(false);
                }
            }
            event.Skip();
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { wrap_status(); event.Skip(); });
    }
    void load(std::shared_ptr<const PreviewPalette::Histogram> histogram, std::vector<Color> colors) {
        m_histogram = std::move(histogram); m_colors = std::move(colors); m_mapping_colors = m_colors;
        m_enabled = false; m_source->SetSelection(0); m_count->SetValue(6); m_fidelity->SetValue(true);
        m_lighting->SetValue(false);
        m_notice.clear();
        for (auto* lock : m_locks) lock->SetValue(false);
        update();
    }
    void clear() { m_histogram.reset(); m_colors.clear(); m_mapping_colors.clear(); m_enabled = false; Hide(); }
    const std::vector<Color>& colors() const { return m_colors; }
    const std::vector<Color>& mapping_colors() const { return m_mapping_colors; }
    bool enabled() const { return m_enabled; }
    bool lighting() const { return m_lighting->GetValue(); }
    struct State : AI::ColorTrialPersistence::State {
        wxString project_signature, notice;
    };
    State state() const {
        State saved;
        saved.colors = m_colors; saved.mapping_colors = m_mapping_colors;
        for (size_t i = 0; i < 6; ++i) saved.locks[i] = m_locks[i]->GetValue();
        saved.source = m_source->GetSelection(); saved.count = m_count->GetValue();
        saved.enabled = m_enabled; saved.fidelity = m_fidelity->GetValue(); saved.lighting = lighting();
        saved.project_signature = m_project_signature; saved.notice = m_notice;
        return saved;
    }
    // Same-workpiece comparison/editing keeps the user's assignments. A new
    // library model still uses load() and starts with its own suggestions.
    void restore(const State& saved) {
        if (!m_histogram || saved.colors.empty() || saved.colors.size() > 6 ||
            saved.colors.size() != saved.mapping_colors.size()) return;
        m_source->SetSelection(saved.source); m_count->SetValue(saved.count);
        m_fidelity->SetValue(saved.fidelity); m_lighting->SetValue(saved.lighting);
        m_colors = saved.colors; m_mapping_colors = saved.mapping_colors;
        m_enabled = saved.enabled; m_notice = saved.notice;
        for (size_t i = 0; i < 6; ++i) m_locks[i]->SetValue(saved.locks[i]);
        if (saved.source == 1) {
            m_project_signature = saved.project_signature;
            if (read_project()) {
                m_notice = _L("工程耗材已变化，已恢复原色，请重新对照。");
                recompute(false); return;
            }
        }
        changed();
    }
    std::function<void()> on_changed;
private:
    static wxColour wx_color(Color c) {
        return wxColour(int(std::lround(c[0]*255)), int(std::lround(c[1]*255)), int(std::lround(c[2]*255)));
    }
    // Same source as the native textured import: logical project slots. Existing
    // mixed recipes are excluded, never misreported as physical loaded materials.
    bool read_project() {
        std::vector<Color> colors; std::vector<wxString> names;
        wxString signature, error;
        auto* bundle = wxGetApp().preset_bundle;
        if (bundle) {
            const auto* values = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
            const auto* mixed = bundle->project_config.option<ConfigOptionBools>("filament_is_mixed");
            signature = wxString::FromUTF8(bundle->printers.get_edited_preset().name);
            for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
                const bool is_mixed = mixed && i < mixed->values.size() && mixed->values[i];
                const std::string hex = values && i < values->values.size() ? values->values[i] : "";
                signature += wxString::Format("|%u:%d:", unsigned(i), int(is_mixed)) + wxString::FromUTF8(hex) + wxString::FromUTF8(bundle->filament_presets[i]);
                if (is_mixed) continue;
                const wxColour color(wxString::FromUTF8(hex));
                if (hex.size() != 7 || hex.front() != '#' || !color.IsOk()) { error = _L("工程存在未配置的耗材颜色，请到准备页补全。"); continue; }
                colors.push_back({color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f});
                names.push_back(wxString::Format(_L("工程耗材 %u · "), unsigned(i + 1)) + wxString::FromUTF8(bundle->filament_presets[i]));
            }
        }
        if (colors.size() > 6) error = _L("工程有超过六种实体耗材，请在准备页选定最多六色后再对照。");
        if (colors.empty() && error.empty()) error = _L("尚未读取到实体耗材，请在准备页配置，或选择手动试色。");
        if (!error.empty()) colors.clear();
        const bool different = signature != m_project_signature;
        m_project_signature = signature; m_project_colors = std::move(colors); m_project_names = std::move(names); m_project_error = error;
        return different;
    }
    void recompute(bool enable = true) {
        if (!m_histogram) return;
        const auto started = std::chrono::steady_clock::now();
        if (m_notice == _L("已保留锁定颜色；如需更少颜色，请先取消部分保留。")) m_notice.clear();
        const int source = m_source->GetSelection();
        std::vector<Color> locked;
        for (size_t i = 0; i < m_colors.size(); ++i) if (m_locks[i]->GetValue()) locked.push_back(m_colors[i]);
        if (source == 1) {
            m_colors = m_project_colors;
            m_mapping_colors = m_colors;
            if (is_portrait_card(m_colors)) apply_portrait_mapping();
            if (!m_colors.empty()) m_count->SetValue(int(m_colors.size()));
        } else if (source >= 3 && size_t(source - 3) < m_packs.size()) {
            m_colors.clear();
            for (const auto& hex : m_packs[source - 3].colors) {
                const wxColour color(wxString::FromUTF8(hex));
                m_colors.push_back({color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f});
            }
            m_mapping_colors = m_colors; m_count->SetValue(int(m_colors.size()));
            m_notice = _L("色卡仅用于试色；点击“应用 / 保存耗材包”可切换工程耗材。");
            if (m_packs[source - 3].name == young_portrait_color_pack().name) apply_portrait_mapping();
        } else if (source == 0) {
            if (locked.size() > size_t(m_count->GetValue())) {
                m_count->SetValue(int(locked.size()));
                m_notice = _L("已保留锁定颜色；如需更少颜色，请先取消部分保留。");
            }
            m_colors = m_histogram->palette(size_t(m_count->GetValue()), locked, m_fidelity->GetValue());
            m_mapping_colors = m_colors;
            for (size_t i = 0; i < 6; ++i) m_locks[i]->SetValue(i < m_colors.size() &&
                std::find(locked.begin(), locked.end(), m_colors[i]) != locked.end());
        } else {
            const auto suggestions = m_histogram->palette(6, {}, true);
            while (m_colors.size() < size_t(m_count->GetValue())) {
                const Color suggestion = suggestions.empty() ? Color{.5f,.5f,.5f} : suggestions[m_colors.size() % suggestions.size()];
                m_colors.push_back(suggestion); m_mapping_colors.push_back(suggestion);
            }
            m_colors.resize(size_t(m_count->GetValue()));
            m_mapping_colors.resize(m_colors.size());
        }
        m_enabled = enable && !m_colors.empty(); changed();
        BOOST_LOG_TRIVIAL(info) << "AI protected color palette: source=" << source << ", colors=" << m_colors.size()
            << ", locks=" << locked.size() << ", elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
    }
    void edit_color(size_t i) {
        if (i >= m_colors.size()) return;
        wxColourData data; data.SetColour(wx_color(m_colors[i])); data.SetChooseFull(true);
        wxColourDialog dialog(this, &data);
        dialog.SetTitle(_L("修改试色颜色（不改工程耗材）"));
        if (dialog.ShowModal() != wxID_OK) return;
        const wxColour color = dialog.GetColourData().GetColour();
        m_colors[i] = {color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f};
        // Preserve the source assignment when a target color is edited. Editing
        // green to blue must recolor that group, not make the blue target unused.
        m_source->SetSelection(2);
        m_notice = _L("已转为手动试色，保留各色对应范围；工程耗材未修改。");
        m_locks[i]->SetValue(true); recompute();
    }
    void changed() { update(); if (on_changed) on_changed(); }
    static bool is_portrait_card(const std::vector<Color>& colors) {
        const auto card = young_portrait_color_pack();
        if (colors.size() != card.colors.size()) return false;
        for (size_t i = 0; i < colors.size(); ++i)
            if (wx_color(colors[i]).GetAsString(wxC2S_HTML_SYNTAX).CmpNoCase(wxString::FromUTF8(card.colors[i])) != 0) return false;
        return true;
    }
    void apply_portrait_mapping() {
        const auto mapping = PreviewPalette::portrait_pack_mapping(m_histogram->palette(6, {}, true), m_colors);
        if (!mapping.enabled) return;
        m_mapping_colors = mapping.mapping_colors; m_colors = mapping.target_colors;
        m_count->SetValue(int(m_colors.size()));
        m_notice = _L("人物包按原颜色组建议换色，可点色块调整。不是人脸或衣物识别；局部修改请到 3D 美颜。");
    }
    void wrap_status() { m_status->Wrap(std::max(FromDIP(220), GetClientSize().x - FromDIP(12))); }
    void update() {
        const int source = m_source->GetSelection();
        Show(bool(m_histogram));
        m_toggle->Enable(!m_colors.empty());
        m_toggle->SetLabel(m_enabled ? _L("查看原色") : wxString::Format(_L("预览 %u 色"), unsigned(m_colors.size())));
        m_count->Enable(source == 0 || source == 2); m_fidelity->Enable(source == 0);
        m_lighting->Enable(m_enabled);
        for (size_t i = 0; i < 6; ++i) {
            const bool visible = i < m_colors.size();
            m_swatches[i]->Show(visible); m_locks[i]->Show(visible && source == 0);
            if (!visible) continue;
            const wxColour color = wx_color(m_colors[i]);
            const wxString hex = color.GetAsString(wxC2S_HTML_SYNTAX);
            m_swatches[i]->SetLabel(hex); m_swatches[i]->SetBackgroundColour(color);
            m_swatches[i]->SetForegroundColour((color.Red()*299 + color.Green()*587 + color.Blue()*114 > 145000) ? *wxBLACK : *wxWHITE);
            const auto project_color = std::find(m_project_colors.begin(), m_project_colors.end(), m_colors[i]);
            const size_t project_index = size_t(std::distance(m_project_colors.begin(), project_color));
            m_swatches[i]->SetToolTip((source == 1 && project_index < m_project_names.size() ? m_project_names[project_index] + " · " : "") +
                (source != 0 ? wx_color(m_mapping_colors[i]).GetAsString(wxC2S_HTML_SYNTAX) + " → " : "") + hex + _L(" · 点击改为试色颜色"));
            m_locks[i]->SetToolTip(_L("固定此颜色，重新推荐时不被其他颜色合并。") + hex);
        }
        wxString text = m_enabled ? (lighting()
            ? wxString::Format(_L("立体展示 · 最多 %u 种基础色；光照会增加明暗深浅。"), unsigned(m_colors.size()))
            : wxString::Format(_L("纯分色 · 最多 %u 种色块，与所选色卡一致，无光照明暗。"), unsigned(m_colors.size())))
            : _L("原色显示 · ");
        text += source == 1 ? _L("使用工程耗材色卡。") : source == 2 ? _L("手动试色。") : source >= 3 ? _L("耗材包试色。") : _L("可点击改色并勾选保留。");
        text += _L("仅供配色对照，不代表实际打印效果；");
        text += m_enabled ? _L("试色方案将带入导入配色，最终按耗材确认。") : _L("导入默认保留关键色分组，最终按耗材确认。");
        if (source == 1 && !m_project_error.empty()) text += "\n" + m_project_error;
        if (!m_notice.empty()) text += "\n" + m_notice;
        m_status->SetLabel(text); wrap_status(); Layout();
        // Controls must not consume the model viewport's existing minimum height.
        GetParent()->SetMinSize(wxSize(FromDIP(420), FromDIP(300) + GetSizer()->CalcMin().y));
        GetParent()->Layout();
    }
    std::vector<FilamentColorPack> m_packs;
    wxButton* m_toggle;
    wxChoice* m_source;
    wxSpinCtrl* m_count;
    wxCheckBox* m_fidelity;
    wxCheckBox* m_lighting;
    wxStaticText* m_status;
    std::array<wxButton*, 6> m_swatches {};
    std::array<wxCheckBox*, 6> m_locks {};
    std::shared_ptr<const PreviewPalette::Histogram> m_histogram;
    std::vector<Color> m_colors, m_project_colors, m_mapping_colors;
    std::vector<wxString> m_project_names;
    wxString m_notice, m_project_error, m_project_signature;
    bool m_enabled {false};
    std::chrono::steady_clock::time_point m_last_check {};
};
} // namespace Slic3r::GUI
