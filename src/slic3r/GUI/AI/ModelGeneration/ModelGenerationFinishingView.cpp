#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/notebook.h>
#include <wx/msgdlg.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/weakref.h>
#include <wx/wrapsizer.h>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;
namespace {
// Native wrapping may keep an entire CJK sentence as one word. Measure the
// displayed text so a narrow tool panel never truncates its instructions.
void wrap_workbench_text(wxStaticText* label, int width)
{
    wxString source = label->GetLabel(), line, result;
    source.Replace("\n", "");
    int lines = 1;
    for (wxUniChar character : source) {
        wxString next = line; next += character;
        if (!line.empty() && label->GetTextExtent(next).x > width) {
            result += line + "\n"; line.clear(); ++lines;
        }
        line += character;
    }
    label->SetLabel(result + line);
    label->SetMinSize(wxSize(1, lines * label->GetCharHeight() + 2));
}
}

void ModelGenerationPanel::on_discard(wxCommandEvent&)
{
    if (m_busy || m_finishing_running || !m_finishing_candidate.empty()) return;
    if (m_ready || m_model_preview_ready || m_style_preview_ready) {
        wxMessageDialog choice(this,
            _L("要先看看重新设计的建议吗？当前历史模型会保留，图片和描述可继续使用。"),
            _L("重新开始"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
        choice.SetYesNoCancelLabels(_L("直接重新开始"), _L("先看建议"), _L("留在当前作品"));
        const int answer = choice.ShowModal();
        if (answer == wxID_CANCEL) return;
        if (answer == wxID_NO) {
            wxString advice;
            if (m_model_refinement.available && !m_model_refinement.summary.empty())
                advice = _L("已有检查建议：\n") + wxString::FromUTF8(m_model_refinement.summary.c_str());
            else
                advice = _L("尚无这件作品的检查结论。可先按下面的方向对照原图：\n\n"
                    "• 人物不像：使用清晰正面参考，描述中明确五官、发型和姿态。\n"
                    "• 形体不完整：确认图片主体完整、遮挡较少，明确底座和连接部位。\n"
                    "• 颜色杂乱：先在3D美颜里试六色、保留嘴唇和服装等关键色。\n"
                    "• 表面小凹凸：先试局部美颜，通常不需要重新生成。\n\n"
                    "这些是设计建议，未运行新的AI分析。");
            wxMessageDialog guidance(this, advice, _L("重新设计建议"), wxOK | wxCANCEL | wxICON_INFORMATION);
            guidance.SetOKCancelLabels(_L("继续重新开始"), _L("返回调整作品"));
            if (guidance.ShowModal() != wxID_OK) return;
        }
    }
    const bool reuse_palette = m_palette_source->GetSelection() == 2 && !current_palette().empty();
    boost::system::error_code reference_error;
    if (!m_reference_image_path.empty() && boost::filesystem::is_regular_file(m_reference_image_path, reference_error)) {
        m_selected_image_path = m_reference_image_path;
        m_selected_image->SetLabel(wxString(m_selected_image_path.filename().wstring()));
    } else if (m_library_model_loaded) {
        m_selected_image_path.clear();
        m_selected_image->SetLabel(_L("未选择图片"));
    }
    if (m_finishing_workbench) set_finishing_workbench(false);
    reset(!m_ready);
    m_palette_recommendation_confirmed = reuse_palette;
    refresh_controls();
    m_prompt->SetFocus();
}

wxWindow* ModelGenerationPanel::build_model_finishing(wxWindow* parent)
{
    auto* scroll = new wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(320), FromDIP(480)), wxVSCROLL | wxBORDER_SIMPLE);
    scroll->SetMinSize(wxSize(FromDIP(320), FromDIP(400)));
    scroll->SetScrollRate(0, FromDIP(12));
    m_finishing_panel = scroll;
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(m_finishing_panel, wxID_ANY, _L("3D 美颜工作台"));
    title->SetFont(wxGetApp().bold_font());
    sizer->Add(title, 0, wxALL, FromDIP(10));
    auto* hint = new wxStaticText(m_finishing_panel, wxID_ANY,
        _L("Alt＋左键旋转，右键平移，滚轮缩放。原件保留，修改可撤销。"));
    wrap_workbench_text(hint, FromDIP(260));
    sizer->Add(hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_tool = new wxChoice(m_finishing_panel, wxID_ANY);
    for (const auto& label : {_L("整体美颜"), _L("局部修整"), _L("多色试色"), _L("网格修复"), _L("统一这块颜色"), _L("清理小杂点")})
        m_finishing_tool->Append(label);
    m_finishing_tool->SetSelection(0);
    sizer->Add(m_finishing_tool, 0, wxEXPAND | wxALL, FromDIP(10));
    m_finishing_gray = new wxCheckBox(m_finishing_panel, wxID_ANY, _L("灰模观察凹凸（仅显示）"));
    sizer->Add(m_finishing_gray, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_gray->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { m_model_preview->set_gray_view(m_finishing_gray->GetValue()); });
    m_finishing_selection_controls = new wxPanel(m_finishing_panel);
    auto* selection = new wxBoxSizer(wxVERTICAL);
    auto* selection_hint = new wxStaticText(m_finishing_selection_controls, wxID_ANY,
        _L("圈选当前可见表面，再涂抹补选或保护细节。橙色参与处理，蓝色受保护；不穿透背面。"));
    wrap_workbench_text(selection_hint, FromDIP(260));
    selection->Add(selection_hint, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_finishing_selection_operation = new wxChoice(m_finishing_selection_controls, wxID_ANY);
    for (const auto& label : {_L("圈选要修改的范围"), _L("涂抹补选"), _L("涂抹保护"), _L("点选相近颜色"), _L("转动模型")})
        m_finishing_selection_operation->Append(label);
    m_finishing_selection_operation->SetSelection(0);
    selection->Add(m_finishing_selection_operation, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    selection->Add(new wxStaticText(m_finishing_selection_controls, wxID_ANY, _L("笔刷大小")), 0);
    m_finishing_radius = new wxSlider(m_finishing_selection_controls, wxID_ANY, 3, 1, 10);
    selection->Add(m_finishing_radius, 0, wxEXPAND);
    m_finishing_selection_status = new wxStaticText(m_finishing_selection_controls, wxID_ANY, _L("尚未选区 · 点击模型开始"));
    selection->Add(m_finishing_selection_status, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    auto* selection_actions = new wxBoxSizer(wxHORIZONTAL);
    auto* undo_selection = new wxButton(m_finishing_selection_controls, wxID_ANY, _L("撤销选区"));
    auto* clear_selection = new wxButton(m_finishing_selection_controls, wxID_ANY, _L("清空选区"));
    selection_actions->Add(undo_selection, 0, wxRIGHT, FromDIP(6));
    selection_actions->Add(clear_selection);
    selection->Add(selection_actions);
    auto* redo_selection = new wxButton(m_finishing_selection_controls, wxID_ANY, _L("重做选区"));
    selection->Add(redo_selection, 0, wxTOP, FromDIP(6));
    redo_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_model_preview->redo_selection(); });
    auto* refine_selection = new wxButton(m_finishing_selection_controls, wxID_ANY, _L("贴合选区边界"));
    refine_selection->SetToolTip(_L("圈选后，在要修改处涂抹补选、在要保留处涂抹保护，再沿颜色和表面边界修正。不会扩大到范围之外。"));
    selection->Add(refine_selection, 0, wxEXPAND | wxTOP, FromDIP(6));
    refine_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_model_preview->refine_selection_boundary(); });
    auto* focus_selection = new wxButton(m_finishing_selection_controls, wxID_ANY, _L("放大选区（F）"));
    focus_selection->SetToolTip(_L("将选中的区域放到画面中央；“完整显示模型”可恢复全貌。"));
    selection->Add(focus_selection, 0, wxEXPAND | wxTOP, FromDIP(8));
    auto* show_selection = m_finishing_overlay = new wxCheckBox(m_finishing_panel, wxID_ANY, _L("显示选区高亮"));
    show_selection->SetValue(true);
    show_selection->SetToolTip(_L("取消勾选可看清选区内原本的颜色和细节；选区仍然有效。"));
    focus_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_model_preview->focus_selection()) {
            m_finishing_status->SetLabel(_L("请先点选模型上的区域，再放大查看。"));
            refresh_model_finishing();
        }
    });
    show_selection->Bind(wxEVT_CHECKBOX, [this, show_selection](wxCommandEvent&) {
        m_model_preview->set_selection_overlay_visible(show_selection->GetValue());
    });
    m_finishing_selection_controls->SetSizer(selection);
    sizer->Add(m_finishing_selection_controls, 0, wxEXPAND | wxALL, FromDIP(10));
    sizer->Add(show_selection, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_selection_operation->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { update_finishing_selection(); });
    m_finishing_radius->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { update_finishing_selection(); });
    undo_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_model_preview->undo_selection(); });
    clear_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_model_preview->clear_selection(); });
    m_finishing_preset = new wxChoice(m_finishing_panel, wxID_ANY);
    for (const auto& label : {_L("轻柔 · 保留细节"), _L("均衡 · 表面柔化"), _L("加强 · 平滑凹凸"), _L("仅修复 · 保留造型")})
        m_finishing_preset->Append(label);
    m_finishing_preset->SetSelection(0);
    sizer->Add(m_finishing_preset, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_smooth = new wxCheckBox(m_finishing_panel, wxID_ANY, _L("表面美化（保护边界和锐边）"));
    m_finishing_smooth->SetValue(true);
    sizer->Add(m_finishing_smooth, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_strength = new wxSlider(m_finishing_panel, wxID_ANY, 15, 0, 100,
        wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
    m_finishing_cleanup_hint = new wxStaticText(m_finishing_panel, wxID_ANY,
        _L("先圈住杂点及周围主色，保护眼睛、花纹等细节。只合并孤立小色块；连续条带可用“统一这块颜色”。"));
    wrap_workbench_text(m_finishing_cleanup_hint, FromDIP(260));
    sizer->Add(m_finishing_cleanup_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    // Native wxSL_LABELS creates sibling labels that overlap after tool panels
    // are shown/hidden. Keep the value in our sizer with the rest of the form.
    m_finishing_strength_value = new wxStaticText(m_finishing_panel, wxID_ANY, _L("处理强度：15%"));
    sizer->Add(m_finishing_strength_value, 0, wxLEFT | wxRIGHT, FromDIP(10));
    m_finishing_strength->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        m_finishing_strength_value->SetLabel(wxString::Format(_L("处理强度：%d%%"), m_finishing_strength->GetValue()));
    });
    m_finishing_strength->SetToolTip(_L("强度越高，柔化越明显。保护轮廓与细小结构；可随时调整并重新预览。"));
    sizer->Add(m_finishing_strength, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_repair = new wxCheckBox(m_finishing_panel, wxID_ANY, _L("网格清理与面朝向修复"));
    m_finishing_repair->SetValue(false);
    sizer->Add(m_finishing_repair, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    auto* actions = new wxBoxSizer(wxVERTICAL);
    auto button = [&](wxButton*& target, const wxString& label) {
        target = new wxButton(m_finishing_panel, wxID_ANY, label);
        actions->Add(target, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    };
    button(m_finishing_preview, _L("预览处理效果"));
    button(m_finishing_compare, _L("查看处理前"));
    button(m_finishing_accept, _L("接受并保存新版本"));
    button(m_finishing_discard, _L("放弃预览"));
    button(m_finishing_undo, _L("返回上个版本"));
    button(m_finishing_redo, _L("重做已保存修整"));
    button(m_finishing_cancel, _L("取消处理"));
    sizer->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
    m_finishing_status = new wxStaticText(m_finishing_panel, wxID_ANY, _L("轻柔处理小凹凸，保留人物特征。松开强度滑块后预览；处理可取消。"));
    wrap_workbench_text(m_finishing_status, FromDIP(260));
    sizer->Add(m_finishing_status, 0, wxEXPAND | wxALL, FromDIP(10));
    m_finishing_panel->SetSizer(sizer);
    m_finishing_preview->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { preview_model_finishing(); });
    m_finishing_accept->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { accept_model_finishing(); });
    m_finishing_discard->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { discard_model_finishing(); });
    m_finishing_undo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { undo_model_finishing(); });
    m_finishing_redo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { redo_model_finishing(); });
    m_finishing_strength->Bind(wxEVT_SCROLL_THUMBRELEASE, [this](wxScrollEvent&) {
        if (!m_busy && m_finishing_workbench && (m_finishing_tool->GetSelection() < 2 || m_finishing_tool->GetSelection() == 5)) preview_model_finishing();
    });
    m_finishing_tool->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int tool = m_finishing_tool->GetSelection();
        m_finishing_smooth->SetValue(tool != 3 && tool != 5);
        m_finishing_repair->SetValue(tool == 3);
        if (tool == 5) {
            m_finishing_strength->SetValue(35);
        }
        // Color tools must show the surface colors, even when the previous
        // geometry tool was inspected as a gray model.
        if (tool == 2 || tool == 4 || tool == 5) {
            m_finishing_gray->SetValue(false);
            m_model_preview->set_gray_view(false);
        }
        m_finishing_status->SetLabel(tool == 2 ? _L("在模型下方试色，可切回原色。选定方案会带入导入配色，最终按实际耗材确认。")
            : tool == 5 ? _L("适用于衣物、头发、皮肤、底座等区域。先保护需要保留的细节，再预览清理结果。")
            : tool == 4 ? _L("点选模型，再用右侧工具换色。改色保存为新版本，原件保留。")
            : tool == 1 ? _L("先点选局部区域，再调整强度。边缘渐变柔化，未选区域保持原样。")
            : tool == 3 ? _L("清理重复／退化面并校正面朝向；不自动补洞或删除部件。")
            : _L("保守柔化整体小凹凸；按当前强度预览后可对比。"));
        refresh_local_recolor_controls(); update_finishing_selection(); refresh_model_finishing();
    });
    m_finishing_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_finishing_canceled) m_finishing_canceled->store(true);
        m_finishing_status->SetLabel(_L("正在取消，本次处理不会替换当前模型。"));
        m_finishing_cancel->Disable();
    });
    auto compare = [this](wxCommandEvent&) {
        if (m_finishing_candidate.empty() || m_busy) return;
        if (show_finishing_version(m_finishing_before ? m_finishing_candidate : m_finishing_source)) {
            m_finishing_before = !m_finishing_before;
            m_finishing_compare->SetLabel(m_finishing_before ? _L("查看处理后") : _L("查看处理前"));
            m_finishing_compare_model->SetLabel(m_finishing_compare->GetLabel());
            m_model_preview_message->SetLabel(m_finishing_before
                ? _L("处理前 · 当前已保存版本") : _L("处理后 · 尚未接受"));
            refresh_model_finishing();
        }
    };
    m_finishing_compare->Bind(wxEVT_BUTTON, compare);
    m_finishing_compare_model->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
        if (m_busy || m_finishing_candidate.empty() || m_finishing_before) return;
        if (show_finishing_version(m_finishing_source)) {
            m_finishing_compare_held = true;
            m_finishing_compare_model->CaptureMouse();
            m_model_preview_message->SetLabel(_L("处理前 · 松开恢复处理后"));
        }
    });
    auto release_compare = [this] {
        if (!m_finishing_compare_held) return;
        m_finishing_compare_held = false;
        if (m_finishing_compare_model->HasCapture()) m_finishing_compare_model->ReleaseMouse();
        if (!m_finishing_candidate.empty()) show_finishing_version(m_finishing_candidate);
        m_model_preview_message->SetLabel(_L("处理后 · 尚未保存"));
    };
    m_finishing_compare_model->Bind(wxEVT_LEFT_UP, [release_compare](wxMouseEvent&) { release_compare(); });
    m_finishing_compare_model->Bind(wxEVT_MOUSE_CAPTURE_LOST, [release_compare](wxMouseCaptureLostEvent&) { release_compare(); });
    m_finishing_smooth->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { refresh_model_finishing(); });
    m_finishing_preset->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int preset = m_finishing_preset->GetSelection();
        m_finishing_smooth->SetValue(preset != 3);
        m_finishing_repair->SetValue(preset == 3 && m_finishing_tool->GetSelection() != 1);
        m_finishing_strength->SetValue(preset == 0 ? 15 : preset == 2 ? 65 : 35);
        refresh_model_finishing();
    });
    m_finishing_panel->Bind(wxEVT_SIZE, [this, hint](wxSizeEvent& event) {
        const int width = std::clamp(m_finishing_panel->GetClientSize().x - FromDIP(24), FromDIP(200), FromDIP(280));
        wrap_workbench_text(hint, width); wrap_workbench_text(m_finishing_status, width); event.Skip();
    });
    for (wxWindow* child : m_finishing_panel->GetChildren())
        child->SetMaxSize(wxSize(FromDIP(280), -1));
    for (wxWindow* child : m_finishing_selection_controls->GetChildren())
        child->SetMaxSize(wxSize(FromDIP(260), -1));
    m_finishing_panel->Hide();
    return m_finishing_panel;
}

