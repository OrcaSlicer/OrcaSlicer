#pragma once

// LifecycleEvents.hpp
// --------------------
// Application lifecycle events (project, slicing, plate editing, preset, printer connection, and
// job activity) that other subsystems -- chiefly the plugin layer above libslic3r -- may want to
// observe. Lives in libslic3r rather than the plugin layer because some events fire from inside
// the slicing engine itself; see fire_lifecycle_event() below.

#include <functional>
#include <string>
#include <utility>

namespace Slic3r
{
    enum class LifecycleEvent {
        // Project (3mf)
        NewProject,
        ProjectOpened,
        ProjectBeforeSave,
        ProjectAfterSave,
        ProjectClosed,
        ProjectDirtyChanged,

        // Slicing pipeline
        SliceStarted,
        SliceGeometryFinished,
        GCodeExportStarted,
        GCodeExportFinished,
        SlicingJobComplete,

        // Plate/model editing
        ObjectAdded,
        ObjectDeleted,
        ObjectTransformed,
        ObjectChanged,
        ObjectRenamed,
        PlateCreated,
        PlateDeleted,
        PlateSelected,
        PlateRenamed,

        // Preset
        PresetSelected,
        PresetSaved,

        // Printer/device
        PrintStateChanged,
        DeviceOnlineChanged,
        DeviceDiscovered,
        DeviceSelected,
        DeviceConnected,
        DeviceDisconnected,
        UploadStarted,
        UploadFinished,

        // Print/send jobs
        PrintJobStarted,
        PrintJobFinished,
        SendJobStarted,
        SendJobFinished,
    };

    // Scoped so callers must qualify (LifecycleEvtCode::Error, not ERROR) -- ERROR/OK collide with
    // Windows macros (wingdi.h) as unqualified names.
    enum class LifecycleEvtCode { Ok, Error, Warn };

    struct LifecycleEventContext
    {
        // The primary subject identifier for the event. This is event-specific (for example, a
        // project/output path, preset name, device id, or object name), must not contain status or
        // prose, and may be empty when the event has no single subject.
        std::string name;

        // Outcome of the operation represented by the event. For state-change and start events,
        // Ok means that the event occurred; it does not imply that a future operation succeeded.
        LifecycleEvtCode code = LifecycleEvtCode::Ok;

        // Optional human-readable detail or diagnostic text. It is not a stable parsing contract;
        // machine-readable data should be represented by a dedicated field or event instead.
        std::string msg;

        // Stable subject/object identifier, when the source model provides one.
        std::string id;

        // Previous value for rename and other before/after events.
        std::string previous_name;

        // Device identifier for printer and job events.
        std::string device_id;

        // Job identifier when the originating queue/task provides one.
        std::string job_id;

        // Source subsystem or operation detail, suitable for filtering but not guaranteed to be
        // exhaustive across versions.
        std::string source;

        // Plate, object, or volume index when the source uses an index rather than a stable id.
        int index = -1;

        // Aggregate project dirty state for ProjectDirtyChanged.
        bool dirty = false;
    };

    inline std::string lifecycle_event_to_string(LifecycleEvent event)
    {
        switch (event) {
        case LifecycleEvent::NewProject: return "NewProject";
        case LifecycleEvent::ProjectOpened: return "ProjectOpened";
        case LifecycleEvent::ProjectBeforeSave: return "ProjectBeforeSave";
        case LifecycleEvent::ProjectAfterSave: return "ProjectAfterSave";
        case LifecycleEvent::ProjectClosed: return "ProjectClosed";
        case LifecycleEvent::ProjectDirtyChanged: return "ProjectDirtyChanged";

        case LifecycleEvent::SliceStarted: return "SliceStarted";
        case LifecycleEvent::SliceGeometryFinished: return "SliceGeometryFinished";
        case LifecycleEvent::GCodeExportStarted: return "GCodeExportStarted";
        case LifecycleEvent::GCodeExportFinished: return "GCodeExportFinished";
        case LifecycleEvent::SlicingJobComplete: return "SlicingJobComplete";

        case LifecycleEvent::ObjectAdded: return "ObjectAdded";
        case LifecycleEvent::ObjectDeleted: return "ObjectDeleted";
        case LifecycleEvent::ObjectTransformed: return "ObjectTransformed";
        case LifecycleEvent::ObjectChanged: return "ObjectChanged";
        case LifecycleEvent::ObjectRenamed: return "ObjectRenamed";
        case LifecycleEvent::PlateCreated: return "PlateCreated";
        case LifecycleEvent::PlateDeleted: return "PlateDeleted";
        case LifecycleEvent::PlateSelected: return "PlateSelected";
        case LifecycleEvent::PlateRenamed: return "PlateRenamed";

        case LifecycleEvent::PresetSelected: return "PresetSelected";
        case LifecycleEvent::PresetSaved: return "PresetSaved";
        case LifecycleEvent::PrintStateChanged: return "PrintStateChanged";

        case LifecycleEvent::DeviceOnlineChanged: return "DeviceOnlineChanged";
        case LifecycleEvent::DeviceDiscovered: return "DeviceDiscovered";
        case LifecycleEvent::DeviceSelected: return "DeviceSelected";
        case LifecycleEvent::DeviceConnected: return "DeviceConnected";
        case LifecycleEvent::DeviceDisconnected: return "DeviceDisconnected";

        case LifecycleEvent::UploadStarted: return "UploadStarted";
        case LifecycleEvent::UploadFinished: return "UploadFinished";
        case LifecycleEvent::PrintJobStarted: return "PrintJobStarted";
        case LifecycleEvent::PrintJobFinished: return "PrintJobFinished";
        case LifecycleEvent::SendJobStarted: return "SendJobStarted";
        case LifecycleEvent::SendJobFinished: return "SendJobFinished";
        default: return "Unknown";
        }
    }

    inline std::string lifecycle_evt_code_to_string(LifecycleEvtCode code)
    {
        switch (code) {
        case LifecycleEvtCode::Ok: return "Ok";
        case LifecycleEvtCode::Error: return "Error";
        case LifecycleEvtCode::Warn: return "Warn";
        default: return "Unknown";
        }
    }

    // Global cross-layer seam (mirrors ConfigBase::set_resolve_capability_fn): any libslic3r code can
    // fire a lifecycle event without depending on the plugin layer above it, which installs the
    // dispatcher here at startup. Not tied to Print/GCode specifically, since nothing here should
    // require callers to hold a Print& just to report an event.
    using LifecycleHookFn = std::function<void(LifecycleEvent, const LifecycleEventContext&)>;

    inline LifecycleHookFn& lifecycle_hook_fn()
    {
        static LifecycleHookFn fn;
        return fn;
    }

    inline void set_lifecycle_hook_fn(LifecycleHookFn fn) { lifecycle_hook_fn() = std::move(fn); }

    inline void fire_lifecycle_event(LifecycleEvent event, const LifecycleEventContext& ctx)
    {
        if (const LifecycleHookFn& fn = lifecycle_hook_fn(); fn)
            fn(event, ctx);
    }
}
