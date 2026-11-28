#pragma once

namespace Slic3r {
namespace GUI {

// Kept apart from ConfigWizard so GUI_App.hpp can name these without the wizard's dialog headers.

// Why is the Wizard run
enum class RunReason {
    RR_DATA_EMPTY,                  // No or empty datadir
    RR_DATA_LEGACY,                 // Pre-updating datadir
    RR_DATA_INCOMPAT,               // Incompatible datadir - Slic3r downgrade situation
    RR_USER,                        // User requested the Wizard from the menus
};

// What page should wizard start on
enum class StartPage {
    SP_WELCOME,
    SP_PRINTERS,
    SP_FILAMENTS,
    SP_MATERIALS,
    SP_CUSTOM,
};

}} // namespace Slic3r::GUI