void ModelGenerationPanel::set_finishing_workbench(bool enabled)
{
    m_finishing_workbench = enabled;
    if (enabled && m_preview_book) { m_preview_book->SetSelection(0); show_model_comparison(); }
    m_workflow_panel->Show(!enabled);
    m_preview_area->Show(!enabled);
    m_expand_images->SetValue(false);
    m_expand_images->Show(!enabled);
    m_zoom_out->Show(!enabled); m_zoom_in->Show(!enabled);
    m_zoom_fit->Show(!enabled); m_preview_zoom->Show(!enabled);
    m_finishing_shortcut->SetLabel(enabled ? _L("返回作品／导入") : _L("3D 美颜工作台"));
    m_preview_stage_hint->Show(!enabled);
    m_preview_message->Show(!enabled);
    m_result_summary->Show(!enabled);
    m_preview_kind->SetLabel(enabled ? _L("美颜工作台") : _L("结果对照"));
    if (enabled) m_local_recolor_toggle->SetValue(false);
    m_model_decision_panel->Hide();
    refresh_local_recolor_controls();
    if (!enabled) { m_finishing_gray->SetValue(false); m_model_preview->set_gray_view(false); }
    update_finishing_selection();
    refresh_model_finishing();
    Layout();
    m_comparison_panel->Layout();
    m_model_page->Layout(); m_model_page->FitInside(); m_model_page->Scroll(0, 0);
}

