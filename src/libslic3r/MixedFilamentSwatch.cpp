#include "MixedFilamentSwatch.hpp"

#include "PrintConfig.hpp"
#include "TriangleMesh.hpp"
#include "Utils.hpp"
#include "nlohmann/json.hpp"
#include "prusa_fdm_mixer.hpp"

#include <openssl/md5.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

namespace Slic3r {

namespace {

constexpr int    k_swatch_max_physicals = 4;
constexpr double k_pad_mm               = 16.0;
constexpr double k_pad_min_mm           = 12.0;
constexpr double k_gap_mm               = 2.0;
constexpr double k_thick_mm             = 2.0;

std::optional<SwatchLUT> s_loaded_lut;
std::optional<SwatchLUT> s_disk_lut;

std::string md5_hex_lower(const std::string &payload)
{
    unsigned char digest[16];
    MD5_CTX       ctx;
    MD5_Init(&ctx);
    if (!payload.empty())
        MD5_Update(&ctx, reinterpret_cast<const unsigned char *>(payload.data()), payload.size());
    MD5_Final(digest, &ctx);
    static const char *k_hex = "0123456789abcdef";
    std::string         out(32, '0');
    for (int i = 0; i < 16; ++i) {
        out[size_t(i) * 2]     = k_hex[(digest[i] >> 4) & 0xf];
        out[size_t(i) * 2 + 1] = k_hex[digest[i] & 0xf];
    }
    return out;
}

std::string format_td(float v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", double(v));
    return buf;
}

std::string trim_copy(std::string s)
{
    boost::algorithm::trim(s);
    return s;
}

bool allowed_contains(const std::vector<std::string> &allowed, const std::string &recipe)
{
    return std::find(allowed.begin(), allowed.end(), recipe) != allowed.end();
}

bool valid_mode(const std::string &mode)
{
    return mode == "translucent" || mode == "opaque" || mode == "overhang";
}

boost::filesystem::path swatch_dir(const std::string &dir)
{
    if (!dir.empty())
        return boost::filesystem::path(dir);
    const std::string &root = data_dir();
    if (root.empty())
        return {};
    return boost::filesystem::path(root) / "spectrum";
}

boost::filesystem::path lut_path(const std::string &dir, const std::string &batch_key)
{
    return swatch_dir(dir) / ("swatch_lut_" + batch_key + ".json");
}

boost::filesystem::path manifest_path(const std::string &dir, const std::string &batch_key)
{
    return swatch_dir(dir) / ("swatch_manifest_" + batch_key + ".json");
}

boost::filesystem::path active_path(const std::string &dir)
{
    return swatch_dir(dir) / "swatch_lut_active.json";
}

bool ensure_dir(const boost::filesystem::path &dir, std::string &error)
{
    if (dir.empty()) {
        error = "Swatch data directory is empty.";
        return false;
    }
    boost::system::error_code ec;
    boost::filesystem::create_directories(dir, ec);
    if (ec) {
        error = "Could not create swatch data directory.";
        return false;
    }
    return true;
}

nlohmann::json lut_to_json(const SwatchLUT &lut)
{
    nlohmann::json j;
    j["version"]         = lut.version;
    j["mode"]            = lut.mode.empty() ? "translucent" : lut.mode;
    j["layer_height_mm"] = lut.layer_height_mm;
    j["batch_key"]       = lut.batch_key;
    nlohmann::json entries = nlohmann::json::array();
    for (const SwatchLUTEntry &e : lut.entries) {
        entries.push_back({{"recipe", e.recipe}, {"L", e.L}, {"a", e.a}, {"b", e.b}});
    }
    j["entries"] = std::move(entries);
    return j;
}

bool finite_lab(double L, double a, double b)
{
    return std::isfinite(L) && std::isfinite(a) && std::isfinite(b);
}

bool consume_entry(const std::string              &recipe,
                   double                          L,
                   double                          a,
                   double                          b,
                   const std::vector<std::string> &allowed_recipes,
                   std::set<std::string>          &seen,
                   SwatchLUT                      &out,
                   std::string                    &error)
{
    if (recipe.empty()) {
        error = "LUT entry is missing recipe.";
        return false;
    }
    if (!allowed_contains(allowed_recipes, recipe)) {
        error = "Unknown recipe '" + recipe + "'.";
        return false;
    }
    if (!finite_lab(L, a, b)) {
        error = "LUT Lab values must be finite numbers.";
        return false;
    }
    if (!seen.insert(recipe).second) {
        error = "Duplicate recipe '" + recipe + "'.";
        return false;
    }
    out.entries.push_back({recipe, L, a, b});
    return true;
}

std::vector<float> card_td_if_match(const std::vector<ColorRGB> &physicals)
{
    const SpectrumPhysicalCard cmyk = spectrum_panchroma_cmyk_card();
    const SpectrumPhysicalCard rgbw = spectrum_panchroma_rgbw_card();
    const SpectrumPhysicalCard *card = nullptr;
    if (spectrum_physicals_match_card(physicals, cmyk))
        card = &cmyk;
    else if (spectrum_physicals_match_card(physicals, rgbw))
        card = &rgbw;
    if (card == nullptr)
        return {};
    return {card->td[0], card->td[1], card->td[2], card->td[3]};
}

} // namespace

const SwatchLUTEntry *SwatchLUT::find_recipe(const std::string &recipe) const
{
    for (const SwatchLUTEntry &e : entries) {
        if (e.recipe == recipe)
            return &e;
    }
    return nullptr;
}

std::string spectrum_swatch_recipe_key(const MixMatchResult &r)
{
    if (r.kind == MixMatchResult::Kind::Physical)
        return "P" + std::to_string(r.physical_id);
    return r.recipe_row;
}

std::vector<std::string> spectrum_swatch_recipe_keys(const std::vector<MixMatchResult> &lattice)
{
    std::vector<std::string> keys;
    keys.reserve(lattice.size());
    for (const MixMatchResult &r : lattice)
        keys.push_back(spectrum_swatch_recipe_key(r));
    return keys;
}

std::string spectrum_compute_batch_key(const std::vector<std::string> &hexes,
                                       const std::vector<std::string> &names,
                                       const std::vector<float>       *td)
{
    std::ostringstream oss;
    oss << "v1\n";
    const size_t n = std::min(hexes.size(), size_t(k_swatch_max_physicals));
    for (size_t i = 0; i < n; ++i) {
        oss << hexes[i] << '\t';
        if (i < names.size())
            oss << names[i];
        oss << '\n';
    }
    if (td != nullptr && !td->empty()) {
        oss << "td";
        const size_t tn = std::min({td->size(), n, size_t(k_swatch_max_physicals)});
        for (size_t i = 0; i < tn; ++i)
            oss << '\t' << format_td((*td)[i]);
        oss << '\n';
    }
    return md5_hex_lower(oss.str());
}

std::string spectrum_compute_batch_key(const std::vector<ColorRGB>    &physicals,
                                       const std::vector<std::string> &names,
                                       const std::vector<float>       *td)
{
    std::vector<std::string> hexes;
    hexes.reserve(std::min(physicals.size(), size_t(k_swatch_max_physicals)));
    for (size_t i = 0; i < physicals.size() && i < size_t(k_swatch_max_physicals); ++i)
        hexes.push_back(encode_color(physicals[i]));
    const std::vector<float> *td_use = td;
    std::vector<float>        card_td;
    if (td_use == nullptr) {
        card_td = card_td_if_match(physicals);
        if (!card_td.empty())
            td_use = &card_td;
    }
    return spectrum_compute_batch_key(hexes, names, td_use);
}

std::string serialize_swatch_lut(const SwatchLUT &lut)
{
    return lut_to_json(lut).dump(2);
}

std::string serialize_swatch_manifest(const SwatchManifest &manifest)
{
    nlohmann::json j;
    j["version"]   = 1;
    j["batch_key"] = manifest.batch_key;
    nlohmann::json pads = nlohmann::json::array();
    for (const SwatchManifestPad &p : manifest.pads) {
        pads.push_back({{"index", p.index},
                        {"row", p.row},
                        {"col", p.col},
                        {"recipe", p.recipe},
                        {"extruder", p.extruder}});
    }
    j["pads"] = std::move(pads);
    return j.dump(2);
}

bool parse_swatch_lut_json(const std::string              &text,
                           const std::vector<std::string> &allowed_recipes,
                           SwatchLUT                      &out,
                           std::string                    &error)
{
    out = SwatchLUT{};
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception &ex) {
        error = std::string("Malformed LUT JSON: ") + ex.what();
        return false;
    }
    if (!j.is_object()) {
        error = "LUT JSON must be an object.";
        return false;
    }
    if (!j.contains("version") || !j["version"].is_number() || j["version"].get<int>() != 1) {
        error = "LUT JSON version must be 1.";
        return false;
    }
    out.version = 1;
    if (j.contains("mode")) {
        if (!j["mode"].is_string()) {
            error = "LUT mode must be a string.";
            return false;
        }
        out.mode = j["mode"].get<std::string>();
        if (!valid_mode(out.mode)) {
            error = "LUT mode must be translucent, opaque, or overhang.";
            return false;
        }
    }
    if (j.contains("layer_height_mm")) {
        if (!j["layer_height_mm"].is_number()) {
            error = "LUT layer_height_mm must be a number.";
            return false;
        }
        out.layer_height_mm = j["layer_height_mm"].get<double>();
        if (!std::isfinite(out.layer_height_mm) || out.layer_height_mm <= 0.0) {
            error = "LUT layer_height_mm must be a positive finite number.";
            return false;
        }
    }
    if (j.contains("batch_key")) {
        if (!j["batch_key"].is_string()) {
            error = "LUT batch_key must be a string.";
            return false;
        }
        out.batch_key = j["batch_key"].get<std::string>();
    }
    if (!j.contains("entries") || !j["entries"].is_array()) {
        error = "LUT JSON is missing entries array.";
        return false;
    }
    std::set<std::string> seen;
    for (const nlohmann::json &e : j["entries"]) {
        if (!e.is_object() || !e.contains("recipe") || !e["recipe"].is_string() ||
            !e.contains("L") || !e.contains("a") || !e.contains("b") ||
            !e["L"].is_number() || !e["a"].is_number() || !e["b"].is_number()) {
            error = "LUT entry must have recipe, L, a, b.";
            return false;
        }
        if (!consume_entry(e["recipe"].get<std::string>(), e["L"].get<double>(), e["a"].get<double>(),
                           e["b"].get<double>(), allowed_recipes, seen, out, error))
            return false;
    }
    return true;
}

