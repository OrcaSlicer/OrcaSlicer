///|/ All PrintConfigCardinality tests (libslic3r suite), in four sections:
///|/   1. Unit tests ([Cardinality])    - classification and sizing plus behavior oracles.
///|/   2. [ProfileCorpus]         - before/after differential over shipped profiles ([.]).
///|/   3. [ProfileCardinalityInvariant] - after-state length invariant over the corpus ([.]).
///|/   4. [ProfileCardinalityGate]      - pre-enforce gate: no profile authors a wrong-length option ([.]).
///|/
///|/ The [.] corpus tests are kept off [Cardinality] because they are heavy; each runs only under its own tag.
///|/
#include <catch2/catch_all.hpp>

#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PrintConfigCardinality.hpp"
#include "libslic3r/Preset.hpp"
#include "profile_corpus.hpp"
#include "test_utils.hpp"

using namespace Slic3r;

namespace {

// Readable cardinality name for assertion messages (Catch2 has no enum stringifier).
const char *cardinality_name(ConfigCardinality a)
{
    switch (a) {
    case ConfigCardinality::Scalar:                 return "Scalar";
    case ConfigCardinality::PerExtruder:            return "PerExtruder";
    case ConfigCardinality::PerFilament:            return "PerFilament";
    case ConfigCardinality::PerExtruderVariant:     return "PerExtruderVariant";
    case ConfigCardinality::PerExtruderVariantDual: return "PerExtruderVariantDual";
    case ConfigCardinality::PerFilamentVariant:     return "PerFilamentVariant";
    case ConfigCardinality::PerProcessVariant:      return "PerProcessVariant";
    case ConfigCardinality::Custom:                 return "Custom";
    }
    return "?";
}

// Sorted "key = serialized" dump; serialize() renders vectors in full so a length change shows.
std::string dump_config(const DynamicPrintConfig &cfg)
{
    std::map<std::string, std::string> lines;
    for (const std::string &key : cfg.keys()) {
        const ConfigOption *opt = cfg.option(key);
        lines[key] = opt ? opt->serialize() : std::string("<null>");
    }
    std::string out;
    for (const auto &kv : lines)
        out += kv.first + " = " + kv.second + "\n";
    return out;
}

struct CorpusViolation { std::string vendor, printer, key; size_t got, want; };

// Build the whole report as one scoped INFO string: Catch2 clears UNSCOPED_INFO at each assertion, so the
// passing CHECKs before the real one would drop the per-profile detail.
std::string violation_report(const char *label, size_t resolved, size_t failed,
                             const std::vector<CorpusViolation> &v)
{
    std::string r = "resolved=" + std::to_string(resolved) + " failed=" + std::to_string(failed)
                  + " " + label + "=" + std::to_string(v.size());
    for (size_t i = 0; i < v.size() && i < 40; ++i)
        r += "\n  " + v[i].vendor + " / " + v[i].printer + " : " + v[i].key
           + " size=" + std::to_string(v[i].got) + " expected=" + std::to_string(v[i].want);
    if (v.size() > 40)
        r += "\n  ... and " + std::to_string(v.size() - 40) + " more";
    return r;
}

} // namespace

// ============================================================================
// 1. Unit tests: cardinality classification and per-N sizing.
// ============================================================================

TEST_CASE("get_parameter_size maps each cardinality bucket to its length", "[Cardinality]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    // Drive the three variant lengths to distinct known sizes so each bucket is distinguishable.
    cfg.set_key_value("printer_extruder_variant",  new ConfigOptionStrings(std::vector<std::string>(2, "x")));
    cfg.set_key_value("filament_extruder_variant", new ConfigOptionStrings(std::vector<std::string>(4, "x")));
    cfg.set_key_value("print_extruder_variant",    new ConfigOptionStrings(std::vector<std::string>(5, "x")));
    const size_t E = 3;

    CHECK(cfg.get_parameter_size("max_layer_height",   E) == E);   // no variant bucket -> extruder count
    CHECK(cfg.get_parameter_size("retraction_length",  E) == 2);   // printer_options_with_variant_1
    CHECK(cfg.get_parameter_size("machine_max_jerk_x", E) == 4);   // printer_options_with_variant_2 (x2)
    CHECK(cfg.get_parameter_size("nozzle_temperature", E) == 4);   // filament_options_with_variant
    CHECK(cfg.get_parameter_size("outer_wall_speed",   E) == 5);   // print_options_with_variant
}

