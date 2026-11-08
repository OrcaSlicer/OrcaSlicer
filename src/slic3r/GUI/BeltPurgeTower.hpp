#pragma once

// BELT_MOD -- the auto-managed purge prism for belt printers (ported from OrcaSlicer's belt branch).
// Kept in its own translation unit so it stays out of Plater.cpp: nothing here runs unless the
// printer is a belt printer with the belt purge tower enabled. See ensure_belt_purge_tower().

namespace Slic3r {
class Model;
class PartPlateList;
namespace GUI {
class ObjectList;

// Inputs of the last prism generation. Idempotence is keyed on these, not on the prism's resulting
// bbox: the prism gets nudged by plate assignment after creation, so comparing its bbox would
// detect a change on every background-process tick and regenerate it, invalidating a fresh slice.
struct BeltPurgeSignature
{
    bool valid          = false;
    int  filament_count = 0;
    long key[12]        = {0};   // rounded geometry and plate inputs (0.1 mm units)
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

// Keep the prism in sync with the config and the plate contents: creates, updates or removes the
// marked ModelObject. Returns true when the model was mutated (the caller refreshes the scene).
// Runs on every background-process update, so it only mutates the model when the desired prism
// differs from the cached signature in `sig`.
bool ensure_belt_purge_tower(Model &model, PartPlateList &partplate_list, ObjectList *obj_list, BeltPurgeSignature &sig);

} // namespace GUI
} // namespace Slic3r
