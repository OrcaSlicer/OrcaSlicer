# Application Structure Overview

WIP...

> [!WARNING]
> !! incomplete, possibly inaccurate, being updated with new info !!

## [`Plater`](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/src/slic3r/GUI/Plater.hpp)

Refers to the entire application. The whole view, file loading, project saving and loading is all managed by this class. This class contains members for the model viewer, the sidebar, gcode viewer and everything else.

## [`Sidebar`](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/src/slic3r/GUI/Plater.hpp)

This is relating the the sidebar in the application window

![full-sidebar](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/GUI/full-sidebar.png?raw=true)

## [`ComboBox`](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/src/slic3r/GUI/Widgets/ComboBox.hpp)

The drop down menus where you can see and select presets

![combobox](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/GUI/combobox.png?raw=true)

## [`Tab`](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/src/slic3r/GUI/Tab.hpp)

Refers to the various windows with settings. e.g. the Popup to edit printer or filament preset. Also the section to edit process preset and the object list. These 4 are managed by `TabPrinter`, `TabFilament`, `TabPrint` and `TabPrintModel` respectively.

![tab-popup](https://github.com/OrcaSlicer/OrcaSlicer/blob/main/doc/images/GUI/tab-popup.png?raw=true)
