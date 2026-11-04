#include "TransmissionDistance.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <ios>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include "LocalesUtils.hpp"
#include "Utils.hpp"

namespace Slic3r {

namespace {
// CIELAB, so "close enough to be the same colour" means what the eye means by it rather than what
// the three bytes happen to say. Local to this file: the only thing it is used for is deciding
// whether two spools are the same spool.
struct Lab
{
    float l, a, b;
};

Lab srgb_to_lab(float r, float g, float b)
{
    const auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    const float rl = lin(r), gl = lin(g), bl = lin(b);
    const float x = (0.4124f * rl + 0.3576f * gl + 0.1805f * bl) / 0.95047f;
    const float y = 0.2126f * rl + 0.7152f * gl + 0.0722f * bl;
    const float z = (0.0193f * rl + 0.1192f * gl + 0.9505f * bl) / 1.08883f;
    const auto  f = [](float t) {
        return t > 216.f / 24389.f ? std::cbrt(t) : (24389.f / 27.f * t + 16.f) / 116.f;
    };
    const float fx = f(x), fy = f(y), fz = f(z);
    return Lab{ 116.f * fy - 16.f, 500.f * (fx - fy), 200.f * (fy - fz) };
}

float delta_e(const Lab &p, const Lab &q)
{
    const float dl = p.l - q.l, da = p.a - q.a, db = p.b - q.b;
    return std::sqrt(dl * dl + da * da + db * db);
}
} // namespace

namespace {
// The words of a name, lower case, with punctuation and the noise every filament name carries
// dropped. "Panchroma(TM) Matte PLA Wine Burgundy" and "Panchroma PLA Matte @base" come out sharing
// panchroma, matte and pla - which is what says they are the same line.
std::vector<std::string> name_tokens(const std::string &name)
{
    static const std::set<std::string> noise = { "filament", "base", "system", "pro", "premium" };
    std::vector<std::string>           out;
    std::string                        word;
    const auto flush = [&]() {
        if (word.size() > 1 && noise.count(word) == 0)
            out.push_back(word);
        word.clear();
    };
    for (unsigned char c : name) {
        if (std::isalnum(c))
            word.push_back(char(std::tolower(c)));
        else
            flush();
    }
    flush();
    return out;
}

// The vendors the table names, as the filament presets spell them. A brand and the name it sells
// filament under are not always the same word: Prusa's presets say "Prusa Polymers" and its filament
// is Prusament.
std::string table_brand(const std::string &vendor)
{
    std::string key;
    for (unsigned char c : vendor)
        if (std::isalnum(c))
            key.push_back(char(std::tolower(c)));
    if (key == "prusapolymers" || key == "prusa" || key == "prusament" || key == "prusaresearch")
        return "prusament";
    if (key == "polymaker")
        return "polymaker";
    if (key == "printedsolid" || key == "jessie")
        return "printedsolid";
    return key;
}
} // namespace

namespace {
// What a library file says about itself, as against the rows in it. `brand` is on the file rather
// than on every row, since a file belongs to one brand - except in a file the importer wrote, where
// one library spans brands and every row names its own.
//
// The rest of it is the source's own row: a library carries its name, where it came from and when it
// was last written, so that the list of sources can be rebuilt out of the directory alone. A sources
// file somebody has deleted costs the order and the enabled flags, not the knowledge of what these
// libraries are.
struct LibraryHeader
{
    std::string brand;
    std::string source;
    std::string origin;
    std::string kind;
    std::time_t updated  = 0;
    bool        imported = false;
};

// One file: a vendor's worth of colours, or a whole imported library.
void read_file(const boost::filesystem::path &path, LibraryHeader &header, std::vector<FilamentTd> &out)
{
    boost::nowide::ifstream in(path.string());
    if (!in.good())
        return;
    nlohmann::json root;
    in >> root;
    header.brand = root.value("brand", std::string());
    // Where the numbers came from, and whether they are ours: a file the importer wrote says so, and
    // the card has to be able to tell somebody else's measurement from the one we shipped.
    header.source   = root.value("source", std::string());
    header.imported = root.value("imported", false);
    header.kind     = root.value("kind", std::string());
    // `origin` is what this writes now; the two fallbacks are what the files already on disk say. The
    // shipped tables name the database they came out of under "url", and a file today's importer wrote
    // names the path it read under "imported_from".
    header.origin = root.value("origin", std::string());
    if (header.origin.empty())
        header.origin = root.value("imported_from", std::string());
    if (header.origin.empty())
        header.origin = root.value("url", std::string());
    header.updated      = std::time_t(root.value("updated", (long long) 0));
    const size_t before = out.size();
    for (const nlohmann::json &one : root.value("filaments", nlohmann::json::array())) {
        if (out.size() - before >= FILAMENT_TD_IMPORT_MAX_ENTRIES)
            break; // no one file is allowed to be the whole of memory, however it got into the directory
        FilamentTd entry;
        entry.brand     = one.value("brand", header.brand);
        entry.name      = one.value("name", std::string());
        entry.type      = one.value("type", std::string());
        entry.td_mm     = one.value("td", 0.f);
        entry.td_source = header.source;
        entry.imported  = header.imported;
        const std::string hex = one.value("color", std::string());
        if (entry.brand.empty() || entry.name.empty() || entry.td_mm <= 0.f || hex.size() < 7)
            continue;
        const auto channel = [&hex](size_t at) { return float(std::stoi(hex.substr(at, 2), nullptr, 16)) / 255.f; };
        entry.color = ColorRGB(channel(1), channel(3), channel(5));
        out.push_back(std::move(entry));
    }
}

// The same, for a caller that wants the rows and not the file's own story.
void read_file(const boost::filesystem::path &path, std::vector<FilamentTd> &out)
{
    LibraryHeader header;
    read_file(path, header, out);
}

// Every .json in a directory, in name order so a build is the same twice running.
void read_dir(const boost::filesystem::path &dir, std::vector<FilamentTd> &out)
{
    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(dir, ec))
        return;
    std::vector<boost::filesystem::path> files;
    for (boost::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec))
        if (it->path().extension() == ".json")
            files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const boost::filesystem::path &file : files) {
        try {
            read_file(file, out);
        } catch (...) {
            // One unreadable file is one vendor's worth of colours missing, not a broken feature.
        }
    }
}

// What ships in `resources/filament_td`, on its own. Held apart from the whole table because the
// importer needs to know what we already measured without seeing what was imported before: a
// re-import of the same library would otherwise read as a file full of duplicates of itself.
const std::vector<FilamentTd> &shipped_table()
{
    static const std::vector<FilamentTd> table = []() {
        std::vector<FilamentTd> out;
        read_dir(boost::filesystem::path(resources_dir()) / "filament_td", out);
        return out;
    }();
    return table;
}

// The directory every library but the shipped one lives in, and the two names in it that are not a
// library: the list of sources, and the file holding the user's own measurements. Both are named
// rather than derived, because the one thing in here that must never be guessed at is which file is
// the user's.
boost::filesystem::path user_td_dir() { return boost::filesystem::path(data_dir()) / "filament_td"; }

const char *const SOURCES_FILE_NAME  = "sources.json";
const char *const PERSONAL_SOURCE_ID = "personal";
const char *const SHIPPED_SOURCE_ID  = "shipped";

// A library file and the source row it describes.
struct LoadedSource
{
    FilamentTdSource        source;
    std::vector<FilamentTd> entries;
};

// Where a kind sits when nothing has said otherwise: the user's own measurements, then the libraries
// they have, then what shipped. This is the order the table has always had, written down - a
// directory with no sources file reads exactly as it did before there were sources.
int kind_rank(FilamentTdSourceKind kind)
{
    switch (kind) {
    case FilamentTdSourceKind::Personal: return 0;
    case FilamentTdSourceKind::Imported: return 1;
    case FilamentTdSourceKind::Subscription: return 2;
    case FilamentTdSourceKind::Shipped: return 3;
    }
    return 2;
}

