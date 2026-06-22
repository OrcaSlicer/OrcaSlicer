#ifndef ORCA_PLUGIN_API_H
#define ORCA_PLUGIN_API_H

/*
 * Engine <-> plugin C ABI for the orca engine.
 *
 * This header is the loader-side counterpart to c_api.h. A plugin shared
 * library includes BOTH headers:
 *   - c_api.h     — the surface the plugin DRIVES the engine through.
 *   - plugin_api.h — the surface the engine LOADS the plugin through.
 *
 * Boundary rules (same as c_api.h):
 *   - POD only across the boundary. No C++ types, no exceptions across the
 *     ABI line. Header compiles cleanly as both C11 and C++17.
 *   - Strings are UTF-8 const char*, borrowed unless documented otherwise.
 *   - Handles are opaque struct pointers; the engine owns their lifetime.
 *
 * Every plugin shared library MUST export the three symbols declared at the
 * bottom of this file: orca_plugin_register, orca_plugin_unregister, and
 * orca_plugin_check_debug_consistent. The engine resolves them by name via
 * dlsym / GetProcAddress. The pattern mirrors the production loader in
 * src/slic3r/Utils/BBLNetworkPlugin.cpp:537-637 (vtable assembly) and
 * src/slic3r/Utils/BBLNetworkPlugin.cpp:222-232 (debug/release sanity check).
 */

#include "orca/c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cross-platform symbol-export decoration. Matches the visibility model used
 * by the engine itself and by BBLNetworkPlugin's exported entry points. */
#if defined(_WIN32)
#  define ORCA_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define ORCA_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* ============ ABI version ============ */

/* The engine refuses to load a plugin whose abi_version argument to
 * orca_plugin_register does not equal ORCA_PLUGIN_ABI_VERSION. Bump this
 * on any breaking change to the layouts, function signatures, or semantics
 * defined in this header. */
#define ORCA_PLUGIN_ABI_VERSION 1u

/* ============ Opaque handles ============ */

/* Registry: opaque object the plugin uses, during orca_plugin_register, to
 * declare its manifest and add slot implementations. */
typedef struct orca_plugin_registry orca_plugin_registry_t;

/* Host: opaque tag for the host vtable struct defined below. The plugin
 * receives a pointer to an orca_plugin_host_t; this tag exists so other
 * APIs can take a host pointer by name. */
typedef struct orca_plugin_host     orca_plugin_host_t;

/* Numeric slot id returned by orca_registry_add_slot and consumed by
 * orca_registry_remove_slot. 0 is reserved as invalid. */
typedef uint64_t orca_plugin_slot_id_t;

/* Lifetime: every handle above is owned by the engine. The plugin may only
 * retain the registry pointer and the host pointer for the duration of the
 * orca_plugin_register call. After that call returns the plugin must use
 * the host vtable (which is stable until orca_plugin_unregister) for any
 * further callbacks into the engine. */

/* ============ Slot kinds ============ */

/* Slot kinds the plugin can implement. The value space is partitioned so the
 * high nibble groups categories:
 *   0x00xx — pipeline / data slots (always active, including headless/CLI).
 *   0x01xx — GUI slots; inert when the engine runs without a GUI host
 *            (CLI, headless, automation). The plugin is still loaded but
 *            registrations for these kinds are stored and never invoked.
 *   0x10xx — raw escape hatch.
 *
 * The vtable layout for each kind is declared in Phase 2 of the plugin work.
 * For now orca_registry_add_slot accepts the vtable as an opaque
 * const void* whose concrete type is determined by `kind`. */
typedef enum {
    ORCA_SLOT_PIPELINE_OBSERVER    = 0x0001,
    ORCA_SLOT_PIPELINE_INTERCEPTOR = 0x0002,
    ORCA_SLOT_GCODE_FILTER         = 0x0003,
    ORCA_SLOT_PLACEHOLDER_PROVIDER = 0x0004,
    ORCA_SLOT_PRINTER_AGENT        = 0x0010,
    ORCA_SLOT_PROFILE_PACK         = 0x0020,
    ORCA_SLOT_SETTINGS_PAGE        = 0x0100,
    ORCA_SLOT_DEVICE_TAB           = 0x0101,
    ORCA_SLOT_MENU_COMMAND         = 0x0102,
    ORCA_SLOT_SIDEBAR_PANEL        = 0x0103,
    ORCA_SLOT_RAW_SUBSCRIBER       = 0x1000
} orca_slot_kind_t;

