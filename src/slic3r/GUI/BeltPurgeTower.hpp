#pragma once

// ORCA-Belt: auto-managed purge prism for belt printers.
//
// Kept in its own translation unit (not buried in Plater.cpp) so it stays out
// of the way of unrelated upstream changes and carries no regression risk for
// normal printers: nothing here runs unless the printer is a belt printer with
// the belt purge tower enabled. See Slic3r::GUI::ensure_belt_purge_tower().

namespace Slic3r {
class Model;
class PartPlateList;
namespace GUI {
class ObjectList;

// Inputs of the last belt purge prism generation. Idempotence is keyed on these
// (not the prism's resulting bbox): the prism gets nudged by plate assignment
// after creation, so comparing its bbox would falsely detect a change and
// regenerate it on every background-process tick — which would re-invalidate a
// freshly sliced result and make the G-code preview unreachable.
struct BeltPurgeSignature
{
    bool valid          = false;
    int  filament_count = 0;
    long key[12]        = {0}; // rounded geometry and plate inputs (0.1 mm units)
    bool operator==(const BeltPurgeSignature &o) const
    {
        if (valid != o.valid || filament_count != o.filament_count)
            return false;
        for (int i = 0; i < 12; ++i)
            if (key[i] != o.key[i])
                return false;
        return true;
    }
};

// Keep the auto-generated belt purge prism in sync with the current config and
// plate contents. Creates / updates / removes the marked prism ModelObject.
// Returns true when the model was mutated (caller should refresh the scene).
// Runs on every background-process update, so it is idempotent: it only mutates
// the model when the desired prism differs from the cached signature in `sig`.
bool ensure_belt_purge_tower(Model &model, PartPlateList &partplate_list, ObjectList *obj_list, BeltPurgeSignature &sig);

} // namespace GUI
} // namespace Slic3r
