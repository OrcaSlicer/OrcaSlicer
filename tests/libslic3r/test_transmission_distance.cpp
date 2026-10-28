#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstddef>
#include <cstdio>
#include <ios>
#include <limits>
#include <string>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/Color.hpp"
#include "libslic3r/TransmissionDistance.hpp"
#include "libslic3r/Utils.hpp"

#include "test_utils.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

TEST_CASE("A transmission distance is looked up by colour, or not at all", "[ChromaStack]")
{
    // A stand-in for the shipped table, so the matching is tested rather than the resources path.
    // Three colours of one line, spread the way a real line is, plus another brand.
    const std::vector<FilamentTd> table = {
        { "polymaker", "Panchroma Matte PLA Cotton White", "PLA", ColorRGB(0.94f, 0.93f, 0.90f), 4.1f },
        { "polymaker", "Panchroma Matte PLA Army Blue", "PLA", ColorRGB(0.22f, 0.29f, 0.37f), 0.1f },
        { "polymaker", "Panchroma Matte PLA Lava Red", "PLA", ColorRGB(0.72f, 0.16f, 0.14f), 0.8f },
        { "prusament", "Prusament PLA Galaxy Black", "PLA", ColorRGB(0.11f, 0.11f, 0.13f), 0.4f },
    };

    SECTION("the vendor and the colour together pick one spool out of the line")
    {
        const FilamentTd *found = filament_td_lookup(table, "Polymaker", "Panchroma PLA Matte @base",
                                                 ColorRGB(0.93f, 0.92f, 0.89f));
        REQUIRE(found != nullptr);
        REQUIRE_THAT(found->td_mm, WithinAbs(4.1f, 1e-4f));

        found = filament_td_lookup(table, "Polymaker", "Panchroma PLA Matte @base", ColorRGB(0.23f, 0.30f, 0.36f));
        REQUIRE(found != nullptr);
        REQUIRE_THAT(found->td_mm, WithinAbs(0.1f, 1e-4f));
    }

    SECTION("the vendor is matched through the name it sells filament under")
    {
        // The presets say Prusa Polymers; the filament is Prusament.
        const FilamentTd *found = filament_td_lookup(table, "Prusa Polymers", "Prusament PLA @base",
                                                 ColorRGB(0.10f, 0.10f, 0.12f));
        REQUIRE(found != nullptr);
        REQUIRE_THAT(found->td_mm, WithinAbs(0.4f, 1e-4f));
    }

    SECTION("a colour the line is not made in gets nothing rather than the nearest thing")
    {
        // This is the failure that matters: answering with a number measured for another spool is
        // worse than answering with nothing, because it looks measured.
        REQUIRE(filament_td_lookup(table, "Polymaker", "Panchroma PLA Matte @base", ColorRGB(1.f, 0.f, 1.f)) == nullptr);
    }

    SECTION("a vendor the table has never heard of gets nothing")
    {
        REQUIRE(filament_td_lookup(table, "Nonesuch Filaments", "Nonesuch PLA Matte",
                                 ColorRGB(0.94f, 0.93f, 0.90f)) == nullptr);
    }

    SECTION("one word in common is a coincidence, not a line")
    {
        REQUIRE(filament_td_lookup(table, "Polymaker", "PLA", ColorRGB(0.94f, 0.93f, 0.90f)) == nullptr);
    }

    SECTION("the right line in a colour of another line of the same brand is still refused")
    {
        // Lava Red is in the table, but not within reach of a blue, and the line matches either way.
        const FilamentTd *found = filament_td_lookup(table, "Polymaker", "Panchroma PLA Matte @base",
                                                 ColorRGB(0.20f, 0.60f, 0.95f));
        REQUIRE(found == nullptr);
    }
}

// --------------------------------------------------------------------------------------------------
// Importing somebody else's library.
//
// Every case below goes through a file on disk, because a file on disk is what the importer takes: a
// library a user downloaded, which is untrusted input from the first byte.
// --------------------------------------------------------------------------------------------------