// Every library in the user's directory, read. The id is the file's stem, which is what makes a
// library removable by deleting a file and a re-import or a re-fetch replace one rather than add
// another beside it.
std::vector<LoadedSource> read_user_sources()
{
    std::vector<LoadedSource>     out;
    boost::system::error_code     ec;
    const boost::filesystem::path dir = user_td_dir();
    if (data_dir().empty() || !boost::filesystem::is_directory(dir, ec))
        return out;

    std::vector<boost::filesystem::path> files;
    for (boost::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec))
        if (it->path().extension() == ".json" && it->path().filename().string() != SOURCES_FILE_NAME)
            files.push_back(it->path());
    std::sort(files.begin(), files.end());

    for (const boost::filesystem::path &file : files) {
        LoadedSource  one;
        LibraryHeader header;
        try {
            read_file(file, header, one.entries);
        } catch (...) {
            continue; // one unreadable library, not a broken feature
        }
        one.source.id = file.stem().string();
        if (one.source.id.empty())
            continue;
        // The kind the file claims, and failing that the kind its `imported` flag implies, which is
        // what every file written before there were kinds says. A file in the user's own directory
        // cannot claim to be shipped, whatever it writes: shipped means "in the resources we
        // installed", and that is a fact about where the file is.
        one.source.kind = header.kind.empty() ? (header.imported ? FilamentTdSourceKind::Imported :
                                                                   FilamentTdSourceKind::Personal) :
                                                filament_td_source_kind_of(header.kind);
        if (one.source.kind == FilamentTdSourceKind::Shipped)
            one.source.kind = FilamentTdSourceKind::Personal;
        // And the file named personal.json is the user's own, whatever it says about itself. This is
        // the one place where the name of a file decides something, and it decides it because the
        // alternative is a library talking its way into the one slot an update may not write to.
        if (one.source.id == PERSONAL_SOURCE_ID)
            one.source.kind = FilamentTdSourceKind::Personal;
        one.source.name   = header.source;
        one.source.origin = header.origin;
        one.source.updated = header.updated;
        if (one.source.updated == 0) {
            const std::time_t written = boost::filesystem::last_write_time(file, ec);
            one.source.updated        = ec ? 0 : written;
        }
        one.source.entries = one.entries.size();
        out.push_back(std::move(one));
    }

    std::stable_sort(out.begin(), out.end(), [](const LoadedSource &a, const LoadedSource &b) {
        return kind_rank(a.source.kind) < kind_rank(b.source.kind);
    });
    return out;
}

// The stored list, which is the part the user decides: the order, the names they gave, what is turned
// off, and which spools they have pinned to their own measurement. Nothing in it is trusted to
// describe a file - the files describe themselves - so a stale row costs nothing.
FilamentTdSourceList read_stored_sources()
{
    FilamentTdSourceList list;
    if (data_dir().empty())
        return list;
    try {
        const boost::filesystem::path file = user_td_dir() / SOURCES_FILE_NAME;
        boost::system::error_code     ec;
        if (!boost::filesystem::is_regular_file(file, ec) || ec)
            return list;
        const boost::uintmax_t size = boost::filesystem::file_size(file, ec);
        if (ec || size == 0 || size > FILAMENT_TD_IMPORT_MAX_BYTES)
            return list;
        boost::nowide::ifstream in(file.string());
        if (!in.good())
            return list;
        nlohmann::json root;
        in >> root;

        FilamentTdSourceList read;
        for (const nlohmann::json &one : root.value("sources", nlohmann::json::array())) {
            if (read.sources.size() >= FILAMENT_TD_SOURCES_MAX)
                break;
            if (!one.is_object())
                continue;
            FilamentTdSource source;
            source.id = one.value("id", std::string());
            if (source.id.empty() || read.find(source.id) != nullptr)
                continue; // a list that names one source twice names it once
            source.name    = one.value("name", std::string());
            source.kind    = filament_td_source_kind_of(one.value("kind", std::string()));
            source.origin  = one.value("origin", std::string());
            source.updated = std::time_t(one.value("updated", (long long) 0));
            source.enabled = one.value("enabled", true);
            read.sources.push_back(std::move(source));
        }
        for (const nlohmann::json &one : root.value("suppressed", nlohmann::json::array())) {
            if (read.suppressed.size() >= FILAMENT_TD_IMPORT_MAX_ENTRIES)
                break;
            if (one.is_string())
                read.suppressed.push_back(one.get<std::string>());
        }
        std::sort(read.suppressed.begin(), read.suppressed.end());
        read.suppressed.erase(std::unique(read.suppressed.begin(), read.suppressed.end()), read.suppressed.end());
        return read;
    } catch (...) {
        // A sources file we cannot read is a list of preferences lost, which is recoverable, and not a
        // reason to come up with no libraries at all.
    }
    return FilamentTdSourceList();
}

// The stored list and the directory, reconciled. The stored list gives the order and the flags; the
// directory gives what actually exists. Rows are matched by id, a stored row whose library has gone is
// dropped unless it is a subscription that has not been fetched yet, and a library the list has never
// heard of is folded in at its kind's place.
FilamentTdSourceList combine_sources(const FilamentTdSourceList &stored, const std::vector<LoadedSource> &found)
{
    FilamentTdSourceList list;
    list.suppressed = stored.suppressed;

    std::set<std::string> placed;
    for (const FilamentTdSource &row : stored.sources) {
        if (placed.count(row.id) != 0)
            continue;
        const auto at = std::find_if(found.begin(), found.end(),
                                     [&row](const LoadedSource &one) { return one.source.id == row.id; });
        if (at != found.end()) {
            // The file's own facts, with the user's preferences kept over them: they renamed it, they
            // turned it off, and neither of those is the file's to say.
            FilamentTdSource merged = at->source;
            if (!row.name.empty())
                merged.name = row.name;
            if (merged.origin.empty())
                merged.origin = row.origin;
            merged.enabled = row.enabled;
            list.sources.push_back(std::move(merged));
        } else if (row.kind == FilamentTdSourceKind::Shipped || row.kind == FilamentTdSourceKind::Subscription) {
            // Shipped has no file in this directory, and a subscription with none is one nobody has
            // fetched yet. Any other row without a file is a library the user deleted.
            FilamentTdSource kept = row;
            kept.entries          = 0;
            list.sources.push_back(std::move(kept));
        }
        placed.insert(row.id);
    }

    for (const LoadedSource &one : found)
        if (placed.count(one.source.id) == 0) {
            const int  rank  = kind_rank(one.source.kind);
            const auto after = std::find_if(list.sources.begin(), list.sources.end(),
                                            [rank](const FilamentTdSource &row) { return kind_rank(row.kind) > rank; });
            list.sources.insert(after, one.source);
            placed.insert(one.source.id);
        }

    // By kind and not by id, because what has to be in the list is the shipped table and not a
    // particular word: a user file that happens to be called shipped.json is a library of theirs, and
    // it is not the resources directory.
    if (std::none_of(list.sources.begin(), list.sources.end(),
                     [](const FilamentTdSource &row) { return row.kind == FilamentTdSourceKind::Shipped; })) {
        FilamentTdSource shipped;
        shipped.id   = SHIPPED_SOURCE_ID;
        shipped.kind = FilamentTdSourceKind::Shipped;
        list.sources.push_back(std::move(shipped));
    }
    for (FilamentTdSource &row : list.sources)
        if (row.kind == FilamentTdSourceKind::Shipped)
            row.entries = shipped_table().size();

    // The one part of the order that is not the user's to change. A measurement somebody took on their
    // own printer is the only number here nobody else can reproduce, so it answers first whatever the
    // file on disk asks for - including a file written by a build that let the list say otherwise, or
    // one edited by hand.
    std::stable_partition(list.sources.begin(), list.sources.end(),
                          [](const FilamentTdSource &row) { return row.kind == FilamentTdSourceKind::Personal; });
    return list;
}
} // namespace