void ModelGenerationPanel::update_finishing_selection()
{
    const bool local = m_finishing_workbench && (m_finishing_tool->GetSelection() == 1 || m_finishing_tool->GetSelection() == 4 || m_finishing_tool->GetSelection() == 5);
    m_model_preview->set_selection_enabled(local && !m_busy && m_finishing_candidate.empty());
    if (!local) return;
    m_finishing_selection_status->SetLabel(wxString::Format(_L("已选 %llu 个面 · 保护 %llu 个面"),
        static_cast<unsigned long long>(m_model_preview->selected_face_count()),
        static_cast<unsigned long long>(m_model_preview->protected_face_count())));
    update_region_mode();
}

void ModelGenerationPanel::refresh_model_finishing()
{
    if (!m_finishing_panel) return;
    if (!m_finishing_undo_path.empty() && m_displayed_model_path != m_finishing_accepted_path) {
        m_finishing_undo_path.clear(); m_finishing_accepted_path.clear();
    }
    if (!m_finishing_running && !m_finishing_candidate.empty() && m_displayed_model_path != m_finishing_source) {
        // Selecting a different library model invalidates only the unaccepted
        // local preview, never the selected file or its history record.
        boost::system::error_code ignored;
        if (m_displayed_model_path != m_finishing_candidate)
            boost::filesystem::remove(m_finishing_candidate, ignored);
        m_finishing_candidate.clear(); m_finishing_source.clear(); m_finishing_undo_path.clear();
    }
    const bool pending = !m_finishing_candidate.empty();
    const bool ready = m_model_preview_ready && is_nonempty_model(m_displayed_model_path);
    m_finishing_panel->Show(m_finishing_workbench && (ready || m_finishing_running || pending));
    const bool editable = ready && !m_busy;
    const int tool = m_finishing_tool->GetSelection();
    const bool cleanup = tool == 5;
    const bool local = tool == 1 || cleanup || tool == 4;
    const bool color = tool == 2 || tool == 4;
    m_model_preview->set_color_controls_visible(!m_finishing_workbench || tool == 2);
    m_finishing_selection_controls->Show(local || tool == 4);
    m_finishing_overlay->Show(local || tool == 4);
    m_finishing_overlay->Enable(editable && !pending);
    m_finishing_selection_controls->Enable(editable && !pending);
    m_finishing_tool->Enable(editable && !pending);
    m_finishing_preset->Show(!color && tool != 3 && !cleanup);
    m_finishing_smooth->Show(!color && tool != 3 && !cleanup);
    m_finishing_strength->Show(!color && tool != 3);
    m_finishing_strength_value->Show(!color && tool != 3);
    m_finishing_strength_value->SetLabel(wxString::Format(_L("处理强度：%d%%"), m_finishing_strength->GetValue()));
    m_finishing_cleanup_hint->Show(cleanup);
    m_finishing_gray->Show(!color && !cleanup);
    m_finishing_strength->SetToolTip(cleanup
        ? _L("力度越大，可合并的杂色块越大。仅处理选区内部；不会自动识别五官、纽扣或花纹。")
        : _L("强度越高，柔化越明显。保护轮廓与细小结构；可随时调整并重新预览。"));
    m_finishing_repair->Show(tool == 3);
    m_finishing_compare_model->Show(pending);
    m_finishing_compare_model->Enable(editable);
    m_finishing_compare_model->SetLabel(m_finishing_before ? _L("当前为处理前") : _L("按住查看处理前"));
    m_finishing_compare_model->GetParent()->Layout();
    m_finishing_preview->Enable(editable);
    m_finishing_preview->SetLabel(pending ? _L("按当前强度重新预览") : cleanup ? _L("预览去杂效果") : _L("预览处理效果"));
    m_finishing_preset->Enable(editable);
    m_finishing_smooth->Enable(editable);
    m_finishing_repair->Enable(editable);
    m_finishing_strength->Enable(editable && (cleanup || m_finishing_smooth->GetValue()));
    for (wxButton* button : {m_finishing_compare, m_finishing_accept, m_finishing_discard}) {
        button->Show(pending); button->Enable(editable);
    }
    m_finishing_preview->Show(!m_finishing_running && (!color || (tool == 4 && pending)));
    m_finishing_cancel->Show(m_finishing_running);
    m_finishing_undo->Show(!m_finishing_undo_path.empty() && !pending && !m_finishing_running);
    m_finishing_undo->Enable(editable);
    if (!m_finishing_redo_path.empty() && m_displayed_model_path != m_finishing_redo_source)
        m_finishing_redo_path.clear();
    m_finishing_redo->Show(!m_finishing_redo_path.empty() && !pending && !m_finishing_running);
    m_finishing_redo->Enable(editable);
    if (pending || m_finishing_running) {
        m_status->SetLabel(m_finishing_running ? _L("正在本地处理，可切换页面或取消。")
            : _L("美颜预览就绪，接受新版本后可导入。"));
        m_import->Disable(); m_recheck_model->Disable(); m_visual_review_model->Disable();
        m_local_recolor_panel->Hide(); m_discard->Disable();
        m_preprocess->Disable(); m_generate->Disable();
        m_model_preview->set_selection_enabled(false);
        if (local) m_finishing_selection_status->SetLabel(wxString::Format(_L("本次处理 %llu 个面 · 未选区域受保护"),
            static_cast<unsigned long long>(m_finishing_options.selected_faces.size())));
    }
    if (m_finishing_running) m_stop->Hide();
    if (m_displayed_model_job_id.rfind("finish-", 0) == 0) {
        m_recheck_model->Disable(); m_visual_review_model->Disable();
        m_recheck_model->SetToolTip(_L("本地处理版本请导入准备页，检查实际打印条件。"));
    }
    wrap_workbench_text(m_finishing_status, FromDIP(260));
    m_finishing_panel->Layout();
    static_cast<wxScrolledWindow*>(m_finishing_panel)->FitInside();
    if (auto* page = dynamic_cast<wxScrolledWindow*>(m_finishing_panel->GetParent())) {
        page->Layout(); page->FitInside();
    }
}

