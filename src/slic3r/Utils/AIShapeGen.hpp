#ifndef slic3r_AIShapeGen_hpp_
#define slic3r_AIShapeGen_hpp_

#include <string>

#include <nlohmann/json.hpp>

namespace Slic3r {

class Model;

// ---------------------------------------------------------------------------
// Text-to-shape core (pure libslic3r — no GUI, no network).
//
// The LLM returns a compact JSON "shape spec": parametric primitives
// (box / cylinder / cone / sphere) combined with CSG (union / difference /
// intersection) and per-node affine transforms (translate / rotate / scale).
// A raw-STL fallback is also accepted. These helpers validate a spec and bake
// it into an in-memory Slic3r::Model ready for the Plater.
// ---------------------------------------------------------------------------

// Human/LLM-readable description of the shape-spec contract; embed in the
// system prompt so the model returns something we can parse.
std::string ai_shape_spec_instructions();

// Build a Model from a validated shape-spec JSON tree.
// Returns false and fills `error` on any validation/geometry failure.
bool ai_build_model_from_spec(const nlohmann::json &spec,
                              const std::string    &name,
                              Model                &out_model,
                              std::string          &error);

// Build a Model from raw STL bytes (ASCII or binary) by staging a temp file.
bool ai_build_model_from_stl(const std::string &stl_bytes,
                             const std::string &name,
                             Model             &out_model,
                             std::string       &error);

// Parse an LLM response that is EITHER a JSON shape-spec (optionally wrapped in
// a ``` code fence) OR raw STL, and build a Model from whichever it is.
bool ai_build_model_from_response(const std::string &response,
                                  const std::string &name,
                                  Model             &out_model,
                                  std::string       &error);

// Build a Model from the JSON arguments of a forced create_model tool call.
// Accepts either {"shape": <node>} or a bare <node>. This is the reliable path:
// a model that returns a tool call is producing structured geometry, not prose.
bool ai_build_model_from_tool_call(const std::string &args_json,
                                   const std::string &name,
                                   Model             &out_model,
                                   std::string       &error);

// Bed-fit check: false (with a warning) if the model's XY/Z extent exceeds the
// given build volume in mm. Used to keep generated geometry printable.
bool ai_model_fits_bed(const Model &model,
                       double       bed_x,
                       double       bed_y,
                       double       bed_z,
                       std::string &warning);

} // namespace Slic3r

#endif // slic3r_AIShapeGen_hpp_
