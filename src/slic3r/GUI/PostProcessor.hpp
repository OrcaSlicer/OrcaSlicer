#ifndef slic3r_GUI_PostProcessor_hpp_
#define slic3r_GUI_PostProcessor_hpp_

#include <functional>
#include <string>

#include "libslic3r/libslic3r.h"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

// Run post-processing scripts (the "post_process" option) and/or the slicing-pipeline plugins'
// Step.psGCodePostProcess seam (the "slicing_pipeline_plugin" option) if defined. Lives in the GUI
// layer because plugins are executed through the embedded-Python PluginManager, which libslic3r must
// not depend on.
// Returns true if a script or plugin was executed.
// Returns false if neither a post-processing script nor plugin was defined.
// Throws an exception on error.
// host is one of "File", "PrusaLink", "Repetier", "SL1Host", "OctoPrint", "FlashAir", "Duet", "AstroBox" ...
// If make_copy, then a temp file will be created for src_path by adding a ".pp" suffix and src_path will be updated.
// In that case the caller is responsible to delete the temp file created. Scripts and plugins always
// run on this working copy so they never touch the original G-code the viewer keeps memory-mapped
// (a writable open of the mapped file fails on Windows with a sharing violation).
// output_name is the final name of the G-code on SD card or when uploaded to PrusaLink or OctoPrint.
// If uploading to PrusaLink or OctoPrint, then the file will be renamed to output_name first on the target host.
// The post-processing script may change the output_name.
extern bool run_post_process_scripts(
    std::string& src_path, bool make_copy, const std::string& host, std::string& output_name, const DynamicPrintConfig& config);

inline bool run_post_process_scripts(std::string& src_path, const DynamicPrintConfig& config)
{
    std::string src_path_name = src_path;
    return run_post_process_scripts(src_path, false, "File", src_path_name, config);
}

// BBS
extern void gcode_add_line_number(const std::string& path, const DynamicPrintConfig& config);
} // namespace Slic3r

#endif /* slic3r_GUI_PostProcessor_hpp_ */
