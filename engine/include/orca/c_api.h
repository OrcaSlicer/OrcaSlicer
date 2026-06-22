#ifndef ORCA_C_API_H
#define ORCA_C_API_H

/*
 * Parallel C ABI for the orca engine.
 *
 * Implementation is deferred to Phase 0.6 (see FULL_TODO_PLAN.md). This header
 * is the design sketch: it locks in the boundary types so the C++ API in
 * Session.hpp / Presets.hpp / etc. cannot drift into shapes that don't have a
 * sensible C projection. Reviewed end-to-end during Phase 0.2.9.
 *
 * Boundary rules:
 *   - POD only across the boundary. No C++ types, no exceptions.
 *   - Strings are UTF-8 const char*. Borrowed unless explicitly documented as
 *     owned-by-caller (free with orca_string_free).
 *   - Handles are opaque struct pointers; lifetime is the owning session.
 *   - Numeric handles (orca_*_handle_t) are uint64_t, 0 is reserved as invalid.
 *   - Errors are returned as orca_error_code_t; out-params are populated only
 *     on ORCA_OK unless documented otherwise.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Opaque handles ---------- */

typedef struct orca_session  orca_session_t;
typedef struct orca_presets  orca_presets_t;
typedef struct orca_project  orca_project_t;
typedef struct orca_slicer   orca_slicer_t;
typedef struct orca_exporter orca_exporter_t;
typedef struct orca_events   orca_events_t;

typedef uint64_t orca_object_id_t;
typedef uint64_t orca_instance_id_t;
typedef uint64_t orca_volume_id_t;
typedef uint64_t orca_load_handle_t;
typedef uint64_t orca_slice_handle_t;
typedef uint64_t orca_export_handle_t;
typedef uint64_t orca_subscription_id_t;

/* ---------- Error codes ---------- */

typedef enum {
    ORCA_OK                    = 0,
    ORCA_ERR_UNKNOWN           = 1,
    ORCA_ERR_INVALID_ARGUMENT  = 2,
    ORCA_ERR_NOT_FOUND         = 3,
    ORCA_ERR_ALREADY_EXISTS    = 4,
    ORCA_ERR_IO                = 5,
    ORCA_ERR_PARSE             = 6,
    ORCA_ERR_CANCELLED         = 7,
    ORCA_ERR_UNSUPPORTED       = 8,
    ORCA_ERR_PERMISSION_DENIED = 9
} orca_error_code_t;

/* Returns the last error message produced on the current thread. Borrowed,
 * valid until the next API call on this thread. */
const char* orca_last_error_message(void);

/* ---------- Enums ---------- */

typedef enum {
    ORCA_PRESET_PRINT             = 0,
    ORCA_PRESET_PRINTER           = 1,
    ORCA_PRESET_FILAMENT          = 2,
    ORCA_PRESET_SLA_PRINT         = 3,
    ORCA_PRESET_SLA_MATERIAL      = 4,
    ORCA_PRESET_PHYSICAL_PRINTER  = 5
} orca_preset_type_t;

typedef enum {
    ORCA_SCOPE_FULL             = 0,
    ORCA_SCOPE_PROJECT          = 1,
    ORCA_SCOPE_PRINT_PRESET     = 2,
    ORCA_SCOPE_PRINTER_PRESET   = 3,
    ORCA_SCOPE_FILAMENT_PRESET  = 4
} orca_config_scope_t;

typedef enum {
    ORCA_VOLUME_MODEL              = 0,
    ORCA_VOLUME_NEGATIVE           = 1,
    ORCA_VOLUME_PARAMETER_MODIFIER = 2,
    ORCA_VOLUME_SUPPORT_BLOCKER    = 3,
    ORCA_VOLUME_SUPPORT_ENFORCER   = 4
} orca_volume_type_t;

typedef enum {
    ORCA_LOAD_DEFAULT               = 0,
    ORCA_LOAD_ADD_DEFAULT_INSTANCES = 1 << 0,
    ORCA_LOAD_CHECK_VERSION         = 1 << 1,
    ORCA_LOAD_MODEL                 = 1 << 2,
    ORCA_LOAD_CONFIG                = 1 << 3,
    ORCA_LOAD_AUXILIARY             = 1 << 4,
    ORCA_LOAD_SILENT                = 1 << 5,
    ORCA_LOAD_IMPERIAL_UNITS        = 1 << 6
} orca_load_strategy_t;