/* ============ Pipeline slot vtables (Phase 2.1.1) ============
 *
 * Observer slots are pure notification — fired for every pipeline step.
 * Interceptor slots return a disposition that gates the step. Both are
 * keyed by the same orca_pipeline_step_t enum.
 *
 * The C++ mirrors of these enums live in <orca/PipelineSteps.hpp> as
 * orca::PipelineStep and orca::PipelineDisposition; their underlying
 * values are kept identical so a static_cast across the boundary in
 * engine/src/Slicer.cpp is well-defined. */
typedef enum {
    ORCA_STEP_BEFORE_SLICE        = 1,
    ORCA_STEP_AFTER_PERIMETERS    = 2,
    ORCA_STEP_AFTER_INFILL        = 3,
    ORCA_STEP_AFTER_IRONING       = 4,
    ORCA_STEP_AFTER_SUPPORTS      = 5,
    ORCA_STEP_BEFORE_WIPE_TOWER   = 6,
    ORCA_STEP_AFTER_SKIRT_BRIM    = 7,
    ORCA_STEP_BEFORE_GCODE_EXPORT = 8,
    ORCA_STEP_AFTER_GCODE_EXPORT  = 9
} orca_pipeline_step_t;

typedef enum {
    ORCA_DISPOSITION_PROCEED = 0,
    ORCA_DISPOSITION_SKIP    = 1,
    ORCA_DISPOSITION_ABORT   = 2
} orca_disposition_t;

typedef struct {
    uint32_t struct_size;
    void (*on_step)(orca_pipeline_step_t step,
                    uint64_t              slice_handle,
                    void*                 user_data);
} orca_slot_pipeline_observer_t;

typedef struct {
    uint32_t struct_size;
    orca_disposition_t (*on_step)(orca_pipeline_step_t step,
                                  uint64_t              slice_handle,
                                  void*                 user_data);
} orca_slot_pipeline_interceptor_t;

/* ------------------------------------------------------------
 * G-code filter slot (Phase 2.2.1)
 *
 * Filter slots run AFTER the engine has written the final G-code to disk and
 * BEFORE any external post-process scripts are invoked. The plugin mutates the
 * file at gcode_path in place (same contract as run_post_process_scripts).
 *
 * Multiple filters chain in PluginRegistry priority order (ascending). Each
 * filter sees the previous filter's output. Returning anything other than
 * ORCA_OK aborts the export and propagates as RuntimeError.
 *
 * Requires permission bit ORCA_PERM_GCODE_MODIFY.
 * ------------------------------------------------------------ */
typedef struct {
    uint32_t struct_size;
    orca_error_code_t (*filter)(const char* gcode_path,
                                uint64_t    slice_handle,
                                void*       user_data);
} orca_slot_gcode_filter_t;

/* ------------------------------------------------------------
 * PlaceholderParser variable provider slot (Phase 2.3.1)
 *
 * Provider slots fire once per slice, BEFORE GCode::do_export captures the
 * parser. The plugin's on_provide callback should use the host vtable's
 * placeholder_set_{string,int,float} entries to push variables into the
 * active Print::placeholder_parser(). Variables set this way are reachable
 * from machine_start_gcode / machine_end_gcode and other PlaceholderParser-
 * rendered templates.
 *
 * Multiple providers fire in PluginRegistry ascending-priority order. Any
 * non-OK rc aborts the slice (mirrors orca_slot_gcode_filter_t).
 *
 * Requires permission bit ORCA_PERM_SETTINGS_WRITE.
 * ------------------------------------------------------------ */
typedef struct {
    uint32_t struct_size;
    orca_error_code_t (*on_provide)(uint64_t slice_handle,
                                    void*    user_data);
} orca_slot_placeholder_provider_t;

