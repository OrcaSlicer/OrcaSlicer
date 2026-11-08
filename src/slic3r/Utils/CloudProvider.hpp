#pragma once

#include <string>

namespace Slic3r {

// Identifiers of the cloud services an ICloudServiceAgent can stand for.
static const std::string ORCA_CLOUD_PROVIDER("orca");
static const std::string BBL_CLOUD_PROVIDER("bbl");
// Unified Slicer: Elegoo account (matrix.elegoo.com), kept separate from the Bambu and Orca accounts.
static const std::string ELEGOO_CLOUD_PROVIDER("elegoo");

} // namespace Slic3r