typedef enum {
    ORCA_TECH_FFF = 0,
    ORCA_TECH_SLA = 1
} orca_slice_technology_t;

typedef enum {
    ORCA_SLICE_NOT_STARTED = 0,
    ORCA_SLICE_QUEUED      = 1,
    ORCA_SLICE_RUNNING     = 2,
    ORCA_SLICE_COMPLETED   = 3,
    ORCA_SLICE_CANCELLED   = 4,
    ORCA_SLICE_FAILED      = 5
} orca_slice_state_t;

/* ---------- Param + status structs ---------- */

typedef struct {
    uint32_t                strategy;   /* bitfield of orca_load_strategy_t */
    int32_t                 plate_id;
} orca_load_options_t;

typedef struct {
    int32_t                 plate_id;
    orca_slice_technology_t technology;
} orca_slice_params_t;

typedef struct {
    orca_slice_state_t state;
    float              progress;
    const char*        message; /* borrowed; valid until next status query */
    const char*        error;   /* borrowed */
} orca_slice_status_t;

typedef struct {
    const char* output_path; /* UTF-8 */
    int32_t     plate_id;
    bool        send_to_printer;
} orca_export_params_t;

/* ---------- Session lifecycle + service accessors ---------- */

orca_session_t*  orca_session_create(void);
void             orca_session_destroy(orca_session_t* session);

orca_presets_t*  orca_session_presets(orca_session_t* session);
orca_project_t*  orca_session_project(orca_session_t* session);
orca_slicer_t*   orca_session_slicer(orca_session_t* session);
orca_exporter_t* orca_session_exporter(orca_session_t* session);
orca_events_t*   orca_session_events(orca_session_t* session);

/* ---------- Presets (typed per primitive) ---------- */

orca_error_code_t orca_presets_opt_int   (const orca_presets_t* p, orca_config_scope_t scope, const char* key, int32_t* out);
orca_error_code_t orca_presets_opt_float (const orca_presets_t* p, orca_config_scope_t scope, const char* key, double*  out);
orca_error_code_t orca_presets_opt_bool  (const orca_presets_t* p, orca_config_scope_t scope, const char* key, bool*    out);
orca_error_code_t orca_presets_opt_string(const orca_presets_t* p, orca_config_scope_t scope, const char* key, const char** out_borrowed);

orca_error_code_t orca_presets_set_int   (orca_presets_t* p, orca_config_scope_t scope, const char* key, int32_t  value);
orca_error_code_t orca_presets_set_float (orca_presets_t* p, orca_config_scope_t scope, const char* key, double   value);
orca_error_code_t orca_presets_set_bool  (orca_presets_t* p, orca_config_scope_t scope, const char* key, bool     value);
orca_error_code_t orca_presets_set_string(orca_presets_t* p, orca_config_scope_t scope, const char* key, const char* value);

orca_error_code_t orca_presets_current  (const orca_presets_t* p, orca_preset_type_t type, const char** out_name_borrowed);
orca_error_code_t orca_presets_set_current(orca_presets_t* p, orca_preset_type_t type, const char* name);
orca_error_code_t orca_presets_set_filament(orca_presets_t* p, int32_t extruder_idx, const char* name);
int32_t           orca_presets_extruder_count(const orca_presets_t* p);
bool              orca_presets_is_bbl_vendor (const orca_presets_t* p);

/* ---------- Project ---------- */

orca_error_code_t orca_project_load_file(orca_project_t* p, const char* path, const orca_load_options_t* options, orca_load_handle_t* out_handle);
orca_error_code_t orca_project_save     (orca_project_t* p, const char* path);
size_t            orca_project_object_count(const orca_project_t* p);
orca_error_code_t orca_project_object_id_at(const orca_project_t* p, size_t idx, orca_object_id_t* out_id);
orca_error_code_t orca_project_remove_object(orca_project_t* p, orca_object_id_t id);
orca_error_code_t orca_project_clear(orca_project_t* p);

/* ---------- Slicer ---------- */