namespace {

// A library file holding whatever text a test wants.
class Library
{
public:
    explicit Library(const std::string &text, const std::string &extension = ".json") : m_file(extension)
    {
        boost::nowide::ofstream out(m_file.string(), std::ios::binary);
        out << text;
    }
    std::string path() const { return m_file.string(); }
    std::string file_name() const { return m_file.path().filename().string(); }

private:
    ScopedTemporaryFile m_file;
};

// data_dir() is process-wide, so it goes back however the test leaves - including through a failed
// REQUIRE - to stay green under --order rand.
class ScopedDataDir
{
public:
    explicit ScopedDataDir(const boost::filesystem::path &dir) : m_previous(data_dir()) { set_data_dir(dir.string()); }
    ~ScopedDataDir() { set_data_dir(m_previous); }
    ScopedDataDir(const ScopedDataDir &)            = delete;
    ScopedDataDir &operator=(const ScopedDataDir &) = delete;

private:
    const std::string m_previous;
};

// Two Polymaker colours, as far apart as two colours of one line get, so that a lookup can tell them
// apart and a near miss can be seen to be refused.
const char *const TWO_JSON_ENTRIES = R"([
    { "brand": "Polymaker", "name": "Panchroma Matte PLA Deep Lagoon", "type": "PLA", "color": "#384a5e", "td": 0.1 },
    { "vendor": "Polymaker", "filament": "Panchroma Matte PLA Ember Red", "material": "PLA", "colour": "b72924", "TD": 0.8 }
])";

// The entry whose name holds `word`, or nullptr. The stored name is composed out of the brand, the
// type and the name the file gave, so a test asks for the row it means by a word only that row has.
const FilamentTd *entry_with(const std::vector<FilamentTd> &table, const std::string &word)
{
    for (const FilamentTd &entry : table)
        if (entry.name.find(word) != std::string::npos)
            return &entry;
    return nullptr;
}

} // namespace

TEST_CASE("A library given as a JSON array imports every row it names fully", "[ChromaStack]")
{
    const Library           library(TWO_JSON_ENTRIES);
    std::vector<FilamentTd> table;
    const FilamentTdImport  result = filament_td_parse(library.path(), table);

    REQUIRE_FALSE(result.unreadable);
    REQUIRE(result.taken == 2);
    REQUIRE(result.skipped() == 0);
    REQUIRE(table.size() == 2);

    // The brand goes in under the key a lookup searches by, not as the file spelled it.
    REQUIRE(table[0].brand == "polymaker");
    REQUIRE(table[1].brand == "polymaker");

    const FilamentTd *blue = entry_with(table, "Deep Lagoon");
    REQUIRE(blue != nullptr);
    REQUIRE_THAT(blue->td_mm, WithinAbs(0.1f, 1e-4f));
    REQUIRE(blue->type == "PLA");
    REQUIRE_THAT(blue->color.r(), WithinAbs(0x38 / 255.f, 1e-3f));
    // The stored name carries the brand, because a preset is named "<brand> <type> @<printer>" and the
    // search wants two words in common with it: a row named only after its colour is unreachable.
    REQUIRE(blue->name == "Polymaker Panchroma Matte PLA Deep Lagoon");

    // The second row spells every one of its fields differently, and arrives the same way.
    const FilamentTd *red = entry_with(table, "Ember Red");
    REQUIRE(red != nullptr);
    REQUIRE_THAT(red->td_mm, WithinAbs(0.8f, 1e-4f));
    REQUIRE_THAT(red->color.g(), WithinAbs(0x29 / 255.f, 1e-3f));
}

TEST_CASE("A library given as an object of filaments names each one by its key", "[ChromaStack]")
{
    // The shape a library takes when it is written as a dictionary: the name is the key, and the brand
    // is spelled the way the maker writes it rather than the way the presets do.
    const Library library(R"({
        "Prusament PLA Galaxy Black": { "brand": "Prusa Research", "type": "PLA", "hex": "#1c1c20", "transmission_distance": 0.4 },
        "Prusament PLA Lipstick Red": { "brand": "Prusa Research", "type": "PLA", "hex": "#d02f37", "transmissionDistance": 2.0 }
    })");
    std::vector<FilamentTd> table;
    const FilamentTdImport  result = filament_td_parse(library.path(), table);

    REQUIRE(result.taken == 2);
    REQUIRE(result.skipped() == 0);

    const FilamentTd *black = entry_with(table, "Galaxy Black");
    REQUIRE(black != nullptr);
    // Prusa Research sells Prusament, and prusament is the key the table and the presets meet on.
    REQUIRE(black->brand == "prusament");
    REQUIRE_THAT(black->td_mm, WithinAbs(0.4f, 1e-4f));
    // The name already says who made it, so nothing is put in front of it: adding the brand as the
    // library spells it would read "Prusa Research Prusament PLA Galaxy Black".
    REQUIRE(black->name == "Prusament PLA Galaxy Black");

    const FilamentTd *red = entry_with(table, "Lipstick Red");
    REQUIRE(red != nullptr);
    REQUIRE_THAT(red->td_mm, WithinAbs(2.f, 1e-4f));
}

