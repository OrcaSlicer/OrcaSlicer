/* Phase 3.4.1 — minimal native plugin that registers a real
 * ORCA_SLOT_PIPELINE_OBSERVER vtable. The observer is a no-op: on_step
 * does nothing. This proves the native runtime can carry a properly
 * shaped slot vtable end-to-end through PluginManager → PluginRegistry.
 *
 * Contrast with tests/engine/fixtures/loader_plugin which registers a
 * marker int as the "vtable" — that's enough for loader smoke but would
 * crash if Print::dispatch_step actually invoked it. This fixture is
 * dispatch-safe.
 */
#include "orca/plugin_api.h"

#include <stddef.h>

/* Counter the host can read by walking the slot's user_data pointer. */
static int g_step_count = 0;

static void noop_on_step(orca_pipeline_step_t step,
                         uint64_t              slice_handle,
                         void*                 user_data)
{
    (void)step;
    (void)slice_handle;
    int* counter = (int*)user_data;
    if (counter) (*counter)++;
}

static const orca_slot_pipeline_observer_t g_observer_vtable = {
    .struct_size = sizeof(orca_slot_pipeline_observer_t),
    .on_step     = noop_on_step,
};

static orca_plugin_slot_id_t g_slot_id = 0;

ORCA_PLUGIN_EXPORT int orca_plugin_check_debug_consistent(int engine_is_debug) {
    (void)engine_is_debug;
    return 0;
}

ORCA_PLUGIN_EXPORT orca_error_code_t orca_plugin_register(
    uint32_t                  abi_version,
    orca_plugin_registry_t*   registry,
    const orca_plugin_host_t* host)
{
    (void)host;
    if (abi_version != ORCA_PLUGIN_ABI_VERSION) return ORCA_ERR_UNSUPPORTED;
    if (!registry) return ORCA_ERR_INVALID_ARGUMENT;

    orca_plugin_manifest_t manifest = {0};
    manifest.struct_size = sizeof(manifest);
    manifest.id          = "test.fixture.native_noop";
    manifest.name        = "Native Noop Observer";
    manifest.version     = "0.0.1";
    manifest.author      = "OrcaSlicer Tests";
    manifest.description = "Phase 3.4.1 native runtime smoke fixture";
    manifest.permissions = 0;
    if (orca_registry_set_manifest(registry, &manifest) != ORCA_OK)
        return ORCA_ERR_UNKNOWN;

    g_slot_id = orca_registry_add_slot(
        registry,
        ORCA_SLOT_PIPELINE_OBSERVER,
        &g_observer_vtable,
        &g_step_count);
    return g_slot_id == 0 ? ORCA_ERR_UNKNOWN : ORCA_OK;
}

ORCA_PLUGIN_EXPORT void orca_plugin_unregister(void) {
    g_slot_id = 0;
}