void ModelGenerationPanel::preview_model_finishing()
{
    if (m_busy || m_shutdown || !m_model_preview_ready) return;
    if (m_model_preview->selection_busy()) {
        m_finishing_status->SetLabel(_L("正在更新选区，完成后即可预览；可按 Esc 取消选区计算。")); return;
    }
    const bool cleanup = m_finishing_tool->GetSelection() == 5;
    const bool recolor = m_finishing_tool->GetSelection() == 4;
    const bool local = m_finishing_tool->GetSelection() == 1 || cleanup || recolor;
    auto selected_faces = local ? (m_finishing_candidate.empty() ? m_model_preview->selected_face_indices()
        : m_finishing_options.selected_faces) : std::vector<size_t>{};
    if (local && selected_faces.empty()) {
        m_finishing_status->SetLabel(cleanup ? _L("请先圈住杂点及周围主色，涂抹保护花纹等细节；未选区域不会改变。")
            : recolor ? _L("请先圈选要换色的区域；涂抹保护可排除需要保留的细节。")
            : _L("请先在模型上选择要柔化的区域；未选区域不会改变。"));
        wrap_workbench_text(m_finishing_status, FromDIP(260));
        m_finishing_panel->Layout();
        return;
    }
    const auto source = m_displayed_model_path;
    if (!is_nonempty_model(source)) {
        m_finishing_status->SetLabel(_L("模型文件已不存在，请从模型库重新加载。")); return;
    }
    AI::ModelFinishingOptions options {!cleanup && !recolor && m_finishing_smooth->GetValue(), !local && m_finishing_repair->GetValue(), m_finishing_strength->GetValue() / 100.0};
    options.clean_color_spots = cleanup;
    options.recolor_selected = recolor;
    if (AI::model_artifact_format(source) == "glb" && (options.repair_mesh || cleanup)) {
        m_finishing_status->SetLabel(cleanup
            ? _L("GLB 的保真保存暂不支持清理杂点。可圈选后统一这块颜色，并保留原版用于对照。")
            : _L("为保留 GLB 原始贴图，请在这里选择表面柔化。需要修复网格时，可先导入准备页，再使用修复功能。"));
        wrap_workbench_text(m_finishing_status, FromDIP(260));
        m_finishing_panel->Layout();
        return;
    }
    if (recolor) {
        m_finishing_color_palette = local_recolor_palette();
        if (m_region_color_index < 0 || size_t(m_region_color_index) >= m_finishing_color_palette.size()) {
            m_finishing_status->SetLabel(_L("请先选择一个可用耗材颜色。")); return;
        }
        const wxColour target(from_u8(m_finishing_color_palette[m_region_color_index]));
        if (!target.IsOk()) return;
        options.target_color = {target.Red()/255.0f, target.Green()/255.0f, target.Blue()/255.0f, 1.0f};
    }
    if (cleanup) options.cleanup_palette = m_finishing_candidate.empty()
        ? m_model_preview->color_trial_mapping().mapping_colors : m_finishing_options.cleanup_palette;
    options.selected_faces = std::move(selected_faces);
    if (!options.smooth_surface && !options.repair_mesh && !options.clean_color_spots && !recolor) {
        m_finishing_status->SetLabel(_L("请至少选择表面美化或网格修复。")); return;
    }
    if (m_finishing_candidate.empty()) {
        const auto selection_state = m_model_preview->selection_state();
        m_finishing_selection_state = selection_state;
        m_finishing_restore_selection = [this, selection_state] { m_model_preview->restore_selection_state(selection_state); };
    }
    if (!m_finishing_candidate.empty()) {
        if (!m_finishing_before && !show_finishing_version(m_finishing_source)) return;
        boost::system::error_code ignored;
        boost::filesystem::remove(m_finishing_candidate, ignored);
        m_finishing_candidate.clear();
    }
    if (m_finishing_worker.joinable()) m_finishing_worker.join();
    const auto color_state = m_model_preview->color_trial_state();
    auto face_overrides = options.repair_mesh ? ModelPreview3D::FaceColorOverrides {} : m_model_preview->face_color_overrides();
    if (cleanup && !face_overrides.empty()) {
        std::unordered_set<size_t> locked;
        for (const auto& entry : face_overrides) locked.insert(entry.first);
        auto& faces = options.selected_faces;
        faces.erase(std::remove_if(faces.begin(), faces.end(), [&](size_t face) { return locked.count(face) != 0; }), faces.end());
        if (faces.empty()) {
            m_finishing_status->SetLabel(_L("选区均为已指定颜色的表面，清理杂点会保留这些颜色。可用“统一这块颜色”重新指定。"));
            refresh_controls(); return;
        }
    }
    if (recolor) {
        std::map<size_t, std::array<float,3>> colors(face_overrides.begin(), face_overrides.end());
        for (size_t face : options.selected_faces) {
            colors[face] = {options.target_color[0], options.target_color[1], options.target_color[2]};
        }
        face_overrides.assign(colors.begin(), colors.end());
    }
    const bool intent_changed = face_overrides != m_model_preview->face_color_overrides();
    m_finishing_candidate_face_overrides = face_overrides;
    m_finishing_options = options;
    m_finishing_redo_path.clear();
    m_finishing_source = source;
    m_finishing_source_context = [this, job = m_job_id, displayed = m_displayed_model_job_id,
        artifact = m_artifact_path, source, palette = m_job_palette, roles = m_job_palette_roles,
        display_palette = m_displayed_model_palette, display_roles = m_displayed_model_palette_roles,
        printable = m_job_use_printable_colors, manifest = m_color_intent_path,
        schema = m_color_intent_schema, hash = m_color_intent_sha256, format = m_artifact_format,
        encoding = m_artifact_color_encoding, quality = m_model_quality, visual = m_visual_quality,
        refinement = m_model_refinement, library = m_library_model_loaded, color_state] {
        m_job_id = job; m_displayed_model_job_id = displayed;
        m_artifact_path = artifact; m_displayed_model_path = source;
        m_job_palette = palette; m_job_palette_roles = roles;
        m_displayed_model_palette = display_palette; m_displayed_model_palette_roles = display_roles;
        m_job_use_printable_colors = printable; m_color_intent_path = manifest;
        m_color_intent_schema = schema; m_color_intent_sha256 = hash;
        m_artifact_format = format; m_artifact_color_encoding = encoding;
        m_model_quality = quality; m_visual_quality = visual; m_model_refinement = refinement;
        m_library_model_loaded = library; m_ready = true; m_artifact_download_started = true;
        m_model_preview_ready = true;
        m_model_preview->restore_color_trial(color_state);
    };
    m_finishing_id = "finish-" + new_request_id();
    const auto destination = source.parent_path() / temp_path(m_finishing_id, AI::model_artifact_format(source)).filename();
    m_finishing_canceled = std::make_shared<std::atomic<bool>>(false);
    const auto canceled = m_finishing_canceled;
    m_finishing_running = true; m_busy = true;
    m_finishing_cancel->Enable();
    m_finishing_status->SetLabel(recolor ? _L("正在生成局部颜色预览，可取消；选区之外保持原样……") : cleanup ? _L("正在清理选区内的小杂色块，可取消……") : _L("正在本地处理三维表面，原始模型保持不变……"));
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    const uint64_t sequence = m_sequence;
    try {
      m_finishing_worker = std::thread([weak, source, destination, options, canceled, sequence, color_state, intent_changed, face_overrides = std::move(face_overrides)] {
        const auto result = AI::finish_model_artifact(source, destination, options, [canceled] { return canceled->load(); });
        auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
        std::string preview_error;
        if (result.success && (result.changed() || intent_changed) && !canceled->load()) {
            try { ModelPreview3D::prepare_model(destination, *prepared, preview_error, face_overrides); }
            catch (const std::exception& e) { preview_error = e.what(); }
        }
        wxGetApp().CallAfter([weak, source, destination, result, sequence, canceled, prepared, preview_error, color_state, intent_changed] {
            if (!weak || weak->m_shutdown || sequence != weak->m_sequence) {
                if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
                return;
            }
            auto* self = weak.get();
            if (self->m_finishing_worker.joinable()) self->m_finishing_worker.join();
            self->m_finishing_running = false; self->m_busy = false;
            self->m_finishing_result = result;
            if (result.canceled || canceled->load()) {
                if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
                self->m_finishing_status->SetLabel(_L("已取消，原始模型保持不变。"));
            }
            else if (!result.success) self->m_finishing_status->SetLabel(_L("处理未完成，原件已保留：") + from_u8(result.error));
            else if (!result.changed() && !intent_changed) {
                boost::system::error_code ignored; boost::filesystem::remove(destination, ignored);
                self->m_finishing_status->SetLabel(self->m_finishing_options.recolor_selected
                    ? _L("选区已经是这个颜色，无需重复保存。可以选择其他颜色或继续编辑范围。")
                    : self->m_finishing_options.clean_color_spots
                    ? _L("未找到可合并的小杂色块。可扩大选区包含周围主色，或用“统一这块颜色”处理连续色带。")
                    : _L("当前设置没有改变模型；可扩大选区或调整强度，边界与锐边保持保护。"));
            } else {
                const auto view = self->m_model_preview->view_state();
                size_t triangles = 0, colors = 0; Vec3d dimensions; std::string error = preview_error;
                if (!error.empty() || !self->m_model_preview->load_prepared_model(
                    std::move(*prepared), {}, triangles, dimensions, colors, error)) {
                    boost::system::error_code ignored; boost::filesystem::remove(destination, ignored);
                    self->m_finishing_status->SetLabel(_L("预览未完成，原件已保留：") + from_u8(error));
                    self->refresh_controls(); return;
                }
                self->m_model_preview->restore_view(view);
                self->m_model_preview->restore_color_trial(color_state);
                // Match the before-view summary before exposing comparison:
                // a stale loading row changes the viewport height on first compare.
                self->m_model_stats->SetLabel(wxString::Format(_L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm"),
                    static_cast<unsigned long long>(triangles), static_cast<unsigned long long>(colors),
                    dimensions.x(), dimensions.y(), dimensions.z()));
                self->m_finishing_candidate = destination; self->m_finishing_before = false;
                self->m_finishing_compare->SetLabel(_L("查看处理前"));
                self->m_model_preview_message->SetLabel(_L("处理后 · 尚未接受；可旋转模型并查看处理前对比。"));
                self->m_finishing_status->SetLabel(self->m_finishing_options.recolor_selected ? wxString::Format(
                    _L("已统一 %llu 个面的颜色。选区外与造型保持不变；对比后接受，或放弃预览继续调整范围。"),
                    static_cast<unsigned long long>(result.recolored_faces)) : self->m_finishing_options.clean_color_spots ? wxString::Format(
                    _L("已清理 %llu 处小杂色块，调整 %llu 个顶点颜色。造型不变；请对比细节后接受新版本。"),
                    static_cast<unsigned long long>(result.cleaned_color_regions),
                    static_cast<unsigned long long>(result.recolored_vertices)) : wxString::Format(
                    _L("已柔化 %llu 个顶点，清理 %llu 个面，校正 %llu 个面。开放边 %llu 条，非流形边 %llu 条（仅作提醒）。"),
                    static_cast<unsigned long long>(result.moved_vertices),
                    static_cast<unsigned long long>(result.removed_degenerate_faces + result.removed_duplicate_faces),
                    static_cast<unsigned long long>(result.reversed_faces),
                    static_cast<unsigned long long>(result.boundary_edges),
                    static_cast<unsigned long long>(result.nonmanifold_edges)));
            }
            self->m_status->SetLabel(self->m_finishing_status->GetLabel());
            self->refresh_controls();
            self->update_finishing_selection();
            if (self->m_finishing_candidate.empty() && self->m_finishing_restore_selection)
                self->m_finishing_restore_selection();
        });
      });
    } catch (const std::exception&) {
        m_finishing_running = false; m_busy = false;
        m_finishing_status->SetLabel(_L("暂时无法启动处理，请稍后重试。原始模型保持不变。"));
        refresh_controls();
    }
}