TEST_CASE("Preset::normalize sizes plain per-filament options to the filament count", "[Cardinality]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({
        { "nozzle_diameter",   "0.4" },
        { "filament_diameter", "1.75,1.75,1.75" },
    });

    Preset::normalize(cfg);

    CHECK(cfg.option<ConfigOptionInts>("filament_printable")->size()   == 3);
    CHECK(cfg.option<ConfigOptionStrings>("filament_type")->size()     == 3);
    CHECK(cfg.option<ConfigOptionStrings>("filament_vendor")->size()   == 3);
}

TEST_CASE("Preset::normalize sizes filament_settings_id to the filament count", "[Cardinality]")
{
    // filament_settings_id is UI-only (not in FullPrintConfig) and Custom cardinality, so no other test covers it.
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({
        { "single_extruder_multi_material", "1" },   // SEMM: skip the non-SEMM set_num_extruders branch
        { "filament_diameter",              "1.75,1.75,1.75" }, // 3 filaments
    });
    cfg.set_key_value("filament_settings_id", new ConfigOptionStrings(std::vector<std::string>{ "id0" })); // short: 1

    Preset::normalize(cfg);

    CHECK(cfg.option<ConfigOptionStrings>("filament_settings_id")->size() == 3); // padded to the filament count
}

TEST_CASE("Preset::normalize sizes per-filament options to the filament count on non-SEMM F>E", "[Cardinality]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({
        { "single_extruder_multi_material", "0" },
        { "nozzle_diameter",   "0.4,0.4" },        // 2 extruders
        { "filament_diameter", "1.75,1.75,1.75" }, // 3 filaments (F > E)
    });

    Preset::normalize(cfg);

    CHECK(cfg.option<ConfigOptionInts>("filament_printable")->size() == 3);
}

TEST_CASE("expected_size(cardinality_of) reproduces get_parameter_size on its domain", "[Cardinality]")
{
    // get_parameter_size's domain is the extruder + variant options; plain PerFilament is sized by
    // normalize, so it is not in the comparison list below.
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({ { "nozzle_diameter", "0.4,0.4,0.4" } });                 // extruders = 3
    cfg.set_key_value("printer_extruder_variant",  new ConfigOptionStrings(std::vector<std::string>(2, "x")));
    cfg.set_key_value("filament_extruder_variant", new ConfigOptionStrings(std::vector<std::string>(4, "x")));
    cfg.set_key_value("print_extruder_variant",    new ConfigOptionStrings(std::vector<std::string>(5, "x")));
    const size_t E = 3;
    const AxisCounts n = axis_counts(cfg);

    for (const char *k : { "max_layer_height", "retraction_length", "machine_max_jerk_x",
                           "nozzle_temperature", "outer_wall_speed" }) {
        CAPTURE(k);
        CHECK(expected_size(cardinality_of(k), n) == cfg.get_parameter_size(k, E));
    }
}

