#include <catch2/catch_all.hpp>

#include <functional>
#include <set>

#include <nlohmann/json.hpp>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/DiagnosticsAllowlist.hpp"
#include "slic3r/Utils/PrivacyChoiceStats.hpp"
#include "slic3r/Utils/PrivacyConsent.hpp"
#include "slic3r/Utils/UsageTelemetry.hpp"

using namespace Slic3r;
using namespace Slic3r::PrivacyConsent;
using json = nlohmann::json;

namespace {

const LegalApprovals SHIPPED{};                         // every approval off (the tests' baseline)
const LegalApprovals US_APPROVED{true, false};          // counsel approved the US preselection
const char *const    WHEN = "2026-10-13T12:00:00Z";

Record choice(const std::string &region, const std::string &country, bool share, bool kept = false,
              const LegalApprovals &approvals = SHIPPED)
{
    return make_choice(region, country, share, kept, "initial_setup", WHEN, approvals);
}

} // namespace

TEST_CASE("Privacy: the shipped build's legal approvals", "[Privacy]")
{
    // The US preselection is on (2026-10-14); statistics are still off.
    CHECK(legal_approvals().us_preselect_share);
    CHECK_FALSE(legal_approvals().privacy_choice_stats);
}

TEST_CASE("Privacy: the Slicing improvement program is its own, explicit permission", "[Privacy]")
{
    const Record member = make_choice("north_america", "CA", true, false, "initial_setup", WHEN, SHIPPED, true);
    CHECK(member.improvement_program);
    CHECK(program_permitted(member, false, SHIPPED));
    // Round-trips, and only ever with sharing.
    std::optional<Record> back = parse(serialize(member));
    REQUIRE(back);
    CHECK(back->improvement_program);
    CHECK_FALSE(make_choice("north_america", "CA", false, false, "initial_setup", WHEN, SHIPPED, true).improvement_program);
    json j = json::parse(serialize(member));
    j["diagnostics_preference"] = "disabled";
    CHECK_FALSE(parse(j.dump()).has_value()); // a "no" carrying the program is not a record
    j = json::parse(serialize(member));
    j["improvement_program"] = "true";
    CHECK_FALSE(parse(j.dump()).has_value());

    // Sharing alone is not the program; a kept US preselection never implies it either.
    CHECK_FALSE(program_permitted(choice("north_america", "CA", true), false, SHIPPED));
    const Record us_kept = choice("north_america", "US", true, /*kept*/ true, US_APPROVED);
    CHECK(us_kept.selection_method == "preselected_confirmed");
    CHECK(diagnostics_permitted(us_kept, false, US_APPROVED));
    CHECK_FALSE(program_permitted(us_kept, false, US_APPROVED));

    // The organisation switch and an older notice end it with diagnostics.
    CHECK_FALSE(program_permitted(member, true, SHIPPED));
    Record old = member;
    old.notice_version = "2026-10-v2";
    CHECK_FALSE(program_permitted(old, false, SHIPPED));
}

TEST_CASE("Privacy: records from before the program still read, and a decline stays a decline", "[Privacy]")
{
    // A 2026-10-v2 record has no "improvement_program" field.
    json v2 = json::parse(serialize(choice("north_america", "CA", false)));
    v2.erase("improvement_program");
    v2["notice_version"] = v2["policy_version"] = "2026-10-v2";
    std::optional<Record> no = parse(v2.dump());
    REQUIRE(no);
    CHECK_FALSE(no->improvement_program);
    CHECK(status(no, SHIPPED) == Status::Declined);

    json v2_yes = json::parse(serialize(choice("north_america", "CA", true)));
    v2_yes.erase("improvement_program");
    v2_yes["notice_version"] = v2_yes["policy_version"] = "2026-10-v2";
    std::optional<Record> yes = parse(v2_yes.dump());
    REQUIRE(yes);
    CHECK(status(yes, SHIPPED) == Status::NeedsNewChoice); // the new wording is asked again
}

