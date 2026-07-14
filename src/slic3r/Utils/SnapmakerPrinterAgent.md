# SnapmakerPrinterAgent — filament matching design notes

This file documents `SnapmakerPrinterAgent::fetch_filament_info` and its helpers: the logic
that maps a Snapmaker U1 AMS tray (vendor/type/sub_type/color, as reported over Moonraker)
to an installed OrcaSlicer filament preset. Update this file when the matching logic
changes materially.

## The problem

The printer sends `filament_sub_type` (e.g. "Matte", "High Speed"), but the OrcaSlicer
filament library has no field for it — a preset's config has no place to record that it's
the Matte variant of PLA versus plain PLA. The library instead leans on `filament_id` to
distinguish variants (each preset is expected to carry a unique-enough id), but the printer
doesn't send one — stock Snapmaker U1 firmware has no `filament_id` field in its protocol
at all. And even if the printer did send a `filament_id`, OrcaSlicer's own UI doesn't
expose or make `filament_id` editable for custom filament profiles, so there's no way for
a user to set one that this code could then rely on.

## The chosen route: Try to determine the subtype from the profile name

If you're thinking that this sounds like a hack, I couldn't agree more with you but
I couldn't come up with any other working solution.  Until the firmware and/or Orca 
catches up, this is (probably?) the best we can do.  This works with both stock and paxx firmware (so far) and custom filament profiles are supported.

Here's how it works:
1.  What comes in as subtype from the printer is first scanned for well-known
subtypes.
1.  The subtypes are normalized to internal names, such as MATTE, HS, etc..
1.  The filament library is scanned where we consider any filament with the
appopriate printer support, and base type.
1.  The profile `name` is scanned for the same well-known subtypes and normalized.
1.  If we find a match, that profile is ranked against any other matches.

The ranking algorithm works as follows:
1.  As before, the first match automatically becomes the best match.
1.  Any match that matches on a subtype is preferred over one that doesn't.
1.  Any match that is derrived from another filament is preferred over one that isn't.  The idea being that a custom filament profile likely exists for a reason.
1.  Color distance is the last tie-breaker.

If this phase doesn't match anything, the two existing fallbacks are next in line.
1.  Type only match.
1.  Generic fallback.

**NOTE:** Once a profile is selected, there's no way to hand that profile back to Orca.  Orca expects a filament_id to be returned from this code.  This is important because Orca will then try to find that filament profile by id and there might likely be duplicates.  User-created filament profiles get a unique auto-generated id so that's not a problem.

**IMPORTANT:** Derived profiles inherit filament_id.  Don't make custom filaments
by using the save-as feature on the filament property editor. You should make a totally new filament profile on the custom filament screen.

## Known gaps / open issues 

1. **The PLA+ / PLA Plus problem**
   OpenSpool has a whitelist of base filament types.  IMO, PLA Plus is a different
   filament type but writing PLA+ into the type field on an rfid will fail in the
   OpenSpool processor.  We are then left to write Plus into the subtype field.  The downside is that our processor needs to know what subtypes to look for in the filament profile name so this comes with that restriction.

1. **Possible future direction: `filament_type` is an array in a preset's JSON, not a
   scalar.** Every consumer in this file currently reads it as a single string
   (`p.config.opt_string("filament_type", 0u)` — i.e. only ever index 0). Since it's
   actually a per-extruder/per-variant array in the underlying config schema, there may be
   a future opportunity to use additional array entries (if populated) as a second source
   of sub-type-ish information distinct from name-scraping — not investigated yet, purely
   a note for later.

1. **filament_id:** While it's unlikely that the stock firmware will start to send
filament_id, there's a very real possibility that it can be done via paxx either
officially from paxx or via a new feature that paxx is implementing via user mods.
With that capability, we could offer increased fidelity for those who choose to
implement it.