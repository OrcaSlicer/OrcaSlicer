#ifndef slic3r_Format_TaichiLang_hpp_
#define slic3r_Format_TaichiLang_hpp_

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

class Model;

std::string model_to_taichi_lang(const Model& model);

// Applies a subset of Taichi-language text back onto the existing Model.
// Currently supported:
// - object name changes
// - instance transforms (offset/rotation/scale/mirror)
// - (minimal compiler) geometry generation: `geometry cube [x,y,z]` inside an object block
// - mesh blocks: `mesh_begin` + `v [...]` + `f [...]` + `mesh_end`
//
// Returns false and sets error on parse/validation failure.
bool apply_taichi_lang_to_model(Model& model, const std::string& text, std::vector<size_t>& changed_object_indices, std::string& error);

// Compiles Taichi-language text into *new* model object(s), appending them to the existing Model.
// Returns false and sets error on parse/validation failure.
bool append_taichi_lang_to_model(Model& model, const std::string& text, std::vector<size_t>& added_object_indices, std::string& error);

} // namespace Slic3r

#endif // slic3r_Format_TaichiLang_hpp_
