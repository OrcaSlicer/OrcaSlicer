#ifndef slic3r_TextureLibrary_hpp_
#define slic3r_TextureLibrary_hpp_

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::GUI {

// One selectable height-map texture in the texture-displacement gizmo's texture picker.
struct TextureLibraryEntry
{
    std::string name;    // display name (the file's stem, e.g. "Wood Grain")
    std::string path;    // absolute path on disk
    bool        is_user; // imported by the user, as opposed to shipped with OrcaSlicer
};

// Every available height-map texture: the ones shipped in resources/textures/displacement first,
// then the user's own from <data_dir>/textures/displacement, each group sorted by name.
//
// The two live in separate directories deliberately: an app update replaces the resources tree
// wholesale, so anything the user imported has to sit somewhere that update can never overwrite or
// delete. `is_user` is what the picker uses to show them under separate headings.
//
// Scanned once and cached. Pass force_rescan after an import, or to pick up a file the user dropped
// into either folder by hand while the app was running.
const std::vector<TextureLibraryEntry> &texture_library(bool force_rescan = false);

// <data_dir>/textures/displacement, created if it does not exist yet. Empty string on failure.
std::string user_texture_dir();

// Reads any image format wxWidgets can open, converts it to the 8-bit grayscale PNG that
// libslic3r's decode_height_texture() understands, and saves it into user_texture_dir() (uniquified
// if that name is taken). The conversion has to happen here rather than in libslic3r, which has no
// image toolkit and so only ever handles the one already-normalized format.
//
// Returns the newly imported entry, or nullopt with `error` set. `source_path` is only read.
std::optional<TextureLibraryEntry> import_texture_to_library(const std::string &source_path, std::string &error);

// Encoded bytes of `path`, ready to hand to TextureDisplacementLayer::image_data. Files already in
// the supported 8-bit grayscale PNG form (everything in the two library folders, by construction)
// are passed through verbatim; anything else - e.g. a colour PNG the user copied into the folder
// by hand - is converted on the fly, so a valid image never silently produces a blank layer.
// Returns nullptr with `error` set if the file cannot be read or decoded at all.
std::shared_ptr<std::vector<unsigned char>> load_texture_image_data(const std::string &path, std::string &error);

} // namespace Slic3r::GUI

#endif // slic3r_TextureLibrary_hpp_
