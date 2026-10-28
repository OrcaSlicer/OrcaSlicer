#ifndef slic3r_TransmissionDistance_hpp_
#define slic3r_TransmissionDistance_hpp_

#include <cstddef>
#include <string>
#include <vector>

#include "Color.hpp"

namespace Slic3r {

// The default `filament_transmission_distance` carries until somebody sets one. A filament nobody
// has measured reads as this, which is how "not known" is told from "known to be 4".
static constexpr float FILAMENT_TD_DEFAULT_MM = 4.f;

// The window a transmission distance has to land in to mean anything. Zero would make a layer
// infinitely opaque and a negative value is not a distance, so the colour model clamps to this same
// window and the `filament_transmission_distance` option is bounded at 0 and 100. A library that
// offers 0, -1 or 1e9 is offering a number nothing can use, and the import clamps it rather than
// dropping the row: the row still says which colour it is and roughly how translucent.
static constexpr float FILAMENT_TD_MIN_MM = 0.05f;
static constexpr float FILAMENT_TD_MAX_MM = 100.f;

// One filament a table knows a transmission distance for, whether we measured it or a library the
// user imported did.
//
// A transmission distance is a property of a *colour*, not of a product line: one maker's own values
// for a single line of matte PLA run from 0.1 to 4.1 across its colours. A filament preset is the
// line, and carries no colour - so the preset is the right home for the number and the wrong thing
// to look it up by. The pair that does identify a spool is the preset's vendor and the colour the
// filament is set to, and both of those a project already knows.
struct FilamentTd
{
    std::string brand;   // as the table spells it, lower case
    std::string name;    // the full name, line and colour together
    std::string type;    // PLA, PETG, ...
    ColorRGB    color;
    float       td_mm = 0.f;

    // The two below are appended rather than ordered where they read best, so that the five-field
    // aggregate initialisation that the existing code and tests use keeps compiling.

    // Where the number came from: the name of the file or library that offered it, as the card shows
    // it. Data, not a sentence: the GUI is what wraps it in translated text.
    std::string td_source;
    // Read from a library the user imported rather than measured here and shipped. The number is
    // somebody else's measurement, so it has to be tellable from ours, both on the card and when two
    // tables offer a value for the same spool.
    bool imported = false;
};

// Every table the program knows: the user's own files under `data_dir()/filament_td`, the libraries
// they imported, and the tables that ship in `resources/filament_td`. Read once on first use, so a
// fresh import is picked up on the next load. Empty when they are all missing or will not parse,
// which is not an error: the option they fill in has a working default.
//
// The order is the precedence, and it only ever decides an exact tie, since the search below picks on
// match quality first: a file the user wrote themselves, then an imported library, then what shipped.
const std::vector<FilamentTd> &filament_td_table();

// Drops the cached table so the next lookup reads the directories again. Called after an import, which
// would otherwise only take effect on the next run.
//
// Call it from the UI thread, and not while anything is holding an entry from the table: the pointer
// filament_td_lookup() returns points into the cache, and a rebuild invalidates it. Every consumer in
// the tree copies the value it wanted out of the entry before returning, which is what makes that
// contract keepable rather than theoretical.
void filament_td_reset_cache();

// The table's best guess at a filament's transmission distance, or nullptr when it has none worth
// offering. `vendor` and `name` come from the filament preset, `color` from the filament itself.
//
// Both halves have to agree: the name has to put the filament in a line the table knows, and the
// colour has to be close enough to one of that line's colours to be that colour. A match on the name
// alone would hand a white filament the number measured for a black one, which is worse than the
// default - it is wrong, and it looks measured.
const FilamentTd *filament_td_lookup(const std::string &vendor, const std::string &name, const ColorRGB &color);

// The same search against a table handed in rather than the shipped one. The matching is the part
// with judgement in it, so it is reachable without a resources directory to read.
const FilamentTd *filament_td_lookup(const std::vector<FilamentTd> &table, const std::string &vendor,
                                     const std::string &name, const ColorRGB &color);

// --------------------------------------------------------------------------------------------------
// Importing somebody else's library
//
// We ship a few hundred measured colours; the HueForge crowd has measured tens of thousands between
// them, and a user who already has that file is one dialog away from predictions worth trusting. So
// the importer reads a file the user points at. Nobody else's data is shipped, and the tables under
// `resources/filament_td` are never touched: an import lands in its own file under
// `data_dir()/filament_td`.
//
// Every such file is untrusted input, because it is a file somebody downloaded. Nothing below throws,
// nothing below trusts a length, and the caps are what stops a hostile file from eating the process.
// --------------------------------------------------------------------------------------------------

// A library this big is already far past every real one, and the point past which a file is more
// likely hostile than useful. Entries past it are counted and dropped.
static constexpr size_t FILAMENT_TD_IMPORT_MAX_ENTRIES = 20000;
// A brand, a name or a type longer than this is not a name, so it is cut rather than stored: one
// field of a few megabytes in every row is how a small file becomes a large allocation.
static constexpr size_t FILAMENT_TD_IMPORT_MAX_STRING = 200;
// The whole file, before any of it is parsed. A twenty thousand entry library is a couple of
// megabytes of text, and a JSON parse builds a tree several times the size of what it read.
static constexpr size_t FILAMENT_TD_IMPORT_MAX_BYTES = 16u * 1024u * 1024u;

// What an import did, so the GUI can say what happened. Counts and not a sentence: the sentence has
// to be translated, with plural forms, and this file is the wrong place for translated text.
struct FilamentTdImport
{
    size_t taken = 0;    // entries normalized, and written when it was an import that read them
    size_t clamped = 0;  // of those, how many had a TD pulled into FILAMENT_TD_MIN/MAX_MM