TEST_CASE("Privacy: regional rules for every North America answer and the rest", "[Privacy]")
{
    SECTION("United States: nothing preselected until counsel approves")
    {
        RegionPolicy p = policy_for("north_america", "US", SHIPPED);
        CHECK(p.jurisdiction == Jurisdiction::UnitedStates);
        CHECK_FALSE(p.preselect_share);
        CHECK(p.page_version == "privacy-us-v1");
        CHECK(p.consent_ui_version == "us-v1");
        CHECK(policy_for("north_america", "US", US_APPROVED).preselect_share);
    }
    SECTION("Canada and Other North America: affirmative choice, even with the US approval")
    {
        CHECK(policy_for("north_america", "CA", US_APPROVED).jurisdiction == Jurisdiction::Canada);
        CHECK_FALSE(policy_for("north_america", "CA", US_APPROVED).preselect_share);
        for (const Country &c : other_north_america()) {
            INFO(c.code);
            CHECK_FALSE(policy_for("north_america", c.code, US_APPROVED).preselect_share);
        }
        CHECK(policy_for("north_america", "MX", SHIPPED).jurisdiction == Jurisdiction::OtherNorthAmerica);
        // The French Caribbean is EU territory.
        CHECK(policy_for("north_america", "GP", SHIPPED).jurisdiction == Jurisdiction::EuEea);
    }
    SECTION("Europe, the UK, Switzerland and unknown places: affirmative choice")
    {
        CHECK(policy_for("europe", "", US_APPROVED).jurisdiction == Jurisdiction::EuEea);
        CHECK(policy_for("europe", "GB", US_APPROVED).jurisdiction == Jurisdiction::UnitedKingdom);
        CHECK(policy_for("europe", "CH", US_APPROVED).jurisdiction == Jurisdiction::Switzerland);
        CHECK(policy_for("asia_pacific", "", US_APPROVED).jurisdiction == Jurisdiction::Unknown);
        CHECK(policy_for("", "", US_APPROVED).jurisdiction == Jurisdiction::Unknown);
        CHECK(policy_for("north_america", "", US_APPROVED).jurisdiction == Jurisdiction::OtherNorthAmerica);
        CHECK(policy_for("north_america", "ZZ", US_APPROVED).jurisdiction == Jurisdiction::Unknown);
        for (const char *region : {"europe", "asia_pacific", "china", "other", ""})
            CHECK_FALSE(policy_for(region, "", US_APPROVED).preselect_share);
    }
    SECTION("country lists")
    {
        CHECK(is_north_america_country("US"));
        CHECK(is_north_america_country("CA"));
        CHECK(is_north_america_country("MX"));
        CHECK(is_north_america_country("JM"));
        CHECK_FALSE(is_north_america_country("GB"));
        CHECK_FALSE(is_north_america_country("us"));
        CHECK_FALSE(is_north_america_country(""));
        CHECK(region_code("North America") == "north_america");
        CHECK(region_code("Others") == "other");
        CHECK(region_code("Atlantis").empty());
    }
}

TEST_CASE("Privacy: what counts as consent", "[Privacy]")
{
    CHECK(status(std::nullopt, SHIPPED) == Status::NotAsked);
    CHECK_FALSE(diagnostics_permitted(std::nullopt, false, SHIPPED));

    // An explicit yes counts everywhere; an explicit no is always a no.
    for (const char *country : {"US", "CA", "MX"}) {
        INFO(country);
        CHECK(status(choice("north_america", country, true), SHIPPED) == Status::Granted);
        CHECK(status(choice("north_america", country, false), SHIPPED) == Status::Declined);
    }
    CHECK(status(choice("europe", "", true), SHIPPED) == Status::Granted);

    // Organisation opt-out beats a yes.
    CHECK(diagnostics_permitted(choice("north_america", "CA", true), false, SHIPPED));
    CHECK_FALSE(diagnostics_permitted(choice("north_america", "CA", true), true, SHIPPED));
}

TEST_CASE("Privacy: a preselected answer is recorded as such and never passes for an explicit one", "[Privacy]")
{
    SECTION("with the US approval off there is nothing to confirm: keeping means choosing")
    {
        Record r = choice("north_america", "US", true, /*kept*/ true, SHIPPED);
        CHECK(r.selection_method == "explicit");
    }
    SECTION("with the US approval on, keeping the preselection is preselected_confirmed")
    {
        Record r = choice("north_america", "US", true, true, US_APPROVED);
        CHECK(r.selection_method == "preselected_confirmed");
        CHECK(status(r, US_APPROVED) == Status::Granted);
        // Approval withdrawn later: no longer counts, ask again.
        CHECK(status(r, SHIPPED) == Status::NeedsNewChoice);
        // Moved to Canada: a US-style answer is not Canadian consent.
        Record moved = r;
        moved.country = "CA";
        CHECK(status(moved, US_APPROVED) == Status::NeedsNewChoice);
        // Changing it is explicit.
        CHECK(choice("north_america", "US", true, false, US_APPROVED).selection_method == "explicit");
        CHECK(choice("north_america", "US", false, true, US_APPROVED).selection_method == "explicit");
    }
    SECTION("Canada never gets a preselection to confirm")
    {
        CHECK(choice("north_america", "CA", true, true, US_APPROVED).selection_method == "explicit");
    }
}

