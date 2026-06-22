// Phase 2.4.7 — printer agent slot end-to-end test.
//
// Proves the ORCA_SLOT_PRINTER_AGENT machinery end-to-end without needing a
// real plugin .so. We register a fake printer-agent vtable directly through
// the engine's PluginRegistry (same approach as test_slot_placeholder_provider
// and test_slot_gcode_filter) and then exercise:
//
//   1. Enumeration: Session::list_printer_agents() sees the slot, and
//      Session::has_printer_agent("<id>") returns true.
//   2. Instantiation: Session::create_printer_agent("<id>") routes through
//      the PrinterAgentAdapter and returns a usable orca::PrinterAgent.
//   3. Method dispatch: connect / disconnect / send_command / start_print /
//      cancel_print all reach the FakeInstance with the right args.
//   4. Host emit round-trip: when the plugin invokes host->emit_status /
//      emit_message / is_cancelled, the adapter routes the call into the
//      std::function callbacks the engine consumer registered via
//      PrinterAgent::on_status / on_message / on_is_cancelled.

#include <catch2/catch_all.hpp>

#include "orca/Session.hpp"
#include "orca/PrinterAgent.hpp"
#include "orca/plugin_api.h"

// PluginRegistry lives in engine/src/ (engine-internal). The engine_tests
// CMakeLists adds ${CMAKE_SOURCE_DIR}/engine/src to the include path.
#include "PluginRegistry.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <string>
#include <utility>

namespace {

// ---------------- Fake printer-agent plugin implementation ----------------
//
// The vtable's create_instance allocates a FakeInstance, stashes the host
// emit-table pointer it was handed, and parks a raw `this` pointer in
// g_last_fake_instance so the test body can reach it after the adapter has
// finished constructing the agent. This is the same "global handle to the
// fixture" pattern used by test_slot_pipeline.cpp's pipeline-observer test.

struct FakeInstance {
    const orca_printer_host_t* host{nullptr};

    // Recorded args from the most recent dispatch call.
    std::string          last_connect_device_id;
    std::string          last_connect_host;
    int                  last_connect_port{0};
    int                  last_connect_use_tls{0};
    std::string          last_send_payload;
    std::string          last_print_path;
    std::string          last_print_job_name;
    int                  last_print_start_immediately{0};
    orca_printer_state_t current_state{ORCA_PRINTER_STATE_DISCONNECTED};

    void fire_status(orca_printer_state_t s, int code, const char* msg) {
        host->emit_status(host->host_ctx, s, code, msg);
    }
    void fire_message(const std::string& payload) {
        host->emit_message(host->host_ctx, payload.data(), payload.size());
    }
    int query_is_cancelled() {
        return host->is_cancelled(host->host_ctx);
    }
};

// Reached by the test body to talk to the FakeInstance the adapter created.
// The fake's create_instance writes its `this` here; destroy_instance clears
// it. Tests run serially under Catch2 so a single global is safe.
FakeInstance* g_last_fake_instance = nullptr;

extern "C" void* fake_create_instance(const orca_printer_host_t* host,
                                      void* /*user_data*/) {
    auto* inst = new FakeInstance{};
    inst->host = host;
    g_last_fake_instance = inst;
    return inst;
}

extern "C" void fake_destroy_instance(void* p) {
    auto* inst = static_cast<FakeInstance*>(p);
    if (inst == g_last_fake_instance)
        g_last_fake_instance = nullptr;
    delete inst;
}

extern "C" orca_error_code_t fake_connect(void*       p,
                                          const char* dev_id,
                                          const char* host_or_ip,
                                          int         port,
                                          const char* /*user*/,
                                          const char* /*pw*/,
                                          int         use_tls) {
    auto* inst = static_cast<FakeInstance*>(p);
    inst->last_connect_device_id = dev_id ? dev_id : "";
    inst->last_connect_host      = host_or_ip ? host_or_ip : "";
    inst->last_connect_port      = port;
    inst->last_connect_use_tls   = use_tls;
    inst->current_state          = ORCA_PRINTER_STATE_CONNECTED;
    return ORCA_OK;
}

extern "C" orca_error_code_t fake_disconnect(void* p) {
    auto* inst = static_cast<FakeInstance*>(p);
    inst->current_state = ORCA_PRINTER_STATE_DISCONNECTED;
    return ORCA_OK;
}

extern "C" orca_printer_state_t fake_current_state(void* p) {
    return static_cast<FakeInstance*>(p)->current_state;
}

extern "C" orca_error_code_t fake_send_command(void*       p,
                                               const char* payload,
                                               size_t      payload_len) {
    auto* inst = static_cast<FakeInstance*>(p);
    inst->last_send_payload.assign(payload, payload_len);
    return ORCA_OK;
}

extern "C" orca_error_code_t fake_start_print(void*       p,
                                              const char* gcode_path,
                                              const char* job_name,
                                              int         start_immediately) {
    auto* inst = static_cast<FakeInstance*>(p);
    inst->last_print_path             = gcode_path ? gcode_path : "";
    inst->last_print_job_name         = job_name   ? job_name   : "";
    inst->last_print_start_immediately = start_immediately;
    return ORCA_OK;
}

extern "C" orca_error_code_t fake_cancel_print(void* /*p*/) {
    return ORCA_OK;
}

orca_slot_printer_agent_t make_fake_vtable() {
    orca_slot_printer_agent_t vt{};
    vt.struct_size       = sizeof(vt);
    vt.agent_id          = "fake.test_agent";
    vt.agent_name        = "Fake Test Agent";
    vt.agent_version     = "0.1.0";
    vt.agent_description = "Test fixture printer agent for Phase 2.4.7";
    vt.create_instance   = &fake_create_instance;
    vt.destroy_instance  = &fake_destroy_instance;
    vt.connect           = &fake_connect;
    vt.disconnect        = &fake_disconnect;
    vt.current_state     = &fake_current_state;
    vt.send_command      = &fake_send_command;
    vt.start_print       = &fake_start_print;
    vt.cancel_print      = &fake_cancel_print;
    return vt;
}

} // namespace

