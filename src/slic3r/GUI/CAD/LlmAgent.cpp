#include "slic3r/GUI/CAD/LlmAgent.hpp"
#include "slic3r/GUI/CAD/McpControl.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/process.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

namespace fs = boost::filesystem;
namespace bp = boost::process;

std::string pref(const char* key)
{
    return wxGetApp().app_config ? wxGetApp().app_config->get(key) : std::string();
}

// The stdio<->socket MCP bridge shipped in resources/cad (a zero-dependency python3 script).
std::string bridge_script()
{
    if (const char* e = std::getenv("ORCA_CAD_MCP_BRIDGE"); e && *e) return e;
    return (fs::path(resources_dir()) / "cad" / "orca_cad_mcp_bridge.py").string();
}

std::vector<std::string> split_args(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

} // namespace

std::string llm_agent_executable()
{
    fs::path exe = pref("llm_cli_path");
    if (exe.empty()) exe = bp::search_path("claude");
    // A desktop launcher does not run the shell profile, so PATH often lacks where the installer put the CLI.
    if (exe.empty())
        for (const char* d : {"/.local/bin", "/.claude/local", "/.npm-global/bin", "/.bun/bin"})
            if (const char* home = std::getenv("HOME"); home && fs::exists(fs::path(home + std::string(d)) / "claude")) { exe = fs::path(home + std::string(d)) / "claude"; break; }
    if (exe.empty() && fs::exists("/usr/local/bin/claude")) exe = "/usr/local/bin/claude";
    return (!exe.empty() && fs::exists(exe)) ? exe.string() : std::string();
}

void llm_agent_run(const std::string& prompt, std::function<void(bool, const std::string&)> done)
{
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) { done(false, "an agent run is already in progress"); return; }

    // Everything read from Preferences is copied here: the worker must not touch app_config.
    const std::string exe    = llm_agent_executable();
    const std::string model  = pref("llm_model");
    const std::string extra  = pref("llm_cli_args");
    const std::string sock   = mcp_socket_path();
    const std::string bridge = bridge_script();
    std::string problem;
    if (exe.empty())                     problem = "Claude Code was not found: install it so that `claude` is on PATH, or set its path in Preferences > LLM / MCP";
    else if (sock.empty())               problem = "the MCP socket is off (ORCA_CAD_MCP=0)";
    else if (!fs::exists(bridge))        problem = "the MCP bridge script is missing: " + bridge;
    else if (bp::search_path("python3").empty()) problem = "python3 is needed to run the MCP bridge and was not found on PATH";
    if (!problem.empty()) { busy = false; done(false, problem); return; }

    std::thread([=]() {
        bool        ok = false;
        std::string text;
        const fs::path tmp = fs::temp_directory_path() / fs::unique_path("orca-llm-%%%%-%%%%");
        const fs::path cfg = tmp.string() + ".mcp.json";
        const fs::path out = tmp.string() + ".out";
        try {
            nlohmann::json j = {{"mcpServers", {{"orca-cad", {{"command", "python3"}, {"args", {bridge, sock}}}}}}};
            std::ofstream(cfg.string()) << j.dump();
            std::vector<std::string> args = {"-p", prompt, "--mcp-config", cfg.string(), "--strict-mcp-config",
                                             "--allowedTools", "mcp__orca-cad"};
            if (!model.empty()) { args.push_back("--model"); args.push_back(model); }
            for (const std::string& a : split_args(extra)) args.push_back(a);
            bp::child c(fs::path(exe), bp::args(args), (bp::std_out & bp::std_err) > out.string(), bp::std_in.close());
            if (!c.wait_for(std::chrono::minutes(10))) { c.terminate(); text = "the agent took longer than 10 minutes"; }
            else {
                std::ifstream in(out.string());
                std::stringstream ss; ss << in.rdbuf();
                text = ss.str();
                ok   = c.exit_code() == 0;
                if (!ok && text.empty()) text = "the agent exited with code " + std::to_string(c.exit_code());
            }
        } catch (const std::exception& e) {
            text = e.what();
        }
        boost::system::error_code ec;
        fs::remove(cfg, ec); fs::remove(out, ec);
        busy = false;
        wxGetApp().CallAfter([done, ok, text]() { done(ok, text); });
    }).detach();
}

}} // namespace Slic3r::GUI