bool ModelGenerationPanel::show_finishing_version(const boost::filesystem::path& path)
{
    const auto view = m_model_preview->view_state();
    const auto color_state = m_model_preview->color_trial_state();
    size_t triangles = 0, colors = 0; Vec3d dimensions = Vec3d::Zero(); std::string error;
    if (!m_model_preview->load_model(path, {}, triangles, dimensions, colors, error,
            path == m_finishing_candidate ? m_finishing_candidate_face_overrides : ModelPreview3D::FaceColorOverrides {})) {
        m_model_preview_ready = false;
        m_finishing_status->SetLabel(_L("预览加载失败，原件仍保留：") + from_u8(error));
        return false;
    }
    m_model_preview->restore_view(view);
    m_model_preview->restore_color_trial(color_state);
    m_model_preview->set_color_controls_visible(!m_finishing_workbench || m_finishing_tool->GetSelection() == 2);
    m_model_preview_ready = true;
    m_model_stats->SetLabel(wxString::Format(_L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm"),
        static_cast<unsigned long long>(triangles), static_cast<unsigned long long>(colors),
        dimensions.x(), dimensions.y(), dimensions.z()));
    return true;
}

void ModelGenerationPanel::select_local_finishing_version(const boost::filesystem::path& path, const std::string& id)
{
    ++m_sequence;
    m_poll_timer.Stop();
    m_job_id.clear(); m_job_palette.clear(); m_job_palette_roles.clear();
    m_job_use_printable_colors = false;
    m_artifact_path = m_displayed_model_path = path;
    m_displayed_model_job_id = id;
    m_displayed_model_palette.clear(); m_displayed_model_palette_roles.clear();
    m_color_intent_path.clear(); m_color_intent_schema.clear(); m_color_intent_sha256.clear();
    m_artifact_format = AI::model_artifact_format(path); m_artifact_color_encoding = "vertex_colors";
    m_ready = true; m_library_model_loaded = true; m_artifact_download_started = false;
    m_awaiting_confirmation = false; m_awaiting_palette_confirmation = false;
    m_last_imported_model_path.clear();
    clear_model_quality();
    m_visual_quality = {};
    m_model_refinement = {};
}