/* ------------------------------------------------------------
 * Printer agent slot (Phase 2.4.2)
 *
 * A printer agent represents a single printer KIND (e.g. "moonraker",
 * "bambu", "klipper"). The engine instantiates an agent via the C lifecycle
 * functions in this vtable and wraps the returned opaque instance pointer
 * in orca::PrinterAgentAdapter (engine/src/PrinterAgentAdapter.{hpp,cpp}),
 * which presents the orca::PrinterAgent C++ interface to engine consumers.
 *
 * Status, message, and cancellation flow is INVERTED across the boundary:
 * instead of the plugin holding a std::function<void(...)> callback (which
 * doesn't cross the C ABI), the host hands the plugin an
 * orca_printer_host_t emit-table at create() time. The plugin invokes
 * host->emit_status(host_ctx, ...) from its own thread when status changes;
 * the host thunk routes the call to the std::function the engine consumer
 * registered via PrinterAgent::on_status.
 *
 * All function pointers EXCEPT create_instance and destroy_instance are
 * nullable. A null pointer means "this agent does not support that
 * operation" — the adapter returns ErrorCode::NotImplemented to the engine
 * caller. Keeps the surface gracefully extensible while letting plugins
 * implement only what they support.
 *
 * Requires permission bit ORCA_PERM_DEVICE_CONTROL.
 * ------------------------------------------------------------ */

/* ---- Host-side emit table delivered to the plugin at create_instance. ----
 * The plugin stores the pointer + host_ctx and invokes the emit_* thunks
 * whenever it has news to deliver. Thunks are safe to call from any thread.
 */
typedef enum {
    ORCA_PRINTER_STATE_DISCONNECTED = 0,
    ORCA_PRINTER_STATE_CONNECTING   = 1,
    ORCA_PRINTER_STATE_CONNECTED    = 2,
    ORCA_PRINTER_STATE_ERROR        = 3
} orca_printer_state_t;

typedef struct {
    uint32_t struct_size;
    void* host_ctx;   /* opaque, plugin treats as a token; host_ctx points to the Adapter */

    /* Status updates — fire whenever the connection state OR a non-fatal
       detail code changes. message may be NULL for empty messages. */
    void (*emit_status)(void*                host_ctx,
                        orca_printer_state_t state,
                        int                  code,
                        const char*          message);

    /* Asynchronous message from the printer (e.g. a Klipper response, a
       Moonraker JSON-RPC notification, a raw Marlin line). payload may be
       NUL-terminated text or arbitrary bytes; payload_len is authoritative. */
    void (*emit_message)(void*       host_ctx,
                         const char* payload,
                         size_t      payload_len);

    /* Synchronous query the plugin may consult during long ops (e.g. mid-
       upload). Returns non-zero if the engine consumer has requested
       cancellation. */
    int (*is_cancelled)(void* host_ctx);
} orca_printer_host_t;

/* ---- Plugin-side vtable. Most fields are nullable. ----
 *
 * Lifecycle contract:
 *   create_instance(host, user_data) -> opaque instance pointer (or NULL on
 *       allocation failure). The host argument is owned by the adapter and
 *       remains valid until destroy_instance returns.
 *   destroy_instance(instance) -> must release everything created in
 *       create_instance. The host pointer must not be touched after this.
 *
 * All other methods take the instance pointer as their first argument and
 * return an orca_error_code_t. ORCA_OK on success. */
