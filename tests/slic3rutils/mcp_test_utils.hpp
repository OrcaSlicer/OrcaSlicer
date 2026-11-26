#pragma once

#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/NativeCommands.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/window.h>

#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Fixtures of the slicer's MCP tests, which need the GUI library; those of the automation core are in
// automation/automation_test_utils.hpp.
namespace Slic3r { namespace GUI { namespace Mcp { namespace Test {

// Every built-in command, the Design tab's among them, as a run with the CAD feature on holds them; sealed.
inline CommandCatalog catalog_with_cad()
{
    CommandCatalog catalog;
    NativeCommands::register_all_commands(catalog, {true});
    catalog.seal();
    return catalog;
}

// A call's context without the MCP core, for a case that runs a command's function itself: UI closures run in place,
// an observation answers true without running, and there are no child calls. It records what the call journals, the
// operations it begins and the commands it submits as jobs.
class TestContext final : public ToolContext
{
public:
    explicit TestContext(bool person = false) : m_person(person) {}

    bool                  person() const override { return m_person; }
    nlohmann::json        run_ui(const std::function<nlohmann::json()>& fn) override { return fn(); }
    nlohmann::json        observe_ui(const std::function<nlohmann::json()>& /*fn*/) override { return true; }
    const std::string&    job_id() const override { return m_job; }
    bool                  cancelled() const override { return false; }
    std::set<std::string> lineage() const override { return {}; }
    void                  set_journal_args(nlohmann::json args) override { journaled = std::move(args); }
    void                  begin_operation() override { ++operations; }
    ToolResult call_child(const std::string& /*tool*/, const nlohmann::json& /*args*/) override
    {
        return ToolResult::error("no child calls");
    }
    ToolResult submit(const std::string& key, const nlohmann::json& /*args*/) override
    {
        submitted.push_back(key);
        return ToolResult::ok();
    }

    int                      operations = 0;
    std::vector<std::string> submitted;
    nlohmann::json           journaled; // what the call last recorded for the journal; null until it records any

private:
    bool        m_person;
    std::string m_job;
};

// A call's context that fails the case when the call reaches its UI closure.
class NoUiContext final : public ToolContext
{
public:
    nlohmann::json run_ui(const std::function<nlohmann::json()>& /*fn*/) override
    {
        FAIL("the call reached its UI closure");
        return nlohmann::json();
    }
    nlohmann::json observe_ui(const std::function<nlohmann::json()>& /*fn*/) override
    {
        FAIL("the call reached its UI closure");
        return nlohmann::json();
    }
    const std::string&    job_id() const override { return m_job; }
    bool                  cancelled() const override { return false; }
    std::set<std::string> lineage() const override { return {}; }
    void                  set_journal_args(nlohmann::json /*args*/) override {}
    void                  begin_operation() override {}
    ToolResult call_child(const std::string& /*tool*/, const nlohmann::json& /*args*/) override
    {
        return ToolResult::error("no child calls");
    }

private:
    std::string m_job;
};

// A label that can't be read, as a widget in a bad state might fail.
class FailingLabel final : public wxStaticText
{
public:
    explicit FailingLabel(wxWindow* parent) : wxStaticText(parent, wxID_ANY, "label") {}
    wxString GetLabel() const override { throw std::runtime_error("unreadable"); }
};

}}}} // namespace Slic3r::GUI::Mcp::Test