void ModelGenerationPanel::accept_model_finishing()
{
    if (m_busy || m_finishing_candidate.empty()) return;
    if (!show_finishing_version(m_finishing_candidate)) return;
    const auto root = generated_models_root();
    nlohmann::json metadata {
        {"schema_version", 4}, {"job_id", m_finishing_id},
        {"model_path", m_finishing_candidate.lexically_relative(root).generic_string()},
        {"source", "local_finishing"}, {"prompt", "3D 美颜与修复"},
        {"source_model", m_finishing_source.lexically_relative(root).generic_string()},
        {"source_sha256", m_finishing_result.source_sha256}, {"model_sha256", m_finishing_result.output_sha256},
        {"palette", nlohmann::json::array()}, {"palette_roles", nlohmann::json::object()},
        {"use_printable_colors", false}, {"generated_at", std::time(nullptr)},
        {"triangle_count", m_finishing_result.faces_after},
        {"dimensions", m_finishing_result.dimensions},
        {"finishing", {{"smooth_surface", m_finishing_options.smooth_surface}, {"repair_mesh", m_finishing_options.repair_mesh},
            {"recolor_selected", m_finishing_options.recolor_selected}, {"target_color", m_finishing_options.target_color},
            {"recolored_faces", m_finishing_result.recolored_faces},
            {"clean_color_spots", m_finishing_options.clean_color_spots}, {"cleanup_palette", m_finishing_options.cleanup_palette},
            {"cleaned_color_regions", m_finishing_result.cleaned_color_regions}, {"recolored_vertices", m_finishing_result.recolored_vertices},
            {"selected_faces", m_finishing_options.selected_faces},
            {"strength", m_finishing_options.strength}, {"moved_vertices", m_finishing_result.moved_vertices},
            {"protected_vertices", m_finishing_result.protected_vertices},
            {"removed_degenerate_faces", m_finishing_result.removed_degenerate_faces},
            {"removed_duplicate_faces", m_finishing_result.removed_duplicate_faces},
            {"reversed_faces", m_finishing_result.reversed_faces},
            {"boundary_edges", m_finishing_result.boundary_edges}, {"nonmanifold_edges", m_finishing_result.nonmanifold_edges},
            {"max_displacement_source_units", m_finishing_result.max_displacement}}}
    };
    if (m_finishing_options.recolor_selected) {
        metadata["recolor_target_palette"] = m_finishing_color_palette;
        metadata["preserves_unselected_face_colors"] = true;
    }
    metadata["face_color_intent"] = m_model_preview->face_color_metadata();
    metadata["color_trial"] = m_model_preview->color_trial_metadata();
    metadata["semantic_color_state"] = m_model_preview->semantic_color_metadata();
    if (!m_finishing_options.repair_mesh && m_finishing_selection_state.selected.size() == m_finishing_result.faces_after)
        metadata["local_selection"] = AI::SurfaceSelectionPersistence::encode(m_finishing_selection_state,
            m_finishing_result.faces_after, m_model_preview->geometry_id());
    if (!m_reference_image_path.empty() && path_is_inside(root, m_reference_image_path))
        metadata["reference_image_path"] = m_reference_image_path.lexically_relative(root).generic_string();
    if (!m_raw_preview_path.empty() && path_is_inside(root, m_raw_preview_path))
        metadata["ai_image_path"] = m_raw_preview_path.lexically_relative(root).generic_string();
    if (!write_json(library_metadata_path(m_finishing_id), metadata)) {
        m_finishing_status->SetLabel(_L("版本记录保存失败，尚未接受；请释放磁盘空间后重试。")); return;
    }
    m_finishing_undo_path = m_finishing_source;
    m_finishing_restore_context = m_finishing_source_context;
    m_finishing_accepted_path = m_finishing_candidate;
    select_local_finishing_version(m_finishing_candidate, m_finishing_id);
    m_finishing_candidate.clear();
    update_finishing_selection();
    m_finishing_status->SetLabel(_L("新版本已保存到模型库。可返回上个版本，或导入准备页重新检查打印条件。"));
    m_status->SetLabel(_L("美颜新版本已保存，可继续导入。"));
    m_model_preview_message->SetLabel(_L("当前显示：已接受的三维处理版本。"));
    load_library_entries(); refresh_controls();
    if (!m_finishing_options.repair_mesh && m_finishing_restore_selection) m_finishing_restore_selection();
}