TEST_CASE("A library with a header beside its filaments is walked into rather than read as one", "[ChromaStack]")
{
    // Our own shape, and the shape most community exports take: a wrapper naming the brand once, with
    // the rows under it.
    const Library library(R"({
        "brand": "Polymaker",
        "source": "somebody's own measurements",
        "unit": "mm",
        "filaments": [
            { "name": "Panchroma Matte PLA Chalk White", "type": "PLA", "color": "#f0eee6", "td": 4.1 },
            { "name": "Panchroma Matte PLA Deep Lagoon", "type": "PLA", "color": "#384a5e", "td": 0.1 }
        ]
    })");
    std::vector<FilamentTd> table;
    const FilamentTdImport  result = filament_td_parse(library.path(), table);

    REQUIRE(result.taken == 2);
    REQUIRE(result.skipped() == 0);
    // The brand named on the wrapper carries down to rows that do not name one.
    REQUIRE(table[0].brand == "polymaker");
    REQUIRE(table[1].brand == "polymaker");
}

TEST_CASE("A library given as a CSV imports whatever order its columns are in", "[ChromaStack]")
{
    SECTION("columns in an order of the file's choosing, with one we do not know")
    {
        const Library library("td,hex,Name,Vendor,Notes\n"
                              "0.8,#b72924,Panchroma Matte PLA Ember Red,Polymaker,measured on a step print\n"
                              "4.1,#f0eee6,Panchroma Matte PLA Chalk White,Polymaker,\n",
                              ".csv");
        std::vector<FilamentTd> table;
        const FilamentTdImport  result = filament_td_parse(library.path(), table);

        REQUIRE(result.taken == 2);
        REQUIRE(result.skipped() == 0);
        const FilamentTd *red = entry_with(table, "Ember Red");
        REQUIRE(red != nullptr);
        REQUIRE(red->brand == "polymaker");
        REQUIRE_THAT(red->td_mm, WithinAbs(0.8f, 1e-4f));
    }

    SECTION("a semicolon export, with a quoted cell and the unit written into the header")
    {
        // What a spreadsheet in a comma-decimal locale writes, commas and all inside the quotes.
        const Library library("Manufacturer;Filament;TD (mm);Colour\n"
                              "Polymaker;\"Panchroma Matte PLA Ember Red, 2024 batch\";0.8 mm;#b72924\n",
                              ".csv");
        std::vector<FilamentTd> table;
        const FilamentTdImport  result = filament_td_parse(library.path(), table);

        REQUIRE(result.taken == 1);
        REQUIRE(table[0].name.find("Ember Red, 2024 batch") != std::string::npos);
        REQUIRE_THAT(table[0].td_mm, WithinAbs(0.8f, 1e-4f));
    }

    SECTION("a CSV with no transmission distance column is some other CSV")
    {
        const Library           library("name,color\nPanchroma Matte PLA Ember Red,#b72924\n", ".csv");
        std::vector<FilamentTd> table;
        const FilamentTdImport  result = filament_td_parse(library.path(), table);

        REQUIRE(result.unreadable);
        REQUIRE(result.taken == 0);
        REQUIRE(table.empty());
    }
}

