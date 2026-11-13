#ifndef slic3r_TextureLibrary_hpp_
#define slic3r_TextureLibrary_hpp_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/TextureDisplacement.hpp"

namespace Slic3r::GUI {

// Where a library texture came from. The picker lists each source under its own heading.
enum class TextureSource { Shipped, User, Plugin };

// One selectable height-map texture in the texture-displacement gizmo's texture picker. A texture that
// came from a plugin pack also carries the provenance the picker groups its sections by.
struct TextureLibraryEntry
{
    std::string   name;   // display name (the file's stem, e.g. "Wood Grain")
    std::string   path;   // absolute path on disk
    TextureSource source; // which of the three sources it came from

    std::string pack_name; // contributing pack's display name; empty for Shipped and User
};

// Every available height-map texture, in the order the picker lists them: the ones shipped in
// resources/textures/displacement first, by name; then the user's own from <data_dir>/textures/displacement,
// most recently used first (see touch_user_texture()) and the rest by name; then the loaded texture
// packs, ordered by pack display name and then by file name.
//
// The shipped and user textures live in separate directories deliberately: an app update replaces the
// resources tree wholesale, so anything the user imported has to sit somewhere that update can never
// overwrite or delete. Pack textures are read-only like the shipped ones. Every texture carries its
// `source`; a pack texture also carries `pack_name`, and the picker groups pack textures into one
// collapsible section per pack name. A pack's name is unique among loaded packs of this capability type -
// the loader refuses a collision with "Capability collision for type ... and name ...", see
// PluginManager::check_registry_locked (PluginManager.cpp:211-225) - so the name on its own identifies a
// section and keeps that pack's entries contiguous.
//
// Scanned once and cached. Pass force_rescan after an import, or to pick up a file the user dropped
// into either folder by hand while the app was running. A rescan also happens on its own when the pack
// registry changes, which is what texture_library_generation() tracks.
const std::vector<TextureLibraryEntry> &texture_library(bool force_rescan = false);

// Monotonic counter, bumped whenever the set of loaded texture packs changes. The GUI reads it to
// decide whether texture_library() must rescan, and plugin callbacks bump it. Safe to read from any
// thread at any time.
uint64_t texture_library_generation();
void     bump_texture_library_generation();

// Moves one of the user's own textures to the front of the recently-used order texture_library() lists
// them in, and stores that order in the app config so it survives a restart. No-op for anything that is
// not in user_texture_dir().
void touch_user_texture(const std::string &path);

// Deletes one of the user's own imported textures from user_texture_dir() and forgets it. Anything outside
// that folder is refused, so a shipped texture can never be removed this way. Layers already using the
// texture are unaffected: they hold the image bytes themselves, and a project stores those. Returns false
// with `error` set on failure.
bool remove_user_texture(const std::string &path, std::string &error);

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

// Writes `image` into user_texture_dir() as a new PNG named after `stem` (uniquified if taken, so an
// existing texture is never overwritten) and returns its library entry. The in-memory counterpart of
// import_texture_to_library(), for an image that was edited rather than read off disk.
//
// Returns nullopt with `error` set on failure.
std::optional<TextureLibraryEntry> save_texture_to_library(const DecodedHeightTexture &image, const std::string &stem,
                                                           std::string &error);

} // namespace Slic3r::GUI

#endif // slic3r_TextureLibrary_hpp_