typedef struct {
    uint32_t struct_size;

    /* Identity — copied into orca::PrinterAgentInfo by the adapter. All
       four MUST be non-NULL and stable for the lifetime of the plugin. */
    const char* agent_id;          /* e.g. "moonraker" — used by Session::create_printer_agent */
    const char* agent_name;        /* human-readable */
    const char* agent_version;     /* semver */
    const char* agent_description; /* one-line capability summary */

    /* Lifecycle — NOT nullable. */
    void* (*create_instance)(const orca_printer_host_t* host, void* user_data);
    void  (*destroy_instance)(void* instance);

    /* Communication — all nullable. Return ORCA_OK on success. */
    orca_error_code_t (*connect)(void*       instance,
                                 const char* device_id,
                                 const char* host_or_ip,
                                 int         port,
                                 const char* username,
                                 const char* password,
                                 int         use_tls);
    orca_error_code_t (*disconnect)(void* instance);

    /* Synchronous state query. Returns the current state directly (NOT an
       error code) — this is the only non-error-code accessor. */
    orca_printer_state_t (*current_state)(void* instance);

    /* Sends a raw payload to the printer. payload_len is authoritative; the
       plugin must not assume NUL termination. */
    orca_error_code_t (*send_command)(void*       instance,
                                      const char* payload,
                                      size_t      payload_len);

    /* Print job control. */
    orca_error_code_t (*start_print)(void*       instance,
                                     const char* gcode_path,
                                     const char* job_name,
                                     int         start_immediately);
    orca_error_code_t (*cancel_print)(void* instance);
} orca_slot_printer_agent_t;

/* ------------------------------------------------------------
 * Profile pack slot (Phase 2.5.2)
 *
 * Profile packs are DATA-ONLY plugins — the plugin's manifest declares
 * `"provides": { "profile_dir": "<relative-path>" }` and PluginManager loads
 * the vendor configs through the engine's Presets API. No code runs from the
 * plugin's side; the slot exists purely so consumers can enumerate which
 * vendor profile packs are installed.
 *
 * Requires permission bit ORCA_PERM_PROFILES_INSTALL.
 * ------------------------------------------------------------ */
typedef struct {
    uint32_t    struct_size;
    /* Absolute filesystem path to the profile directory the engine loaded.
       Stable for the lifetime of the plugin registration. */
    const char* profile_dir;
} orca_slot_profile_pack_t;

/* ============ UI builder ABI (Phase 4.0) ============ */
/*
 * Every UI-side slot kind (settings_page, sidebar_panel, device_tab) hands
 * the plugin an orca_ui_builder_t at mount time. The plugin makes
 * imperative builder calls to populate the container — labels, text
 * fields, dropdowns, buttons — plus an HTML escape hatch for full
 * webview embed. Field values are addressed by a plugin-chosen string
 * `key`; the host owns persistence and surfaces value changes through
 * on_value_changed.
 *
 * The builder pointer is valid only for the duration of the on_build
 * callback. Field accessors (get_value/set_value) work post-build via
 * the page handle the plugin captured.
 *
 * Requires permission bit ORCA_PERM_UI_ATTACH for any slot kind that
 * uses it.
 */

typedef struct orca_ui_page  orca_ui_page_t;   /* opaque page handle */
typedef struct orca_ui_group orca_ui_group_t;  /* opaque group/section handle */

typedef void (*orca_ui_value_changed_fn_t)(
    orca_ui_page_t* page,
    const char*     key,
    const char*     json_value,  /* "42" / "\"hello\"" / "true" / "1.5" */
    void*           user_data);

typedef void (*orca_ui_button_click_fn_t)(orca_ui_page_t* page, void* user_data);