TEST_CASE("ConfigCardinality classifies every vector option, and none is left unclassified", "[Cardinality]")
{
    // per_n is the cardinality independently re-derived from the per-N option sets; the loop checks cardinality_of agrees,
    // and that every other vector option is explicitly Custom (a new one defaults to Scalar and fails here).
    auto in = [](const std::set<std::string> &s, const std::string &k) { return s.count(k) > 0; };
    const std::set<std::string> landmines_ext = { "extruder_offset", "extruder_colour" };
    const std::set<std::string> landmines_fil = { "filament_map", "filament_colour",
                                                  "filament_colour_type", "filament_multi_colour" };
    const std::set<std::string> fil(Preset::filament_options().begin(), Preset::filament_options().end());
    // Preset::normalize skips these when sizing filament options: compatible_* and the dev/AMS drying
    // options (filament_dev_options), so they are Custom (owner-sized), not PerFilament.
    std::set<std::string> not_per_filament = { "compatible_prints", "compatible_printers" };
    not_per_filament.insert(filament_dev_options.begin(), filament_dev_options.end());

    for (const auto &kv : print_config_def.options) {
        const std::string &k = kv.first;
        if (!kv.second.default_value || !kv.second.default_value->is_vector())
            continue;
        ConfigCardinality per_n = ConfigCardinality::Scalar; // Scalar = not yet matched to a per-N set
        if      (in(printer_extruder_options, k))        per_n = ConfigCardinality::PerExtruder;
        else if (in(printer_options_with_variant_1, k))  per_n = ConfigCardinality::PerExtruderVariant;
        else if (in(printer_options_with_variant_2, k))  per_n = ConfigCardinality::PerExtruderVariantDual;
        else if (in(filament_options_with_variant, k))   per_n = ConfigCardinality::PerFilamentVariant;
        else if (in(print_options_with_variant, k))      per_n = ConfigCardinality::PerProcessVariant;
        else if (in(fil, k) && !in(not_per_filament, k)) per_n = ConfigCardinality::PerFilament;
        else if (in(landmines_ext, k))                   per_n = ConfigCardinality::PerExtruder;
        else if (in(landmines_fil, k))                   per_n = ConfigCardinality::PerFilament;
        const ConfigCardinality got  = cardinality_of(k);
        const ConfigCardinality want = per_n != ConfigCardinality::Scalar ? per_n : ConfigCardinality::Custom;
        INFO(k << " -> " << cardinality_name(got) << " (expected " << cardinality_name(want) << ")");
        CHECK(got == want);
    }
}

static size_t vsize(const DynamicPrintConfig &c, const char *k)
{
    return static_cast<const ConfigOptionVectorBase *>(c.option(k))->size();
}

TEST_CASE("enforce_cardinality extends short per-N vectors to their cardinality length", "[Cardinality]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({
        { "nozzle_diameter",   "0.4,0.4" },        // 2 extruders
        { "filament_diameter", "1.75,1.75,1.75" }, // 3 filaments (SEMM default)
    });
    cfg.set_key_value("printer_extruder_variant", new ConfigOptionStrings(std::vector<std::string>(2, "x")));
    cfg.set_key_value("machine_max_jerk_x", new ConfigOptionFloats(std::vector<double>{ 10.0 })); // size 1

    cfg.enforce_cardinality();

    CHECK(vsize(cfg, "filament_printable") == 3); // PerFilament -> filament count
    CHECK(vsize(cfg, "machine_max_jerk_x") == 4); // PerExtruderVariantDual -> extruder-variant count * 2
}

TEST_CASE("enforce_cardinality resizes a longer-than-cardinality vector to exact (shrinks stale tail)", "[Cardinality]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({ { "nozzle_diameter", "0.4,0.4" }, { "filament_diameter", "1.75,1.75" } });
    // Longer than the extruder count (e.g. stale entries from a 4-extruder profile).
    cfg.set_key_value("max_layer_height", new ConfigOptionFloats(std::vector<double>{ 0.3, 0.3, 0.3, 0.3 }));

    cfg.enforce_cardinality();

    CHECK(vsize(cfg, "max_layer_height") == 2); // PerExtruder -> resized to exactly the extruder count
}

TEST_CASE("enforce_cardinality keeps per-filament arrays at the filament count on non-SEMM F>E", "[Cardinality]")
{
    // filament_map drives the filament->extruder assignment; shrinking it to the extruder count
    // corrupts multi-filament slicing.
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({
        { "single_extruder_multi_material", "0" },   // non-SEMM (multi-extruder)
        { "nozzle_diameter",   "0.4,0.4" },          // 2 extruders
        { "filament_diameter", "1.75,1.75,1.75" },   // 3 filaments (F > E)
    });
    cfg.set_key_value("filament_map", new ConfigOptionInts(std::vector<int>{ 1, 2, 1 })); // size 3

    cfg.enforce_cardinality();

    CHECK(vsize(cfg, "filament_map")       == 3); // NOT truncated to the extruder count
    CHECK(vsize(cfg, "filament_printable") == 3);
}