void ModelGenerationPanel::discard_model_finishing()
{
    if (m_busy || m_finishing_candidate.empty()) return;
    if (!show_finishing_version(m_finishing_source)) return;
    boost::system::error_code ignored;
    boost::filesystem::remove(m_finishing_candidate, ignored);
    m_finishing_candidate.clear();
    m_finishing_status->SetLabel(_L("已放弃预览，恢复处理前模型。"));
    m_status->SetLabel(m_finishing_status->GetLabel());
    m_model_preview_message->SetLabel(_L("当前显示：处理前模型。"));
    refresh_controls();
    update_finishing_selection();
    if (m_finishing_restore_selection)
        m_finishing_restore_selection();
}

void ModelGenerationPanel::undo_model_finishing()
{
    if (m_busy || m_finishing_undo_path.empty()) return;
    const auto redo_colors = m_model_preview->color_trial_state();
    if (!show_finishing_version(m_finishing_undo_path)) return;
    m_finishing_redo_preview = [this, redo_colors] { m_model_preview->restore_color_trial(redo_colors); };
    m_finishing_redo_path = m_finishing_accepted_path;
    m_finishing_redo_id = m_displayed_model_job_id;
    m_finishing_redo_source = m_finishing_undo_path;
    ++m_sequence;
    if (m_finishing_restore_context) m_finishing_restore_context();
    if (m_finishing_restore_selection) m_finishing_restore_selection();
    refresh_model_quality_card();
    m_finishing_undo_path.clear(); m_finishing_source.clear();
    m_finishing_status->SetLabel(_L("已返回上个版本。处理后的版本仍在模型库中，可随时重新选用。"));
    m_status->SetLabel(_L("已返回上个版本，可继续导入。"));
    m_model_preview_message->SetLabel(_L("当前显示：上个版本。"));
    refresh_controls();
}