bool parse_swatch_lut_csv(const std::string              &text,
                          const std::vector<std::string> &allowed_recipes,
                          SwatchLUT                      &out,
                          std::string                    &error)
{
    out = SwatchLUT{};
    std::istringstream iss(text);
    std::string        line;
    bool               header_done = false;
    std::set<std::string> seen;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty())
            continue;
        if (trimmed.front() == '#') {
            // Optional "# batch_key=..." / "# mode=..." / "# layer_height_mm=..."
            const auto eq = trimmed.find('=');
            if (eq != std::string::npos) {
                std::string key = trim_copy(trimmed.substr(1, eq - 1));
                std::string val = trim_copy(trimmed.substr(eq + 1));
                boost::algorithm::to_lower(key);
                if (key == "batch_key")
                    out.batch_key = val;
                else if (key == "mode") {
                    if (!valid_mode(val)) {
                        error = "LUT mode must be translucent, opaque, or overhang.";
                        return false;
                    }
                    out.mode = val;
                } else if (key == "layer_height_mm") {
                    try {
                        out.layer_height_mm = std::stod(val);
                    } catch (...) {
                        error = "LUT layer_height_mm must be a number.";
                        return false;
                    }
                    if (!std::isfinite(out.layer_height_mm) || out.layer_height_mm <= 0.0) {
                        error = "LUT layer_height_mm must be a positive finite number.";
                        return false;
                    }
                }
            }
            continue;
        }
        std::vector<std::string> cols;
        boost::algorithm::split(cols, trimmed, boost::is_any_of(","));
        for (std::string &c : cols)
            boost::algorithm::trim(c);
        if (!header_done) {
            header_done = true;
            if (!cols.empty() && boost::algorithm::iequals(cols[0], "recipe"))
                continue;
        }
        if (cols.size() < 4) {
            error = "CSV row must be recipe,L,a,b.";
            return false;
        }
        double L = 0, a = 0, b = 0;
        try {
            L = std::stod(cols[1]);
            a = std::stod(cols[2]);
            b = std::stod(cols[3]);
        } catch (...) {
            error = "CSV Lab values must be numbers.";
            return false;
        }
        if (!consume_entry(cols[0], L, a, b, allowed_recipes, seen, out, error))
            return false;
    }
    return true;
}