TEST_CASE("Preset::normalize sizes plain per-filament but leaves variant options alone", "[Cardinality]")
{
    // normalize runs before variant expansion, so a variant option's count is not established yet.
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({ { "nozzle_diameter", "0.4" }, { "filament_diameter", "1.75,1.75,1.75" } });

    // nozzle_temperature is PerFilamentVariant; with no filament_extruder_variant present, normalize
    // must leave it, not shrink to the default-1 count.
    cfg.set_key_value("nozzle_temperature", new ConfigOptionInts(std::vector<int>{ 200, 200, 200 }));

    Preset::normalize(cfg);

    CHECK(vsize(cfg, "filament_printable")  == 3); // PerFilament sized to filament count
    CHECK(vsize(cfg, "nozzle_temperature")  == 3); // PerFilamentVariant left untouched by normalize
}

TEST_CASE("enforce_per_filament_cardinality sizes plain per-filament but not variant or landmine keys", "[Cardinality]")
{
    // The load-time sizer runs before variant expansion: it sizes only plain per-filament (count known)
    // and leaves variant options AND landmine PerFilament keys (filament_map etc.) for enforce_cardinality later.
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({ { "nozzle_diameter", "0.4" }, { "filament_diameter", "1.75,1.75,1.75" } }); // 3 filaments
    cfg.set_key_value("filament_printable", new ConfigOptionInts(std::vector<int>{ 1 })); // PerFilament (plain)
    cfg.set_key_value("nozzle_temperature", new ConfigOptionInts(std::vector<int>{ 200 })); // PerFilamentVariant
    cfg.set_key_value("filament_map",       new ConfigOptionInts(std::vector<int>{ 1 })); // PerFilament (landmine)

    cfg.enforce_per_filament_cardinality();

    CHECK(vsize(cfg, "filament_printable") == 3); // plain per-filament sized to the filament count
    CHECK(vsize(cfg, "nozzle_temperature") == 1); // variant option left alone (count not final at load)
    CHECK(vsize(cfg, "filament_map")       == 1); // landmine left alone at load; enforce_cardinality sizes it later
}

TEST_CASE("the cardinality gate flags an authored contradictory per-N length", "[Cardinality]")
{
    // Falsification for [ProfileCardinalityGate]: proves cardinality_contradictions() flags an authored wrong length
    // (and passes a clean config), so a green gate over the shipped profiles means something.
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({ { "nozzle_diameter", "0.4,0.4" }, { "filament_diameter", "1.75,1.75" } }); // 2 ext, 2 fil
    CHECK(cardinality_contradictions(cfg).empty()); // clean baseline: nothing authored to a wrong length

    // filament_map with 3 entries on a 2-filament printer - a genuine, authored contradiction.
    cfg.set_key_value("filament_map", new ConfigOptionInts(std::vector<int>{ 1, 2, 1 }));
    bool flagged = false;
    for (const CardinalityMismatch &m : cardinality_contradictions(cfg))
        if (m.key == "filament_map") { flagged = true; CHECK(m.got == 3); CHECK(m.want == 2); }
    CHECK(flagged);
}

TEST_CASE("enforce_cardinality keeps per-filament-variant arrays at the filament count when the variant list is sized",
          "[Cardinality]")
{
    // The corpus invariant test checks got == want after the resize, so a self-consistent shrink would pass
    // it; guard PerFilamentVariant sizing directly here (filament_extruder_variant == the filament count).
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({ { "nozzle_diameter", "0.4" } });
    cfg.set_key_value("filament_diameter",         new ConfigOptionFloats(std::vector<double>(3, 1.75)));
    cfg.set_key_value("filament_extruder_variant", new ConfigOptionStrings(std::vector<std::string>(3, "Direct Drive Standard")));
    cfg.set_key_value("nozzle_temperature",        new ConfigOptionInts(std::vector<int>{ 210, 220, 230 }));

    cfg.enforce_cardinality();

    CHECK(vsize(cfg, "nozzle_temperature") == 3); // PerFilamentVariant, not shrunk below the filament count
}

