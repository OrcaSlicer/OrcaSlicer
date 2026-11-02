#include "TransmissionDistance.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

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
// file belongs to one brand.
void read_file(const boost::filesystem::path &path, std::vector<FilamentTd> &out)
{
    boost::nowide::ifstream in(path.string());
    if (!in.good())
        return;
    nlohmann::json root;
    in >> root;
    const std::string brand = root.value("brand", std::string());
    for (const nlohmann::json &one : root.value("filaments", nlohmann::json::array())) {
        FilamentTd entry;
        entry.brand = one.value("brand", brand);
        entry.name  = one.value("name", std::string());
        entry.type  = one.value("type", std::string());
        entry.td_mm = one.value("td", 0.f);
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
} // namespace

const std::vector<FilamentTd> &filament_td_table()
{
    static const std::vector<FilamentTd> table = []() {
        std::vector<FilamentTd> out;
        // The user's own first, so that a value somebody measured for their own spool wins the ties
        // against the one that shipped - the search keeps the first of equals.
        read_dir(boost::filesystem::path(data_dir()) / "filament_td", out);
        read_dir(boost::filesystem::path(resources_dir()) / "filament_td", out);
        return out;
    }();
    return table;
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

} // namespace Slic3r