TEST_CASE("Every colour spelling a library might use arrives as the same colour", "[ChromaStack]")
{
    // All of these are 0x336699, which is also 51,102,153 and 0.2,0.4,0.6 of full scale.
    const std::string spelling = GENERATE(as<std::string>{}, "\"#336699\"", "\"336699\"", "\"0x336699\"", "\"#369\"",
                                          "\"#336699ff\"", "\"51,102,153\"", "\"rgb(51, 102, 153)\"",
                                          "[51, 102, 153]", "[0.2, 0.4, 0.6]");

    const Library library("[{ \"brand\": \"Polymaker\", \"name\": \"Panchroma Matte PLA Blue Shade\", \"td\": 1.5,"
                          " \"color\": " +
                          spelling + " }]");
    std::vector<FilamentTd> table;
    const FilamentTdImport  result = filament_td_parse(library.path(), table);

    REQUIRE(result.taken == 1);
    REQUIRE_THAT(table[0].color.r(), WithinAbs(0.2f, 2e-3f));
    REQUIRE_THAT(table[0].color.g(), WithinAbs(0.4f, 2e-3f));
    REQUIRE_THAT(table[0].color.b(), WithinAbs(0.6f, 2e-3f));
}

TEST_CASE("A row missing what a lookup searches by is skipped, and said to be", "[ChromaStack]")
{
    const Library library(R"([
        { "brand": "Polymaker", "name": "Panchroma Matte PLA Deep Lagoon", "color": "#384a5e" },
        { "brand": "Polymaker", "color": "#384a5e", "td": 1.0 },
        { "name": "Panchroma Matte PLA Chalk White", "color": "#f0eee6", "td": 4.1 },
        { "brand": "Polymaker", "name": "Panchroma Matte PLA Ember Red", "color": "not a colour", "td": 0.8 },
        { "brand": "Polymaker", "name": "Panchroma Matte PLA Ivory Haze", "color": "#f0eee6", "td": "about four" },
        { "brand": "Polymaker", "name": "Panchroma Matte PLA Dusk White", "color": "#eeeeee", "td": -3 }
    ])");
    std::vector<FilamentTd> table;
    const FilamentTdImport  result = filament_td_parse(library.path(), table);

    REQUIRE_FALSE(result.unreadable);
    // Only the last row survives, and it survives because a TD out of range is clamped rather than
    // thrown away: the row still says which colour it is and roughly how translucent.
    REQUIRE(result.taken == 1);
    REQUIRE(result.clamped == 1);
    REQUIRE(table.size() == 1);
    REQUIRE_THAT(table[0].td_mm, WithinAbs(FILAMENT_TD_MIN_MM, 1e-4f));

    REQUIRE(result.skipped_no_td == 2);
    REQUIRE(result.skipped_no_name == 1);
    REQUIRE(result.skipped_no_brand == 1);
    REQUIRE(result.skipped_no_color == 1);
    REQUIRE(result.skipped() == 5);
}

TEST_CASE("A file that is not a library at all imports as nothing", "[ChromaStack]")
{
    const std::string junk = GENERATE(as<std::string>{},
                                      // Prose, which is what a readme lying in the same folder is.
                                      "This is not a filament library at all.\nIt is a note to self.\n",
                                      // JSON that stops half way, which is what a half finished
                                      // download is.
                                      "[{ \"name\": \"Panchroma Matte PLA Ember Red\", \"td\":",
                                      // Nothing.
                                      "",
                                      // Bytes, with the NULs and the quotes a hostile file would
                                      // carry.
                                      std::string("\x00\x01\x02\xff\"\"\"{{{", 10),
                                      // Nested deeply enough to take a recursive parser's stack with
                                      // it, which is why the depth is counted before anything parses.
                                      std::string(5000, '[') + std::string(5000, ']'));

    const Library           library(junk);
    std::vector<FilamentTd> table;
    const FilamentTdImport  result = filament_td_parse(library.path(), table);

    REQUIRE(result.taken == 0);
    REQUIRE(table.empty());
    REQUIRE(result.written_path.empty());
}

TEST_CASE("A library longer than the cap is cut off at it and the rest counted", "[ChromaStack]")
{
    // A file claiming more filaments than anybody has measured is either a mistake or an attack, and
    // either way the cap is what keeps it from being as large as it says it is.
    const size_t over = 10;
    std::string  text = "[";
    for (size_t i = 0; i < FILAMENT_TD_IMPORT_MAX_ENTRIES + over; ++i) {
        // Five digits in the name, so that every row is its own spool rather than a duplicate of the
        // one before it. A single character is not a word that names are told apart by.
        char index[8] = {};
        std::snprintf(index, sizeof(index), "%05zu", i);
        if (i != 0)
            text += ",";
        text += "{\"brand\":\"Polymaker\",\"name\":\"Panchroma Matte PLA Shade ";
        text += index;
        text += "\",\"color\":\"#384a5e\",\"td\":1.5}";
    }
    text += "]";

    const Library           library(text);
    std::vector<FilamentTd> table;
    const FilamentTdImport  result = filament_td_parse(library.path(), table);

    REQUIRE(result.skipped_over_cap == over);
    REQUIRE(result.taken == FILAMENT_TD_IMPORT_MAX_ENTRIES);
    REQUIRE(result.skipped_duplicate == 0);
    REQUIRE(table.size() == result.taken);
}

