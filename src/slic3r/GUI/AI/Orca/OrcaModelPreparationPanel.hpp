#pragma once

#include <functional>
#include <cstdint>
#include <wx/panel.h>
#include <wx/timer.h>

class wxButton;
class wxCheckBox;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r::GUI {
class Plater;

class OrcaModelPreparationPanel final : public wxPanel
{
public:
    OrcaModelPreparationPanel(wxWindow* parent, Plater& plater,
                              std::function<bool()> busy, std::function<void()> changed);
private:
    int editable_object(wxString& reason) const;
    void refresh();
    void apply();
    void show_feedback(const wxString& message);
    Plater& m_plater;
    std::function<bool()> m_busy;
    std::function<void()> m_changed;
    wxStaticText* m_context;
    wxStaticText* m_feedback;
    wxTextCtrl* m_height;
    wxCheckBox* m_base;
    wxButton* m_apply;
    uint64_t m_displayed_object_id {0};
    wxString m_feedback_text;
    wxTimer m_timer;
};
} // namespace Slic3r::GUI
