#include <wx/app.h>
#include <wx/event.h>
#include <wx/frame.h>
#include <wx/init.h>
#include <wx/sizer.h>
#include <iostream>

#include "slic3r/GUI/SupportOverrideNotice.hpp"

class SupportNoticeTestApp : public wxApp
{
public:
    bool OnInit() override { return true; }
};
wxIMPLEMENT_APP_NO_MAIN(SupportNoticeTestApp);

int main(int argc, char** argv)
{
    // A GUI session is needed for the native toolkit, even with a hidden frame.
    if (!wxEntryStart(argc, argv))
        return 77;
    if (!wxTheApp->CallOnInit()) {
        wxEntryCleanup();
        return 77;
    }
    auto* frame       = new wxFrame(nullptr, wxID_ANY, "Support override notice test");
    bool has_override = false;
    auto* panel       = new Slic3r::GUI::SupportOverrideNotice(frame, [&] { return has_override; });
    auto* group       = new wxBoxSizer(wxVERTICAL);
    group->Add(panel);
    frame->SetSizer(group);
    int failures = 0;
    auto check   = [&](bool expected, const char* state) {
        const bool actual = panel->IsShown();
        std::cout << state << ": expected=" << expected << " actual=" << actual << '\n';
        if (actual != expected)
            ++failures;
    };
    panel->Hide();
    group->ShowItems(true);
    check(false, "no override after group activation");
    has_override = true;
    group->ShowItems(true);
    check(true, "enabled override after group activation");
    group->ShowItems(false);
    check(false, "group hidden");
    group->ShowItems(true);
    check(true, "group shown with override");
    has_override = false;
    group->ShowItems(true);
    check(false, "override cleared before mode visibility pass");
    delete frame;
    wxTheApp->OnExit();
    wxEntryCleanup();
    std::cout << "visibility failures=" << failures << '\n';
    return failures ? 1 : 0;
}