void ModelGenerationPanel::redo_model_finishing()
{
    if (m_busy || m_finishing_redo_path.empty() || m_displayed_model_path != m_finishing_redo_source) return;
    if (!show_finishing_version(m_finishing_redo_path)) return;
    m_finishing_undo_path = m_finishing_redo_source;
    m_finishing_accepted_path = m_finishing_redo_path;
    select_local_finishing_version(m_finishing_redo_path, m_finishing_redo_id);
    if (m_finishing_redo_preview) m_finishing_redo_preview();
    m_finishing_redo_path.clear();
    m_finishing_status->SetLabel(_L("已重做修整，恢复已保存版本。"));
    m_status->SetLabel(m_finishing_status->GetLabel());
    m_model_preview_message->SetLabel(_L("当前显示：已接受的三维处理版本。"));
    refresh_controls(); update_finishing_selection();
}

void ModelGenerationPanel::stop_model_finishing()
{
    if (m_finishing_canceled) m_finishing_canceled->store(true);
    if (m_finishing_worker.joinable()) m_finishing_worker.join();
    m_finishing_running = false;
    if (!m_finishing_candidate.empty()) {
        boost::system::error_code ignored; boost::filesystem::remove(m_finishing_candidate, ignored);
        m_finishing_candidate.clear();
    }
}
} // namespace Slic3r::GUI