TEST_CASE("The same spool measured twice keeps the later number", "[ChromaStack]")
{
    SECTION("the same brand, name and colour is one spool, however often it is listed")
    {
        const Library library(R"([
            { "brand": "Polymaker", "name": "Panchroma Matte PLA Deep Lagoon", "color": "#384a5e", "td": 0.1 },
            { "brand": "Polymaker", "name": "Panchroma Matte PLA Deep Lagoon", "color": "#384a5e", "td": 0.3 }
        ])");
        std::vector<FilamentTd> table;
        const FilamentTdImport  result = filament_td_parse(library.path(), table);

        REQUIRE(result.taken == 1);
        REQUIRE(result.skipped_duplicate == 1);
        REQUIRE(table.size() == 1);
        // The later row is the correction, so the later row is the one kept.
        REQUIRE_THAT(table[0].td_mm, WithinAbs(0.3f, 1e-4f));
    }

    SECTION("one name in two colours is two spools")
    {
        const Library library(R"([
            { "brand": "Polymaker", "name": "Panchroma Matte PLA Deep Lagoon", "color": "#384a5e", "td": 0.1 },
            { "brand": "Polymaker", "name": "Panchroma Matte PLA Deep Lagoon", "color": "#f0eee6", "td": 4.1 }
        ])");
        std::vector<FilamentTd> table;
        const FilamentTdImport  result = filament_td_parse(library.path(), table);

        REQUIRE(result.taken == 2);
        REQUIRE(result.skipped_duplicate == 0);
    }
}

TEST_CASE("An imported number says where it came from", "[ChromaStack]")
{
    const Library           library(TWO_JSON_ENTRIES);
    std::vector<FilamentTd> table;
    const FilamentTdImport  result = filament_td_parse(library.path(), table);

    REQUIRE(result.taken == 2);
    REQUIRE(result.source == library.file_name());
    for (const FilamentTd &entry : table) {
        // The card shows this as where the number came from, so it has to name the file.
        REQUIRE(entry.td_source == library.file_name());
        // And somebody else's measurement has to be tellable from one we shipped.
        REQUIRE(entry.imported);
    }
}

TEST_CASE("An imported entry is found by the colour as well as the name, or not at all", "[ChromaStack]")
{
    const Library           library(TWO_JSON_ENTRIES);
    std::vector<FilamentTd> table;
    REQUIRE(filament_td_parse(library.path(), table).taken == 2);

    SECTION("the preset's line and the filament's colour together find the spool")
    {
        const FilamentTd *found = filament_td_lookup(table, "Polymaker", "Panchroma PLA Matte @base",
                                                     ColorRGB(0.22f, 0.29f, 0.37f));
        REQUIRE(found != nullptr);
        REQUIRE_THAT(found->td_mm, WithinAbs(0.1f, 1e-4f));
        REQUIRE(found->imported);
    }

    SECTION("the same line in a colour the library does not hold gets nothing")
    {
        // Both halves have to agree. Answering with the number measured for another spool would be
        // worse than answering with nothing, because it would look measured.
        REQUIRE(filament_td_lookup(table, "Polymaker", "Panchroma PLA Matte @base", ColorRGB(0.1f, 0.9f, 0.2f)) ==
                nullptr);
    }

    SECTION("another maker's preset gets nothing out of this library")
    {
        REQUIRE(filament_td_lookup(table, "Prusa Polymers", "Prusament PLA @MK4IS", ColorRGB(0.22f, 0.29f, 0.37f)) ==
                nullptr);
    }
}

