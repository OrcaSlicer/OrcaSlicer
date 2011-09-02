#ifndef slic3r_GUI_Profile_hpp_
#define slic3r_GUI_Profile_hpp_

// Profiling support using the Shiny intrusive profiler
//#define SLIC3R_PROFILE_GUI
#if defined(SLIC3R_PROFILE) && defined(SLIC3R_PROFILE_GUI)
	#include <Shiny/Shiny.h>
	#define SLIC3R_GUI_PROFILE_FUNC() PROFILE_FUNC()
	#define SLIC3R_GUI_PROFILE_BLOCK(name) PROFILE_BLOCK(name)
	#define SLIC3R_GUI_PROFILE_UPDATE() PROFILE_UPDATE()
	#define SLIC3R_GUI_PROFILE_OUTPUT(x) PROFILE_OUTPUT(x)
#else
	#define SLIC3R_GUI_PROFILE_FUNC()
	#define SLIC3R_GUI_PROFILE_BLOCK(name)
	#define SLIC3R_GUI_PROFILE_UPDATE()
	#define SLIC3R_GUI_PROFILE_OUTPUT(x)
#endif

#endif // slic3r_GUI_Profile_hpp_
