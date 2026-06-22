#include "orca/plugin_api.h"

#include <stddef.h>

/* Tiny vtable for an ORCA_SLOT_PIPELINE_OBSERVER. The Phase 2 spec says these
 * are fn-pointer structs; for Phase 1 the registry treats vtable as opaque
 * const void*, so we just register a non-null pointer to prove registration
 * mechanics work. */
static int g_observer_vtable_marker = 0xC0DE;

/* Trivial state retained between register and unregister, so we can prove
 * unregister was called. */
static int g_initialised = 0;
static orca_plugin_slot_id_t g_slot_id = 0;

ORCA_PLUGIN_EXPORT int orca_plugin_check_debug_consistent(int engine_is_debug) {
    /* Build matches the engine's debug/release status by construction (this
     * fixture is built with the same toolchain + flags as the engine). */
    (void)engine_is_debug;
    return 0;
}

ORCA_PLUGIN_EXPORT orca_error_code_t orca_plugin_register(
    uint32_t                  abi_version,
    orca_plugin_registry_t*   registry,
    const orca_plugin_host_t* host)
{
    if (abi_version != ORCA_PLUGIN_ABI_VERSION) return ORCA_ERR_UNSUPPORTED;
    if (!registry || !host) return ORCA_ERR_INVALID_ARGUMENT;

    /* The PluginManager already set our plugin id on the registry before
     * calling us, so add_slot is safe. */
    orca_plugin_manifest_t manifest = {0};
    manifest.struct_size = sizeof(manifest);
    manifest.id          = "test.fixture.loader_plugin";
    manifest.name        = "Loader Test Fixture";
    manifest.version     = "0.0.1";
    manifest.author      = "OrcaSlicer Tests";
    manifest.description = "Phase 1.5.1 loader smoke test fixture";
    manifest.permissions = 0;

    if (orca_registry_set_manifest(registry, &manifest) != ORCA_OK)
        return ORCA_ERR_UNKNOWN;

    g_slot_id = orca_registry_add_slot(
        registry,
        ORCA_SLOT_PIPELINE_OBSERVER,
        (const void*)&g_observer_vtable_marker,
        NULL);

    if (g_slot_id == 0) return ORCA_ERR_UNKNOWN;

    g_initialised = 1;
    return ORCA_OK;
}

ORCA_PLUGIN_EXPORT void orca_plugin_unregister(void) {
    g_initialised = 0;
    g_slot_id     = 0;
}