TEST_CASE("Privacy: records round-trip and anything odd fails closed", "[Privacy]")
{
    const Record r = choice("north_america", "CA", true);
    std::optional<Record> back = parse(serialize(r));
    REQUIRE(back);
    CHECK(back->country == "CA");
    CHECK(back->policy_version == POLICY_VERSION);
    CHECK(back->notice_version == NOTICE_VERSION);
    CHECK(back->consent_ui_version == "ca-v1");
    CHECK(back->choice_timestamp == WHEN);
    CHECK_FALSE(back->model_upload_permission);

    json j = json::parse(serialize(r));
    auto broken = [&](const std::function<void(json &)> &change) {
        json copy = j;
        change(copy);
        return !parse(copy.dump()).has_value();
    };
    CHECK(!parse("").has_value());
    CHECK(!parse("{not json").has_value());
    CHECK(!parse("[]").has_value());
    CHECK(broken([](json &x) { x["extra"] = "field"; }));
    CHECK(broken([](json &x) { x.erase("country"); }));
    CHECK(broken([](json &x) { x["diagnostics_preference"] = "maybe"; }));
    CHECK(broken([](json &x) { x["selection_method"] = "implied"; }));
    CHECK(broken([](json &x) { x["country"] = "GB"; }));       // not North America
    CHECK(broken([](json &x) { x["country"] = "ZZ"; }));       // not a North America country
    CHECK(!broken([](json &x) { x["country"] = ""; }));        // Central America & Caribbean
    CHECK(broken([](json &x) { x["region"] = "mars"; }));
    CHECK(broken([](json &x) { x["choice_timestamp"] = ""; })); // a choice made now has a time
    CHECK(broken([](json &x) { x["choice_timestamp"] = "yesterday"; }));
    CHECK(broken([](json &x) { x["model_upload_permission"] = true; }));
    CHECK(broken([](json &x) { x["model_upload_permission"] = "false"; }));
    CHECK(broken([](json &x) { x["diagnostics_preference"] = 1; }));

    // A newer notice: a yes must be asked again, a no stays a no.
    Record old_yes = r, old_no = choice("north_america", "CA", false);
    old_yes.notice_version = old_no.notice_version = "2020-01-v0";
    CHECK(status(old_yes, SHIPPED) == Status::NeedsNewChoice);
    CHECK(status(old_no, SHIPPED) == Status::Declined);

    // A decline needs no region (switching off in Preferences before ever choosing one).
    CHECK(parse(serialize(make_choice("", "", false, false, "preferences", WHEN, SHIPPED))).has_value());
    CHECK(!parse(serialize(make_choice("", "", true, false, "preferences", WHEN, SHIPPED))).has_value());
}

TEST_CASE("Privacy: the config record, corrupt config and pre-0.1.6 configs", "[Privacy]")
{
    SECTION("saving writes the mirror; a corrupt record is never asked = off")
    {
        AppConfig config;
        save(config, choice("north_america", "US", true));
        CHECK(config.get_bool("usage_diagnostics"));
        CHECK(status(load(config), SHIPPED) == Status::Granted);
        config.set(CONFIG_KEY, "{\"region\":");
        CHECK(!load(config).has_value());
        CHECK_FALSE(diagnostics_permitted(load(config), false, SHIPPED));
        save(config, choice("north_america", "US", false));
        CHECK_FALSE(config.get_bool("usage_diagnostics"));
    }
    SECTION("an old explicit off survives the update as a decline, once")
    {
        AppConfig config;
        config.set_bool("usage_diagnostics", false);
        config.set("region", "Europe");
        CHECK(migrate_legacy_setting(config));
        std::optional<Record> r = load(config);
        REQUIRE(r);
        CHECK(status(r, SHIPPED) == Status::Declined);
        CHECK(r->selection_method == "legacy_setting");
        CHECK(r->choice_timestamp.empty()); // never invented
        CHECK(r->region == "europe");
        CHECK_FALSE(migrate_legacy_setting(config));
    }
    SECTION("the old default on is not consent, and does not turn into a decline on the next launch")
    {
        AppConfig config;
        config.set_bool("usage_diagnostics", true);
        CHECK_FALSE(migrate_legacy_setting(config));
        CHECK(status(load(config), SHIPPED) == Status::NotAsked);
        CHECK_FALSE(config.get_bool("usage_diagnostics"));
        CHECK_FALSE(migrate_legacy_setting(config)); // the mirror "false" is not an old opt-out
        CHECK(status(load(config), SHIPPED) == Status::NotAsked);
    }
    SECTION("a fresh install is asked")
    {
        AppConfig config;
        CHECK_FALSE(migrate_legacy_setting(config));
        CHECK(status(load(config), SHIPPED) == Status::NotAsked);
    }
}

