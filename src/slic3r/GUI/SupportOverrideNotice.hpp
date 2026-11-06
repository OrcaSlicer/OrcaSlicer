#pragma once

#include <functional>
#include <utility>
#include <wx/panel.h>

namespace Slic3r::GUI {

// Option groups recursively show all their children during page activation and
// mode changes. Keep the notice conditional even during those visibility passes.
class SupportOverrideNotice : public wxPanel
{
public:
    SupportOverrideNotice(wxWindow* parent, std::function<bool()> should_show) : wxPanel(parent), m_should_show(std::move(should_show)) {}

    bool Show(bool show = true) override { return wxPanel::Show(show && m_should_show()); }

private:
    std::function<bool()> m_should_show;
};

} // namespace Slic3r::GUI
