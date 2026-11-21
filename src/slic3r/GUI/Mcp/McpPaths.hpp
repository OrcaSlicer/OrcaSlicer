#pragma once

#include <boost/filesystem/path.hpp>

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// UTF-8 text to a native path and back: wide on Windows, where boost's narrow conversion uses the
// ANSI code page. Agents, replays, scripts, JSON and Orca's data_dir() all speak UTF-8.
boost::filesystem::path path_from_utf8(const std::string& utf8);
std::string             path_to_utf8(const boost::filesystem::path& path);

// A key under which two spellings of one file compare equal: the path normalized, and where the file
// system ignores case (Windows, macOS), case-folded over all of Unicode, not just ASCII, with one kind of
// separator; on macOS also in the decomposed form its file systems compare in.
std::string path_compare_key(const boost::filesystem::path& path);

// A file argument from an agent, a replay or a script. Paths are UTF-8 and must be absolute: a relative path
// would resolve against <datadir>/log, where the GUI keeps its working directory.
std::optional<boost::filesystem::path> resolve_input_path(const std::string& path, std::string& err);

// An output file: absolute, its directory must exist, and an existing file is replaced only when
// `overwrite` is true.
std::optional<boost::filesystem::path> resolve_output_path(const std::string& path, bool overwrite, std::string& err);

// An existing directory (multi-file exports).
std::optional<boost::filesystem::path> resolve_output_dir(const std::string& path, std::string& err);

// The folder files are saved to when a call names none: the run's folder when the command line gave one
// (set_run_output_dir), else <datadir>/automation/out. Created on first use.
boost::filesystem::path output_dir();
// An empty path takes the run's folder back.
void set_run_output_dir(const boost::filesystem::path& dir);

// The folder for the results of a replay run that names no out_dir: "replay-YYYYMMDD-HHMMSS" for the local time
// `when`, in the run's folder when the command line gave one (set_run_output_dir), else in
// <temp>/orca_action_replay, so a run leaves nothing beside the replay file or in the data folder. A name a folder
// already has, such as that of the replay running this one as a step in the same second, gets "-2", "-3"... It is
// not created: the run creates it.
boost::filesystem::path replay_output_dir(std::time_t when = std::time(nullptr));

// A path in `dir` for a new file named "OrcaSlicer-<kind>-YYYYMMDD-HHMMSS<extension>" for the local time
// `when`, so files of one kind sort by when they were made and say where they came from. A name taken by a
// file, or by one with any of `companions` (the extensions of files made from it later), gets "-2", "-3"...
boost::filesystem::path timestamped_path(const boost::filesystem::path& dir, const std::string& kind, const std::string& extension,
                                         const std::vector<std::string>& companions = {}, std::time_t when = std::time(nullptr));

}}} // namespace Slic3r::GUI::Mcp
