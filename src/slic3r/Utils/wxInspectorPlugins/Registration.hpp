#pragma once

#include "DPIAwarePlugin.hpp"
#include "CustomWidgetsPlugin.hpp"

#include <wx/inspector/inspector.h>

inline void RegisterOrcaInspectorPlugins()
{
    static DPIAwarePlugin          dpiaware;
    static CustomWidgetsPlugin     customWidgets;
    wxInspector::RegisterPlugin(&dpiaware);
    wxInspector::RegisterPlugin(&customWidgets);
}