bool parse_swatch_manifest_json(const std::string &text, SwatchManifest &out, std::string &error)
{
    out = SwatchManifest{};
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception &ex) {
        error = std::string("Malformed swatch manifest JSON: ") + ex.what();
        return false;
    }
    if (!j.is_object()) {
        error = "Swatch manifest JSON must be an object.";
        return false;
    }
    if (j.contains("batch_key") && j["batch_key"].is_string())
        out.batch_key = j["batch_key"].get<std::string>();
    if (!j.contains("pads") || !j["pads"].is_array()) {
        error = "Swatch manifest is missing pads array.";
        return false;
    }
    for (const nlohmann::json &p : j["pads"]) {
        if (!p.is_object() || !p.contains("index") || !p.contains("recipe") || !p["recipe"].is_string()) {
            error = "Swatch manifest pad must have index and recipe.";
            return false;
        }
        SwatchManifestPad pad;
        pad.index    = p["index"].get<int>();
        pad.row      = p.value("row", 0);
        pad.col      = p.value("col", 0);
        pad.recipe   = p["recipe"].get<std::string>();
        pad.extruder = p.value("extruder", 1);
        out.pads.push_back(std::move(pad));
    }
    return true;
}

bool save_swatch_lut(const SwatchLUT &lut, std::string &error, const std::string &dir)
{
    const boost::filesystem::path folder = swatch_dir(dir);
    if (!ensure_dir(folder, error))
        return false;
    if (lut.batch_key.empty()) {
        error = "LUT batch_key is empty.";
        return false;
    }
    try {
        save_string_file(lut_path(dir, lut.batch_key), serialize_swatch_lut(lut));
    } catch (...) {
        error = "Could not write LUT file.";
        return false;
    }
    return true;
}

