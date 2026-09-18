#pragma once

#include "SemanticColoring.hpp"

namespace Slic3r::AI::SemanticColoring {

// Add material-supported dark-hair evidence to one model-independent body
// mask. The recognizer remains responsible for its own labels; this business
// refinement only joins a source-color component that already contains enough
// reliable Hair pixels. Returns false on cancellation without changing output.
bool refine_dark_hair_mask(const RGBImage&, Prediction&, const Cancel& = {});

} // namespace Slic3r::AI::SemanticColoring
