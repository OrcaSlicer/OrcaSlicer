#pragma once

// Links the spools a printer reports to filament profiles.
//
// The link is made the first time a filament, or a colour of it, shows up on a sync: the sync asks
// which profile it takes (SpoolProfileMapDialog::run_link). The printer's slot edit window can
// change it later. From then on the printer reports that filament with the linked profile's id, so
// the stock "sync with printer" lands on that profile. Only a printer whose agent published its
// spools through SpoolProfileMap takes part; for any other every function here does nothing.

#include <string>
#include <vector>

#include "slic3r/Utils/SpoolProfileMap.hpp"

namespace Slic3r { namespace GUI {

// One filament linked to a profile: which lane it came from, which profile it is linked to.
struct SpoolChoice
{
    int         slot_index = 0;
    std::string preset_name;
    std::string filament_id;
};

// The filament id of the profile linked to this spool, for the agent to report in place of the
// printer's own. Empty when the spool has no link, so the printer's own id stays.
std::string spool_linked_filament_id(const SpoolIdentity& spool);

// The user picked the profile with this filament id for one slot in the slot edit window.
void spool_link_from_slot(const std::string& dev_id, int slot_index, const std::string& filament_id);

// Whether this sync has to ask: a filament on the printer, or a colour of it, has no link yet.
// False when linking is off, the sync only takes colors, or the printer published no spools.
bool spool_sync_needs_asking(const std::string& dev_id);

// Points each reported lane of the list the sync commits at the profile picked for it. The printer
// reported its lanes before the user answered, so without this the commit lands on the old link.
void point_sync_at(const std::vector<SpoolChoice>& chosen);

// Called just before the spools are committed: the links of the filaments on this printer. Empty when
// the printer published no spools, the feature is switched off, or the sync only takes colors.
std::vector<SpoolChoice> spool_linked_choices(const std::string& dev_id);

// Called right after the commit with what spool_linked_choices returned. A profile the user made
// themselves carries the filament id of the system profile it came from, so the commit can only
// land on that parent; this puts the user's own profile back in its place.
void apply_links(const std::vector<SpoolChoice>& chosen);

}} // namespace Slic3r::GUI