TEST_CASE("Privacy: each decision is reported once on the update check, and nothing else is", "[Privacy]")
{
    SECTION("a first answer, the same answer again, then a change")
    {
        AppConfig config;
        CHECK(config.get(CHOICE_REPORT_KEY).empty());
        save(config, choice("europe", "", false));
        CHECK(config.get(CHOICE_REPORT_KEY) == "private");
        config.set(CHOICE_REPORT_KEY, ""); // the update check delivered it
        save(config, choice("europe", "", false));
        CHECK(config.get(CHOICE_REPORT_KEY).empty());
        save(config, choice("europe", "", true));
        CHECK(config.get(CHOICE_REPORT_KEY) == "shared");
    }
    SECTION("an answer to new notice wording is a decision, even when it is the same answer")
    {
        AppConfig config;
        Record old = choice("europe", "", false);
        old.notice_version = "2026-10-v2";
        save(config, old);
        config.set(CHOICE_REPORT_KEY, "");
        save(config, choice("europe", "", false));
        CHECK(config.get(CHOICE_REPORT_KEY) == "private");
    }
    SECTION("a pre-0.1.6 opt-out carried over is not a decision made now")
    {
        AppConfig config;
        config.set_bool("usage_diagnostics", false);
        CHECK(migrate_legacy_setting(config));
        CHECK(config.get(CHOICE_REPORT_KEY).empty());
    }
    SECTION("the report holds only the choice")
    {
        AppConfig config;
        save(config, choice("north_america", "US", true, true, US_APPROVED));
        CHECK(config.get(CHOICE_REPORT_KEY) == "shared");
    }
}

TEST_CASE("Privacy: a decision made before choices were reported is reported once", "[Privacy]")
{
    // What a 0.1.6 decision looks like to the next version: a record, no report pending.
    auto decided_in_016 = [](bool share) {
        AppConfig config;
        save(config, choice("europe", "", share));
        config.set(CHOICE_REPORT_KEY, "");
        config.set(EARLIER_CHOICE_REPORTED_KEY, "");
        return config;
    };
    SECTION("keep my data private, once")
    {
        AppConfig config = decided_in_016(false);
        CHECK(queue_earlier_choice_report(config));
        CHECK(config.get(CHOICE_REPORT_KEY) == "private");
        config.set(CHOICE_REPORT_KEY, ""); // the update check delivered it
        CHECK_FALSE(queue_earlier_choice_report(config));
        CHECK(config.get(CHOICE_REPORT_KEY).empty());
    }
    SECTION("share")
    {
        AppConfig config = decided_in_016(true);
        CHECK(queue_earlier_choice_report(config));
        CHECK(config.get(CHOICE_REPORT_KEY) == "shared");
    }
    SECTION("no decision yet: nothing now, and save() reports it when it is made")
    {
        AppConfig config;
        CHECK_FALSE(queue_earlier_choice_report(config));
        CHECK(config.get(CHOICE_REPORT_KEY).empty());
        save(config, choice("europe", "", false));
        CHECK(config.get(CHOICE_REPORT_KEY) == "private");
        config.set(CHOICE_REPORT_KEY, "");
        CHECK_FALSE(queue_earlier_choice_report(config)); // already ran: no second report
    }
    SECTION("a carried-over pre-0.1.6 opt-out is not reported")
    {
        AppConfig config;
        config.set_bool("usage_diagnostics", false);
        CHECK(migrate_legacy_setting(config));
        CHECK_FALSE(queue_earlier_choice_report(config));
        CHECK(config.get(CHOICE_REPORT_KEY).empty());
    }
    SECTION("a report already pending is left as it is")
    {
        AppConfig config;
        save(config, choice("europe", "", true));
        config.set(EARLIER_CHOICE_REPORTED_KEY, "");
        CHECK_FALSE(queue_earlier_choice_report(config));
        CHECK(config.get(CHOICE_REPORT_KEY) == "shared");
    }
}

