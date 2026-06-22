// Phase 2.4.3 — PrinterAgentAdapter implementation.

#include "PrinterAgentAdapter.hpp"

#include <cstring>
#include <utility>

namespace orca {

// ----- Thunks -----------------------------------------------------------

void PrinterAgentAdapter::emit_status_thunk(void*                host_ctx,
                                            orca_printer_state_t state,
                                            int                  code,
                                            const char*          message)
{
    auto* self = static_cast<PrinterAgentAdapter*>(host_ctx);
    if (self == nullptr)
        return;
    OnStatus cb;
    {
        std::lock_guard<std::mutex> lock(self->callbacks_mutex_);
        cb = self->on_status_cb_;
    }
    if (!cb)
        return;
    PrinterAgentStatus status;
    status.state   = static_cast<PrinterAgentState>(state);
    status.code    = code;
    status.message = (message != nullptr) ? std::string{message} : std::string{};
    cb(status);
}

void PrinterAgentAdapter::emit_message_thunk(void*       host_ctx,
                                             const char* payload,
                                             size_t      payload_len)
{
    auto* self = static_cast<PrinterAgentAdapter*>(host_ctx);
    if (self == nullptr || payload == nullptr)
        return;
    OnMessage cb;
    {
        std::lock_guard<std::mutex> lock(self->callbacks_mutex_);
        cb = self->on_message_cb_;
    }
    if (!cb)
        return;
    cb(std::string_view{payload, payload_len});
}

int PrinterAgentAdapter::is_cancelled_thunk(void* host_ctx)
{
    auto* self = static_cast<PrinterAgentAdapter*>(host_ctx);
    if (self == nullptr)
        return 0;
    IsCancelled cb;
    {
        std::lock_guard<std::mutex> lock(self->callbacks_mutex_);
        cb = self->on_is_cancelled_cb_;
    }
    if (!cb)
        return 0;
    return cb() ? 1 : 0;
}

// ----- Lifecycle --------------------------------------------------------

PrinterAgentAdapter::PrinterAgentAdapter(const orca_slot_printer_agent_t* vtable,
                                         void*                            vtable_user_data,
                                         PrinterAgentInfo                 info)
    : vtable_{vtable}
    , vtable_user_data_{vtable_user_data}
    , info_{std::move(info)}
{
    host_.struct_size  = sizeof(host_);
    host_.host_ctx     = this;
    host_.emit_status  = &PrinterAgentAdapter::emit_status_thunk;
    host_.emit_message = &PrinterAgentAdapter::emit_message_thunk;
    host_.is_cancelled = &PrinterAgentAdapter::is_cancelled_thunk;
}

PrinterAgentAdapter::~PrinterAgentAdapter()
{
    if (instance_ != nullptr && vtable_ != nullptr && vtable_->destroy_instance != nullptr) {
        vtable_->destroy_instance(instance_);
    }
    instance_ = nullptr;
}

Result<void> PrinterAgentAdapter::initialize()
{
    if (vtable_ == nullptr || vtable_->create_instance == nullptr) {
        return err(ErrorCode::InvalidArgument, "printer agent vtable is missing create_instance");
    }
    instance_ = vtable_->create_instance(&host_, vtable_user_data_);
    if (instance_ == nullptr) {
        return err(ErrorCode::InvalidState, "printer agent create_instance returned NULL");
    }
    return ok();
}

// ----- orca::PrinterAgent implementation -------------------------------

const PrinterAgentInfo& PrinterAgentAdapter::info() const
{
    return info_;
}

Result<void> PrinterAgentAdapter::connect(const PrinterConnection& target)
{
    if (vtable_->connect == nullptr)
        return err(ErrorCode::NotImplemented, "printer agent does not implement connect");
    const auto rc = vtable_->connect(instance_,
                                     target.device_id.c_str(),
                                     target.host.c_str(),
                                     target.port,
                                     target.username.c_str(),
                                     target.password.c_str(),
                                     target.use_tls ? 1 : 0);
    if (rc != ORCA_OK)
        return err(ErrorCode::InvalidState, "printer agent connect returned non-OK");
    return ok();
}

Result<void> PrinterAgentAdapter::disconnect()
{
    if (vtable_->disconnect == nullptr)
        return err(ErrorCode::NotImplemented, "printer agent does not implement disconnect");
    const auto rc = vtable_->disconnect(instance_);
    if (rc != ORCA_OK)
        return err(ErrorCode::InvalidState, "printer agent disconnect returned non-OK");
    return ok();
}

PrinterAgentState PrinterAgentAdapter::state() const
{
    if (vtable_->current_state == nullptr)
        return PrinterAgentState::Disconnected;
    return static_cast<PrinterAgentState>(vtable_->current_state(instance_));
}

Result<void> PrinterAgentAdapter::send_command(std::string_view payload)
{
    if (vtable_->send_command == nullptr)
        return err(ErrorCode::NotImplemented, "printer agent does not implement send_command");
    const auto rc = vtable_->send_command(instance_, payload.data(), payload.size());
    if (rc != ORCA_OK)
        return err(ErrorCode::InvalidState, "printer agent send_command returned non-OK");
    return ok();
}

Result<void> PrinterAgentAdapter::start_print(const PrintJob& job)
{
    if (vtable_->start_print == nullptr)
        return err(ErrorCode::NotImplemented, "printer agent does not implement start_print");
    const auto rc = vtable_->start_print(instance_,
                                         job.gcode_path.c_str(),
                                         job.job_name.c_str(),
                                         job.start_immediately ? 1 : 0);
    if (rc != ORCA_OK)
        return err(ErrorCode::InvalidState, "printer agent start_print returned non-OK");
    return ok();
}

Result<void> PrinterAgentAdapter::cancel_print()
{
    if (vtable_->cancel_print == nullptr)
        return err(ErrorCode::NotImplemented, "printer agent does not implement cancel_print");
    const auto rc = vtable_->cancel_print(instance_);
    if (rc != ORCA_OK)
        return err(ErrorCode::InvalidState, "printer agent cancel_print returned non-OK");
    return ok();
}

void PrinterAgentAdapter::on_status(OnStatus cb)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    on_status_cb_ = std::move(cb);
}

void PrinterAgentAdapter::on_message(OnMessage cb)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    on_message_cb_ = std::move(cb);
}

void PrinterAgentAdapter::on_is_cancelled(IsCancelled cb)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    on_is_cancelled_cb_ = std::move(cb);
}

} // namespace orca