bool load_swatch_lut(const std::string &batch_key, SwatchLUT &out, std::string &error,
                     const std::string &dir)
{
    out = SwatchLUT{};
    if (batch_key.empty()) {
        error = "LUT batch_key is empty.";
        return false;
    }
    const boost::filesystem::path path = lut_path(dir, batch_key);
    if (!boost::filesystem::exists(path)) {
        error = "LUT file not found.";
        return false;
    }
    std::string text;
    try {
        load_string_file(path, text);
    } catch (...) {
        error = "Could not read LUT file.";
        return false;
    }
    // Disk load does not re-check live lattice; recipes were validated at import.
    std::vector<std::string> allowed;
    nlohmann::json           j;
    try {
        j = nlohmann::json::parse(text);
        if (j.contains("entries") && j["entries"].is_array()) {
            for (const nlohmann::json &e : j["entries"]) {
                if (e.is_object() && e.contains("recipe") && e["recipe"].is_string())
                    allowed.push_back(e["recipe"].get<std::string>());
            }
        }
    } catch (...) {
        error = "Malformed LUT JSON.";
        return false;
    }
    return parse_swatch_lut_json(text, allowed, out, error);
}

bool save_swatch_manifest(const SwatchManifest &manifest, std::string &error, const std::string &dir)
{
    const boost::filesystem::path folder = swatch_dir(dir);
    if (!ensure_dir(folder, error))
        return false;
    if (manifest.batch_key.empty()) {
        error = "Swatch manifest batch_key is empty.";
        return false;
    }
    try {
        save_string_file(manifest_path(dir, manifest.batch_key), serialize_swatch_manifest(manifest));
    } catch (...) {
        error = "Could not write swatch manifest.";
        return false;
    }
    return true;
}

