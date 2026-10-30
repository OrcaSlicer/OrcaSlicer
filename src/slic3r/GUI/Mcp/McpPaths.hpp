#pragma once

#include <boost/filesystem/path.hpp>

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// UTF-8 text to a native path and back: wide on Windows, where boost's narrow conversion uses the
// ANSI code page. Agents, scripts, JSON and Orca's data_dir() all speak UTF-8.
boost::filesystem::path path_from_utf8(const std::string& utf8);
std::string             path_to_utf8(const boost::filesystem::path& path);

// A key under which two spellings of one file compare equal: the path normalized, and where the file
// system ignores case (Windows, macOS), case-folded over all of Unicode, not just ASCII, with one kind of
// separator; on macOS also in the decomposed form its file systems compare in.
std::string path_compare_key(const boost::filesystem::path& path);

// A file argument from an agent or a script. Paths are UTF-8 and must be absolute: a relative path
// would resolve against <datadir>/log, where the GUI keeps its working directory.
std::optional<boost::filesystem::path> resolve_input_path(const std::string& path, std::string& err);

// An output file: absolute, its directory must exist, and an existing file is replaced only when
// `overwrite` is true.
std::optional<boost::filesystem::path> resolve_output_path(const std::string& path, bool overwrite, std::string& err);

// An existing directory (multi-file exports).
std::optional<boost::filesystem::path> resolve_output_dir(const std::string& path, std::string& err);

// Where screenshots, recordings and script outputs go when no path is given: <datadir>/mcp/out, created
// when missing.
boost::filesystem::path output_dir();

// A path in `dir` for a new file named "OrcaSlicer-<kind>-YYYYMMDD-HHMMSS<extension>" for the local time
// `when`, so files of one kind sort by when they were made and say where they came from. A name taken by a
// file, or by one with any of `companions` (the extensions of files made from it later), gets "-2", "-3"...
boost::filesystem::path timestamped_path(const boost::filesystem::path& dir, const std::string& kind, const std::string& extension,
                                         const std::vector<std::string>& companions = {}, std::time_t when = std::time(nullptr));

// The parameter of a Speed Dial command that takes a path: a plain absolute path, or a JSON object
// string {"path": "...", "overwrite": true}.
struct PathParam
{
    std::string path;
    bool        overwrite = false;
};
std::optional<PathParam> parse_path_param(const std::string& param, std::string& err);

}}} // namespace Slic3r::GUI::Mcp
