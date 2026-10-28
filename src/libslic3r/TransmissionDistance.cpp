#include "TransmissionDistance.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
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
// One file: a vendor's worth of colours. `brand` is on the file rather than on every row, since a
// file belongs to one brand - except in a file the importer wrote, where one library spans brands and
// every row names its own.
void read_file(const boost::filesystem::path &path, std::vector<FilamentTd> &out)
{
    boost::nowide::ifstream in(path.string());
    if (!in.good())
        return;
    nlohmann::json root;
    in >> root;
    const std::string brand = root.value("brand", std::string());
    // Where the numbers came from, and whether they are ours: a file the importer wrote says so, and
    // the card has to be able to tell somebody else's measurement from the one we shipped.
    const std::string source   = root.value("source", std::string());
    const bool        imported = root.value("imported", false);
    const size_t      before   = out.size();
    for (const nlohmann::json &one : root.value("filaments", nlohmann::json::array())) {
        if (out.size() - before >= FILAMENT_TD_IMPORT_MAX_ENTRIES)
            break; // no one file is allowed to be the whole of memory, however it got into the directory
        FilamentTd entry;
        entry.brand     = one.value("brand", brand);
        entry.name      = one.value("name", std::string());
        entry.type      = one.value("type", std::string());
        entry.td_mm     = one.value("td", 0.f);
        entry.td_source = source;
        entry.imported  = imported;
        const std::string hex = one.value("color", std::string());
        if (entry.brand.empty() || entry.name.empty() || entry.td_mm <= 0.f || hex.size() < 7)
            continue;
        const auto channel = [&hex](size_t at) { return float(std::stoi(hex.substr(at, 2), nullptr, 16)) / 255.f; };
        entry.color = ColorRGB(channel(1), channel(3), channel(5));
        out.push_back(std::move(entry));
    }
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
} // namespace

// The table is cached, and an import has to be visible without a restart, so it is a guarded global
// rather than a function-local static. The mutex makes a rebuild atomic with respect to any lookup
// that starts during it; what it cannot do is keep a pointer *already* handed out valid across one,
// which is why filament_td_reset_cache() is documented as a UI-thread call and why every consumer
// copies the number it wanted (see ChromaFilament) instead of holding the entry.
static std::mutex              s_td_table_mutex;
static std::vector<FilamentTd> s_td_table;
static bool                    s_td_table_loaded = false;

const std::vector<FilamentTd> &filament_td_table()
{
    std::lock_guard<std::mutex> lock(s_td_table_mutex);
    if (!s_td_table_loaded) {
        std::vector<FilamentTd> out;
        read_dir(boost::filesystem::path(data_dir()) / "filament_td", out);
        // Precedence, and it only decides an exact tie, since the search below picks on match quality
        // first and keeps the first of equals: a value the user measured for their own spool, then one
        // out of a library they imported, then what shipped. A number somebody wrote down themselves
        // beats a community library, and both beat nothing; an import that matches a spool better than
        // a shipped entry does still wins, because that is the search's business and not the order's.
        std::stable_partition(out.begin(), out.end(), [](const FilamentTd &entry) { return !entry.imported; });
        const std::vector<FilamentTd> &shipped = shipped_table();
        out.insert(out.end(), shipped.begin(), shipped.end());
        s_td_table        = std::move(out);
        s_td_table_loaded = true;
    }
    return s_td_table;
}

void filament_td_reset_cache()
{
    std::lock_guard<std::mutex> lock(s_td_table_mutex);
    s_td_table.clear();
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

// What makes two rows the same spool: the brand key, the words of the name, and the colour as the
// three bytes it is written as. Two rows this agrees on are one measurement offered twice.
std::string dedupe_key(const FilamentTd &entry)
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

// The file name an import lands under. One file per source, named after it, so that re-importing a
// corrected library replaces it instead of adding to a pile, and so that a user can delete an
// imported library without touching the values they measured themselves.
std::string import_file_name(const boost::filesystem::path &from)
{
    std::string slug;
    for (unsigned char c : from.stem().string()) {
        if (std::isalnum(c))
            slug.push_back(char(std::tolower(c)));
        else if (!slug.empty() && slug.back() != '-')
            slug.push_back('-');
        if (slug.size() >= 48)
            break;
    }
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();
    return "imported-" + (slug.empty() ? std::string("library") : slug) + ".json";
}
} // namespace

FilamentTdImport filament_td_parse(const std::string &path, std::vector<FilamentTd> &out)
{
    FilamentTdImport result;
    const boost::filesystem::path from(path);
    // The file's own name is the provenance, and data rather than a sentence: the GUI is what says
    // "imported from" around it, in the user's language.
    result.source = from.filename().string();

    std::string text;
    if (!read_text(from, text)) {
        result.unreadable = true;
        return result;
    }

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
        shipped_keys.insert(dedupe_key(entry));

    std::map<std::string, size_t> at;
    std::vector<Normalized>       kept;
    kept.reserve(normalized.size());
    for (Normalized &one : normalized) {
        const std::string key = dedupe_key(one.entry);
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

    try {
        const boost::filesystem::path dir = boost::filesystem::path(data_dir()) / "filament_td";
        boost::system::error_code     ec;
        boost::filesystem::create_directories(dir, ec);
        const boost::filesystem::path file = dir / import_file_name(boost::filesystem::path(path));

        // Our own shape, so that read_file() above picks it up with no special case: the brand on
        // every row rather than on the file, since one library spans brands, and the flag that says
        // these numbers are somebody else's.
        nlohmann::json root;
        root["source"]        = result.source;
        root["imported"]      = true;
        root["imported_from"] = boost::filesystem::path(path).string();
        root["unit"]          = "mm";
        nlohmann::json list   = nlohmann::json::array();
        for (const FilamentTd &entry : entries) {
            nlohmann::json one;
            one["brand"] = entry.brand;
            one["name"]  = entry.name;
            if (!entry.type.empty())
                one["type"] = entry.type;
            one["color"] = color_hex(entry.color);
            // Three decimals, rounded in double so that the file reads 1.2 rather than a float's
            // nearest neighbour to it. A transmission distance is known to about a tenth of a
            // millimetre, and the digits past that are noise written out as though measured.
            one["td"] = std::round(double(entry.td_mm) * 1000.) / 1000.;
            list.push_back(std::move(one));
        }
        root["filaments"] = std::move(list);

        // Through a temporary and into place, so that an import interrupted half way leaves the
        // previous one whole rather than a file that is half a library.
        const boost::filesystem::path temp = file.string() + ".tmp";
        {
            boost::nowide::ofstream write(temp.string(), std::ios::binary | std::ios::trunc);
            if (!write.good())
                return result;
            // Replacing rather than refusing on bad UTF-8: the names came out of a file we do not
            // trust, and one byte somebody's exporter got wrong is not a reason to lose the library.
            write << root.dump(1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
            if (!write.good())
                return result;
        }
        boost::filesystem::remove(file, ec); // rename over an existing file is not portable
        boost::filesystem::rename(temp, file, ec);
        if (ec) {
            boost::filesystem::remove(temp, ec);
            return result;
        }
        result.written_path = file.string();
    } catch (...) {
        // A write that did not happen is reported by an empty written_path, not by an exception
        // reaching the GUI.
    }
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

} // namespace Slic3r