bool load_swatch_manifest(const std::string &batch_key, SwatchManifest &out, std::string &error,
                          const std::string &dir)
{
    out = SwatchManifest{};
    const boost::filesystem::path path = manifest_path(dir, batch_key);
    if (!boost::filesystem::exists(path)) {
        error = "Swatch manifest not found.";
        return false;
    }
    std::string text;
    try {
        load_string_file(path, text);
    } catch (...) {
        error = "Could not read swatch manifest.";
        return false;
    }
    return parse_swatch_manifest_json(text, out, error);
}

bool save_swatch_active_key(const std::string &batch_key, const std::string &dir, std::string &error)
{
    const boost::filesystem::path folder = swatch_dir(dir);
    if (!ensure_dir(folder, error))
        return false;
    try {
        nlohmann::json j;
        j["batch_key"] = batch_key;
        save_string_file(active_path(dir), j.dump(2));
    } catch (...) {
        error = "Could not write active LUT pointer.";
        return false;
    }
    return true;
}

bool load_swatch_active_key(std::string &batch_key, const std::string &dir)
{
    batch_key.clear();
    const boost::filesystem::path path = active_path(dir);
    if (!boost::filesystem::exists(path))
        return false;
    try {
        std::string text;
        load_string_file(path, text);
        const nlohmann::json j = nlohmann::json::parse(text);
        if (!j.contains("batch_key") || !j["batch_key"].is_string())
            return false;
        batch_key = j["batch_key"].get<std::string>();
        return !batch_key.empty();
    } catch (...) {
        return false;
    }
}

void spectrum_store_loaded_lut(SwatchLUT lut, const std::string &dir)
{
    s_loaded_lut = std::move(lut);
    if (s_loaded_lut && !s_loaded_lut->batch_key.empty()) {
        std::string err;
        save_swatch_active_key(s_loaded_lut->batch_key, dir, err);
    }
}

const SwatchLUT *spectrum_loaded_lut()
{
    return s_loaded_lut ? &*s_loaded_lut : nullptr;
}

void spectrum_reset_lut_session()
{
    s_loaded_lut.reset();
    s_disk_lut.reset();
}

const SwatchLUT *spectrum_lut_for_batch(const std::string &batch_key, const std::string &dir)
{
    if (batch_key.empty())
        return nullptr;
    if (s_loaded_lut && s_loaded_lut->batch_key == batch_key)
        return &*s_loaded_lut;
    if (s_disk_lut && s_disk_lut->batch_key == batch_key)
        return &*s_disk_lut;
    SwatchLUT   loaded;
    std::string err;
    if (load_swatch_lut(batch_key, loaded, err, dir) && loaded.batch_key == batch_key) {
        s_disk_lut = std::move(loaded);
        return &*s_disk_lut;
    }
    return nullptr;
}

bool spectrum_lut_is_stale(const std::string &live_batch_key, const std::string &dir)
{
    if (s_loaded_lut && s_loaded_lut->batch_key != live_batch_key)
        return true;
    std::string active;
    if (load_swatch_active_key(active, dir) && active != live_batch_key)
        return true;
    return false;
}

