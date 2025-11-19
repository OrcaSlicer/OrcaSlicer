#ifndef _prusaslicer_technologies_h_
#define _prusaslicer_technologies_h_

//=============
// debug techs
//=============
// Shows camera target in the 3D scene
#define ENABLE_SHOW_CAMERA_TARGET 0
// Log debug messages to console when changing selection
#define ENABLE_SELECTION_DEBUG_OUTPUT 0
// Renders a small sphere in the center of the bounding box of the current selection when no gizmo is active
#define ENABLE_RENDER_SELECTION_CENTER 0
// Shows an imgui dialog with camera related data
#define ENABLE_CAMERA_STATISTICS 0
// Enable extracting thumbnails from selected gcode and save them as png files
#define ENABLE_THUMBNAIL_GENERATOR_DEBUG 0
// Disable synchronization of unselected instances
#define DISABLE_INSTANCES_SYNCH 0
// Use wxDataViewRender instead of wxDataViewCustomRenderer
#define ENABLE_NONCUSTOM_DATA_VIEW_RENDERING 0
// Enable project dirty state manager debug window
#define ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW 0


// Enable rendering of objects using environment map
#define ENABLE_ENVIRONMENT_MAP 0
// Enable smoothing of objects normals
#define ENABLE_SMOOTH_NORMALS 0
// Enable rendering markers for options in preview as fixed screen size points
#define ENABLE_FIXED_SCREEN_SIZE_POINT_MARKERS 1

// Enable style editor in develop mode
#define ENABLE_IMGUI_STYLE_EDITOR	0

// Enable rework of Reload from disk command
#define ENABLE_RELOAD_FROM_DISK_REWORK 1

// Enable rendering modifiers and similar objects always as transparent
#define ENABLE_MODIFIERS_ALWAYS_TRANSPARENT 1

// Enable modified ImGuiWrapper::slider_float() to create a compound widget where
// an additional button can be used to set the keyboard focus into the slider
// to allow the user to type in the desired value
#define ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT 1
// Enable fit print volume command for circular printbeds
#define ENABLE_ENHANCED_PRINT_VOLUME_FIT 1
// Enable picking using raytracing
#define ENABLE_RAYCAST_PICKING_DEBUG 0

// Enable imgui debug dialog for new gcode viewer (using libvgcode)
#define ENABLE_NEW_GCODE_VIEWER_DEBUG 0
// Enable extension of tool position imgui dialog to show actual speed profile
#define ENABLE_ACTUAL_SPEED_DEBUG 1

#endif // _prusaslicer_technologies_h_