// The table and the list of sources are cached together, because they are one read: the list is what
// says which files to read and in what order, and the table is those files concatenated in it. An
// import or an update has to be visible without a restart, so they are guarded globals rather than
// function-local statics. The mutex makes a rebuild atomic with respect to any lookup that starts
// during it; what it cannot do is keep a pointer *already* handed out valid across one, which is why
// filament_td_reset_cache() is documented as a UI-thread call and why every consumer in the tree
// copies the value it wanted out of the entry (see ChromaFilament) instead of holding the entry.
static std::mutex              s_td_table_mutex;
static std::vector<FilamentTd> s_td_table;
static FilamentTdSourceList    s_td_sources;
static bool                    s_td_table_loaded = false;

// Read the sources, then the enabled ones in order, stamping every entry with the source it came out
// of. The concatenation is the precedence, and it only ever decides an exact tie, since the search
// picks on match quality first: a library that matches the spool in hand better than the source above
// it does still win, which is the search's business and not the order's.
static void td_load_locked()
{
    if (s_td_table_loaded)
        return;
    const std::vector<LoadedSource> found = read_user_sources();
    FilamentTdSourceList            list  = combine_sources(read_stored_sources(), found);

    std::vector<FilamentTd> out;
    for (const FilamentTdSource &row : list.sources) {
        if (!row.enabled)
            continue;
        const size_t before = out.size();
        if (row.kind == FilamentTdSourceKind::Shipped) {
            const std::vector<FilamentTd> &shipped = shipped_table();
            out.insert(out.end(), shipped.begin(), shipped.end());
        } else {
            const auto at = std::find_if(found.begin(), found.end(),
                                         [&row](const LoadedSource &one) { return one.source.id == row.id; });
            if (at == found.end())
                continue; // a subscription nobody has fetched yet, which has no library to contribute
            out.insert(out.end(), at->entries.begin(), at->entries.end());
        }
        for (size_t i = before; i < out.size(); ++i) {
            out[i].source_id   = row.id;
            out[i].source_kind = row.kind;
            // Kept in step with the kind rather than read off the file, so that the two can never
            // disagree: a card that tells somebody else's measurement from ours by this flag would
            // otherwise be one hand-edited file away from being wrong.
            out[i].imported = row.kind == FilamentTdSourceKind::Imported ||
                              row.kind == FilamentTdSourceKind::Subscription;
        }
    }

    s_td_table        = std::move(out);
    s_td_sources      = std::move(list);
    s_td_table_loaded = true;
}

const std::vector<FilamentTd> &filament_td_table()
{
    std::lock_guard<std::mutex> lock(s_td_table_mutex);
    td_load_locked();
    return s_td_table;
}

const FilamentTdSourceList &filament_td_sources()
{
    std::lock_guard<std::mutex> lock(s_td_table_mutex);
    td_load_locked();
    return s_td_sources;
}

void filament_td_reset_cache()
{
    std::lock_guard<std::mutex> lock(s_td_table_mutex);
    s_td_table.clear();
    s_td_sources      = FilamentTdSourceList();
    s_td_table_loaded = false;
}

const FilamentTd *filament_td_lookup(const std::string &vendor, const std::string &name, const ColorRGB &color)
{
    return filament_td_lookup(filament_td_table(), vendor, name, color);
}

const FilamentTd *filament_td_lookup(const std::vector<FilamentTd> &table, const std::string &vendor,
                                 const std::string &name, const ColorRGB &color)
{
    if (table.empty())
        return nullptr;
    const std::string brand = table_brand(vendor);
    if (brand.empty())
        return nullptr;

    const std::vector<std::string> want = name_tokens(name);
    if (want.empty())
        return nullptr;
    const Lab want_lab = srgb_to_lab(color.r(), color.g(), color.b());

    // The line first, then the colour within it. Scored rather than filtered, because a preset's name
    // and a spool's name agree on most words but rarely on all of them.
    const FilamentTd *best       = nullptr;
    int             best_words = 0;
    float           best_de    = 0.f;
    for (const FilamentTd &entry : table) {
        if (entry.brand != brand)
            continue;
        const std::vector<std::string> have = name_tokens(entry.name);
        int                            words = 0;
        for (const std::string &w : want)
            if (std::find(have.begin(), have.end(), w) != have.end())
                ++words;
        if (words < 2)
            continue; // one word in common is a coincidence, not a line
        const float de = delta_e(want_lab, srgb_to_lab(entry.color.r(), entry.color.g(), entry.color.b()));
        if (best == nullptr || words > best_words || (words == best_words && de < best_de)) {
            best       = &entry;
            best_words = words;
            best_de    = de;
        }
    }
    // Near enough to be the same colour, and deliberately strict. Measured against the table itself:
    // inside one line the colours are well separated, so a filament set to the colour it really is
    // lands within a few dE of its own entry, while a near miss falls outside. At 12 the worst pair
    // the table holds (Cotton White at 4.1 against Pastel Candy at 2.9) could be confused; at 6 the
    // worst remaining confusion is 0.1 mm, which is below what the number is known to anyway.
    //
    // No match is the right answer when in doubt: the field has a working default, and a white
    // filament handed the number measured for a grey one is worse than a default - it is wrong, and
    // it looks measured.
    return (best != nullptr && best_de < 6.f) ? best : nullptr;
}

