/*
 * Pure-C smoke test for OrcaSlicer's engine C ABI (Phase 0.6.2).
 *
 * Validates only what the engine actually does today with no PresetBundle
 * or Model attached: session lifecycle, service accessors, last-error
 * round-trip, event subscribe/unsubscribe, and the deterministic stub
 * surfaces (Project::clear, Project::save, opt_int without a bundle).
 *
 * This file MUST compile as C11. No C++ symbols, no extern "C".
 */

#include "orca/c_api.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
static int g_pass = 0;

#define EXPECT(cond, msg) do { \
    if (cond) { ++g_pass; } else { \
        ++g_fail; \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); \
    } \
} while (0)

static int g_evt_seen = 0;
static void on_event(orca_event_kind_t kind, const void* payload, void* ud) {
    (void)kind; (void)payload; (void)ud;
    g_evt_seen = 1;
}

int main(void) {
    /* 1. Null-arg safety on session accessors. */
    orca_session_destroy(NULL);                              /* must not crash */
    EXPECT(orca_session_presets(NULL)  == NULL, "presets(NULL) returns NULL");
    EXPECT(orca_session_project(NULL)  == NULL, "project(NULL) returns NULL");
    EXPECT(orca_session_slicer(NULL)   == NULL, "slicer(NULL) returns NULL");
    EXPECT(orca_session_exporter(NULL) == NULL, "exporter(NULL) returns NULL");
    EXPECT(orca_session_events(NULL)   == NULL, "events(NULL) returns NULL");

    /* 2. Last-error message accessor is never NULL — returns "" before any error. */
    EXPECT(orca_last_error_message() != NULL, "last_error_message never NULL");

    /* 3. Session lifecycle: create succeeds, all service accessors return non-NULL. */
    orca_session_t* s = orca_session_create();
    EXPECT(s != NULL, "session_create non-NULL");
    if (!s) {
        fprintf(stderr, "engine_c_abi: %d passed, %d failed\n", g_pass, g_fail);
        return 1;
    }
    orca_presets_t*  presets  = orca_session_presets(s);
    orca_project_t*  project  = orca_session_project(s);
    orca_slicer_t*   slicer   = orca_session_slicer(s);
    orca_exporter_t* exporter = orca_session_exporter(s);
    orca_events_t*   events   = orca_session_events(s);
    EXPECT(presets  != NULL, "presets accessor non-NULL");
    EXPECT(project  != NULL, "project accessor non-NULL");
    EXPECT(slicer   != NULL, "slicer accessor non-NULL");
    EXPECT(exporter != NULL, "exporter accessor non-NULL");
    EXPECT(events   != NULL, "events accessor non-NULL");

    /* 4. Project stubs return UNSUPPORTED with the engine's stub message. */
    {
        orca_error_code_t rc = orca_project_clear(project);
        EXPECT(rc == ORCA_ERR_UNSUPPORTED, "project_clear returns UNSUPPORTED");
        const char* msg = orca_last_error_message();
        EXPECT(msg != NULL, "last_error_message non-NULL after clear");
        EXPECT(msg != NULL && strstr(msg, "Project::clear") != NULL,
               "stub message contains Project::clear");
    }
    {
        orca_error_code_t rc = orca_project_save(project, "/tmp/orca_c_abi_save.3mf");
        EXPECT(rc == ORCA_ERR_UNSUPPORTED, "project_save returns UNSUPPORTED");
    }
    EXPECT(orca_project_object_count(project) == 0, "project_object_count == 0");

    /* 5. Presets — no bundle attached. */
    EXPECT(orca_presets_extruder_count(presets) == 0,
           "extruder_count == 0 (no bundle)");
    EXPECT(orca_presets_is_bbl_vendor(presets) == false,
           "is_bbl_vendor == false (no bundle)");
    {
        int32_t out = 0;
        orca_error_code_t rc = orca_presets_opt_int(presets, ORCA_SCOPE_FULL,
                                                    "layer_height", &out);
        EXPECT(rc != ORCA_OK, "opt_int(no bundle) returns non-OK");
        const char* msg = orca_last_error_message();
        EXPECT(msg != NULL, "last_error_message non-NULL after opt_int");
        EXPECT(msg != NULL && strstr(msg, "no PresetBundle attached") != NULL,
               "no-bundle message present");
    }

    /* 6. Slicer state — no work issued yet. */
    EXPECT(orca_slicer_is_busy(slicer) == false, "slicer not busy initially");

    /* 7. Events: subscribe returns id > 0, unsubscribe must not crash,
     *    bad inputs return 0. */
    {
        orca_subscription_id_t id = orca_events_subscribe(
            events, ORCA_EVT_SLICING_PROGRESS, on_event, NULL);
        EXPECT(id > 0, "subscribe returns non-zero id");
        orca_events_unsubscribe(events, id);                  /* must not crash */
        EXPECT(orca_events_subscribe(events, ORCA_EVT_SLICING_PROGRESS,
                                     NULL, NULL) == 0,
               "subscribe with cb=NULL returns 0");
        EXPECT(orca_events_subscribe(NULL, ORCA_EVT_SLICING_PROGRESS,
                                     on_event, NULL) == 0,
               "subscribe with events=NULL returns 0");
    }

    /* Reference the unused callback flag so a future signal-firing assertion
     * can pick it up without the compiler warning today. */
    (void)g_evt_seen;

    /* 8. Lifecycle loop — create/destroy must be idempotent across repeats. */
    orca_session_destroy(s);
    for (int i = 0; i < 5; ++i) {
        orca_session_t* t = orca_session_create();
        EXPECT(t != NULL, "repeated session_create non-NULL");
        orca_session_destroy(t);
    }

    fprintf(stderr, "engine_c_abi: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
