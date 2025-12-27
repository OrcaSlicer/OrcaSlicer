#include "PostProcessor.hpp"

#include "libslic3r/Utils.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/I18N.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

// BBS
#include <iostream>
#include <fstream>

#ifdef WIN32

// The standard Windows includes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

// https://blogs.msdn.microsoft.com/twistylittlepassagesallalike/2011/04/23/everyone-quotes-command-line-arguments-the-wrong-way/
// This routine appends the given argument to a command line such that CommandLineToArgvW will return the argument string unchanged.
// Arguments in a command line should be separated by spaces; this function does not add these spaces.
// Argument    - Supplies the argument to encode.
// CommandLine - Supplies the command line to which we append the encoded argument string.
static void quote_argv_winapi(const std::wstring &argument, std::wstring &commmand_line_out)
{
	// Don't quote unless we actually need to do so --- hopefully avoid problems if programs won't parse quotes properly.
	if (argument.empty() == false && argument.find_first_of(L" \t\n\v\"") == argument.npos)
		commmand_line_out.append(argument);
	else {
		commmand_line_out.push_back(L'"');
		for (auto it = argument.begin(); ; ++ it) {
			unsigned number_backslashes = 0;
			while (it != argument.end() && *it == L'\\') {
				++ it;
				++ number_backslashes;
			}
			if (it == argument.end()) {
				// Escape all backslashes, but let the terminating double quotation mark we add below be interpreted as a metacharacter.
				commmand_line_out.append(number_backslashes * 2, L'\\');
				break;
			} else if (*it == L'"') {
				// Escape all backslashes and the following double quotation mark.
				commmand_line_out.append(number_backslashes * 2 + 1, L'\\');
				commmand_line_out.push_back(*it);
			} else {
				// Backslashes aren't special here.
				commmand_line_out.append(number_backslashes, L'\\');
				commmand_line_out.push_back(*it);
			}
		}
		commmand_line_out.push_back(L'"');
	}
}

static DWORD execute_process_winapi(const std::wstring &command_line)
{
    // Extract the current environment to be passed to the child process.
	std::wstring envstr;
	{
		wchar_t *env = GetEnvironmentStrings();
		assert(env != nullptr);
		const wchar_t* var = env;
		size_t totallen = 0;
		size_t len;
		while ((len = wcslen(var)) > 0) {
			totallen += len + 1;
			var += len + 1;
		}
		envstr = std::wstring(env, totallen);
		FreeEnvironmentStrings(env);
	}

	STARTUPINFOW startup_info;
	memset(&startup_info, 0, sizeof(startup_info));
	startup_info.cb			 = sizeof(STARTUPINFO);
#if 0
	startup_info.dwFlags	 = STARTF_USESHOWWINDOW;
	startup_info.wShowWindow = SW_HIDE;
#endif
	PROCESS_INFORMATION process_info;
	if (! ::CreateProcessW(
            nullptr /* lpApplicationName */, (LPWSTR)command_line.c_str(), nullptr /* lpProcessAttributes */, nullptr /* lpThreadAttributes */, false /* bInheritHandles */,
			CREATE_UNICODE_ENVIRONMENT /* | CREATE_NEW_CONSOLE */ /* dwCreationFlags */, (LPVOID)envstr.c_str(), nullptr /* lpCurrentDirectory */, &startup_info, &process_info))
		throw Slic3r::RuntimeError(std::string("Failed starting the script ") + boost::nowide::narrow(command_line) + ", Win32 error: " + std::to_string(int(::GetLastError())));
	::WaitForSingleObject(process_info.hProcess, INFINITE);
	ULONG rc = 0;
	::GetExitCodeProcess(process_info.hProcess, &rc);
	::CloseHandle(process_info.hThread);
	::CloseHandle(process_info.hProcess);
	return rc;
}

