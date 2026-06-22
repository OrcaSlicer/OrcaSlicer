#pragma once
// Phase 2.4.3 — PrinterAgentAdapter — concrete orca::PrinterAgent that
// dispatches to a plugin-provided orca_slot_printer_agent_t C-vtable.
// Instantiated by Session::create_printer_agent (Phase 2.4.4); the engine
// caller sees a clean orca::PrinterAgent interface and never touches the
// raw vtable.
//
// Threading model: emit_status / emit_message / is_cancelled may be invoked
// from a plugin-owned thread. The Adapter snapshots its currently-registered
// std::function callbacks under a mutex on each emit dispatch.

#include "orca/PrinterAgent.hpp"
#include "orca/plugin_api.h"

#include <mutex>

namespace orca {

class PrinterAgentAdapter final : public PrinterAgent {
public:
    // The adapter takes ownership of calling vtable->destroy_instance on its
    // stored instance pointer in the destructor. vtable_user_data is the
    // user_data the registry stored at slot-registration time and is forwarded
    // to vtable->create_instance.
    PrinterAgentAdapter(const orca_slot_printer_agent_t* vtable,
                        void*                            vtable_user_data,
                        PrinterAgentInfo                 info);

    ~PrinterAgentAdapter() override;

    // Non-copyable, non-movable — the host_ctx points to `this` and must
    // remain stable.
    PrinterAgentAdapter(const PrinterAgentAdapter&)            = delete;
    PrinterAgentAdapter& operator=(const PrinterAgentAdapter&) = delete;
    PrinterAgentAdapter(PrinterAgentAdapter&&)                 = delete;
    PrinterAgentAdapter& operator=(PrinterAgentAdapter&&)      = delete;

    // Two-stage init: call after construction. Returns InvalidArgument if the
    // slot's create_instance is null (shouldn't happen per ABI contract) or
    // InvalidState if create_instance returned NULL.
    Result<void> initialize();

    // orca::PrinterAgent ----------------------------------------------------
    const PrinterAgentInfo& info() const override;
    Result<void>            connect(const PrinterConnection& target) override;
    Result<void>            disconnect() override;
    PrinterAgentState       state() const override;
    Result<void>            send_command(std::string_view payload) override;
    Result<void>            start_print(const PrintJob& job) override;
    Result<void>            cancel_print() override;
    void                    on_status(OnStatus cb) override;
    void                    on_message(OnMessage cb) override;
    void                    on_is_cancelled(IsCancelled cb) override;

private:
    // Thunks the host emit table points at. Reinterpret host_ctx as
    // PrinterAgentAdapter* and dispatch to the std::function callbacks.
    static void emit_status_thunk(void*                host_ctx,
                                  orca_printer_state_t state,
                                  int                  code,
                                  const char*          message);
    static void emit_message_thunk(void*       host_ctx,
                                   const char* payload,
                                   size_t      payload_len);
    static int  is_cancelled_thunk(void* host_ctx);

    // Resolved by initialize().
    const orca_slot_printer_agent_t* vtable_;
    void*                            vtable_user_data_;
    void*                            instance_{nullptr};
    orca_printer_host_t              host_{};
    PrinterAgentInfo                 info_;

    mutable std::mutex callbacks_mutex_;
    OnStatus           on_status_cb_;
    OnMessage          on_message_cb_;
    IsCancelled        on_is_cancelled_cb_;
};

} // namespace orca