TEST_CASE("Phase 2.4.7 — printer agent slot is enumerable and instantiable",
          "[plugin][slots][printer_agent]") {
    auto session = orca::Session::create();
    REQUIRE(session);

    // The vtable's string fields are borrowed by the registry, so the storage
    // has to outlive every list/instantiate call below. A function-scope
    // static makes the lifetime explicit and matches how a real plugin would
    // keep its identity strings in its .rodata.
    static const orca_slot_printer_agent_t fake_vtable = make_fake_vtable();

    auto& registry = session->plugin_registry();
    registry.set_current_plugin_id("test.printer_agent");
    const auto slot_id = registry.add_slot(ORCA_SLOT_PRINTER_AGENT,
                                           &fake_vtable, nullptr, /*priority=*/0);
    REQUIRE(slot_id != 0);
    registry.clear_current_plugin_id();

    // Enumeration sees the fake.
    const auto list = session->list_printer_agents();
    REQUIRE_FALSE(list.empty());
    auto it = std::find_if(list.begin(), list.end(),
                           [](const orca::PrinterAgentInfo& info) {
                               return info.id == "fake.test_agent";
                           });
    REQUIRE(it != list.end());
    CHECK(it->name        == "Fake Test Agent");
    CHECK(it->version     == "0.1.0");
    CHECK(it->description == "Test fixture printer agent for Phase 2.4.7");

    CHECK(session->has_printer_agent("fake.test_agent"));
    CHECK_FALSE(session->has_printer_agent("does.not.exist"));

    // Instantiate the agent — exercises PrinterAgentAdapter end-to-end.
    auto agent_or = session->create_printer_agent("fake.test_agent");
    REQUIRE(agent_or.ok());
    auto agent = std::move(agent_or).value();
    REQUIRE(agent != nullptr);

    CHECK(agent->info().id          == "fake.test_agent");
    CHECK(agent->info().name        == "Fake Test Agent");
    CHECK(agent->info().version     == "0.1.0");
    CHECK(agent->info().description == "Test fixture printer agent for Phase 2.4.7");

    // Missing id returns NotFound (orca::ErrorCode::NotFound — same code the
    // adapter raises when no slot matches).
    auto missing = session->create_printer_agent("does.not.exist");
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == orca::ErrorCode::NotFound);

    auto removed = registry.remove_slot(slot_id);
    CHECK(removed.ok());
}

