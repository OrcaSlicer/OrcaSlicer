#ifndef slic3r_AIGenerate_hpp_
#define slic3r_AIGenerate_hpp_

#include <functional>
#include <string>

namespace Slic3r { namespace GUI {

// Generate a 3D shape from a text prompt and drop it on the plate, reusing the
// full pipeline (machine context -> forced create_model tool call -> geometry ->
// repair loop -> Plater injection) on a background Job. Safe to call from the
// main thread; `on_done` is invoked on the main thread with (ok, message).
//
// Shared by the AI menu dialog and the compact in-panel Generate control.
void ai_generate_shape_to_plate(const std::string &prompt,
                                std::function<void(bool ok, const std::string &message)> on_done);

}} // namespace Slic3r::GUI

#endif // slic3r_AIGenerate_hpp_
