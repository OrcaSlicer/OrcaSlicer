#pragma once

class wxWindow;

namespace Slic3r { namespace GUI {

// Opens the modal Customize Toolbar dialog. On OK it saves the layout to AppConfig ([toolbar] layout) and
// rebuilds the Prepare canvas's main toolbar and gizmo bar.
void show_toolbar_customize_dialog(wxWindow* parent);

}} // namespace Slic3r::GUI