TEST_CASE("enforce_cardinality floors per-variant options to the base count when the variant list is unexpanded", "[Cardinality]")
{
    // A config can reach enforce_cardinality with PerFilamentVariant options at the filament count but
    // filament_extruder_variant still size-1 (a path that skips variant expansion). axis_counts floors
    // the variant count to the base count, so these options keep their per-filament length.
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({ { "nozzle_diameter", "0.4" }, { "filament_diameter", "1.75,1.75,1.75" } }); // 1 ext, 3 fil
    cfg.set_key_value("nozzle_temperature", new ConfigOptionInts(std::vector<int>{ 200, 210, 220 }));

    cfg.enforce_cardinality();

    CHECK(vsize(cfg, "nozzle_temperature") == 3); // floored to the filament count, not shrunk to the variant-list size 1
}

TEST_CASE("normalize_fdm_2 resizes no per-N vector", "[Cardinality]")
{
    // In Print::apply, normalize_fdm_2 runs just before enforce_cardinality and only flips scalar flags; it must
    // never resize a per-N vector, so per-N sizing stays owned entirely by enforce_cardinality. Enforce first here
    // only to pin known lengths, then confirm normalize_fdm_2 leaves them unchanged. ByObject + >1 object drives
    // its flag-flipping branch.
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({ { "nozzle_diameter", "0.4,0.4" }, { "filament_diameter", "1.75,1.75" } });
    cfg.set_key_value("print_sequence",     new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));
    cfg.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    cfg.enforce_cardinality();

    std::map<std::string, size_t> before;
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *o = cfg.option(key); o != nullptr && o->is_vector())
            before[key] = static_cast<const ConfigOptionVectorBase *>(o)->size();

    cfg.normalize_fdm_2(/*num_objects=*/2, /*used_filaments=*/2);
    REQUIRE(cfg.option<ConfigOptionBool>("enable_prime_tower")->value == false); // the branch actually ran

    for (const auto &[key, len] : before) {
        CAPTURE(key);
        CHECK(vsize(cfg, key.c_str()) == len);
    }
}

TEST_CASE("cardinality_stride is 2 only for the dual machine-limit pair", "[Cardinality]")
{
    CHECK(cardinality_stride(ConfigCardinality::PerExtruderVariantDual) == 2);
    for (ConfigCardinality c : { ConfigCardinality::Scalar,             ConfigCardinality::PerExtruder,
                                 ConfigCardinality::PerFilament,        ConfigCardinality::PerExtruderVariant,
                                 ConfigCardinality::PerFilamentVariant, ConfigCardinality::PerProcessVariant,
                                 ConfigCardinality::Custom }) {
        CAPTURE(static_cast<int>(c));
        CHECK(cardinality_stride(c) == 1);
    }
}

TEST_CASE("resize_to_cardinality sizes a single option through the public per-key entry point", "[Cardinality]")
{
    // The per-key sizer convergence callers use: same rule as enforce_cardinality, one option at a time.
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({ { "nozzle_diameter", "0.4,0.4" }, { "filament_diameter", "1.75,1.75" } });
    cfg.set_key_value("max_layer_height",   new ConfigOptionFloats(std::vector<double>{ 0.3 }));      // short: 1
    cfg.set_key_value("filament_printable", new ConfigOptionInts(std::vector<int>{ 1, 1, 1, 1 }));    // long: 4

    const AxisCounts n = axis_counts(cfg);
    resize_to_cardinality(cfg, "max_layer_height",   n); // PerExtruder -> extruder count
    resize_to_cardinality(cfg, "filament_printable", n); // PerFilament -> filament count

    CHECK(vsize(cfg, "max_layer_height")   == 2);
    CHECK(vsize(cfg, "filament_printable") == 2);
}

// ============================================================================
// 2. [ProfileCorpus]: resolve shipped profiles through full_config() and dump a
//    deterministic sorted config. Diff the dump on main vs branch (see the file
//    header of profile_corpus.hpp). Hidden ([.]); ORCA_CORPUS_* env overrides.
// ============================================================================