typedef struct {
    uint32_t struct_size;

    /* Section grouping. Subsequent add_* calls land inside the most
     * recently-opened group; groups may nest. */
    orca_ui_group_t* (*push_group)(orca_ui_page_t* page, const char* label);
    void             (*pop_group) (orca_ui_page_t* page);

    /* Read-only / structural. */
    void (*add_label)    (orca_ui_page_t* page, const char* text);
    void (*add_separator)(orca_ui_page_t* page);

    /* Editable fields. `key` is the plugin's addressable name. */
    void (*add_text_field) (orca_ui_page_t* page, const char* label,
                            const char* key, const char* default_value);
    void (*add_int_field)  (orca_ui_page_t* page, const char* label,
                            const char* key, int64_t default_value);
    void (*add_float_field)(orca_ui_page_t* page, const char* label,
                            const char* key, double  default_value);
    void (*add_bool_field) (orca_ui_page_t* page, const char* label,
                            const char* key, int default_value);

    /* Drop-down. options is a NULL-terminated array of UTF-8 strings. */
    void (*add_combo)(orca_ui_page_t* page, const char* label,
                      const char* key,
                      const char* const* options,
                      const char* default_value);

    /* Button. on_click fires on the UI thread. */
    void (*add_button)(orca_ui_page_t* page, const char* label,
                       orca_ui_button_click_fn_t on_click,
                       void* user_data);

    /* Escape hatch — host renders a wxWebView whose HTML body is `html`.
     * Used by plugins needing full UI control. The webview gets the
     * standard window.orca.* bridge. */
    void (*add_html)(orca_ui_page_t* page, const char* html);

    /* Reactive accessor — register once per page; fires on the UI thread
     * whenever ANY field value changes. */
    void (*on_value_changed)(orca_ui_page_t* page,
                             orca_ui_value_changed_fn_t cb,
                             void* user_data);

    /* Read / write field values from any thread. Returned string is
     * heap-allocated UTF-8 JSON; caller frees via host->string_free. */
    const char* (*get_value)(orca_ui_page_t* page, const char* key);
    void        (*set_value)(orca_ui_page_t* page, const char* key,
                             const char* json_value);
} orca_ui_builder_t;

/* ============ UI slot vtables (Phase 4.0) ============ */

/* Settings-page slot (Phase 4.1.1) — adds a custom options page to one
 * of the engine's settings tabs. */
typedef enum {
    ORCA_SETTINGS_TAB_PRINT    = 0,
    ORCA_SETTINGS_TAB_FILAMENT = 1,
    ORCA_SETTINGS_TAB_PRINTER  = 2
} orca_settings_tab_t;

typedef struct {
    uint32_t            struct_size;
    orca_settings_tab_t tab;
    const char*         page_title;
    void              (*on_build)(orca_ui_page_t* page,
                                  const orca_ui_builder_t* builder,
                                  void* user_data);
} orca_slot_settings_page_t;

/* Sidebar-panel slot (Phase 4.1.2) — attaches a panel below the
 * existing sidebar. */
typedef struct {
    uint32_t    struct_size;
    const char* panel_title;
    void      (*on_build)(orca_ui_page_t* page,
                          const orca_ui_builder_t* builder,
                          void* user_data);
} orca_slot_sidebar_panel_t;

/* Menu-command slot (Phase 4.1.3) — adds an entry to the named menu. */
typedef enum {
    ORCA_MENU_FILE  = 0,
    ORCA_MENU_EDIT  = 1,
    ORCA_MENU_VIEW  = 2,
    ORCA_MENU_TOOLS = 3,
    ORCA_MENU_HELP  = 4
} orca_menu_t;

typedef struct {
    uint32_t    struct_size;
    orca_menu_t menu;
    const char* label;
    const char* shortcut;     /* may be NULL; e.g. "Ctrl+Shift+P" */
    void      (*on_click)(void* user_data);
} orca_slot_menu_command_t;

/* Device-tab slot (Phase 4.1.4) — adds a custom tab to the device pane. */
typedef struct {
    uint32_t    struct_size;
    const char* tab_title;
    /* Filter — printers whose vendor matches this string get the tab.
     * NULL or empty means show on all printers. */
    const char* match_vendor;
    void      (*on_build)(orca_ui_page_t* page,
                          const orca_ui_builder_t* builder,
                          void* user_data);
} orca_slot_device_tab_t;

/* ============ Manifest + permissions ============ */

/* Permission bits declared by the plugin in orca_plugin_manifest_t.permissions.
 *
 * For native shared-library plugins, permissions are honor-system: the engine
 * surfaces declared permissions to the user (install confirmation, settings
 * UI) but cannot prevent a native plugin from doing whatever its process can
 * do. For WASM plugins, the same bits drive Wasmtime import gating and ARE
 * enforced at runtime. Always declare the smallest accurate set. */