SwatchPlateBuild build_swatch_plate(Model                             &model,
                                    const std::vector<MixMatchResult> &lattice,
                                    size_t                             mix_base,
                                    const std::string                 &batch_key,
                                    double                             bed_mm)
{
    SwatchPlateBuild out;
    if (lattice.empty()) {
        out.error = "Swatch lattice is empty.";
        return out;
    }
    size_t mix_count = 0;
    for (const MixMatchResult &r : lattice) {
        if (r.kind == MixMatchResult::Kind::Mix)
            ++mix_count;
    }
    if (mix_count > SPECTRUM_MIX_ENABLED_CAP) {
        out.error = "Swatch mix count exceeds mix-row cap 64.";
        return out;
    }
    if (mix_base == 0) {
        for (const MixMatchResult &r : lattice) {
            if (r.kind == MixMatchResult::Kind::Physical)
                mix_base = std::max(mix_base, size_t(r.physical_id));
        }
        if (mix_base == 0)
            mix_base = 1;
    }

    const int n    = int(lattice.size());
    int       cols = std::max(1, int(std::ceil(std::sqrt(double(n)))));
    int       rows = (n + cols - 1) / cols;
    double    pad  = k_pad_mm;
    auto      extents = [&](double p) {
        const double w = double(cols) * p + double(std::max(0, cols - 1)) * k_gap_mm;
        const double d = double(rows) * p + double(std::max(0, rows - 1)) * k_gap_mm;
        return std::pair<double, double>{w, d};
    };
    while (pad > k_pad_min_mm) {
        const auto ed = extents(pad);
        if (ed.first <= bed_mm && ed.second <= bed_mm)
            break;
        pad -= 1.0;
    }
    {
        const auto ed = extents(pad);
        if (ed.first > bed_mm || ed.second > bed_mm) {
            out.error = "Swatch grid does not fit the bed.";
            return out;
        }
    }

    ModelObject *obj = model.add_object();
    if (obj == nullptr) {
        out.error = "Could not add swatch object.";
        return out;
    }
    obj->name = "Mix Swatch Sheet";
    obj->add_instance();

    const auto   ed      = extents(pad);
    const double origin_x = -ed.first * 0.5 + pad * 0.5;
    const double origin_y = ed.second * 0.5 - pad * 0.5;
    const double pitch    = pad + k_gap_mm;

    std::string mix_defs;
    size_t      mix_index = 0;
    out.manifest.batch_key = batch_key;
    out.manifest.pads.reserve(lattice.size());

    for (int i = 0; i < n; ++i) {
        const MixMatchResult &cand = lattice[size_t(i)];
        const int             row  = i / cols;
        const int             col  = i % cols;
        int                   extruder = 1;
        if (cand.kind == MixMatchResult::Kind::Physical) {
            extruder = int(std::max(1u, cand.physical_id));
        } else {
            extruder = int(mix_base + 1 + mix_index);
            ++mix_index;
            if (!mix_defs.empty())
                mix_defs += ';';
            mix_defs += cand.recipe_row;
        }

        TriangleMesh mesh = make_cube(pad, pad, k_thick_mm);
        ModelVolume *vol  = obj->add_volume(mesh);
        if (vol == nullptr) {
            out.error = "Could not add swatch pad volume.";
            return out;
        }
        vol->name = spectrum_swatch_recipe_key(cand);
        vol->config.set_key_value("extruder", new ConfigOptionInt(extruder));
        const double x = origin_x + double(col) * pitch;
        const double y = origin_y - double(row) * pitch;
        vol->set_offset(Vec3d(x, y, k_thick_mm * 0.5));

        SwatchManifestPad pad_rec;
        pad_rec.index    = i;
        pad_rec.row      = row;
        pad_rec.col      = col;
        pad_rec.recipe   = spectrum_swatch_recipe_key(cand);
        pad_rec.extruder = extruder;
        out.manifest.pads.push_back(std::move(pad_rec));
    }

    if (!mix_defs.empty()) {
        MixedFilamentManager mgr;
        mgr.load_definitions(mix_defs);
        out.mixed_filament_definitions = mgr.serialize_definitions();
    }
    obj->invalidate_bounding_box();
    out.object = obj;
    out.valid  = true;
    return out;
}

} // namespace Slic3r