orca_error_code_t orca_slicer_request (orca_slicer_t* s, const orca_slice_params_t* params, orca_slice_handle_t* out_handle);
void              orca_slicer_cancel  (orca_slicer_t* s, orca_slice_handle_t handle);
void              orca_slicer_status  (const orca_slicer_t* s, orca_slice_handle_t handle, orca_slice_status_t* out);
bool              orca_slicer_is_busy (const orca_slicer_t* s);

/* ---------- Exporter ---------- */

orca_error_code_t orca_exporter_export(orca_exporter_t* e, const orca_export_params_t* params, orca_export_handle_t* out_handle);
void              orca_exporter_cancel(orca_exporter_t* e, orca_export_handle_t handle);

/* ---------- Events ---------- */

typedef enum {
    ORCA_EVT_SLICING_PROGRESS = 1,
    ORCA_EVT_SLICING_FINISHED = 2,
    ORCA_EVT_EXPORT_BEGAN     = 3,
    ORCA_EVT_EXPORT_FINISHED  = 4,
    ORCA_EVT_PRESET_CHANGED   = 5,
    ORCA_EVT_PROJECT_LOADED   = 6,
    ORCA_EVT_OBJECT_ADDED     = 7,
    ORCA_EVT_OBJECT_REMOVED   = 8,
    ORCA_EVT_BEFORE_SLICE        = 9,
    ORCA_EVT_AFTER_PERIMETERS    = 10,
    ORCA_EVT_AFTER_INFILL        = 11,
    ORCA_EVT_AFTER_IRONING       = 12,
    ORCA_EVT_AFTER_SUPPORTS      = 13,
    ORCA_EVT_BEFORE_WIPE_TOWER   = 14,
    ORCA_EVT_AFTER_SKIRT_BRIM    = 15,
    ORCA_EVT_BEFORE_GCODE_EXPORT = 16,
    ORCA_EVT_AFTER_GCODE_EXPORT  = 17
} orca_event_kind_t;

typedef void (*orca_event_callback_t)(orca_event_kind_t kind, const void* payload, void* user_data);

orca_subscription_id_t orca_events_subscribe  (orca_events_t* events, orca_event_kind_t kind, orca_event_callback_t cb, void* user_data);
void                   orca_events_unsubscribe(orca_events_t* events, orca_subscription_id_t id);

/* Event payloads — passed through the void* in the callback, valid only for the duration of the callback. */

typedef struct {
    orca_slice_handle_t handle;
    float               progress;
    const char*         message;
} orca_evt_slicing_progress_t;

typedef struct {
    orca_slice_handle_t handle;
    bool                success;
    const char*         error;
} orca_evt_slicing_finished_t;

typedef struct {
    orca_export_handle_t handle;
    const char*          path;
} orca_evt_export_began_t;

typedef struct {
    orca_export_handle_t handle;
    bool                 success;
    size_t               line_count;
    const char*          error;
} orca_evt_export_finished_t;

typedef struct {
    orca_preset_type_t type;
    const char*        name;
} orca_evt_preset_changed_t;

typedef struct {
    orca_load_handle_t handle;
    const char*        path;
} orca_evt_project_loaded_t;

typedef struct { orca_object_id_t id; } orca_evt_object_added_t;
typedef struct { orca_object_id_t id; } orca_evt_object_removed_t;

typedef struct {
    orca_slice_handle_t handle;
} orca_evt_before_slice_t;

typedef struct {
    orca_slice_handle_t handle;
    size_t              object_count;
} orca_evt_after_perimeters_t;

typedef struct {
    orca_slice_handle_t handle;
    size_t              object_count;
} orca_evt_after_infill_t;

typedef struct {
    orca_slice_handle_t handle;
} orca_evt_after_ironing_t;

typedef struct {
    orca_slice_handle_t handle;
} orca_evt_after_supports_t;

typedef struct {
    orca_slice_handle_t handle;
    bool                has_wipe_tower;
} orca_evt_before_wipe_tower_t;

typedef struct {
    orca_slice_handle_t handle;
} orca_evt_after_skirt_brim_t;

typedef struct {
    orca_slice_handle_t handle;
    const char*         output_path;
} orca_evt_before_gcode_export_t;

typedef struct {
    orca_slice_handle_t handle;
    const char*         output_path;
    size_t              line_count;
} orca_evt_after_gcode_export_t;

#ifdef __cplusplus
}
#endif

#endif /* ORCA_C_API_H */