#define ORCA_PERM_NETWORK          (1ull << 0)
#define ORCA_PERM_FILESYSTEM_READ  (1ull << 1)
#define ORCA_PERM_FILESYSTEM_WRITE (1ull << 2)
#define ORCA_PERM_SETTINGS_READ    (1ull << 3)
#define ORCA_PERM_SETTINGS_WRITE   (1ull << 4)
#define ORCA_PERM_PROFILES_INSTALL (1ull << 5)
#define ORCA_PERM_DEVICE_CONTROL   (1ull << 6)
#define ORCA_PERM_SLICE_INTERCEPT  (1ull << 7)
#define ORCA_PERM_GCODE_MODIFY     (1ull << 8)
#define ORCA_PERM_UI_ATTACH        (1ull << 9)
#define ORCA_PERM_EVENTS_RAW       (1ull << 10)

/* Plugin self-description. Field order is part of the ABI; do not reorder
 * within a given ORCA_PLUGIN_ABI_VERSION. New fields must be appended after
 * a struct_size bump on both sides. The engine validates struct_size before
 * reading any other field. */
typedef struct {
    uint32_t    struct_size;  /* sizeof(orca_plugin_manifest_t); engine validates */
    const char* id;           /* reverse-DNS, e.g. "com.example.my-plugin" */
    const char* name;         /* human-readable */
    const char* version;      /* semver string, e.g. "1.0.0" */
    const char* author;
    const char* description;
    uint64_t    permissions;  /* bitfield of ORCA_PERM_* */
} orca_plugin_manifest_t;

/* ============ Host vtable ============ */

/* Logging callback. `level` follows the standard ordering used by the engine:
 *   0 = trace, 1 = debug, 2 = info, 3 = warn, 4 = error. */
typedef void (*orca_log_fn_t)(int level, const char* msg);

/* Raw event subscriber callback. Unlike orca_event_callback_t from c_api.h,
 * this variant carries the payload size so the plugin can survive seeing
 * future payload structs that grew. The plugin must treat `payload` as
 * borrowed and valid only for the duration of the call. */
typedef void (*orca_raw_event_callback_t)(
    orca_event_kind_t kind,
    const void*       payload,
    size_t            payload_size,
    void*             user_data);

/* The host vtable the engine populates and hands to the plugin via
 * orca_plugin_register. struct_size is at the top so engines older than the
 * plugin can be detected: the plugin must only read fields whose offset is
 * strictly less than struct_size. */
typedef struct orca_plugin_host {
    uint32_t struct_size;

    /* Logging. Safe to call from any thread. */
    orca_log_fn_t log;

    /* Session — the engine session this plugin is bound to. The returned
     * pointer is owned by the engine and valid until orca_plugin_unregister
     * returns. */
    orca_session_t* (*session)(void);

    /* Event subscription — wraps orca_events_subscribe from c_api.h.
     * The plugin must call events_unsubscribe for every id it received
     * before orca_plugin_unregister returns. */
    orca_subscription_id_t (*events_subscribe)(
        orca_event_kind_t     kind,
        orca_event_callback_t cb,
        void*                 user_data);
    void (*events_unsubscribe)(orca_subscription_id_t id);

    /* Raw escape-hatch subscriber — the "I know what I'm doing" backdoor.
     * Receives EVERY event whose bit is set in kind_mask, including future
     * event kinds the plugin was not built against. Requires
     * ORCA_PERM_EVENTS_RAW; the engine returns 0 (invalid id) otherwise. */
    orca_subscription_id_t (*events_subscribe_raw)(
        uint64_t                  kind_mask,
        orca_raw_event_callback_t cb,
        void*                     user_data);

    /* Preset / config access — wraps orca_presets_* from c_api.h. The
     * const variant is always available; presets_mut returns NULL if the
     * plugin lacks ORCA_PERM_SETTINGS_WRITE. */
    const orca_presets_t* (*presets_const)(void);
    orca_presets_t*       (*presets_mut)(void);

    /* Profile-pack loader — wraps PresetBundle::load_vendor_configs_from_json.
     * Requires ORCA_PERM_PROFILES_INSTALL. */
    orca_error_code_t (*load_profile_pack)(const char* dir);

    /* PlaceholderParser variable injection (Phase 2.3). All three variants
     * upsert a single named variable into the engine's placeholder context. */
    orca_error_code_t (*placeholder_set_string)(const char* name, const char* value);
    orca_error_code_t (*placeholder_set_int)   (const char* name, int64_t     value);
    orca_error_code_t (*placeholder_set_float) (const char* name, double      value);

    /* PrinterAgent registration (Phase 2.4). `vtable` is an
     * orca_slot_printer_agent_t* once Phase 2.4 lands; today it is treated
     * opaquely. `self` is passed back into every agent vtable callback. */
    orca_error_code_t (*register_printer_agent)(
        const char* agent_id,
        const void* vtable,
        void*       self);

    /* String free — for the few host calls that hand owned UTF-8 buffers
     * back to the plugin. Pairs with the same allocator the engine uses
     * internally so cross-DLL frees stay safe. */
    void (*string_free)(const char* s);
} orca_plugin_host_t;

