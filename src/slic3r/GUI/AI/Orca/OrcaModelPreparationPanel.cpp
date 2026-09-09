#include "OrcaModelPreparationPanel.hpp"
#include "OrcaModelPreparation.hpp"
#include "slic3r/GUI/AI/AIWindowAppearance.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <cmath>

namespace Slic3r::GUI {

OrcaModelPreparationPanel::OrcaModelPreparationPanel(wxWindow* parent, Plater& plater,
    std::function<bool()> busy, std::function<void()> changed)
    : wxPanel(parent), m_plater(plater), m_busy(std::move(busy)), m_changed(std::move(changed)), m_timer(this)
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(this, wxID_ANY, _L("调整当前模型（可跳过）"));
    title->SetFont(wxGetApp().bold_font());
    root->Add(title, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_context = new wxStaticText(this, wxID_ANY, "");
    root->Add(m_context, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    root->Add(new wxStaticText(this, wxID_ANY, _L("总高度（mm，含底座）")), 0, wxBOTTOM, FromDIP(4));
    m_height = new wxTextCtrl(this, wxID_ANY, "120");
    root->Add(m_height, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_base = new wxCheckBox(this, wxID_ANY, _L("添加 3 mm 圆底座"));
    m_base->SetToolTip(_L("圆底座覆盖模型投影并留出 2 mm 边缘，与模型重叠 0.2 mm。请在原生预览中检查连接与支撑。"));
    root->Add(m_base, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_apply = new wxButton(this, wxID_ANY, _L("应用尺寸与底座"));
    root->Add(m_apply, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_feedback = new wxStaticText(this, wxID_ANY, _L("编辑保存在当前工程，可使用 Orca 撤销。生成原件保留在模型库。"));
    m_feedback_text = m_feedback->GetLabel();
    root->Add(m_feedback, 0, wxEXPAND);
    SetSizer(root);
    m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { apply(); });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { if (IsShownOnScreen()) refresh(); }, m_timer.GetId());
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        const int width = std::max(1, GetClientSize().x);
        m_context->Wrap(width);
        m_feedback->SetLabel(m_feedback_text);
        m_feedback->Wrap(width);
        event.Skip();
    });
    m_timer.Start(1000);
    refresh();
}

int OrcaModelPreparationPanel::editable_object(wxString& reason) const
{
    if ((m_busy && m_busy()) || m_plater.is_background_process_slicing() ||
        m_plater.get_view3D_canvas3D()->get_gizmos_manager().is_running()) {
        reason = _L("请先结束切片或当前编辑工具。");
        return -1;
    }
    if (m_plater.printer_technology() != ptFFF) {
        reason = _L("此准备操作用于当前 FDM 工程。");
        return -1;
    }
    const int index = m_plater.get_selected_object_idx();
    auto* plate = m_plater.get_partplate_list().get_curr_plate();
    if (index < 0 || static_cast<size_t>(index) >= m_plater.model().objects.size() ||
        !m_plater.get_selection().is_single_full_instance() ||
        m_plater.model().objects[index]->instances.size() != 1) {
        reason = _L("请在当前打印板完整选中一个只有单份实例的模型。");
        return -1;
    }
    if (plate == nullptr || plate->is_locked() || !plate->contain_instance(index, 0)) {
        reason = _L("请选中当前未锁定打印板上的模型。");
        return -1;
    }
    return index;
}

void OrcaModelPreparationPanel::refresh()
{
    // Keep native theme/font changes inside this feature, including while idle.
    if (GetBackgroundColour() != wxGetApp().get_window_default_clr() || GetFont() != wxGetApp().normal_font()) {
        refresh_ai_appearance(GetParent());
        GetParent()->Layout();
    }
    wxString context;
    const int index = editable_object(context);
    m_displayed_object_id = 0;
    if (index >= 0) {
        const auto* object = m_plater.model().objects[index];
        m_displayed_object_id = object->id().id;
        context = from_u8(object->name) + wxString::Format(_L(" · 当前高 %.1f mm"), object->instance_bounding_box(0).size().z());
    }
    if (wxGetApp().preset_bundle != nullptr)
        context += _L("\n设备：") + from_u8(wxGetApp().preset_bundle->printers.get_selected_preset().name) +
            _L("\n材料和工艺沿用左侧当前配置；适配结果以原生切片为准。");
    m_context->SetLabel(context);
    m_context->Wrap(std::max(1, GetClientSize().x));
    m_apply->Enable(index >= 0);
    Layout();
    GetParent()->Layout();
    if (auto* scroll = dynamic_cast<wxScrolledWindow*>(GetParent())) scroll->FitInside();
}

void OrcaModelPreparationPanel::show_feedback(const wxString& message)
{
    m_feedback_text = message;
    m_feedback->SetLabel(message);
    m_feedback->Wrap(std::max(1, GetClientSize().x));
}

void OrcaModelPreparationPanel::apply()
{
    wxString feedback;
    const int index = editable_object(feedback);
    double height = 0;
    if (index < 0) {
        show_feedback(feedback);
    } else if (m_plater.model().objects[index]->id().id != m_displayed_object_id) {
        show_feedback(_L("选中的模型已变化。请确认当前模型和高度后再次应用。"));
    } else if (!m_height->GetValue().ToDouble(&height) || !std::isfinite(height) || height < 1 || height > 1000 ||
               (m_base->GetValue() && height <= 3)) {
        show_feedback(_L("请输入 1–1000 mm 的高度；含底座时须大于 3 mm。"));
        m_height->SetFocus();
    } else if (!m_base->GetValue() &&
               std::abs(m_plater.model().objects[index]->instance_bounding_box(0).size().z() - height) < 1e-5) {
        show_feedback(_L("当前模型已是此高度，无需再次应用。"));
    } else {
        m_apply->Disable();
        bool transaction_started = false;
        try {
            auto* object = m_plater.model().objects[index];
            auto proposal = prepare_model(*object, {height, m_base->GetValue(), 3.0});
            {
                Plater::TakeSnapshot snapshot(&m_plater, _u8L("调整模型尺寸与底座"));
                transaction_started = true;
                apply_model_preparation(*object, proposal);
                m_plater.select_view_3D("3D");
                auto* list = m_plater.sidebar().obj_list();
                if (object->volumes.size() > 1)
                    list->reorder_volumes_and_get_selection(index);
                else
                    m_plater.changed_object(index);
                list->notify_instance_updated(index);
                list->select_item(ObjectVolumeID{object, nullptr});
                list->update_plate_values_for_items();
                m_plater.show_object_info();
                save_object_mesh(*object);
                m_plater.update_title_dirty_status();
            }
            m_base->SetValue(false);
            show_feedback(_L("已更新当前模型。请重新检查打印适配；可在 Orca 撤销，或保存 3MF 保留编辑版本。"));
            if (m_changed) m_changed();
        } catch (const std::exception& error) {
            if (transaction_started && m_plater.can_undo()) m_plater.undo();
            show_feedback(std::string(error.what()) == "base_already_exists" ?
                _L("该模型已有此底座。可以只调整高度，或使用 Orca 撤销后重新添加。") :
                _L("未能应用。请检查模型是否有效且未带切割连接件；原件仍保留，可以调整后重试。"));
        }
    }
    m_feedback->Wrap(std::max(1, GetClientSize().x));
    refresh();
    GetParent()->Layout();
}

} // namespace Slic3r::GUI
