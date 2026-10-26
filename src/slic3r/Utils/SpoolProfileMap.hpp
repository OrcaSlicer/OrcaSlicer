#pragma once

// Spool -> filament profile map.
//
// A printer that can report what is loaded in each of its lanes publishes that report here
// (SpoolProfileMap::get().publish). When the user hits "sync", the sidebar offers one filament
// profile per reported spool and remembers the choice, so the next sync is silent.
//
// The whole feature is inert until something publishes: with nothing published the slicer takes
// exactly the path it took before. Grep for "SpoolProfileMap" to find every call site.

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// What the printer says sits in one lane. Everything except slot_index is optional and free
// form -- whatever the printer reports is shown to the user as it arrived.
struct SpoolIdentity
{
    int         slot_index   = 0;
    bool        has_filament = false;
    std::string vendor;     // "Elegoo"
    std::string material;   // "PLA", "PLA-CF", "PETG HF" ...
    std::string name;       // commercial name, when the printer knows it
    std::string color;      // "#RRGGBB" of the first colour, empty when unknown
    // Every colour the spool has, "#RRGGBB" or "#RRGGBBAA" when the printer sent an alpha. More
    // than one means a multicolour spool.
    std::vector<std::string> colors;
    std::string uid;        // spool/tag id, when the printer has one
    std::string filament_id; // the profile id the printer says this spool matches, when it says one
    // Anything else the printer sent for this lane, in the order it arrived. Displayed as is,
    // never parsed, so a printer can add fields without a slicer change.
    std::vector<std::pair<std::string, std::string>> extra;
};

// What a printer says about one lane, as it said it: key and value, in the order they arrived.
// Keys are matched without regard to case, spaces or underscores, and unknown keys are kept.
using SpoolFields = std::vector<std::pair<std::string, std::string>>;

// The one place a printer's report becomes a SpoolIdentity. Permissive on the way in: any key, any
// spelling, a lane the printer says nothing about is simply an identity with little in it. Strict
// on the way out: the fields the slicer understands are filled in, everything else lands in extra.
// A colour field may hold several colours separated by comma, semicolon, bar or space.
SpoolIdentity spool_identity_from_fields(int slot_index, bool has_filament, const SpoolFields& fields);

// A filament profile the user may pick, flattened out of the preset collection so the
// selection rules below can be tested without a PresetBundle.
struct SpoolPresetCandidate
{
    std::string name;       // preset name, as it goes into PresetBundle::filament_presets
    std::string material;   // filament_type
    std::string vendor;     // filament_vendor
    std::string filament_id; // the id printers use to name this filament, may be empty
    std::string color;      // default_filament_colour, "#RRGGBB", may be empty
    bool        is_system = false;
};

// One saved choice. The label is what the spool was called when the choice was made, kept so the
// list stays readable when all the key carries is a tag number. The colours are the ones this
// filament has already been seen in, so a colour never seen before can be recognised as new.
struct SpoolLink
{
    std::string              key;
    std::string              preset_name;
    std::string              label;
    std::vector<std::string> colors;
    // The colours of this filament the user put on a profile of their own, as "#RRGGBB" -> profile
    // name. Every colour not named here uses preset_name.
    std::map<std::string, std::string> color_presets;
};

// The answers the user gives on the saved-links panel, kept next to the links themselves.
struct SpoolMapSettings
{
    bool enabled      = true;  // off means the picker never opens and synchronizing behaves as it always did
    bool ask_only_new = true;  // ask only about a filament, or a colour of it, that has no link yet
    // Off asks about a generic spool on every sync: a printer that only says "PLA" says it for every
    // PLA, so one link would put all of them on one profile.
    bool remember_generic = false;
};

// Everything spool_profile_map.json holds.
struct SpoolStore
{
    std::map<std::string, SpoolLink> links;
    SpoolMapSettings                 settings;
};

// Stable identifier for "this filament", used to remember the user's choice. Built from the name
// the user is shown, so the same filament is one link however many spools of it exist and however
// many printers report it. Never returns an empty string.
std::string spool_mapping_key(const SpoolIdentity& spool);

// As much of the spool as the key still carries, so a saved link can be shown to the user long
// after the printer that reported it went away. An unreadable key gives an empty identity.
SpoolIdentity spool_identity_from_key(const std::string& key);

// A spool the printer described by its material alone: no name, no vendor, no tag.
bool spool_is_generic(const SpoolIdentity& spool);

// What to call a spool in a list: the name the printer reported, else its vendor and material.
// Empty only when the printer reported nothing at all.
std::string spool_display_name(const SpoolIdentity& spool);

// "#RRGGBB" out of whatever the printer sent ("RRGGBBAA", "0xRRGGBB"...), empty when it cannot be
// read at all.
std::string spool_normalize_color(const std::string& color);

// As spool_normalize_color, but an alpha channel the printer sent is kept ("#RRGGBBAA"), unless it
// is fully opaque.
std::string spool_normalize_color_alpha(const std::string& color);