TEST_CASE("Privacy: model uploads need their own answer, for one file, once", "[Privacy]")
{
    ModelUploadRequest q;
    q.file_label    = "bracket.3mf";
    q.file_id       = "abc123";
    q.included_data = "The project as a 3MF file";
    q.purpose       = "Troubleshooting";
    q.recipients    = "Unlayered 3D, Inc.";
    q.retention     = "90 days";

    CHECK(request_complete(q));
    ModelUploadRequest missing = q;
    missing.retention.clear();
    CHECK_FALSE(request_complete(missing));
    CHECK_FALSE(ModelUploadAuthorization::granted(missing, false).consume("abc123"));

    // Nothing by default, and usage diagnostics consent never makes one.
    ModelUploadAuthorization none;
    CHECK_FALSE(none.consume("abc123"));
    CHECK_FALSE(choice("north_america", "US", true).model_upload_permission);

    ModelUploadAuthorization yes = ModelUploadAuthorization::granted(q, /*ai*/ true);
    CHECK_FALSE(yes.ai_training_allowed()); // training was not proposed, so a tick cannot allow it
    CHECK_FALSE(yes.consume("other-file"));
    CHECK(yes.consume("abc123"));
    CHECK_FALSE(yes.consume("abc123")); // single use: the next model is asked again

    q.ai_training_proposed = true;
    CHECK(ModelUploadAuthorization::granted(q, true).ai_training_allowed());
    CHECK_FALSE(ModelUploadAuthorization::granted(q, false).ai_training_allowed());
}

TEST_CASE("Privacy-choice statistics: strict schema", "[Privacy]")
{
    PrivacyChoiceStats::Event e{"privacy_choice_submitted", "north_america", "CA", "privacy-na-v1", NOTICE_VERSION,
                                "declined", "initial_setup", "explicit"};
    std::optional<json> body = PrivacyChoiceStats::to_body(e);
    REQUIRE(body);
    CHECK(body->size() == 8);
    CHECK((*body)["country"] == "CA");
    CHECK((*body)["page_version"] == "privacy-na-v1");
    CHECK((*body)["notice_version"] == NOTICE_VERSION);

    json extra = *body;
    extra["install"] = "a1b2";
    CHECK_FALSE(PrivacyChoiceStats::valid_body(extra)); // no identifiers, no unknown fields
    json slicer = *body;
    slicer["printer"] = "X1C";
    CHECK_FALSE(PrivacyChoiceStats::valid_body(slicer));

    PrivacyChoiceStats::Event shown = e;
    shown.event_type = "privacy_page_shown";
    shown.choice.clear();
    shown.selection_method.clear();
    REQUIRE(PrivacyChoiceStats::to_body(shown));
    CHECK_FALSE(PrivacyChoiceStats::to_body(shown)->contains("choice"));

    PrivacyChoiceStats::Event bad = e;
    bad.choice = "maybe";
    CHECK_FALSE(PrivacyChoiceStats::to_body(bad));
    bad = e;
    bad.event_type = "privacy_page_clicked";
    CHECK_FALSE(PrivacyChoiceStats::to_body(bad));
    bad = e;
    bad.country = "GB"; // not a North America country
    CHECK_FALSE(PrivacyChoiceStats::to_body(bad));
    bad = e;
    bad.selection_method = "preselected_confirmed"; // a decline is never a kept preselection
    CHECK_FALSE(PrivacyChoiceStats::to_body(bad));
}

TEST_CASE("Privacy-choice statistics: off without approval and a backend", "[Privacy]")
{
    CHECK_FALSE(PrivacyChoiceStats::transmission_allowed(legal_approvals().privacy_choice_stats, PrivacyChoiceStats::ENDPOINT, false));
    CHECK_FALSE(PrivacyChoiceStats::transmission_allowed(true, "", false));
    CHECK_FALSE(PrivacyChoiceStats::transmission_allowed(false, "https://example.invalid/x", false));
    CHECK_FALSE(PrivacyChoiceStats::transmission_allowed(true, "https://example.invalid/x", true));
    CHECK(PrivacyChoiceStats::transmission_allowed(true, "https://example.invalid/x", false));
    // Disabled: submitting returns at once and never throws, so setup never waits on it.
    PrivacyChoiceStats::submit(json{{"event_type", "privacy_page_shown"}});
}