TEST_CASE("Phase 2.4.7 — printer agent dispatches commands + delivers emits",
          "[plugin][slots][printer_agent]") {
    auto session = orca::Session::create();
    REQUIRE(session);

    static const orca_slot_printer_agent_t fake_vtable = make_fake_vtable();
    auto& registry = session->plugin_registry();
    registry.set_current_plugin_id("test.printer_agent");
    const auto slot_id = registry.add_slot(ORCA_SLOT_PRINTER_AGENT,
                                           &fake_vtable, nullptr, /*priority=*/0);
    REQUIRE(slot_id != 0);
    registry.clear_current_plugin_id();

    auto agent_or = session->create_printer_agent("fake.test_agent");
    REQUIRE(agent_or.ok());
    auto agent = std::move(agent_or).value();
    REQUIRE(agent != nullptr);
    REQUIRE(g_last_fake_instance != nullptr);

    // Capture state for the host-emit round-trip checks. Atomics because the
    // host vtable is documented as callable from any thread (mirrors the
    // production threading model — the test calls them synchronously from
    // the main thread, but we still treat them as cross-thread to keep the
    // pattern honest).
    std::atomic<int>        status_invocations{0};
    std::atomic<int>        message_invocations{0};
    std::string             last_status_message;
    std::string             last_message;
    orca::PrinterAgentState last_state{orca::PrinterAgentState::Disconnected};

    agent->on_status([&](const orca::PrinterAgentStatus& s) {
        status_invocations.fetch_add(1, std::memory_order_relaxed);
        last_state          = s.state;
        last_status_message = s.message;
    });

    agent->on_message([&](std::string_view payload) {
        message_invocations.fetch_add(1, std::memory_order_relaxed);
        last_message = std::string{payload};
    });

    std::atomic<bool> cancellation_requested{false};
    agent->on_is_cancelled([&] { return cancellation_requested.load(); });

    SECTION("connect / disconnect / send_command / start_print / cancel_print") {
        orca::PrinterConnection conn;
        conn.device_id = "dev-1";
        conn.host      = "192.168.0.10";
        conn.port      = 7125;
        conn.username  = "user";
        conn.password  = "pw";
        conn.use_tls   = true;

        REQUIRE(agent->connect(conn).ok());
        CHECK(g_last_fake_instance->last_connect_device_id == "dev-1");
        CHECK(g_last_fake_instance->last_connect_host      == "192.168.0.10");
        CHECK(g_last_fake_instance->last_connect_port      == 7125);
        CHECK(g_last_fake_instance->last_connect_use_tls   == 1);

        CHECK(agent->state() == orca::PrinterAgentState::Connected);

        REQUIRE(agent->send_command("M104 S200").ok());
        CHECK(g_last_fake_instance->last_send_payload == "M104 S200");

        orca::PrintJob job;
        const std::string job_path = std::string{TEST_TMP_DIR} + "/printer_agent_fake.gcode";
        job.gcode_path        = job_path;
        job.job_name          = "test-job";
        job.start_immediately = true;
        REQUIRE(agent->start_print(job).ok());
        CHECK(g_last_fake_instance->last_print_path              == job_path);
        CHECK(g_last_fake_instance->last_print_job_name          == "test-job");
        CHECK(g_last_fake_instance->last_print_start_immediately == 1);

        REQUIRE(agent->cancel_print().ok());

        REQUIRE(agent->disconnect().ok());
        CHECK(agent->state() == orca::PrinterAgentState::Disconnected);
    }

    SECTION("emit_status round-trip from plugin to host callback") {
        g_last_fake_instance->fire_status(ORCA_PRINTER_STATE_CONNECTED, 0, "ready");

        CHECK(status_invocations.load() == 1);
        CHECK(last_state == orca::PrinterAgentState::Connected);
        CHECK(last_status_message == "ready");
    }

    SECTION("emit_message round-trip from plugin to host callback") {
        const std::string payload = "{\"status\":\"printing\"}";
        g_last_fake_instance->fire_message(payload);

        CHECK(message_invocations.load() == 1);
        CHECK(last_message == payload);
    }

    SECTION("is_cancelled round-trip from host callback to plugin query") {
        CHECK(g_last_fake_instance->query_is_cancelled() == 0);
        cancellation_requested.store(true);
        CHECK(g_last_fake_instance->query_is_cancelled() == 1);
    }

    auto removed = registry.remove_slot(slot_id);
    CHECK(removed.ok());
}
