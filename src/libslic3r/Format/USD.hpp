#ifndef slic3r_Format_USD_hpp_
#define slic3r_Format_USD_hpp_

#include <string>

namespace Slic3r {

class TriangleMesh;
class Model;

// Load a USD stage into a provided model. Handles .usd, .usda, .usdc and .usdz.
//
// Meshes are imported in millimetres and Z-up, applying the stage's own
// metersPerUnit and upAxis. The Model overload gives each prim its own
// ModelVolume named for its prim path; the TriangleMesh overload merges them.
// Subdivision is not evaluated: a cage is imported as its control mesh, which
// is what the existing macOS ModelIO path does.
//
// `message` receives a user-facing explanation on failure, which
// Model::read_from_file raises as a RuntimeError. On success anything skipped
// is tallied into it too, but the caller only reads it on failure, so that
// summary reaches the log alone.
extern bool load_usd(const char *path, TriangleMesh *meshptr, std::string &message);
extern bool load_usd(const char *path, Model *model, std::string &message,
                     const char *object_name = nullptr);

}; // namespace Slic3r

#endif /* slic3r_Format_USD_hpp_ */