/* ============ Registry surface ============ */

/* Functions the plugin INVOKES on the registry during orca_plugin_register.
 * The registry is single-threaded for the duration of that call; the plugin
 * must not retain the pointer past it. */

/* Declare the plugin's manifest. Must be called exactly once before any
 * orca_registry_add_slot. Returns ORCA_ERR_ALREADY_EXISTS on a second call,
 * ORCA_ERR_INVALID_ARGUMENT if manifest->struct_size is wrong. */
orca_error_code_t orca_registry_set_manifest(
    orca_plugin_registry_t*       registry,
    const orca_plugin_manifest_t* manifest);

/* Register a slot implementation. `vtable` is a pointer to the slot-specific
 * struct selected by `kind` (the engine casts internally based on kind).
 * `user_data` is passed back into every vtable callback. Returns 0 on
 * failure; otherwise a non-zero id usable with orca_registry_remove_slot. */
orca_plugin_slot_id_t orca_registry_add_slot(
    orca_plugin_registry_t* registry,
    orca_slot_kind_t        kind,
    const void*             vtable,
    void*                   user_data);

/* Remove a previously added slot. After return the engine will not invoke
 * the slot's vtable again. */
orca_error_code_t orca_registry_remove_slot(
    orca_plugin_registry_t* registry,
    orca_plugin_slot_id_t   slot_id);

/* ============ Mandatory plugin exports ============ */

/* The three symbols every plugin shared library MUST export. The engine
 * resolves them by name via dlsym / GetProcAddress and calls them in the
 * order documented below. */

/* Called once when the engine loads the library, after
 * orca_plugin_check_debug_consistent has succeeded. The plugin uses
 * `registry` to declare itself and `host` to learn how to call back into
 * the engine. Both pointers are owned by the engine; only `host` is valid
 * after this call returns.
 *
 * Returns ORCA_OK on success; any other code causes the engine to log the
 * error and unload the library. The plugin should return
 * ORCA_ERR_UNSUPPORTED if abi_version != ORCA_PLUGIN_ABI_VERSION. */
ORCA_PLUGIN_EXPORT orca_error_code_t orca_plugin_register(
    uint32_t                  abi_version,
    orca_plugin_registry_t*   registry,
    const orca_plugin_host_t* host);

/* Called once just before the engine dlclose()s the library. The plugin
 * tears down its state and unsubscribes any outstanding event handlers.
 * After this returns the plugin must not call any host function. */
ORCA_PLUGIN_EXPORT void orca_plugin_unregister(void);

/* Called by the engine BEFORE orca_plugin_register to detect debug/release
 * ABI mismatches that wreck C++ types across the boundary. Mirrors
 * src/slic3r/Utils/BBLNetworkPlugin.cpp:222-232.
 *
 * `engine_is_debug` is non-zero if the engine was built with debug C++
 * runtime, zero otherwise. The plugin returns 0 if its own debug/release
 * status matches, non-zero if it differs. A non-zero return aborts the
 * load with a clear error before any C++ types cross the boundary. */
ORCA_PLUGIN_EXPORT int orca_plugin_check_debug_consistent(int engine_is_debug);

#ifdef __cplusplus
}
#endif

#endif /* ORCA_PLUGIN_API_H */