namespace {
// --------------------------------------------------------------------------------------------------
// Importing somebody else's library.
//
// Everything below treats its input as hostile, because the input is a file a user downloaded. No
// path out of here throws, no length is trusted, and every loop is bounded.
// --------------------------------------------------------------------------------------------------

// A field name, however the file spelled it: lower case, letters and digits only. "TD", "td",
// "transmission_distance" and "transmissionDistance" collapse onto two spellings between them, which
// is what lets one table of names serve a CSV header and a JSON key alike.
std::string field_key(const std::string &raw)
{
    std::string out;
    for (unsigned char c : raw)
        if (std::isalnum(c))
            out.push_back(char(std::tolower(c)));
    return out;
}

enum class Field { None, Brand, Name, Type, Color, Td };

// The spellings community libraries actually use. Anything else is ignored rather than guessed at:
// a field we do not know is a field whose meaning we would be inventing.
Field field_of(const std::string &key)
{
    if (key == "brand" || key == "vendor" || key == "manufacturer" || key == "maker")
        return Field::Brand;
    if (key == "name" || key == "filament" || key == "title" || key == "filamentname")
        return Field::Name;
    if (key == "type" || key == "material" || key == "filamenttype")
        return Field::Type;
    if (key == "color" || key == "colour" || key == "hex" || key == "rgb" || key == "hexcolor" ||
        key == "colourhex" || key == "colorhex" || key == "hexcode")
        return Field::Color;
    if (key == "td" || key == "tdmm" || key == "transmissiondistance" || key == "transmissiondistancemm")
        return Field::Td;
    return Field::None;
}

std::string trimmed(const std::string &text)
{
    size_t from = 0, to = text.size();
    while (from < to && std::isspace((unsigned char) text[from]))
        ++from;
    while (to > from && std::isspace((unsigned char) text[to - 1]))
        --to;
    return text.substr(from, to - from);
}

// Trimmed and cut to length. A field of a few megabytes in every row is how a small file turns into a
// large allocation, and nothing that long is a filament's name anyway.
std::string field_text(const std::string &raw)
{
    std::string out = trimmed(raw);
    if (out.size() > FILAMENT_TD_IMPORT_MAX_STRING)
        out.resize(FILAMENT_TD_IMPORT_MAX_STRING);
    return out;
}

std::string color_hex(const ColorRGB &color)
{
    const auto byte = [](float v) { return int(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
    char       out[8] = {};
    std::snprintf(out, sizeof(out), "#%02x%02x%02x", byte(color.r()), byte(color.g()), byte(color.b()));
    return std::string(out);
}

// A colour however the file spells it: "#rrggbb", "rrggbb", "0xrrggbb", "#rgb", "#rrggbbaa" with the
// alpha dropped, "rgb(r,g,b)" or a bare "r,g,b". False when none of those fit, which is a row with no
// usable colour rather than a row to guess at: the whole point of the table is that a transmission
// distance belongs to a colour.
bool parse_color(const std::string &raw, ColorRGB &out)
{
    std::string text = trimmed(raw);
    if (text.empty())
        return false;

    // "rgb(12, 34, 56)" and "rgba(...)", which is what a value copied out of CSS looks like.
    const size_t open = text.find('(');
    if (open != std::string::npos && field_key(text.substr(0, open)).compare(0, 3, "rgb") == 0) {
        const size_t close = text.rfind(')');
        text = text.substr(open + 1, close == std::string::npos ? std::string::npos : close - open - 1);
    }

    if (text.find(',') != std::string::npos) {
        // Three numbers. 0 to 255 unless they are all within 0 to 1 and at least one of them has a
        // fraction, which is the only way to tell "1,1,1" (a near black in bytes) from a float triple.
        double       values[3] = { 0., 0., 0. };
        size_t       count     = 0;
        bool         fractional = false;
        size_t       at        = 0;
        while (count < 3 && at <= text.size()) {
            const size_t      end  = text.find(',', at);
            const std::string cell = trimmed(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
            size_t            used = 0;
            const double      v    = string_to_double_decimal_point(cell, &used);
            if (used == 0 || !std::isfinite(v))
                return false;
            fractional = fractional || v != std::floor(v);
            values[count++] = v;
            if (end == std::string::npos)
                break;
            at = end + 1;
        }
        if (count != 3)
            return false;
        const bool unit = fractional && values[0] <= 1. && values[1] <= 1. && values[2] <= 1.;
        const auto channel = [unit](double v) {
            return std::clamp(float(unit ? v : v / 255.), 0.f, 1.f);
        };
        out = ColorRGB(channel(values[0]), channel(values[1]), channel(values[2]));
        return true;
    }

    // Hex, with whatever decoration it arrived wearing.
    if (text.front() == '#')
        text.erase(0, 1);
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text.erase(0, 2);
    if (text.size() != 3 && text.size() != 6 && text.size() != 8)
        return false;
    int digits[8] = {};
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = (unsigned char) text[i];
        if (c >= '0' && c <= '9')
            digits[i] = c - '0';
        else if (c >= 'a' && c <= 'f')
            digits[i] = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digits[i] = c - 'A' + 10;
        else
            return false;
    }
    if (text.size() == 3) // "#f0a", the short form, each digit doubled
        out = ColorRGB(float(digits[0] * 17) / 255.f, float(digits[1] * 17) / 255.f, float(digits[2] * 17) / 255.f);
    else // six digits, or eight with an alpha this table has no use for
        out = ColorRGB(float(digits[0] * 16 + digits[1]) / 255.f, float(digits[2] * 16 + digits[3]) / 255.f,
                       float(digits[4] * 16 + digits[5]) / 255.f);
    return true;
}

// A transmission distance in millimetres. A trailing unit ("1.2 mm") stops the parse where the number
// ends, which is what we want; a value outside the window is pulled into it rather than dropped,
// since the row still says which colour it is and roughly how translucent.
bool parse_td(const std::string &raw, float &out, bool &clamped)
{
    std::string text = trimmed(raw);
    if (!text.empty() && text.front() == '+')
        text.erase(0, 1); // the number parser below wants the digits, not a sign it reads as junk
    size_t       used = 0;
    const double value = string_to_double_decimal_point(text, &used);
    if (used == 0 || !std::isfinite(value))
        return false;
    const float asked = float(value);
    out     = std::clamp(asked, FILAMENT_TD_MIN_MM, FILAMENT_TD_MAX_MM);
    clamped = out != asked;
    return true;
}

// A row as the file had it, before any of it is believed.
struct RawEntry
{
    std::string brand, name, type, color, td;
};

// Rows, bounded. Past the cap they are counted and dropped: the count is what the user is told, and
// dropping is what keeps a file that claims ten million filaments from being ten million filaments.
struct Collected
{
    std::vector<RawEntry> rows;
    size_t                over_cap = 0;

    void add(RawEntry row)
    {
        if (rows.size() < FILAMENT_TD_IMPORT_MAX_ENTRIES)
            rows.push_back(std::move(row));
        else
            ++over_cap;
    }
};

void assign(RawEntry &row, Field field, const std::string &text)
{
    switch (field) {
    case Field::Brand: row.brand = field_text(text); break;
    case Field::Name: row.name = field_text(text); break;
    case Field::Type: row.type = field_text(text); break;
    case Field::Color: row.color = field_text(text); break;
    case Field::Td: row.td = field_text(text); break;
    case Field::None: break;
    }
}

// ---- JSON --------------------------------------------------------------------------------------

// One JSON value as text, so that a number, a string and a three element array all reach the same
// parser. Arrays are cut at four elements: an "rgb" is three, and nothing else here is a list.
std::string scalar_text(const nlohmann::json &value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_boolean())
        return value.get<bool>() ? "1" : "0";
    if (value.is_number_float())
        return float_to_string_decimal_point(value.get<double>(), 6);
    if (value.is_number_unsigned())
        return std::to_string(value.get<unsigned long long>());
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    if (value.is_array()) {
        std::string out;
        size_t      count = 0;
        for (const nlohmann::json &one : value) {
            if (++count > 4)
                break;
            if (!out.empty())
                out.push_back(',');
            out += scalar_text(one);
        }
        return out;
    }
    return std::string();
}

// Whether this object is a filament or a wrapper around some. A filament names at least one of the
// three fields that make one, and holds nothing but scalars; a wrapper (our own files, or a library
// with a "filaments" list and a header beside it) holds an object or a list, so it is walked into.
bool looks_like_entry(const nlohmann::json &object)
{
    bool any = false;
    for (auto it = object.begin(); it != object.end(); ++it) {
        const Field field = field_of(field_key(it.key()));
        if (field == Field::Name || field == Field::Color || field == Field::Td)
            any = true;
        if (it.value().is_object() || (it.value().is_array() && field != Field::Color))
            return false;
    }
    return any;
}

// A library is not standardized, so the shape is searched for rather than assumed: an array of
// filaments, an object whose values are filaments, or either of those inside a wrapper. `brand_hint`
// carries a brand named on a wrapper down to rows that do not name one, the way our own files put the
// brand on the file; `name_hint` is the key a filament hung off, which is its name in a library
// written as {"Prusament PLA Galaxy Black": {...}}.
//
// The depth is capped because a file can nest as deeply as it likes and we have one stack.
void collect_json(const nlohmann::json &node, const std::string &brand_hint, const std::string &name_hint,
                  int depth, Collected &collected)
{
    static constexpr int MAX_WALK_DEPTH = 6;
    if (depth > MAX_WALK_DEPTH)
        return;
    if (node.is_array()) {
        for (const nlohmann::json &one : node)
            collect_json(one, brand_hint, std::string(), depth + 1, collected);
        return;
    }
    if (!node.is_object())
        return;

    if (looks_like_entry(node)) {
        RawEntry row;
        row.brand = field_text(brand_hint);
        row.name  = field_text(name_hint);
        for (auto it = node.begin(); it != node.end(); ++it)
            if (const Field field = field_of(field_key(it.key())); field != Field::None) {
                const std::string text = scalar_text(it.value());
                if (!trimmed(text).empty())
                    assign(row, field, text);
            }
        collected.add(std::move(row));
        return;
    }

    std::string brand = brand_hint;
    for (auto it = node.begin(); it != node.end(); ++it)
        if (field_of(field_key(it.key())) == Field::Brand && !scalar_text(it.value()).empty())
            brand = scalar_text(it.value());
    for (auto it = node.begin(); it != node.end(); ++it)
        collect_json(it.value(), brand, it.key(), depth + 1, collected);
}

// How deep the text nests, counted before any parser is handed it. nlohmann recurses both building
// the tree and tearing it down, so a file that is nothing but ten thousand open brackets takes the
// stack with it, and a stack that is gone cannot be caught. Quoted text is skipped so that a bracket
// inside a name does not count.
size_t json_depth(const std::string &text)
{
    size_t depth = 0, worst = 0;
    bool   in_string = false, escaped = false;
    for (char c : text) {
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"')
            in_string = true;
        else if (c == '[' || c == '{')
            worst = std::max(worst, ++depth);
        else if ((c == ']' || c == '}') && depth > 0)
            --depth;
    }
    return worst;
}

// ---- CSV ---------------------------------------------------------------------------------------

// One line into cells, honouring "quotes" and the doubled "" inside them, and nothing else: a
// filament library is a table of short strings, not a spreadsheet. A quoted cell holding a newline is
// the one thing this gets wrong, and it costs that one row.
std::vector<std::string> split_csv(const std::string &line, char separator)
{
    std::vector<std::string> cells;
    std::string              cell;
    bool                     quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c != '"')
                cell.push_back(c);
            else if (i + 1 < line.size() && line[i + 1] == '"')
                cell.push_back(line[++i]);
            else
                quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == separator) {
            cells.push_back(cell);
            cell.clear();
        } else if (c != '\r') {
            cell.push_back(c);
        }
    }
    cells.push_back(cell);
    return cells;
}

