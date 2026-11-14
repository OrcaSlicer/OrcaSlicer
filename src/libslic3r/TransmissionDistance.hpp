#ifndef slic3r_TransmissionDistance_hpp_
#define slic3r_TransmissionDistance_hpp_

#include <cstddef>
#include <ctime>
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

// Where a transmission distance came from, as a kind rather than as a file name. The four kinds are
// not a taxonomy for its own sake: each one answers a different question about how much the number is
// worth and who is allowed to change it.
//
// Personal is the one that matters. A number the user measured on their own printer with their own
// spool is the best evidence there is, and it is also the only one nobody else can reproduce, so it
// lives in its own file, outranks every library, and no update is ever allowed to write to it.
enum class FilamentTdSourceKind
{
    Shipped,      // `resources/filament_td`, measured by us and read-only
    Personal,     // the user's own measurements, under `data_dir()/filament_td`
    Imported,     // a library the user pointed the importer at once
    Subscription, // a library the user subscribed to, re-fetched by the GUI and merged
};

// The kind as it is written in a file, and read back. A word rather than a number, so that a sources
// file is readable by the person whose sources it lists, and so that a kind added later does not
// renumber the ones before it.
const char *filament_td_source_kind_key(FilamentTdSourceKind kind);
FilamentTdSourceKind filament_td_source_kind_of(const std::string &key);

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

    // Everything below is appended rather than ordered where it reads best, so that the five-field
    // aggregate initialisation that the existing code and tests use keeps compiling. A field added
    // later goes on the end for the same reason.

    // Where the number came from: the name of the file or library that offered it, as the card shows
    // it. Data, not a sentence: the GUI is what wraps it in translated text.
    std::string td_source;
    // Read from a library the user imported rather than measured here and shipped. The number is
    // somebody else's measurement, so it has to be tellable from ours, both on the card and when two
    // tables offer a value for the same spool.
    bool imported = false;

    // Which of `filament_td_sources()` offered this number, by id, and what kind of a source that is.
    // `td_source` is the sentence fragment the card prints; these two are what the program compares,
    // which is the difference between showing provenance and acting on it. A source the user can
    // reorder, disable or update makes "where did this number come from" a question with an answer
    // the program has to hold, not just display.
    std::string          source_id;
    FilamentTdSourceKind source_kind = FilamentTdSourceKind::Shipped;
};

// --------------------------------------------------------------------------------------------------
// The sources a transmission distance can come from
//
// Measured data is the kind of thing a community produces and shares, so a user ends up with several
// libraries at once: what we ship, a file they imported, a list somebody keeps up to date, and their
// own measurements. Those are not interchangeable, and which of them answers for a given spool is a
// decision the user has to be able to see and change - hence a list, in precedence order, each row
// saying where it came from, when it was last refreshed and whether it counts at all.
//
// One rule is not the user's to change, because getting it wrong loses work that cannot be recovered:
// a Personal source always sits above every other kind, whatever order the file on disk asks for, and
// nothing in here ever writes to a Personal source's file except the call that records a measurement.
// A subscription that refreshes itself every week is otherwise one bad fetch away from overwriting the
// afternoon somebody spent printing wedges.
// --------------------------------------------------------------------------------------------------

// More libraries than anybody keeps, and the point past which a sources file is a mistake rather than
// a list. Rows past it are dropped on the way in.
static constexpr size_t FILAMENT_TD_SOURCES_MAX = 256;

// One library the program knows about. `id` is also the stem of its file under
// `data_dir()/filament_td`, which is what makes a source removable by deleting a file and what keeps a
// re-import or a re-fetch replacing its library rather than piling up another copy.
struct FilamentTdSource
{
    std::string          id;                                     // stable, file-name safe, unique in the list
    std::string          name;                                   // what the card shows; data, not a sentence
    FilamentTdSourceKind kind = FilamentTdSourceKind::Imported;
    std::string          origin;                                 // the file it was imported from, or the URL it is fetched from
    std::time_t          updated = 0;                            // when its file was last written, 0 for never
    bool                 enabled = true;                         // off means its entries are not read at all
    size_t               entries = 0;                            // how many its file holds, for the card to say
};