TEST_CASE("Privacy-choice statistics: one impression and one decision per flow", "[Privacy]")
{
    std::vector<json> sent;
    PrivacyChoiceStats::Event shown{"privacy_page_shown", "north_america", "US", "privacy-na-v1", NOTICE_VERSION, "", "initial_setup", ""};
    PrivacyChoiceStats::Event accepted{"privacy_choice_submitted", "north_america", "US", "privacy-na-v1", NOTICE_VERSION,
                                       "accepted", "initial_setup", "explicit"};
    PrivacyChoiceStats::Event declined = accepted;
    declined.choice = "declined";
    {
        PrivacyChoiceStats::FlowCounter flow([&sent](const json &b) { sent.push_back(b); });
        flow.page_shown(shown);
        flow.page_shown(shown); // re-render
        flow.page_shown(shown); // back, then forward again
        flow.choice_made(accepted);
        flow.choice_made(declined); // changed their mind before leaving: the last answer stands
        flow.finish();
        flow.finish(); // a retry
    }
    REQUIRE(sent.size() == 2);
    CHECK(sent[0]["event_type"] == "privacy_page_shown");
    CHECK(sent[1]["choice"] == "declined");
    for (const json &b : sent)
        CHECK(PrivacyChoiceStats::valid_body(b));

    // A flow that ends without a choice reports only the impression.
    sent.clear();
    {
        PrivacyChoiceStats::FlowCounter flow([&sent](const json &b) { sent.push_back(b); });
        flow.page_shown(shown);
        flow.finish();
    }
    CHECK(sent.size() == 1);
}

TEST_CASE("Diagnostics allowlist: events, properties and values", "[Privacy]")
{
    using namespace DiagnosticsAllowlist;
    json props, counts;

    CHECK_FALSE(filter("model_geometry", {{"mesh", "..."}}, json(), props, counts));
    CHECK(props.empty());

    REQUIRE(filter("slice",
                   {{"result", "ok"},
                    {"objects", 3},
                    {"project_name", "My secret bracket"},
                    {"filaments", {"Generic PLA @BBL X1C", "C:\\Users\\alex\\PLA.json"}},
                    {"settings", {"layer_height=0.2", "o0.infill_mesh_path=/home/jane/mesh.stl", "post_process=C:/x.py"}},
                    {"error", "Cannot open C:\\Users\\Jane Smith\\Documents\\bracket.stl"},
                    {"parts", {"x=40;y=20;z=10;vol=2700;area=1700;ovh=5;base=20;inst=1"}}},
                   {{"parts:ironing", 2}, {"file:bracket.stl", 1}, {"tab:preview", -1}}, props, counts));
    CHECK(props["result"] == "ok");
    CHECK(props["objects"] == 3);
    CHECK_FALSE(props.contains("project_name"));
    CHECK(props["filaments"] == json::array({"Generic PLA @BBL X1C"}));
    CHECK(props["settings"] == json::array({"layer_height=0.2"}));
    CHECK_FALSE(props.contains("error"));
    CHECK(props["parts"].size() == 1);
    CHECK(counts == json{{"parts:ironing", 2}});

    // Values
    CHECK(safe_string("0.1.6"));
    CHECK(safe_string("UnlayeredSlicer.dll+0x1a2b3c"));
    CHECK(safe_string("thumbnails=96x96/PNG"));
    CHECK_FALSE(safe_string("C:/Users/jane"));
    CHECK_FALSE(safe_string("\\\\server\\share"));
    CHECK_FALSE(safe_string("/home/jane/x"));
    CHECK_FALSE(safe_string("~/x"));
    CHECK_FALSE(safe_string("key=/Users/jane/x"));
    CHECK_FALSE(safe_string("bracket.3mf"));
    CHECK_FALSE(safe_string("Benchy.STL"));
    CHECK_FALSE(safe_string("part.gcode"));
    CHECK_FALSE(safe_string("jane@example.com"));
    CHECK_FALSE(safe_string("two\nlines"));
    CHECK_FALSE(safe_string(std::string(500, 'a')));
    CHECK_FALSE(safe_string("https://example.com/a"));

    CHECK(allowed_count_key("gizmo:cut"));
    CHECK_FALSE(allowed_count_key("gizmo:"));
    CHECK_FALSE(allowed_count_key("project:bracket"));

    // A crash location with its caller chain survives (it used to be dropped for being over 120).
    const std::string chain = "UnlayeredSlicer.dll+0x4862937>UnlayeredSlicer.dll+0x3ec547c>UnlayeredSlicer.dll+0x3f8e3cc"
                              ">UnlayeredSlicer.dll+0x189b228>wxmsw32u_core_vc.dll+0x12345>USER32.dll+0x1a2b3";
    REQUIRE(chain.size() > 120);
    REQUIRE(chain.size() <= 190);
    REQUIRE(filter("crash", {{"version", "0.1.7"}, {"exception", "0xC0000005"}, {"at", chain}}, json(), props, counts));
    CHECK(props["at"] == chain);
    // Other properties keep the 120 limit.
    REQUIRE(filter("crash", {{"version", "0.1.7"}, {"phase", std::string(150, 'a')}}, json(), props, counts));
    CHECK_FALSE(props.contains("phase"));
}

