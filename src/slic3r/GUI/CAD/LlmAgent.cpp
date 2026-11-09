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
#include <mutex>
#include <sstream>
#include <stdexcept>
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

std::string clip(std::string s, size_t n)
{
    if (s.size() > n) { s.resize(n); s += "..."; }
    return s;
}

const char* kSystemPrompt =
    "You are the assistant inside Orca-CAD, a parametric CAD application. The user has a design open. Use the orca-cad MCP tools "
    "to inspect and edit it (units are millimetres); do not edit files on disk. Reply briefly: say what you did, not how the tools work.";

class Run final : public LlmAgentRun {
public:
    void cancel() override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_cancelled = true;
        if (m_child && m_child->running()) m_child->terminate();
    }
    void set_child(bp::child* c) { std::lock_guard<std::mutex> lk(m_mutex); m_child = c; }
    bool cancelled() { std::lock_guard<std::mutex> lk(m_mutex); return m_cancelled; }
private:
    std::mutex m_mutex;
    bp::child* m_child     = nullptr;
    bool       m_cancelled = false;
};

std::atomic<bool> g_busy{false};

} // namespace

static bool llm_agent_is_opencode() { return pref("llm_agent") == "opencode"; }

std::string llm_agent_executable()
{
    const std::string name = llm_agent_is_opencode() ? "opencode" : "claude";
    fs::path exe = pref("llm_cli_path");
    if (exe.empty()) exe = bp::search_path(name);
    // A desktop launcher does not run the shell profile, so PATH often lacks where the installer put the CLI.
    if (exe.empty())
        for (const char* d : {"/.local/bin", "/.claude/local", "/.npm-global/bin", "/.bun/bin", "/.opencode/bin"})
            if (const char* home = std::getenv("HOME"); home && fs::exists(fs::path(home + std::string(d)) / name)) { exe = fs::path(home + std::string(d)) / name; break; }
    if (exe.empty() && fs::exists("/usr/local/bin/" + name)) exe = "/usr/local/bin/" + name;
    return (!exe.empty() && fs::exists(exe)) ? exe.string() : std::string();
}

