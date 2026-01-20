#ifndef slic3r_Format_GaussianSplatTaichiLang_hpp_
#define slic3r_Format_GaussianSplatTaichiLang_hpp_

#include <string>
#include <vector>

namespace Slic3r {

struct GaussianSplatImportSpec
{
    std::string              object_name;
    std::vector<std::string> image_paths;

    // Placeholder geometry size in mm (until real Taichi runtime is wired).
    double preview_cube_size_mm { 20.0 };
};

// Generates an OrcaSlicer Taichi-language (.tai) script that describes a Gaussian Splat import job.
//
// Notes:
// - The current TaichiLang compiler in this repo only supports `geometry cube [x,y,z]` and raw mesh blocks.
// - Therefore we embed Gaussian Splat inputs as explicit directives (forward-compatible) AND also emit a
//   simple preview cube so the script produces a visible object today.
std::string gaussian_splat_to_taichi_lang(const GaussianSplatImportSpec& spec);

} // namespace Slic3r

#endif // slic3r_Format_GaussianSplatTaichiLang_hpp_