// Every source, in precedence order, plus the entries the user has pinned to their own measurement.
//
// The order decides an exact tie and nothing else, because a lookup picks on match quality first: a
// library that matches the spool in hand better than the source above it does still wins, which is the
// search's business and not the order's.
struct FilamentTdSourceList
{
    std::vector<FilamentTdSource> sources;
    // Entry keys (see filament_td_entry_key) the user has excluded from updates. A spool the user
    // measured themselves, whose number an update must not touch even where the library it came from
    // has since changed its mind.
    std::vector<std::string> suppressed;

    const FilamentTdSource *find(const std::string &id) const;
    bool                    is_suppressed(const std::string &entry_key) const;
};

// What makes two rows the same spool: the brand key, the words of the name and the colour as the three
// bytes it is written as. Public because suppression is a list of these, and because a GUI that offers
// "keep my own number for this one" has to be able to name the one.
std::string filament_td_entry_key(const FilamentTd &entry);

// The sources as they stand: the list stored in `data_dir()/filament_td/sources.json`, with every
// library actually present in that directory folded into it, and the shipped table last.
//
// A directory with no sources file is the common case and not a special one: the list is derived from
// what is there, which comes out as the order this always had - the user's own measurements, then the
// libraries they imported, then what shipped.
//
// Same contract as filament_td_table(), whose cache it shares: the reference points into that cache
// and a reset invalidates it, so copy what you mean to keep.
const FilamentTdSourceList &filament_td_sources();

// Write a source list back, and drop the cached table so the next lookup sees it. Only the order, the
// names, `enabled` and the suppression list are taken from what is handed in; the counts and the
// timestamps are the files' own business and are re-derived on the next load.
//
// False when the file could not be written, which leaves the list on disk as it was.
bool filament_td_sources_save(const FilamentTdSourceList &list);

// Subscribe to a library at a URL. Writes nothing but the source list: there is no fetch here, because
// libslic3r does no networking. The entries arrive when the GUI fetches the URL and hands the text to
// filament_td_update_prepare(). Returns the new source's id, or the existing one when that URL is
// already subscribed to, or an empty string when the list could not be written.
std::string filament_td_subscribe(const std::string &url, const std::string &name);

// Forget a source: its row goes out of the list and its library file is deleted. A Personal source is
// refused, because that file is the one thing in here that cannot be fetched again. Shipped is refused
// too, since it is not ours to delete; disable it instead.
bool filament_td_source_remove(const std::string &id);

// Keep this spool's number against every update, or stop keeping it. The key is held rather than the
// entry, so that suppression survives the library changing its own copy of the row.
bool filament_td_suppress(const FilamentTd &entry, bool suppressed);

// Record a measurement of the user's own into `data_dir()/filament_td/personal.json`, which outranks
// every library and which no update writes to. `suppress` also pins the spool, which is almost always
// what somebody who just measured it wants: the number they measured is the number they meant.
//
// Entries are keyed the way a lookup keys them, so measuring the same spool twice corrects it rather
// than listing it twice. False when the file could not be written.
bool filament_td_record_measured(const FilamentTd &entry, bool suppress = true);

// Everything in the Personal sources, which is what "the user's own measurements" means: the rows
// filament_td_record_measured() wrote, and anything the user put in a file of their own beside them.
std::vector<FilamentTd> filament_td_personal_entries();

// Every table the program knows, concatenated: the enabled sources of filament_td_sources() in their
// precedence order, each entry stamped with the source that offered it. Read once on first use, so a
// fresh import or a refreshed subscription is picked up on the next load. Empty when they are all
// missing or will not parse, which is not an error: the option they fill in has a working default.
//
// The order is the precedence, and it only ever decides an exact tie, since the search below picks on
// match quality first: the user's own measurements, then the libraries they have, then what shipped.
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

// --------------------------------------------------------------------------------------------------
// Sharing a library back, and keeping a subscribed one up to date
// --------------------------------------------------------------------------------------------------