// Whichever of the usual separators the header line leans on. A library exported from a spreadsheet
// in a comma-decimal locale is semicolon separated, and a tab separated export is still a table.
char csv_separator(const std::string &header)
{
    char   best = ',';
    size_t most = 0;
    for (char candidate : { ',', ';', '\t', '|' })
        if (const size_t count = size_t(std::count(header.begin(), header.end(), candidate)); count > most) {
            best = candidate;
            most = count;
        }
    return best;
}

// A header row naming the columns, then a row per filament. Unknown columns are ignored, and the
// order of the known ones is whatever the file chose.
bool collect_csv(const std::string &text, Collected &collected)
{
    size_t     at        = 0;
    const auto next_line = [&text, &at](std::string &line) {
        while (at < text.size()) {
            const size_t end = text.find('\n', at);
            line             = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at               = end == std::string::npos ? text.size() : end + 1;
            if (!trimmed(line).empty())
                return true;
        }
        return false;
    };

    std::string line;
    if (!next_line(line))
        return false;
    const char         separator = csv_separator(line);
    std::vector<Field> columns;
    bool               usable = false;
    for (const std::string &cell : split_csv(line, separator)) {
        columns.push_back(field_of(field_key(cell)));
        usable = usable || columns.back() == Field::Td;
    }
    // With no transmission distance column there is nothing here to import, and the file is some other
    // CSV that happened to be pointed at.
    if (!usable)
        return false;

    // The separator is settled by the header; every row after it is split the same way.
    while (next_line(line)) {
        const std::vector<std::string> cells = split_csv(line, separator);
        RawEntry                       row;
        for (size_t i = 0; i < cells.size() && i < columns.size(); ++i)
            if (!trimmed(cells[i]).empty())
                assign(row, columns[i], cells[i]);
        collected.add(std::move(row));
    }
    return true;
}

// ---- normalizing -------------------------------------------------------------------------------

// The name an entry is stored under. The search wants two words in common with the filament preset's
// name, and a preset is named "<brand> <type> @<printer>", so a row whose name is only its colour
// ("Wine Red") could never be reached by anything. The brand and the type go in front of it when they
// are not already in it, which is how our own tables read as well.
std::string composed_name(const std::string &brand, const std::string &type, const std::string &name)
{
    std::string              out  = name;
    std::vector<std::string> have = name_tokens(out);
    const auto               absent = [&have](const std::string &part) {
        const std::vector<std::string> words = name_tokens(part);
        if (words.empty())
            return false;
        for (const std::string &word : words)
            if (std::find(have.begin(), have.end(), word) == have.end())
                return true;
        return false;
    };
    if (absent(type)) {
        out  = type + " " + out;
        have = name_tokens(out);
    }
    // The brand goes in front unless the name already says who made it, either the way the library
    // spells it or the way the presets key it: "Prusament PLA Galaxy Black" wants nothing adding, and
    // what it would get is "Prusa Research" in front of the word Prusament.
    const std::string key = table_brand(brand);
    if (absent(brand) && std::find(have.begin(), have.end(), key) == have.end())
        out = brand + " " + out;
    return trimmed(out);
}

} // namespace

std::string filament_td_entry_key(const FilamentTd &entry)
{
    std::string key = entry.brand;
    key += '|';
    for (const std::string &word : name_tokens(entry.name)) {
        key += word;
        key += ' ';
    }
    key += '|';
    key += color_hex(entry.color);
    return key;
}

namespace {

struct Normalized
{
    FilamentTd entry;
    bool       clamped = false;
};

// One row into one entry, or nothing with a reason. A row has to name a maker, a name, a colour and a
// number: the first three are what a lookup searches by, so a row missing any of them is a number
// nothing could ever find again, and the fourth is the number itself.
bool normalize(const RawEntry &row, const std::string &source, Normalized &out, FilamentTdImport &result)
{
    if (name_tokens(row.name).empty()) {
        ++result.skipped_no_name;
        return false;
    }
    // Through the same spelling table a lookup goes through, so that a library writing "Prusa
    // Research" and a preset saying "Prusa Polymers" meet at the one key, prusament.
    const std::string brand = table_brand(row.brand);
    if (brand.empty()) {
        ++result.skipped_no_brand;
        return false;
    }
    ColorRGB color;
    if (!parse_color(row.color, color)) {
        ++result.skipped_no_color;
        return false;
    }
    float td      = 0.f;
    bool  clamped = false;
    if (!parse_td(row.td, td, clamped)) {
        ++result.skipped_no_td;
        return false;
    }

    out.entry.brand     = brand;
    out.entry.name      = composed_name(row.brand, row.type, row.name);
    out.entry.type      = row.type;
    out.entry.color     = color;
    out.entry.td_mm     = td;
    out.entry.td_source = source;
    out.entry.imported  = true;
    out.clamped         = clamped;
    return true;
}

// The whole file as text, with the size looked at before a byte of it is read.
bool read_text(const boost::filesystem::path &path, std::string &out)
{
    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(path, ec) || ec)
        return false;
    const boost::uintmax_t size = boost::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > FILAMENT_TD_IMPORT_MAX_BYTES)
        return false;
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    if (!in.good())
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (out.compare(0, 3, "\xEF\xBB\xBF") == 0)
        out.erase(0, 3); // a BOM, which a spreadsheet writes and nothing here wants to see
    return !out.empty();
}

// JSON or CSV, decided by what is in the file rather than by what it is called: a library saved as
// "library.txt" is still JSON if it starts with a brace.
bool looks_like_json(const std::string &text)
{
    for (char c : text) {
        if (std::isspace((unsigned char) c))
            continue;
        return c == '{' || c == '[';
    }
    return false;
}