    // Why the rest were dropped. Each one is a thing the user can look at and fix in their file,
    // which is why they are counted apart rather than summed.
    size_t skipped_no_name = 0;
    size_t skipped_no_brand = 0;  // nothing can look up a filament whose maker is not named
    size_t skipped_no_color = 0;  // nor one with no colour: a TD belongs to a colour
    size_t skipped_no_td = 0;
    size_t skipped_duplicate = 0;
    size_t skipped_over_cap = 0;  // past FILAMENT_TD_IMPORT_MAX_ENTRIES

    size_t skipped() const
    {
        return skipped_no_name + skipped_no_brand + skipped_no_color + skipped_no_td + skipped_duplicate +
               skipped_over_cap;
    }

    // The file could not be opened, is larger than the cap, or parsed as neither JSON nor CSV. Told
    // apart from a file that parsed and held nothing we could use, which is the user's file to fix.
    bool unreadable = false;

    // The `td_source` stamped on every entry taken: the imported file's own name.
    std::string source;
    // The file under `data_dir()/filament_td` the entries went to. Empty when nothing was written,
    // so `taken > 0` with this empty is the one remaining failure: the write itself did not happen.
    std::string written_path;
};

// Read a filament library the user points at, normalize it into our shape, and write it to one file
// per source under `data_dir()/filament_td`, which `filament_td_table()` reads on the next load.
// Importing the same path again replaces that file rather than adding another, so a corrected library
// is just a re-import.
//
// Never throws, whatever the file holds.
FilamentTdImport filament_td_import(const std::string &path);

// The same read with nothing written, appending to `out`. The parsing is the part with judgement in
// it, so it is reachable without a data directory to write into.
FilamentTdImport filament_td_parse(const std::string &path, std::vector<FilamentTd> &out);

// A transmission distance as the `filament_transmission_distance` option wants it written, so that
// "put this measurement into the preset" does not have to reinvent the rounding. Clamped to the range
// the option allows, and cut to the two decimals a TD is known to: more digits are noise that makes a
// preset look measured to a precision nobody measured.
std::string filament_td_format_mm(float td_mm);

} // namespace Slic3r

#endif // slic3r_TransmissionDistance_hpp_
