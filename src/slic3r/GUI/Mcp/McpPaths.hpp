#pragma once

#include <boost/filesystem/path.hpp>

#include <optional>
#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

// A file argument from an agent or a script. Paths are UTF-8 and must be absolute: a relative path
// would resolve against <datadir>/log, where the GUI keeps its working directory.
std::optional<boost::filesystem::path> resolve_input_path(const std::string& path, std::string& err);

// An output file: absolute, its directory must exist, and an existing file is replaced only when
// `overwrite` is true.
std::optional<boost::filesystem::path> resolve_output_path(const std::string& path, bool overwrite, std::string& err);

// An existing directory (multi-file exports).
std::optional<boost::filesystem::path> resolve_output_dir(const std::string& path, std::string& err);

// The parameter of a Speed Dial command that takes a path: a plain absolute path, or a JSON object
// string {"path": "...", "overwrite": true}.
struct PathParam
{
    std::string path;
    bool        overwrite = false;
};
std::optional<PathParam> parse_path_param(const std::string& param, std::string& err);

}}} // namespace Slic3r::GUI::Mcp