std::shared_ptr<LlmAgentRun> llm_agent_start(const std::string& prompt, const std::string& session_id, const std::vector<std::string>& read_dirs,
                                             std::function<void(const LlmEvent&)> on_event,
                                             std::function<void(bool, const std::string&)> done)
{
    if (g_busy.exchange(true)) { done(false, "an agent run is already in progress"); return nullptr; }

    // Everything read from Preferences is copied here: the worker must not touch app_config.
    const bool        oc     = llm_agent_is_opencode();
    const std::string exe    = llm_agent_executable();
    const std::string model  = pref("llm_model");
    const std::string extra  = pref("llm_cli_args");
    const std::string sock   = mcp_socket_path();
    const std::string bridge = bridge_script();
    std::string problem;
    if (exe.empty())                     problem = std::string(oc ? "opencode" : "Claude Code") + " was not found: install it so that `" + (oc ? "opencode" : "claude") + "` is on PATH, or set its path in Preferences > LLM / MCP";
    else if (oc && model.empty())        problem = "opencode needs a model in Preferences > LLM / MCP, written provider/model (see `opencode models`)";
    else if (sock.empty())               problem = "the MCP socket is off (ORCA_CAD_MCP=0)";
    else if (!fs::exists(bridge))        problem = "the MCP bridge script is missing: " + bridge;
    else if (bp::search_path("python3").empty()) problem = "python3 is needed to run the MCP bridge and was not found on PATH";
    if (!problem.empty()) { g_busy = false; done(false, problem); return nullptr; }

    auto run = std::make_shared<Run>();
    std::thread([=]() {
        bool        ok = false;
        std::string err;
        const fs::path tmp = fs::temp_directory_path() / fs::unique_path("orca-llm-%%%%-%%%%");
        const fs::path cfg = tmp.string() + ".mcp.json";
        const fs::path errf = tmp.string() + ".err";
        auto post = [](std::function<void()> f) { wxGetApp().CallAfter(std::move(f)); };
        bool got_result = false;
        try {
            std::vector<std::string> args;
            std::vector<std::string> env_kv;   // opencode only
            fs::path start_dir;
            if (oc) {
                // The isolation below (--standalone + private config) exists from opencode 2; v1 would run with the user's global config.
                bp::ipstream vout;
                bp::child vc(fs::path(exe), "--version", bp::std_out > vout, bp::std_err > bp::null, bp::std_in.close());
                std::string ver; std::getline(vout, ver); vc.wait();
                const size_t digit = ver.find_first_of("0123456789");
                if (digit == std::string::npos || std::atoi(ver.c_str() + digit) < 2)
                    throw std::runtime_error("opencode 2 or newer is required (found '" + ver + "'); set the path of a newer one in Preferences > LLM / MCP");
                // opencode reads ONLY this config (its own XDG_CONFIG_HOME): the user's global servers and shell access are not in play.
                // Everything is denied except reading and the orca-cad MCP, whose tools opencode exposes through its sandboxed
                // JavaScript `execute` tool (no shell, no require, no filesystem). Verified with opencode 2.2.5.
                fs::create_directories(tmp.string() + ".oc/opencode");
                // Anything not listed is denied, never "ask": with no one to answer, a prompt would hang the turn. opencode parks oversized
                // tool results in its data dir and the agent reads them back from there.
                const char* home = std::getenv("HOME");
                nlohmann::json ext = {{"*", "deny"}, {"/tmp/**", "allow"}};
                if (home) ext[std::string(home) + "/.local/share/opencode/tool-output/**"] = "allow";
                for (const std::string& d : read_dirs) ext[d + "/**"] = "allow";
                nlohmann::json oj = {
                    {"permission", {{"bash", "deny"}, {"edit", "deny"}, {"write", "deny"}, {"patch", "deny"}, {"webfetch", "deny"},
                                    {"websearch", "deny"}, {"task", "deny"}, {"todowrite", "deny"}, {"skill", "deny"},
                                    {"read", "allow"}, {"glob", "allow"}, {"grep", "allow"}, {"external_directory", ext}}},
                    {"mcp", {{"orca-cad", {{"type", "local"}, {"command", {"python3", bridge, sock}}, {"enabled", true}}}}},
                    {"autoupdate", false}};
                std::ofstream((tmp.string() + ".oc/opencode/opencode.json").c_str()) << oj.dump();
                start_dir = tmp.string() + ".oc";
                args = {"run", "--standalone", "--format", "json", "--model", model};
                if (!session_id.empty()) { args.push_back("--session"); args.push_back(session_id); }
                args.push_back(std::string(kSystemPrompt) + "\n\n" + prompt);
            } else {
            nlohmann::json j = {{"mcpServers", {{"orca-cad", {{"command", "python3"}, {"args", {bridge, sock}}}}}}};
            std::ofstream(cfg.string()) << j.dump();
            args = {"-p", prompt, "--output-format", "stream-json", "--verbose",
                                             "--mcp-config", cfg.string(), "--strict-mcp-config", "--allowedTools", "mcp__orca-cad,Read",
                                             "--append-system-prompt", kSystemPrompt};
            for (const std::string& d : read_dirs) { args.push_back("--add-dir"); args.push_back(d); }
            if (!session_id.empty()) { args.push_back("--resume"); args.push_back(session_id); }
            if (!model.empty()) { args.push_back("--model"); args.push_back(model); }
            }
            for (const std::string& a : split_args(extra)) args.push_back(a);

            bp::ipstream out;
            bp::environment env = boost::this_process::environment();
            if (oc) env["XDG_CONFIG_HOME"] = tmp.string() + ".oc";
            bp::child c(fs::path(exe), bp::args(args), bp::env = env, bp::start_dir = start_dir.empty() ? fs::temp_directory_path() : start_dir,
                        bp::std_out > out, bp::std_err > errf.string(), bp::std_in.close());
            run->set_child(&c);
            std::string line, oc_session, oc_text;
            while (std::getline(out, line)) {
                if (line.empty() || line[0] != '{') continue;
                nlohmann::json ev = nlohmann::json::parse(line, nullptr, false);
                if (ev.is_discarded()) continue;
                const std::string type = ev.value("type", "");
                if (oc) {
                    // opencode --format json: step_start / text / tool_use / step_finish, each with sessionID and a "part".
                    oc_session = ev.value("sessionID", oc_session);
                    const nlohmann::json part = ev.contains("part") ? ev["part"] : nlohmann::json::object();
                    LlmEvent e;
                    if (type == "text") { e.kind = LlmEvent::Text; e.text = part.value("text", ""); oc_text = e.text; }
                    else if (type == "tool_use") {
                        e.kind = LlmEvent::Tool;
                        const nlohmann::json in = part.contains("state") && part["state"].contains("input") ? part["state"]["input"] : nlohmann::json::object();
                        e.text = part.value("tool", "") + "  " + clip(in.value("code", in.dump()), 200);
                    } else continue;
                    if (!e.text.empty()) post([on_event, e]() { on_event(e); });
                    continue;
                }
                if (type == "assistant" && ev.contains("message") && ev["message"].contains("content") && ev["message"]["content"].is_array()) {
                    for (const auto& blk : ev["message"]["content"]) {
                        const std::string bt = blk.value("type", "");
                        LlmEvent e;
                        if (bt == "text") { e.kind = LlmEvent::Text; e.text = blk.value("text", ""); }
                        else if (bt == "tool_use") {
                            std::string name = blk.value("name", "");
                            if (name.rfind("mcp__orca-cad__", 0) == 0) name = name.substr(15);
                            e.kind = LlmEvent::Tool;
                            e.text = name + "  " + clip(blk.contains("input") ? blk["input"].dump() : std::string(), 200);
                        } else continue;
                        if (!e.text.empty()) post([on_event, e]() { on_event(e); });
                    }
                } else if (type == "result") {
                    LlmEvent e;
                    e.kind       = LlmEvent::Result;
                    e.text       = ev.value("result", "");
                    e.session_id = ev.value("session_id", "");
                    e.error      = ev.value("is_error", false);
                    got_result   = true;
                    post([on_event, e]() { on_event(e); });
                }
            }
            c.wait();
            run->set_child(nullptr);
            if (oc && !oc_text.empty() && !run->cancelled()) {
                // opencode has no final "result" event: its last text is the reply.
                LlmEvent e; e.kind = LlmEvent::Result; e.text = oc_text; e.session_id = oc_session; got_result = true;
                post([on_event, e]() { on_event(e); });
            }
            ok = got_result && c.exit_code() == 0;
            if (run->cancelled()) err = "stopped";
            else if (!ok) {
                std::ifstream in(errf.string());
                std::stringstream ss; ss << in.rdbuf();
                err = ss.str();
                if (err.empty()) err = "the agent exited with code " + std::to_string(c.exit_code());
            }
        } catch (const std::exception& e) {
            err = e.what();
        }
        boost::system::error_code ec;
        fs::remove(cfg, ec); fs::remove(errf, ec); fs::remove_all(tmp.string() + ".oc", ec);
        g_busy = false;
        post([done, ok, err]() { done(ok, err); });
    }).detach();
    return run;
}

void llm_agent_run(const std::string& prompt, std::function<void(bool, const std::string&)> done)
{
    auto reply = std::make_shared<std::string>();
    llm_agent_start(prompt, "", {},
        [reply](const LlmEvent& e) { if (e.kind == LlmEvent::Result) *reply = e.text; },
        [reply, done](bool ok, const std::string& err) { done(ok, ok ? *reply : err); });
}

}} // namespace Slic3r::GUI