// A library as a file: the shape filament_td_parse() reads, which is also the shape every library
// under `data_dir()/filament_td` is written in. One shape for what we write and what we read is what
// makes an export and an import the same thing seen from two sides, and it is why a library can be
// moved between machines by copying a file.
nlohmann::json library_json(const std::vector<FilamentTd> &entries, const std::string &name,
                            FilamentTdSourceKind kind, const std::string &origin, std::time_t updated)
{
    nlohmann::json root;
    if (!name.empty())
        root["source"] = name;
    root["kind"] = filament_td_source_kind_key(kind);
    // Written as well as the kind, for a build that predates kinds: that one reads this flag and
    // nothing else to tell somebody else's numbers from ours, and a file written here still has to
    // read correctly there.
    root["imported"] = kind == FilamentTdSourceKind::Imported || kind == FilamentTdSourceKind::Subscription;
    if (!origin.empty())
        root["origin"] = origin;
    root["unit"] = "mm";
    if (updated != 0)
        root["updated"] = (long long) updated;

    nlohmann::json list = nlohmann::json::array();
    for (const FilamentTd &entry : entries) {
        nlohmann::json one;
        one["brand"] = entry.brand;
        one["name"]  = entry.name;
        if (!entry.type.empty())
            one["type"] = entry.type;
        one["color"] = color_hex(entry.color);
        // Three decimals, rounded in double so that the file reads 1.2 rather than a float's nearest
        // neighbour to it. A transmission distance is known to about a tenth of a millimetre, and the
        // digits past that are noise written out as though measured. Three is also two more than any
        // path into here can produce, which is what makes the round trip through the importer exact.
        one["td"] = std::round(double(entry.td_mm) * 1000.) / 1000.;
        list.push_back(std::move(one));
    }
    root["filaments"] = std::move(list);
    return root;
}

// Through a temporary and into place, so that a write interrupted half way leaves the file that was
// there whole rather than half a library.
bool write_json_atomic(const boost::filesystem::path &file, const nlohmann::json &root)
{
    try {
        boost::system::error_code ec;
        boost::filesystem::create_directories(file.parent_path(), ec);
        const boost::filesystem::path temp = file.string() + ".tmp";
        {
            boost::nowide::ofstream write(temp.string(), std::ios::binary | std::ios::trunc);
            if (!write.good())
                return false;
            // Replacing rather than refusing on bad UTF-8: the names came out of a file we do not
            // trust, and one byte somebody's exporter got wrong is not a reason to lose the library.
            write << root.dump(1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
            if (!write.good())
                return false;
        }
        boost::filesystem::remove(file, ec); // rename over an existing file is not portable
        boost::filesystem::rename(temp, file, ec);
        if (ec) {
            boost::filesystem::remove(temp, ec);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

// A file-name safe, stable slug out of whatever the user called a thing.
std::string slug(const std::string &text, size_t cap = 48)
{
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c))
            out.push_back(char(std::tolower(c)));
        else if (!out.empty() && out.back() != '-')
            out.push_back('-');
        if (out.size() >= cap)
            break;
    }
    while (!out.empty() && out.back() == '-')
        out.pop_back();
    return out;
}

// The file a library lives in. An id that is not a plain file name gets nothing back: ids are file
// stems, but the list they are read from is a file somebody can edit, and "../../something" is not an
// id. Every path in here that writes or deletes goes through this.
boost::filesystem::path library_file(const std::string &id)
{
    if (id.empty() || data_dir().empty() || boost::filesystem::path(id).filename().string() != id)
        return boost::filesystem::path();
    return user_td_dir() / (id + ".json");
}

// Whether an update would be saying anything about a spool we already have. The brand and the colour
// are equal already - they are what the key is made of - so what is left is the number, the name as
// the library spells it now, and the type.
bool same_measurement(const FilamentTd &a, const FilamentTd &b)
{
    return a.name == b.name && a.type == b.type && std::fabs(a.td_mm - b.td_mm) < 1e-4f;
}

// The file name an import lands under. One file per source, named after it, so that re-importing a
// corrected library replaces it instead of adding to a pile, and so that a user can delete an
// imported library without touching the values they measured themselves.
std::string import_file_name(const boost::filesystem::path &from)
{
    const std::string stem = slug(from.stem().string());
    return "imported-" + (stem.empty() ? std::string("library") : stem) + ".json";
}
} // namespace

FilamentTdImport filament_td_parse(const std::string &path, std::vector<FilamentTd> &out)
{
    const boost::filesystem::path from(path);
    std::string                   text;
    if (!read_text(from, text)) {
        FilamentTdImport result;
        // The file's own name is the provenance, and data rather than a sentence: the GUI is what
        // says "imported from" around it, in the user's language.
        result.source     = from.filename().string();
        result.unreadable = true;
        return result;
    }
    return filament_td_parse_text(text, from.filename().string(), out);
}

FilamentTdImport filament_td_parse_text(const std::string &raw, const std::string &source_name,
                                        std::vector<FilamentTd> &out)
{
    FilamentTdImport result;
    result.source = source_name;
    if (raw.empty() || raw.size() > FILAMENT_TD_IMPORT_MAX_BYTES) {
        // Nothing, or more than a library is: a fetch that failed and gave us its error page instead,
        // or a file big enough that parsing it is the attack.
        result.unreadable = true;
        return result;
    }
    // A BOM, which a spreadsheet writes and nothing here wants to see. read_text() takes it off what
    // it reads; text handed to us straight off a URL has had nobody to take it off.
    const std::string text = raw.compare(0, 3, "\xEF\xBB\xBF") == 0 ? raw.substr(3) : raw;

    Collected collected;
    try {
        if (looks_like_json(text)) {
            static constexpr size_t MAX_JSON_DEPTH = 64;
            if (json_depth(text) > MAX_JSON_DEPTH) {
                result.unreadable = true;
                return result;
            }
            collect_json(nlohmann::json::parse(text), std::string(), std::string(), 0, collected);
        } else if (!collect_csv(text, collected)) {
            result.unreadable = true;
            return result;
        }
    } catch (...) {
        // Malformed past the point of reading: a file that is not a library, which is the user's to
        // fix and never ours to crash over.
        result.unreadable = true;
        return result;
    }

    result.skipped_over_cap = collected.over_cap;

    std::vector<Normalized> normalized;
    normalized.reserve(collected.rows.size());
    for (const RawEntry &row : collected.rows) {
        Normalized one;
        if (normalize(row, result.source, one, result))
            normalized.push_back(std::move(one));
    }

    // Dedupe. Within the file the later row wins, because a library's later rows are its corrections.
    // Against what shipped the shipped entry wins an exact match, since that one is our own
    // measurement with its own provenance and the import would only shadow it with the same number;
    // an imported entry that is not an exact match still competes, and wins where it matches a spool
    // better, which is the search's business.
    std::set<std::string> shipped_keys;
    for (const FilamentTd &entry : shipped_table())
        shipped_keys.insert(filament_td_entry_key(entry));

    std::map<std::string, size_t> at;
    std::vector<Normalized>       kept;
    kept.reserve(normalized.size());
    for (Normalized &one : normalized) {
        const std::string key = filament_td_entry_key(one.entry);
        if (shipped_keys.count(key) != 0) {
            ++result.skipped_duplicate;
            continue;
        }
        const auto found = at.find(key);
        if (found == at.end()) {
            at.emplace(key, kept.size());
            kept.push_back(std::move(one));
        } else {
            kept[found->second] = std::move(one);
            ++result.skipped_duplicate;
        }
    }

    out.reserve(out.size() + kept.size());
    for (Normalized &one : kept) {
        result.taken += 1;
        result.clamped += one.clamped ? 1 : 0;
        out.push_back(std::move(one.entry));
    }
    return result;
}

FilamentTdImport filament_td_import(const std::string &path)
{
    std::vector<FilamentTd> entries;
    FilamentTdImport        result = filament_td_parse(path, entries);
    if (entries.empty() || data_dir().empty())
        return result;

    const boost::filesystem::path file = user_td_dir() / import_file_name(boost::filesystem::path(path));
    // Our own shape, so that read_file() above picks it up with no special case: the brand on every
    // row rather than on the file, since one library spans brands, and the kind that says these
    // numbers are somebody else's. A write that did not happen is reported by an empty written_path,
    // not by an exception reaching the GUI.
    if (write_json_atomic(file, library_json(entries, result.source, FilamentTdSourceKind::Imported,
                                             boost::filesystem::path(path).string(), std::time(nullptr))))
        result.written_path = file.string();
    return result;
}