TEST_CASE("Profile corpus resolves identically before/after", "[.][ProfileCorpus]")
{
    REQUIRE(CorpusTest::repo_root().string().size() > 0);

    const std::vector<std::string> vendors = CorpusTest::vendors_from_env();

    // Default to a portable, auto-cleaned temp path; ORCA_CORPUS_OUT overrides it to persist the dump for a diff.
    ScopedTemporaryFile default_out(".txt");
    std::string out_path = default_out.string();
    if (const char *p = std::getenv("ORCA_CORPUS_OUT"))
        out_path = p;
    std::ofstream out(out_path, std::ios::binary);
    REQUIRE(out.is_open());

    auto [resolved, failed] = CorpusTest::for_each_resolved_config(
        vendors, [&](const CorpusTest::Combo &combo, DynamicPrintConfig &cfg) {
            out << "### VENDOR: " << combo.vendor << " | PRINTER: " << combo.printer
                << " | NOZZLES: " << combo.nozzles << " | FILAMENTS: " << combo.filaments
                << " | FILAMENT_PRESET: " << combo.filament
                << " | PROCESS: " << combo.process << "\n"
                << dump_config(cfg) << "\n" << std::flush;   // flush: pinpoint a baseline abort
        });

    INFO("resolved=" << resolved << " failed=" << failed << " out=" << out_path);
    CHECK(resolved > 0);
    CHECK(failed == 0);
}

// ============================================================================
// 3. [ProfileCardinalityInvariant]: for every resolved config, enforce_cardinality() then
//    assert every per-N option's length == expected_size(cardinality, counts). Branch-only
//    (calls the cardinality API). Hidden ([.]).
// ============================================================================

TEST_CASE("Every resolved profile satisfies the cardinality length invariant after enforce_cardinality",
          "[.][ProfileCardinalityInvariant]")
{
    const std::vector<std::string> vendors = CorpusTest::vendors_from_env();

    std::vector<CorpusViolation> violations;

    auto [resolved, failed] = CorpusTest::for_each_resolved_config(
        vendors, [&](const CorpusTest::Combo &combo, DynamicPrintConfig &cfg) {
            cfg.enforce_cardinality();                          // the after-state under test
            const AxisCounts n = axis_counts(cfg);
            for (const std::string &key : cfg.keys()) {
                const size_t want = expected_size(cardinality_of(key), n);
                if (want == 0)                            // Scalar / Custom: not per-N
                    continue;
                const ConfigOption *opt = cfg.option(key);
                if (! opt || ! opt->is_vector())
                    continue;
                const size_t got = static_cast<const ConfigOptionVectorBase *>(opt)->size();
                if (got == want)
                    continue;
                if (got == 0) {                           // mirror enforce_cardinality's empty-skip
                    const ConfigOption *def = FullPrintConfig::defaults().option(key);
                    const bool default_empty = def == nullptr || ! def->is_vector()
                        || static_cast<const ConfigOptionVectorBase *>(def)->empty();
                    if (default_empty)
                        continue;
                }
                violations.push_back({ combo.vendor, combo.printer, key, got, want });
            }
        });

    INFO(violation_report("violations", resolved, failed, violations));

    CHECK(resolved > 0);
    CHECK(failed == 0);
    CHECK(violations.empty());
}

// ============================================================================
// 4. [ProfileCardinalityGate]: like the invariant but WITHOUT enforce_cardinality - no shipped
//    profile should author a per-N length that contradicts its axis count. Hidden
//    ([.], ~12 s); meant for the profile-validation CI leg on resources/profiles.
// ============================================================================

TEST_CASE("No shipped profile sets a per-N option to a contradictory length", "[.][ProfileCardinalityGate]")
{
    const std::vector<std::string> vendors = CorpusTest::vendors_from_env();

    std::vector<CorpusViolation> violations;

    auto [resolved, failed] = CorpusTest::for_each_resolved_config(
        vendors, [&](const CorpusTest::Combo &combo, DynamicPrintConfig &cfg) {
            // No enforce_cardinality() here: check the profile as resolved, before the slice-boundary coercion.
            for (const CardinalityMismatch &m : cardinality_contradictions(cfg))
                violations.push_back({ combo.vendor, combo.printer, m.key, m.got, m.want });
        });

    INFO(violation_report("contradictions", resolved, failed, violations));

    CHECK(resolved > 0);
    CHECK(failed == 0);
    CHECK(violations.empty());
}