// Run the script. If it is a perl script, run it through the bundled perl interpreter.
// If it is a batch file, run it through the cmd.exe.
// Otherwise run it directly.
static int run_script(const std::string &script, const std::string &gcode, std::string &/*std_err*/)
{
    // Unpack the argument list provided by the user.
    int     nArgs;
    LPWSTR *szArglist = CommandLineToArgvW(boost::nowide::widen(script).c_str(), &nArgs);
    if (szArglist == nullptr || nArgs <= 0) {
        // CommandLineToArgvW failed. Maybe the command line escapment is invalid?
		throw Slic3r::RuntimeError(std::string("Post processing script ") + script + " on file " + gcode + " failed. CommandLineToArgvW() refused to parse the command line path.");
    }

    std::wstring command_line;
    std::wstring command = szArglist[0];
	if (! boost::filesystem::exists(boost::filesystem::path(command)))
		throw Slic3r::RuntimeError(std::string("The configured post-processing script does not exist: ") + boost::nowide::narrow(command));
    if (boost::iends_with(command, L".pl")) {
        // This is a perl script. Run it through the perl interpreter.
        // The current process may be slic3r.exe or slic3r-console.exe.
        // Find the path of the process:
        wchar_t wpath_exe[_MAX_PATH + 1];
        ::GetModuleFileNameW(nullptr, wpath_exe, _MAX_PATH);
        boost::filesystem::path path_exe(wpath_exe);
        boost::filesystem::path path_perl = path_exe.parent_path() / "perl" / "perl.exe";
        if (! boost::filesystem::exists(path_perl)) {
			LocalFree(szArglist);
			throw Slic3r::RuntimeError(std::string("Perl interpreter ") + path_perl.string() + " does not exist.");
        }
        // Replace it with the current perl interpreter.
        quote_argv_winapi(boost::nowide::widen(path_perl.string()), command_line);
        command_line += L" ";
    } else if (boost::iends_with(command, ".bat")) {
        // Run a batch file through the command line interpreter.
        command_line = L"cmd.exe /C ";
    }

    for (int i = 0; i < nArgs; ++ i) {
        quote_argv_winapi(szArglist[i], command_line);
        command_line += L" ";
    }
    LocalFree(szArglist);
	quote_argv_winapi(boost::nowide::widen(gcode), command_line);
    return (int)execute_process_winapi(command_line);
}

#else
    // POSIX

#include <cstdlib>   // getenv()
#include <sstream>
#include <boost/process.hpp>

namespace process = boost::process;

static int run_script(const std::string &script, const std::string &gcode, std::string &std_err)
{
    // Try to obtain user's default shell
    const char *shell = ::getenv("SHELL");
    if (shell == nullptr) { shell = "/bin/sh"; }

    // Quote and escape the gcode path argument
    std::string command { script };
    command.append(" '");
    for (char c : gcode) {
        if (c == '\'') { command.append("'\\''"); }
        else { command.push_back(c); }
    }
    command.push_back('\'');

    BOOST_LOG_TRIVIAL(debug) << boost::format("Executing script, shell: %1%, command: %2%") % shell % command;

    process::ipstream istd_err;
    process::child child(shell, "-c", command, process::std_err > istd_err);

    std_err.clear();
    std::string line;

    while (child.running() && std::getline(istd_err, line)) {
        std_err.append(line);
        std_err.push_back('\n');
    }

    child.wait();
    return child.exit_code();
}

#endif

