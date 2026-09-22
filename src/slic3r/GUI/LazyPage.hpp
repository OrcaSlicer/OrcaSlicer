#pragma once

#include <functional>
#include <string>
#include <utility>

#include <wx/bookctrl.h>
#include <wx/panel.h>
#include <wx/sizer.h>

#include "Lazy.hpp"

namespace Slic3r { namespace GUI {

// Applies the app's dark-mode pass to a panel built after the frame's own pass ran.
void apply_dark_ui_to_lazy_panel(wxWindow* panel);

// A page whose panel is built the first time the page is shown, or earlier by an
// IdleScheduler; nothing builds while the frame is still hidden. The Lazy side holds the
// panel and, for a LazyInstance panel type, registers it for the type's statics.
template <class Panel>
class LazyPage : public wxPanel, public Lazy<Panel>
{
public:
    using Factory = std::function<Panel*(wxWindow* parent)>;

    LazyPage(wxWindow* parent, std::string name, int order, Factory make = [](wxWindow* parent) { return new Panel(parent); })
        : wxPanel(parent), Lazy<Panel>(std::move(name), order, [this, make = std::move(make)] {
            Panel* panel = make(this);
            GetSizer()->Add(panel, 1, wxEXPAND);
            // Hidden while its page is, as an inserted page would be.
            if (!IsShown())
                panel->Hide();
            return panel;
        })
    {
        SetSizer(new wxBoxSizer(wxVERTICAL));
        // Shown by the book when its tab is selected.
        Hide();
        this->when_built([this](Panel& panel) {
            apply_dark_ui_to_lazy_panel(&panel);
            Layout();
        });
    }

    // The parent book currently lists this page.
    bool in_book() const
    {
        auto* book = dynamic_cast<wxBookCtrlBase*>(GetParent());
        return book != nullptr && book->FindPage(this) != wxNOT_FOUND;
    }

    // Pending only while the tab is in the book.
    bool pending() const override { return in_book() && Lazy<Panel>::pending(); }

    // Forwarded so the panel's own Show() override stays its activation hook; wx calls this
    // virtual from the book's ShowWithEffect() only for wxSHOW_EFFECT_NONE, the default.
    bool Show(bool show = true) override
    {
        const bool changed = wxPanel::Show(show);
        if (show) {
            // The book shows its first page as it is inserted, before startup has chosen the
            // start page, so a hidden frame builds nothing; MainFrame::Show() completes it.
            if (this->built() || wxGetTopLevelParent(this)->IsShown()) {
                if (Panel* panel = this->ensure())
                    panel->Show(true);
                // The sizer skipped the panel while the book kept it hidden.
                Layout();
            }
        } else if (Panel* panel = this->get()) {
            panel->Show(false);
        }
        return changed;
    }
};

}} // namespace Slic3r::GUI
