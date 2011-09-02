#include "Process.hpp"

#include <libslic3r/AppConfig.hpp>

#include "../GUI/GUI.hpp"
// for file_wildcards()
#include "../GUI/GUI_App.hpp"
// localization
#include "../GUI/I18N.hpp"

#include <iostream>
#include <fstream>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

// For starting another OrcaSlicer instance on OSX.
// Fails to compile on Windows on the build server.
#ifdef __APPLE__
    #include <boost/process/spawn.hpp>
    #include <boost/process/args.hpp>
#endif

#include <wx/stdpaths.h>

namespace Slic3r {
namespace GUI {

enum class NewSlicerInstanceType {
	Slicer,
	GCodeViewer
};

// Start a new Slicer process instance either in a Slicer mode or in a G-code mode.
// Optionally load a 3MF, STL or a G-code on start.
static void start_new_slicer_or_gcodeviewer(const NewSlicerInstanceType instance_type, const std::vector<wxString> paths_to_open, bool single_instance)
{
#ifdef _WIN32
	wxString path;
	wxFileName::SplitPath(wxStandardPaths::Get().GetExecutablePath(), &path, nullptr, nullptr, wxPATH_NATIVE);
	path += "\\";
	path += (instance_type == NewSlicerInstanceType::Slicer) ? "orca-slicer.exe" : "bambu-gcodeviewer.exe";
	std::vector<const wchar_t*> args;
	args.reserve(4);
	args.emplace_back(path.wc_str());
	if (!paths_to_open.empty()) {
		for (const auto& file : paths_to_open)
			args.emplace_back(file);
	}
	if (instance_type == NewSlicerInstanceType::Slicer && single_instance)
		args.emplace_back(L"--single-instance");
	args.emplace_back(nullptr);
	BOOST_LOG_TRIVIAL(info) << "Trying to spawn a new slicer \"" << into_u8(path) << "\"";
	// Don't call with wxEXEC_HIDE_CONSOLE, OrcaSlicer in GUI mode would just show the splash screen. It would not open the main window though, it would
	// just hang in the background.
	if (wxExecute(const_cast<wchar_t**>(args.data()), wxEXEC_ASYNC) <= 0)
		BOOST_LOG_TRIVIAL(error) << "Failed to spawn a new slicer \"" << into_u8(path);
#else 
	// Own executable path.
	boost::filesystem::path bin_path = into_path(wxStandardPaths::Get().GetExecutablePath());
#if defined(__APPLE__)
	{
        auto bundle_path = bin_path.parent_path().parent_path().parent_path();
		//bin_path = bin_path.parent_path() / "OrcaSlicer";
        bin_path = "/usr/bin/open";
		// On Apple the wxExecute fails, thus we use boost::process instead.
		BOOST_LOG_TRIVIAL(info) << "Trying to spawn a new slicer \"" << bin_path.string() << "\"";
		try {
            std::vector<std::string> args = { "-n", "-a", bundle_path.string()};
            if (!paths_to_open.empty()) {
                for (const auto& file : paths_to_open)
                    args.emplace_back(into_u8(file));
            }
            args.emplace_back("--args");
			if (instance_type == NewSlicerInstanceType::GCodeViewer)
				args.emplace_back("--gcodeviewer");
			if (instance_type == NewSlicerInstanceType::Slicer && single_instance)
				args.emplace_back("--single-instance");
			boost::process::spawn(bin_path, args);
		}
		catch (const std::exception& ex) {
			BOOST_LOG_TRIVIAL(error) << "Failed to spawn a new slicer \"" << bin_path.string() << "\": " << ex.what();
		}
	}
#else // Linux or Unix
	{
		std::vector<const char*> args;
		args.reserve(3);
#ifdef __linux__
		static const char* gcodeviewer_param = "--gcodeviewer";
		{
			// If executed by an AppImage, start the AppImage, not the main process.
			// see https://docs.appimage.org/packaging-guide/environment-variables.html#id2
			const char* appimage_binary = std::getenv("APPIMAGE");
			if (appimage_binary) {
				args.emplace_back(appimage_binary);
				if (instance_type == NewSlicerInstanceType::GCodeViewer)
					args.emplace_back(gcodeviewer_param);
			}
		}
#endif // __linux__
		std::string my_path;
		if (args.empty()) {
			// Binary path was not set to the AppImage in the Linux specific block above, call the application directly.
			my_path = (bin_path.parent_path() / ((instance_type == NewSlicerInstanceType::Slicer) ? "orca-slicer" : "bambu-gcodeviewer")).string();
			args.emplace_back(my_path.c_str());
		}
		std::string to_open;
		if (!paths_to_open.empty()) {
			for (const auto& file : paths_to_open) {
				to_open = into_u8(file);
				args.emplace_back(to_open.c_str());
			}
		}
		if (instance_type == NewSlicerInstanceType::Slicer && single_instance)
			args.emplace_back("--single-instance");
		args.emplace_back(nullptr);
		BOOST_LOG_TRIVIAL(info) << "Trying to spawn a new slicer \"" << args[0] << "\"";
		if (wxExecute(const_cast<char**>(args.data()), wxEXEC_ASYNC | wxEXEC_MAKE_GROUP_LEADER) <= 0)
			BOOST_LOG_TRIVIAL(error) << "Failed to spawn a new slicer \"" << args[0];
	}
#endif // Linux or Unix
#endif // Win32
}
static void start_new_slicer_or_gcodeviewer(const NewSlicerInstanceType instance_type, const wxString* path_to_open, bool single_instance)
{
	std::vector<wxString> paths;
	if (path_to_open != nullptr)
		paths.emplace_back(path_to_open->wc_str());
	start_new_slicer_or_gcodeviewer(instance_type, paths, single_instance);
}

void start_new_slicer(const wxString *path_to_open, bool single_instance)
{
	start_new_slicer_or_gcodeviewer(NewSlicerInstanceType::Slicer, path_to_open, single_instance);
}
void start_new_slicer(const std::vector<wxString>& files, bool single_instance)
{
	start_new_slicer_or_gcodeviewer(NewSlicerInstanceType::Slicer, files, single_instance);
}

void start_new_gcodeviewer(const wxString *path_to_open)
{
	start_new_slicer_or_gcodeviewer(NewSlicerInstanceType::GCodeViewer, path_to_open, false);
}

void start_new_gcodeviewer_open_file(wxWindow *parent)
{
    wxFileDialog dialog(parent ? parent : wxGetApp().GetTopWindow(),
        _L("Open G-code file:"),
        from_u8(wxGetApp().app_config->get_last_dir()), wxString(),
        file_wildcards(FT_GCODE), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) {
        wxString path = dialog.GetPath();
        start_new_gcodeviewer(&path);
    }
}

} // namespace GUI
} // namespace Slic3r
