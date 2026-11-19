#pragma once

class wxWindow;

namespace Slic3r { namespace GUI {

// Opens the modal Customize Toolbar dialog. On OK it saves the layout to AppConfig ([toolbar] layout) and
// rebuilds the Prepare canvas's main toolbar and gizmo bar.
void show_toolbar_customize_dialog(wxWindow* parent);

// Opens, after the current event, a menu at the pointer that offers the Customize Toolbar dialog: the menu a
// right click on the toolbar or the gizmo bar shows.
void show_toolbar_context_menu();

}} // namespace Slic3r::GUI