std::string filament_td_format_mm(float td_mm)
{
    const float value = std::isfinite(td_mm) ? std::clamp(td_mm, FILAMENT_TD_MIN_MM, FILAMENT_TD_MAX_MM)
                                             : FILAMENT_TD_DEFAULT_MM;
    std::string out = float_to_string_decimal_point(std::round(value * 100.f) / 100.f, 2);
    // Trailing zeros are digits the measurement does not have.
    while (out.size() > 1 && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    return out;
}

// --------------------------------------------------------------------------------------------------
// The sources, and the operations a GUI performs on them
//
// Everything below reads the list, changes one thing in it and writes it back, because the list is
// small and the alternative - a live object the GUI mutates - would have to answer what happens when
// two of them disagree. The cost is a file write per change, on a path a user takes a few times a
// year.
// --------------------------------------------------------------------------------------------------

const char *filament_td_source_kind_key(FilamentTdSourceKind kind)
{
    switch (kind) {
    case FilamentTdSourceKind::Shipped: return "shipped";
    case FilamentTdSourceKind::Personal: return "personal";
    case FilamentTdSourceKind::Imported: return "imported";
    case FilamentTdSourceKind::Subscription: return "subscription";
    }
    return "imported";
}

FilamentTdSourceKind filament_td_source_kind_of(const std::string &key)
{
    if (key == "shipped")
        return FilamentTdSourceKind::Shipped;
    if (key == "personal")
        return FilamentTdSourceKind::Personal;
    if (key == "subscription")
        return FilamentTdSourceKind::Subscription;
    // Imported for a word we do not know, because it is the kind that claims least: it ranks below
    // the user's own measurements, and nothing refreshes it behind their back.
    return FilamentTdSourceKind::Imported;
}

const FilamentTdSource *FilamentTdSourceList::find(const std::string &id) const
{
    const auto at = std::find_if(sources.begin(), sources.end(),
                                 [&id](const FilamentTdSource &row) { return row.id == id; });
    return at == sources.end() ? nullptr : &*at;
}

bool FilamentTdSourceList::is_suppressed(const std::string &entry_key) const
{
    // Linear, and deliberately not a binary search: the list is written sorted but it is also a list a
    // caller can build by hand, and a search that quietly misses entries in an unsorted one would fail
    // in the direction this whole feature exists to prevent. A merge over a large library builds its
    // own set once (see filament_td_merge), so the cost lands nowhere that repeats.
    return std::find(suppressed.begin(), suppressed.end(), entry_key) != suppressed.end();
}

bool filament_td_sources_save(const FilamentTdSourceList &list)
{
    if (data_dir().empty())
        return false;

    nlohmann::json root;
    root["version"] = 1;

    nlohmann::json       rows = nlohmann::json::array();
    std::set<std::string> seen;
    for (const FilamentTdSource &row : list.sources) {
        if (rows.size() >= FILAMENT_TD_SOURCES_MAX)
            break;
        if (row.id.empty() || !seen.insert(row.id).second)
            continue; // a list that names one source twice names it once
        nlohmann::json one;
        one["id"] = row.id;
        if (!row.name.empty())
            one["name"] = row.name;
        one["kind"] = filament_td_source_kind_key(row.kind);
        if (!row.origin.empty())
            one["origin"] = row.origin;
        if (row.updated != 0)
            one["updated"] = (long long) row.updated;
        one["enabled"] = row.enabled;
        rows.push_back(std::move(one));
    }
    root["sources"] = std::move(rows);

    std::vector<std::string> keys = list.suppressed;
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    if (keys.size() > FILAMENT_TD_IMPORT_MAX_ENTRIES)
        keys.resize(FILAMENT_TD_IMPORT_MAX_ENTRIES);
    root["suppressed"] = keys;

    if (!write_json_atomic(user_td_dir() / SOURCES_FILE_NAME, root))
        return false;
    filament_td_reset_cache();
    return true;
}

std::string filament_td_subscribe(const std::string &url, const std::string &name)
{
    const std::string where = trimmed(url);
    if (where.empty() || where.size() > FILAMENT_TD_IMPORT_MAX_STRING * 8)
        return std::string();

    FilamentTdSourceList list = filament_td_sources();
    for (const FilamentTdSource &row : list.sources)
        if (row.kind == FilamentTdSourceKind::Subscription && row.origin == where)
            return row.id; // subscribing twice to one list is subscribing once

    std::string base = "sub-" + slug(name.empty() ? where : name);
    if (base == "sub-")
        base = "sub-library";
    std::string id = base;
    for (int n = 2; list.find(id) != nullptr && n < 1000; ++n)
        id = base + "-" + std::to_string(n);
    if (list.find(id) != nullptr)
        return std::string();

    FilamentTdSource row;
    row.id     = id;
    row.name   = name.empty() ? where : name;
    row.kind   = FilamentTdSourceKind::Subscription;
    row.origin = where;
    // Below the libraries already there and above the shipped table: a list somebody has just gone and
    // found is worth more than nothing and less than what they had already chosen to keep. The order
    // is theirs to change afterwards, which is the point of there being one.
    const auto at = std::find_if(list.sources.begin(), list.sources.end(), [](const FilamentTdSource &row) {
        return row.kind == FilamentTdSourceKind::Shipped;
    });
    list.sources.insert(at, std::move(row));
    return filament_td_sources_save(list) ? id : std::string();
}

bool filament_td_source_remove(const std::string &id)
{
    FilamentTdSourceList    list = filament_td_sources();
    const FilamentTdSource *row  = list.find(id);
    if (row == nullptr || row->kind == FilamentTdSourceKind::Personal || row->kind == FilamentTdSourceKind::Shipped)
        return false;
    const boost::filesystem::path file = library_file(id);
    if (file.empty())
        return false;

    list.sources.erase(list.sources.begin() + (row - list.sources.data()));
    // The list first and the file second. A library whose row has gone is a file nothing reads; a row
    // whose library has gone is a source the user sees and cannot use.
    if (!filament_td_sources_save(list))
        return false;
    boost::system::error_code ec;
    boost::filesystem::remove(file, ec);
    filament_td_reset_cache();
    return true;
}

bool filament_td_suppress(const FilamentTd &entry, bool suppressed)
{
    if (entry.brand.empty() || entry.name.empty())
        return false;
    const std::string    key  = filament_td_entry_key(entry);
    FilamentTdSourceList list = filament_td_sources();
    const auto           at   = std::find(list.suppressed.begin(), list.suppressed.end(), key);
    if (suppressed) {
        if (at != list.suppressed.end())
            return true;
        if (list.suppressed.size() >= FILAMENT_TD_IMPORT_MAX_ENTRIES)
            return false;
        list.suppressed.push_back(key);
    } else {
        if (at == list.suppressed.end())
            return true;
        list.suppressed.erase(at);
    }
    return filament_td_sources_save(list);
}

bool filament_td_record_measured(const FilamentTd &entry, bool suppress)
{
    const boost::filesystem::path file = library_file(PERSONAL_SOURCE_ID);
    if (file.empty())
        return false;

    // Through the same normalizing a library's rows go through, so that a measurement is found by
    // exactly what a library's row is found by - and so that the file round trips through the importer
    // unchanged, which is what lets it be carried to another machine by copying it.
    FilamentTd one;
    one.brand = table_brand(entry.brand);
    if (one.brand.empty() || name_tokens(entry.name).empty())
        return false;
    one.name        = composed_name(entry.brand, entry.type, entry.name);
    one.type        = entry.type;
    one.color       = entry.color;
    one.td_mm       = std::isfinite(entry.td_mm) ? std::clamp(entry.td_mm, FILAMENT_TD_MIN_MM, FILAMENT_TD_MAX_MM) :
                                                   FILAMENT_TD_DEFAULT_MM;
    one.source_id   = PERSONAL_SOURCE_ID;
    one.source_kind = FilamentTdSourceKind::Personal;

    std::vector<FilamentTd>   entries;
    boost::system::error_code ec;
    if (boost::filesystem::is_regular_file(file, ec) && !ec) {
        try {
            read_file(file, entries);
        } catch (...) {
            // A personal library we cannot read is not one to write over. Somebody can go and look at
            // the file; nothing here can give it back once it is gone.
            return false;
        }
    }

    const std::string key      = filament_td_entry_key(one);
    bool              replaced = false;
    for (FilamentTd &have : entries)
        if (filament_td_entry_key(have) == key) {
            have     = one; // measuring the same spool again corrects it rather than listing it twice
            replaced = true;
            break;
        }
    if (!replaced) {
        if (entries.size() >= FILAMENT_TD_IMPORT_MAX_ENTRIES)
            return false;
        entries.push_back(one);
    }

    // No name on the file, so that an entry out of it carries no library name: a number of your own is
    // not from anywhere, and the card says what Personal means in the user's language rather than
    // printing a word out of a file.
    if (!write_json_atomic(file, library_json(entries, std::string(), FilamentTdSourceKind::Personal, std::string(),
                                              std::time(nullptr))))
        return false;
    filament_td_reset_cache();
    // Pinning it is almost always what somebody who has just measured a spool wants: the number they
    // measured is the number they meant, and a library that disagrees is not news.
    return suppress ? filament_td_suppress(one, true) : true;
}

std::vector<FilamentTd> filament_td_personal_entries()
{
    // Read afresh rather than filtered out of the table, deliberately: a Personal source the user has
    // turned off is still theirs, and an export that quietly left it out would be exactly the kind of
    // loss everything else in here is arranged to prevent.
    std::vector<FilamentTd> out;
    for (const LoadedSource &one : read_user_sources()) {
        if (one.source.kind != FilamentTdSourceKind::Personal)
            continue;
        for (const FilamentTd &entry : one.entries) {
            FilamentTd copy  = entry;
            copy.source_id   = one.source.id;
            copy.source_kind = FilamentTdSourceKind::Personal;
            copy.imported    = false;
            out.push_back(std::move(copy));
        }
    }
    return out;
}

bool filament_td_export(const std::string &path, const std::vector<FilamentTd> &entries, const std::string &name)
{
    if (path.empty())
        return false;
    // Written as a personal library, because that is what leaves a machine this way: your own
    // measurements, on their way to your other printer. Dropping the file into that machine's
    // `filament_td` directory makes it a Personal source there, which is right - they are still your
    // measurements. Somebody else's library arrives through the importer instead, and is stamped as
    // theirs.
    return write_json_atomic(boost::filesystem::path(path),
                             library_json(entries, name, FilamentTdSourceKind::Personal, std::string(),
                                          std::time(nullptr)));
}

FilamentTdExport filament_td_export_personal(const std::string &path)
{
    FilamentTdExport              out;
    const std::vector<FilamentTd> entries = filament_td_personal_entries();
    out.ok                                = filament_td_export(path, entries, std::string());
    out.written                           = out.ok ? entries.size() : 0;
    return out;
}

FilamentTdUpdate filament_td_merge(const std::vector<FilamentTd> &current, const std::vector<FilamentTd> &fetched,
                                   const FilamentTdSourceList &sources)
{
    FilamentTdUpdate out;
    // One set, built once: is_suppressed() is linear, and a merge asks it a question per row.
    const std::set<std::string> pinned(sources.suppressed.begin(), sources.suppressed.end());

    std::map<std::string, const FilamentTd *> have;
    for (const FilamentTd &entry : current)
        have.emplace(filament_td_entry_key(entry), &entry);

    std::set<std::string> seen;
    out.merged.reserve(std::max(current.size(), fetched.size()));
    for (const FilamentTd &entry : fetched) {
        const std::string key = filament_td_entry_key(entry);
        if (!seen.insert(key).second)
            continue; // the library lists one spool twice; the first of them stands
        const auto at = have.find(key);
        if (pinned.count(key) != 0) {
            // The user measured this one. The library does not get to say anything about it, whether
            // it is offering a correction or offering the spool for the first time: suppression is a
            // statement about the spool, not about this version of this file.
            ++out.suppressed;
            if (at != have.end())
                out.merged.push_back(*at->second);
            continue;
        }
        if (at == have.end()) {
            if (out.merged.size() >= FILAMENT_TD_IMPORT_MAX_ENTRIES) {
                // Past what one library may hold. Only additions are refused here: an entry already in
                // the file, and above all one the user has pinned, is never dropped to make room.
                ++out.over_cap;
                continue;
            }
            ++out.added;
            out.merged.push_back(entry);
        } else if (same_measurement(*at->second, entry)) {
            ++out.unchanged;
            out.merged.push_back(*at->second); // the entry as it is, provenance and all
        } else {
            ++out.updated;
            out.merged.push_back(entry);
        }
    }

    // What the library used to list and does not any more. Gone upstream is gone, unless it is one the
    // user pinned, in which case it stays exactly as it is: an update is not allowed to take away a
    // number somebody measured, and a library dropping a row is still an update.
    for (const FilamentTd &entry : current) {
        const std::string key = filament_td_entry_key(entry);
        if (!seen.insert(key).second)
            continue;
        if (pinned.count(key) != 0) {
            ++out.suppressed;
            out.merged.push_back(entry);
        } else {
            ++out.dropped;
        }
    }
    return out;
}

FilamentTdUpdate filament_td_update_prepare(const std::string &source_id, const std::string &text,
                                            const std::string &source_name)
{
    FilamentTdUpdate            out;
    const FilamentTdSourceList  list = filament_td_sources();
    const FilamentTdSource     *row  = list.find(source_id);
    if (source_id == PERSONAL_SOURCE_ID || (row != nullptr && (row->kind == FilamentTdSourceKind::Personal ||
                                                               row->kind == FilamentTdSourceKind::Shipped)))
        return out; // not a library an update may rewrite: nothing added, nothing dropped, nothing done

    std::vector<FilamentTd> fetched;
    const std::string       name = !source_name.empty() ? source_name :
                                   (row != nullptr && !row->name.empty() ? row->name : source_id);
    FilamentTdImport        parsed = filament_td_parse_text(text, name, fetched);
    if (parsed.unreadable) {
        // A fetch that did not arrive, or arrived as somebody's error page. It changes nothing, which
        // is the whole point of working the update out before applying it.
        out.parsed = std::move(parsed);
        return out;
    }

    std::vector<FilamentTd>       current;
    const boost::filesystem::path file = library_file(source_id);
    boost::system::error_code     ec;
    if (!file.empty() && boost::filesystem::is_regular_file(file, ec) && !ec) {
        try {
            read_file(file, current);
        } catch (...) {
            current.clear(); // an unreadable library is one every row of the new one is news to
        }
    }

    out        = filament_td_merge(current, fetched, list);
    out.parsed = std::move(parsed);
    return out;
}

bool filament_td_update_apply(const std::string &source_id, const FilamentTdUpdate &update)
{
    // The three refusals that make a measurement impossible to lose. Not because the merge is careful
    // - it is, but careful is a property that can be edited away - but because this is the only path
    // by which a fetched library reaches disk, it writes one file, and that file can never be the one
    // the user's own measurements are in.
    if (source_id == PERSONAL_SOURCE_ID)
        return false;
    const FilamentTdSourceList  list = filament_td_sources();
    const FilamentTdSource     *row  = list.find(source_id);
    if (row == nullptr || row->kind == FilamentTdSourceKind::Personal || row->kind == FilamentTdSourceKind::Shipped)
        return false;
    const boost::filesystem::path file = library_file(source_id);
    if (file.empty())
        return false;

    if (!write_json_atomic(file, library_json(update.merged, row->name, row->kind, row->origin, std::time(nullptr))))
        return false;
    // The row's own count and timestamp are re-derived from the file it has just written, so there is
    // nothing to save: the sources file holds what the user decided, and the libraries hold the facts.
    filament_td_reset_cache();
    return true;
}

} // namespace Slic3r