TEST_CASE("Where two tables know one spool equally well, the one with precedence answers", "[ChromaStack]")
{
    // The order the whole table is assembled in, which is the precedence: a value the user measured
    // themselves, then one out of a library they imported, then what shipped. It decides an exact tie
    // and nothing else, because the search picks on match quality first.
    std::vector<FilamentTd> table = {
        { "polymaker", "Panchroma Matte PLA Deep Lagoon", "PLA", ColorRGB(0.22f, 0.29f, 0.37f), 0.1f },
        { "polymaker", "Panchroma Matte PLA Deep Lagoon", "PLA", ColorRGB(0.22f, 0.29f, 0.37f), 0.9f },
    };
    table[1].imported  = true;
    table[1].td_source = "somebody-elses-library.csv";

    SECTION("a tie goes to the number the user measured")
    {
        const FilamentTd *found = filament_td_lookup(table, "Polymaker", "Panchroma PLA Matte @base",
                                                     ColorRGB(0.22f, 0.29f, 0.37f));
        REQUIRE(found != nullptr);
        REQUIRE_FALSE(found->imported);
        REQUIRE_THAT(found->td_mm, WithinAbs(0.1f, 1e-4f));
    }

    SECTION("an import that matches the spool better is not held back by the order")
    {
        // Not a tie any more: the imported entry is the colour this filament actually is, and the
        // other one is a colour the filament is not.
        table[0].color = ColorRGB(0.95f, 0.1f, 0.6f);

        const FilamentTd *found = filament_td_lookup(table, "Polymaker", "Panchroma PLA Matte @base",
                                                     ColorRGB(0.22f, 0.29f, 0.37f));
        REQUIRE(found != nullptr);
        REQUIRE(found->imported);
        REQUIRE_THAT(found->td_mm, WithinAbs(0.9f, 1e-4f));
    }
}

TEST_CASE("An import lands in the folder the tables are read from", "[ChromaStack]")
{
    ScopedTemporaryDir     data;
    const ScopedDataDir    scoped(data.path());
    const Library          library(TWO_JSON_ENTRIES);
    const FilamentTdImport result = filament_td_import(library.path());

    REQUIRE(result.taken == 2);
    REQUIRE_FALSE(result.written_path.empty());
    REQUIRE(boost::filesystem::exists(result.written_path));
    // Under data_dir(), where the user's own tables live: nothing is ever written into the resources
    // the program ships.
    REQUIRE(boost::filesystem::path(result.written_path).parent_path() == data.path() / "filament_td");

    SECTION("and reads back as what went into it")
    {
        std::vector<FilamentTd> table;
        const FilamentTdImport  again = filament_td_parse(result.written_path, table);
        REQUIRE(again.taken == 2);
        REQUIRE(again.skipped() == 0);
        const FilamentTd *blue = entry_with(table, "Deep Lagoon");
        REQUIRE(blue != nullptr);
        REQUIRE(blue->brand == "polymaker");
        REQUIRE_THAT(blue->td_mm, WithinAbs(0.1f, 1e-4f));
    }

    SECTION("and importing the same library again replaces that one file")
    {
        const FilamentTdImport twice = filament_td_import(library.path());
        REQUIRE(twice.taken == 2);
        REQUIRE(twice.written_path == result.written_path);

        size_t files = 0;
        for (boost::filesystem::directory_iterator it(data.path() / "filament_td"), end; it != end; ++it)
            ++files;
        REQUIRE(files == 1);
    }
}

TEST_CASE("A transmission distance is written into a preset as a number the option accepts", "[ChromaStack]")
{
    // Two decimals, and no trailing zeros: a TD is known to about a tenth of a millimetre, and digits
    // past that only make a preset look measured to a precision nobody measured.
    REQUIRE(filament_td_format_mm(4.f) == "4");
    REQUIRE(filament_td_format_mm(0.4f) == "0.4");
    REQUIRE(filament_td_format_mm(1.236f) == "1.24");
    // Clamped to what the option allows rather than written out of range.
    REQUIRE(filament_td_format_mm(-2.f) == filament_td_format_mm(FILAMENT_TD_MIN_MM));
    REQUIRE(filament_td_format_mm(1000.f) == "100");
    // A number that is not one at all still has to come out as something the option will take.
    REQUIRE(filament_td_format_mm(std::numeric_limits<float>::quiet_NaN()) == "4");
}