TEST_CASE("Usage diagnostics redact paths with spaces and bare model file names", "[Privacy]")
{
    using UsageTelemetry::redact_message;
    CHECK(redact_message("Cannot open C:\\Users\\Jane Smith\\Documents\\bracket.stl", {}) == "Cannot open <path>");
    CHECK(redact_message("Cannot open /home/jane smith/parts/bracket.stl", {}) == "Cannot open <path>");
    CHECK(redact_message("Failed to load bracket_v2.3mf", {}) == "Failed to load <file>");
    CHECK(redact_message("Layer height 0.2 is too small", {}) == "Layer height 0.2 is too small");
    CHECK(redact_message("The configured post-processing script does not exist: ~/Clouds/MEGA/Models/ArcWelder", {}) ==
          "The configured post-processing script does not exist: <path>");
    CHECK(redact_message("Script ~\\tools\\arc failed", {}) == "Script <path> failed");
    CHECK(redact_message("Script ./tools/arc.py failed", {}) == "Script <path> failed");
    CHECK(redact_message("Script ../arc failed", {}) == "Script <path> failed");
    CHECK(redact_message("Missing Clouds/MEGA/ArcWelder here", {}) == "Missing <path> here");
    CHECK(redact_message("A/B tilt limit", {}) == "A/B tilt limit");
}

TEST_CASE("Privacy: one click on the region page names the place and its rules", "[Privacy]")
{
    REQUIRE(places().size() == 10);
    std::set<std::string> ids;
    for (const Place &p : places()) {
        INFO(p.id);
        CHECK(ids.insert(p.id).second);
        CHECK(place(p.id) == &p);
        CHECK(place_of(p.region, p.country) == &p);
        CHECK(is_region_code(p.region));
        CHECK(!std::string(p.label).empty());
        CHECK(region_code(p.guide_region) == p.region);
        // Every place's choice is a valid record, and its page version passes the statistics schema.
        const Record r = make_choice(p.region, p.country, true, false, "initial_setup", WHEN, SHIPPED);
        CHECK(parse(serialize(r)).has_value());
        CHECK(status(r, SHIPPED) == Status::Granted);
        PrivacyChoiceStats::Event e{"privacy_page_shown", p.region, p.country, policy_for(p.region, p.country, SHIPPED).page_version,
                                    NOTICE_VERSION, "", "initial_setup", ""};
        CHECK(PrivacyChoiceStats::to_body(e).has_value());
        // Only the United States could ever start with "share" selected, and only with approval.
        CHECK_FALSE(policy_for(p.region, p.country, SHIPPED).preselect_share);
        CHECK(policy_for(p.region, p.country, US_APPROVED).preselect_share == (std::string(p.id) == "us"));
    }
    CHECK(policy_for("north_america", "MX", SHIPPED).jurisdiction == Jurisdiction::OtherNorthAmerica);
    CHECK(policy_for("europe", "", SHIPPED).jurisdiction == Jurisdiction::EuEea);
    CHECK(policy_for("europe", "GB", SHIPPED).jurisdiction == Jurisdiction::UnitedKingdom);
    CHECK(policy_for("europe", "CH", SHIPPED).jurisdiction == Jurisdiction::Switzerland);
    CHECK(place("atlantis") == nullptr);
}