// The colours of one look of a filament, as they are kept on a link: "#RRGGBB" for a plain spool,
// "#RRGGBB,#RRGGBBAA,..." for a multicolour one. Empty when the printer sent no colour.
std::string spool_variant_color(const SpoolIdentity& spool);

// The colours inside a variant, each as spool_normalize_color_alpha gives it.
std::vector<std::string> spool_variant_colors(const std::string& variant);

// 0 = same material, 1 = same material family (PLA-CF vs PLA), 2 = unrelated.
int spool_material_rank(const std::string& candidate_material, const std::string& reported_material);

// The profiles the picker offers for one spool, most likely first. With include_all set nothing
// is dropped, only reordered. A profile named by always_include is always kept and sorted first,
// so a remembered choice never disappears from the list that offers it.
std::vector<SpoolPresetCandidate> spool_filter_candidates(const std::vector<SpoolPresetCandidate>& all,
                                                          const SpoolIdentity&                     spool,
                                                          bool                                     include_all,
                                                          const std::string&                       always_include = std::string());

// Which profile to preselect. A remembered choice wins whenever it is still available.
std::string spool_choose_preset(const std::vector<SpoolPresetCandidate>& candidates,
                                const SpoolIdentity&                     spool,
                                const std::string&                       remembered);

// The profile saved for this filament, or the one saved for this colour of it when that colour was
// given one of its own. Empty when the filament has no link.
std::string spool_remembered_preset(const std::map<std::string, SpoolLink>& links,
                                    const std::string&                      key,
                                    const std::string&                      color = std::string());

// Nothing saved for this filament, or nothing saved for this colour of it.
bool spool_is_new(const std::map<std::string, SpoolLink>& links, const std::string& key, const std::string& color);

// Couple a filament to a profile. Everything already filed for this filament, under whatever key
// it was saved with, is folded into the one link: one filament is one row, never one row per spool
// or per printer. An empty profile name forgets the link.
void spool_remember(std::map<std::string, SpoolLink>& links,
                    const std::string&                key,
                    const std::string&                preset_name,
                    const std::string&                label = std::string(),
                    const std::string&                color = std::string());

// One colour of a filament on a profile of its own. An empty profile name, or the filament's own
// profile, puts the colour back with the rest of the filament.
void spool_remember_color(std::map<std::string, SpoolLink>& links,
                          const std::string&                key,
                          const std::string&                color,
                          const std::string&                preset_name);

// The picker's answer for one spool. It goes on the filament while the filament has no profile
// yet, and on the colour once it has a different one, so answering for a colour never moves the
// colours that were already answered for.
void spool_remember_spool(std::map<std::string, SpoolLink>& links,
                          const std::string&                key,
                          const std::string&                preset_name,
                          const std::string&                label,
                          const std::string&                color);

// Every saved link, named spools first and alphabetically, so the panel reads like a shelf rather
// than a dump.
std::vector<SpoolLink> spool_sorted_links(const std::map<std::string, SpoolLink>& links);

// The store out of the text of spool_profile_map.json, including the files earlier builds wrote.
// Nullopt when the text cannot be read, so the caller can tell a broken file from an empty one.
std::optional<SpoolStore> spool_store_from_json(const std::string& json_text);
std::string               spool_store_to_json(const SpoolStore& store);

class SpoolProfileMap
{
public:
    static SpoolProfileMap& get();

    // Printer side: the agent calls this right after it has parsed the printer's answer. This is a
    // live report, not a saved choice, so it is never written to disk.
    void publish(const std::string& dev_id, std::vector<SpoolIdentity> spools);
    // Slicer side: empty means "this printer told us nothing", i.e. behave as before.
    std::vector<SpoolIdentity> spools(const std::string& dev_id) const;

    // The saved choices are not tied to a printer: the same filament carries the same profile
    // whichever machine it is hanging on, so one list serves every printer. They belong to the
    // logged-in account, like its presets. Each call applies the matching spool_* function above
    // to that list; the changing ones write it straight back.
    std::string            remembered(const std::string& key, const std::string& color = std::string());
    bool                   is_new_spool(const std::string& key, const std::string& color);
    std::vector<SpoolLink> all_remembered();
    void                   remember(const std::string& key,
                                    const std::string& preset_name,
                                    const std::string& label = std::string(),
                                    const std::string& color = std::string());
    void remember_color(const std::string& key, const std::string& color, const std::string& preset_name);
    void remember_spool(const std::string& key,
                        const std::string& preset_name,
                        const std::string& label,
                        const std::string& color);

    SpoolMapSettings settings();
    void             set_settings(const SpoolMapSettings& settings);

private:
    SpoolProfileMap() = default;

    // The logged-in account's store, read again whenever the account changed since the last call.
    SpoolStore& store();
    void        save() const;

    mutable std::mutex                                m_mutex;
    std::map<std::string, std::vector<SpoolIdentity>> m_published;
    SpoolStore                                        m_store;
    std::string                                       m_store_path;
    bool                                              m_store_unreadable = false;
};

} // namespace Slic3r
