#include "McpPersonContext.hpp"

#include "McpCore.hpp"
#include "McpService.hpp"
#include "McpTypes.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/format.hpp"

#include <nlohmann/json.hpp>

#include <wx/app.h>
#include <wx/string.h>

#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

// What a person is shown once a job they started ends: what the command says, its error, or that it finished.
// `title` is the command's, in the UI's language (NativeCommands::tile_title()).
AppActionRunResult ended_notice(const ToolResult& result, const std::string& title)
{
    if (result.notice)
        return *result.notice;
    if (result.is_error)
        return {AppActionRunResult::Level::Error, from_u8(result.message), false};
    // TRN %1% is the title of a command a person started, which ran on in the background.
    return {AppActionRunResult::Level::Success, format_wxstr(_L("Finished: %1%"), from_u8(title))};
}

} // namespace

const std::string& PersonContext::job_id() const
{
    static const std::string none;
    return none;
}

bool PersonContext::cancelled() const
{
    // No GUI_App in a unit test, which runs commands with this context too.
    const auto* app = dynamic_cast<const GUI_App*>(wxTheApp);
    return app != nullptr && app->is_closing();
}

ToolResult PersonContext::call_child(const std::string& tool, const nlohmann::json& /*args*/)
{
    throw ToolFailure("A person's pick does not call " + tool);
}

McpService& PersonContext::service()
{
    auto* app = dynamic_cast<GUI_App*>(wxTheApp);
    if (app == nullptr || app->is_closing())
        throw ToolFailure("OrcaSlicer is closing");
    if (McpService* s = app->mcp_service(); s == nullptr || !s->started())
        app->start_mcp_service(false);
    McpService* s = app->mcp_service();
    if (s == nullptr || !s->started() || s->stopping())
        throw ToolFailure("The automation runtime could not start");
    return *s;
}

ToolResult PersonContext::submit(const std::string& key, const nlohmann::json& args)
{
    const std::shared_ptr<const NativeCommand> command = NativeCommands::instance().lookup(key);
    if (command == nullptr)
        throw ToolFailure("Unknown command " + key);
    McpService* const     owner   = &service();
    const std::thread::id caller  = std::this_thread::get_id();
    auto                  refusal = std::make_shared<std::optional<ToolResult>>();
    const std::string     title   = into_u8(NativeCommands::tile_title(*command));
    const std::string     job     = owner->core().submit(*command, args, [owner, caller, refusal, title](const ToolResult& r) {
        // A refusal comes at once, on this thread: it is the reply. A job's result comes on the job's thread,
        // while the service still runs it, and is shown on the UI thread.
        if (std::this_thread::get_id() == caller) {
            *refusal = r;
            return;
        }
        owner->post_ui([r, title] {
            if (wxTheApp != nullptr && !wxGetApp().is_closing())
                NativeCommands::notify_result(ended_notice(r, title));
        });
    });
    if (job.empty()) {
        const std::string why = *refusal ? (*refusal)->message : std::string("OrcaSlicer is closing");
        ToolResult        failed = ToolResult::error(why);
        failed.notice            = std::make_shared<const AppActionRunResult>(AppActionRunResult{AppActionRunResult::Level::Error, from_u8(why), false});
        return failed;
    }
    ToolResult started = ToolResult::ok({{"status", "running"}, {"job", job}, {"tool", key}});
    // TRN %1% is the title of a command a person started, which runs on in the background.
    const wxString shown = format_wxstr(_L("Started: %1%"), from_u8(title));
    started.notice       = std::make_shared<const AppActionRunResult>(AppActionRunResult{AppActionRunResult::Level::Info, shown});
    return started;
}

}}} // namespace Slic3r::GUI::Mcp