namespace Slic3r {

//! macro used to mark string used at localization,
//! return same string
#define L(s) (s)
#define _(s) Slic3r::I18N::translate(s)

// BBS
void gcode_add_line_number(const std::string& path, const DynamicPrintConfig& config)
{
    const ConfigOptionBool* opt = config.opt<ConfigOptionBool>("gcode_add_line_number");
    if (!opt->getBool())
        return;

    auto gcode_file = boost::filesystem::path(path);
    if (!boost::filesystem::exists(gcode_file))
        return;

    std::fstream fs;
    std::string new_gcode;
    fs.open(gcode_file.c_str(), std::fstream::in | std::fstream::out);

    size_t line_number = 1;
    std::string gcode_line;
    while (std::getline(fs, gcode_line)) {
        char num_str[128];
        memset(num_str, 0, sizeof(num_str));
        snprintf(num_str, sizeof(num_str), "%zd", line_number);
        new_gcode += std::string("N") + num_str + " " + gcode_line + "\n";
        line_number++;
    }

    fs.clear();
    fs.seekp(0, std::ios_base::beg);
    fs.write(new_gcode.c_str(), new_gcode.length());
    fs.close();
}

// Run post processing script / scripts if defined.
// Returns true if a post-processing script was executed.
// Returns false if no post-processing script was defined.
// Throws an exception on error.
// host is one of "File", "PrusaLink", "Repetier", "SL1Host", "OctoPrint", "FlashAir", "Duet", "AstroBox" ...
// For a "File" target, a temp file will be created for src_path by adding a ".pp" suffix and src_path will be updated.
// In that case the caller is responsible to delete the temp file created.
// output_name is the final name of the G-code on SD card or when uploaded to PrusaLink or OctoPrint.
// If uploading to PrusaLink or OctoPrint, then the file will be renamed to output_name first on the target host.
// The post-processing script may change the output_name.
bool run_post_process_scripts(std::string &src_path, bool make_copy, const std::string &host, std::string &output_name, const DynamicPrintConfig &config)
{
    const auto *post_process = config.opt<ConfigOptionStrings>("post_process");
    if (// likely running in SLA mode
        post_process == nullptr || 
        // no post-processing script
        post_process->values.empty())
        return false;

    std::string path;
    if (make_copy) {
        // Don't run the post-processing script on the input file, it will be memory mapped by the G-code viewer.
        // Make a copy.
        path = src_path + ".pp";
        // First delete an old file if it exists.
        try {
            if (boost::filesystem::exists(path))
                boost::filesystem::remove(path);
        } catch (const std::exception &err) {
            BOOST_LOG_TRIVIAL(error) << Slic3r::format("Failed deleting an old temporary file %1% before running a post-processing script: %2%", path, err.what());
        }
        // Second make a copy.
        std::string error_message;
        if (copy_file(src_path, path, error_message, false) != SUCCESS)
            throw Slic3r::RuntimeError(Slic3r::format("Failed making a temporary copy of G-code file %1% before running a post-processing script: %2%", src_path, error_message));
    } else {
        // Don't make a copy of the G-code before running the post-processing script.
        path = src_path;
    }

    auto delete_copy = [&path, &src_path, make_copy]() {
        if (make_copy)
            try {
                if (boost::filesystem::exists(path))
                    boost::filesystem::remove(path);
            } catch (const std::exception &err) {
                BOOST_LOG_TRIVIAL(error) << Slic3r::format("Failed deleting a temporary copy %1% of a G-code file %2% : %3%", path, src_path, err.what());
            }
    };

    auto gcode_file = boost::filesystem::path(path);
    if (! boost::filesystem::exists(gcode_file))
        throw Slic3r::RuntimeError(std::string("Post-processor can't find exported gcode file"));

    // Store print configuration into environment variables.
    config.setenv_();
    // Let the post-processing script know the target host ("File", "PrusaLink", "Repetier", "SL1Host", "OctoPrint", "FlashAir", "Duet", "AstroBox" ...)
    boost::nowide::setenv("SLIC3R_PP_HOST", host.c_str(), 1);
    // Let the post-processing script know the final file name. For "File" host, it is a full path of the target file name and its location, for example pointing to an SD card.
    // For "PrusaLink" or "OctoPrint", it is a file name optionally with a directory on the target host.
    boost::nowide::setenv("SLIC3R_PP_OUTPUT_NAME", output_name.c_str(), 1);

    // Path to an optional file that the post-processing script may create and populate it with a single line containing the output_name replacement.
    std::string path_output_name = path + ".output_name";
    auto remove_output_name_file = [&path_output_name, &src_path]() {
        try {
            if (boost::filesystem::exists(path_output_name))
                boost::filesystem::remove(path_output_name);
        } catch (const std::exception &err) {
            BOOST_LOG_TRIVIAL(error) << Slic3r::format("Failed deleting a file %1% carrying the final name / path of a G-code file %2%: %3%", path_output_name, src_path, err.what());
        }
    };
    // Remove possible stalled path_output_name of the previous run.
    remove_output_name_file();

    try {
        for (const std::string &scripts : post_process->values) {
    		std::vector<std::string> lines;
    		boost::split(lines, scripts, boost::is_any_of("\r\n"));
            for (std::string script : lines) {
                // Ignore empty post processing script lines.
                boost::trim(script);
                if (script.empty())
                    continue;
                BOOST_LOG_TRIVIAL(info) << "Executing script " << script << " on file " << path;
                std::string std_err;
                const int result = run_script(script, gcode_file.string(), std_err);
                if (result != 0) {
                    const std::string msg = std_err.empty() ? (boost::format("Post-processing script %1% on file %2% failed.\nError code: %3%") % script % path % result).str()
                        : (boost::format("Post-processing script %1% on file %2% failed.\nError code: %3%\nOutput:\n%4%") % script % path % result % std_err).str();
                    BOOST_LOG_TRIVIAL(error) << msg;
                    delete_copy();
                    throw Slic3r::RuntimeError(msg);
                }
                if (! boost::filesystem::exists(gcode_file)) {
                    const std::string msg = (boost::format(_(L(
                        "Post-processing script %1% failed.\n\n"
                        "The post-processing script is expected to change the G-code file %2% in place, but the G-code file was deleted and likely saved under a new name.\n"
                        "Please adjust the post-processing script to change the G-code in place and consult the manual on how to optionally rename the post-processed G-code file.\n")))
                        % script % path).str();
                    BOOST_LOG_TRIVIAL(error) << msg;
                    throw Slic3r::RuntimeError(msg);
                }
            }
        }
        if (boost::filesystem::exists(path_output_name)) {
            try {
                // Read a single line from path_output_name, which should contain the new output name of the post-processed G-code.
                boost::nowide::fstream f;
                f.open(path_output_name, std::ios::in);
                std::string new_output_name;
                std::getline(f, new_output_name);
                f.close();

                if (host == "File") {
                    namespace fs = boost::filesystem;
                    fs::path op(new_output_name);
                    if (op.is_relative() && op.has_filename() && op.parent_path().empty()) {
                        // Is this just a filename? Make it an absolute path.
                        auto outpath = fs::path(output_name).parent_path();
                        outpath /= op.string();
                        new_output_name = outpath.string();
                    }
                    else {
                        if (! op.is_absolute() || ! op.has_filename())
                            throw Slic3r::RuntimeError("Unable to parse desired new path from output name file");
                    }
                    if (! fs::exists(fs::path(new_output_name).parent_path()))
                        throw Slic3r::RuntimeError(Slic3r::format("Output directory does not exist: %1%",
                                                                  fs::path(new_output_name).parent_path().string()));
                }

                BOOST_LOG_TRIVIAL(trace) << "Post-processing script changed the file name from " << output_name << " to " << new_output_name;
                output_name = new_output_name;
            } catch (const std::exception &err) {
                throw Slic3r::RuntimeError(Slic3r::format("run_post_process_scripts: Failed reading a file %1% "
                                                          "carrying the final name / path of a G-code file: %2%",
                                                          path_output_name, err.what()));
            }
            remove_output_name_file();
        }
    } catch (...) {
        remove_output_name_file();
        delete_copy();
        throw;
    }

    src_path = std::move(path);
    return true;
}

} // namespace Slic3r