// Write entries out in the shape the importer reads, so that a library can leave this machine the way
// one arrives: a personal library moved to a second printer, or handed to somebody who asked what you
// measured. The round trip through filament_td_parse() is exact in the five fields that are data - the
// brand key, the name, the type, the colour and the number - and deliberately not in the provenance,
// which the importer re-stamps with the file it actually read. Where you got a file is a fact about
// you, not a claim the file gets to make.
//
// `name` is written as the library's own name, for a card to show after somebody imports it. False
// when the file could not be written; writing an empty library is not an error, so that "export" with
// nothing measured yet produces a valid file rather than a failure.
bool filament_td_export(const std::string &path, const std::vector<FilamentTd> &entries, const std::string &name);

// The user's own measurements, out to a file. Returns how many were written, and false in `ok` when
// the write failed - the two are separate because zero entries written successfully is a perfectly
// good answer for somebody who has not measured anything yet.
struct FilamentTdExport
{
    bool   ok      = false;
    size_t written = 0;
};
FilamentTdExport filament_td_export_personal(const std::string &path);

// What an update would do, before it does any of it. An update to measured data is never silent: the
// counts below are what the GUI shows for confirmation, and `merged` is what gets written if the user
// says yes.
struct FilamentTdUpdate
{
    size_t added      = 0;  // spools this library did not list before
    size_t updated    = 0;  // listed before, with a different number, colour or name
    size_t unchanged  = 0;  // listed before, identical
    size_t suppressed = 0;  // the user's own, left exactly as they are
    size_t dropped    = 0;  // listed before, gone from the library now, and not the user's own
    // Additions past what one library is allowed to hold. Only additions are ever refused for room:
    // an entry already in the file, and above all one the user has pinned, is never dropped to make
    // space for a new one.
    size_t over_cap = 0;

    // The library's own file after the update. Written only by filament_td_update_apply().
    std::vector<FilamentTd> merged;

    // What the fetched text itself gave: whether it read at all, and which rows it lost on the way.
    // The counts above are about the merge, these are about the file, and a user looking at an update
    // that added nothing wants to know which of the two it was.
    FilamentTdImport parsed;

    bool nothing_to_do() const { return added == 0 && updated == 0 && dropped == 0; }
};

// Parse a fetched library and work out what merging it into an existing source would do. Writes
// nothing and touches no file: libslic3r does no networking, so the GUI fetches the URL and hands the
// bytes here, and the user sees the counts before anything is committed.
//
// `source_id` names the source being refreshed. A source that does not exist yet merges against an
// empty library, which is how the first fetch of a new subscription reports itself as all additions.
// A Personal source is refused and comes back with everything zero: that file is not a library's to
// rewrite.
FilamentTdUpdate filament_td_update_prepare(const std::string &source_id, const std::string &text,
                                            const std::string &source_name);

// The same decision, against tables handed in rather than read from disk. The merge is the part with
// judgement in it, so it is reachable without a data directory.
FilamentTdUpdate filament_td_merge(const std::vector<FilamentTd> &current, const std::vector<FilamentTd> &fetched,
                                   const FilamentTdSourceList &sources);

// Commit what filament_td_update_prepare() worked out: the merged library to that source's own file,
// the time onto its row, and the cached table dropped so the next lookup reads it.
//
// This is the only path by which a fetched library reaches disk, it writes one file and that file is
// the source's own, and it refuses a Personal or Shipped source outright. Those three together are why
// a user's own measurement cannot be lost to an update: not because the merge is careful, but because
// no code path exists that would write over the file holding it.
bool filament_td_update_apply(const std::string &source_id, const FilamentTdUpdate &update);

// Read a library out of text already in hand rather than out of a file, appending to `out`. The GUI
// has the bytes of a fetched URL and no file to put them in; `source_name` is what the entries are
// stamped with, since there is no file name to take it from.
FilamentTdImport filament_td_parse_text(const std::string &text, const std::string &source_name,
                                        std::vector<FilamentTd> &out);

// A transmission distance as the `filament_transmission_distance` option wants it written, so that
// "put this measurement into the preset" does not have to reinvent the rounding. Clamped to the range
// the option allows, and cut to the two decimals a TD is known to: more digits are noise that makes a
// preset look measured to a precision nobody measured.
std::string filament_td_format_mm(float td_mm);

} // namespace Slic3r

#endif // slic3r_TransmissionDistance_hpp_
