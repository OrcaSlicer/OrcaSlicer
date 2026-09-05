#pragma once

#include "Color.hpp"
#include "MixedFilamentMatch.hpp"
#include "Model.hpp"

#include <string>
#include <vector>

namespace Slic3r {

struct SwatchLUTEntry
{
    std::string recipe; // "P{n}" or serialize_mix_recipe row
    double      L = 0;
    double      a = 0;
    double      b = 0;
};

struct SwatchLUT
{
    int         version         = 1;
    std::string mode            = "translucent"; // opaque/overhang reserved
    double      layer_height_mm = 0.08;
    std::string batch_key;
    std::vector<SwatchLUTEntry> entries;

    const SwatchLUTEntry *find_recipe(const std::string &recipe) const;
};

struct SwatchManifestPad
{
    int         index    = 0;
    int         row      = 0;
    int         col      = 0;
    std::string recipe;
    int         extruder = 1; // 1-based Physical or Mix
};

struct SwatchManifest
{
    std::string                     batch_key;
    std::vector<SwatchManifestPad>  pads;
};

struct SwatchPlateBuild
{
    bool            valid = false;
    std::string     error;
    std::string     mixed_filament_definitions;
    SwatchManifest  manifest;
    ModelObject    *object = nullptr;
};

// "P{n}" for Physical; Mix recipe_row otherwise.
std::string spectrum_swatch_recipe_key(const MixMatchResult &r);

std::vector<std::string> spectrum_swatch_recipe_keys(const std::vector<MixMatchResult> &lattice);

// First-4 hexes + names + TD-if-set. nullptr / empty td omits the TD section.
// Partial td hashes the available prefix only. Same inputs → same key.
std::string spectrum_compute_batch_key(const std::vector<std::string> &hexes,
                                       const std::vector<std::string> &names,
                                       const std::vector<float>       *td = nullptr);

// Card TDs when physicals hex-match a vendor card; else omit.
std::string spectrum_compute_batch_key(const std::vector<ColorRGB>  &physicals,
                                       const std::vector<std::string> &names,
                                       const std::vector<float>       *td = nullptr);

std::string serialize_swatch_lut(const SwatchLUT &lut);
std::string serialize_swatch_manifest(const SwatchManifest &manifest);

// allowed_recipes: "P{n}" and Mix recipe_rows. Unknown recipe → false (no silent drop).
bool parse_swatch_lut_json(const std::string              &text,
                           const std::vector<std::string> &allowed_recipes,
                           SwatchLUT                      &out,
                           std::string                    &error);

bool parse_swatch_lut_csv(const std::string              &text,
                          const std::vector<std::string> &allowed_recipes,
                          SwatchLUT                      &out,
                          std::string                    &error);

bool parse_swatch_manifest_json(const std::string &text, SwatchManifest &out, std::string &error);

// dir empty → data_dir()/spectrum. Returns false if the path cannot be written.
bool save_swatch_lut(const SwatchLUT &lut, std::string &error, const std::string &dir = {});
bool load_swatch_lut(const std::string &batch_key, SwatchLUT &out, std::string &error,
                     const std::string &dir = {});
bool save_swatch_manifest(const SwatchManifest &manifest, std::string &error,
                          const std::string &dir = {});
bool load_swatch_manifest(const std::string &batch_key, SwatchManifest &out, std::string &error,
                          const std::string &dir = {});

// Last imported LUT for this process (may be stale vs live batch).
// dir empty → data_dir()/spectrum. Also writes swatch_lut_active.json.
void             spectrum_store_loaded_lut(SwatchLUT lut, const std::string &dir = {});
const SwatchLUT *spectrum_loaded_lut();
// Matching loaded or on-disk LUT for batch_key. nullptr if none. Never returns a stale LUT.
const SwatchLUT *spectrum_lut_for_batch(const std::string &batch_key, const std::string &dir = {});
// True when an imported/active LUT exists and its batch_key != live (survives process restart).
bool             spectrum_lut_is_stale(const std::string &live_batch_key, const std::string &dir = {});
void             spectrum_reset_lut_session();

// One ModelObject, one volume per pad. Pad extruder = Physical 1–n or Mix mix_base+1….
// Grid is row-major and fits bed_mm (Ultra S 270). Does not use PicPrint plate helpers.
SwatchPlateBuild build_swatch_plate(Model                            &model,
                                    const std::vector<MixMatchResult> &lattice,
                                    size_t                             mix_base,
                                    const std::string                 &batch_key = {},
                                    double                             bed_mm    = 270.0);

} // namespace Slic3r
