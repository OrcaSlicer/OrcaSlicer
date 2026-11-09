#include <boost/filesystem/path.hpp>
#include <cctype>
#include <exception>
#include <functional>
#include <ios>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
#include <wx/string.h>
#include "slic3r/GUI/CAD/LlmAgent.hpp"
#include "slic3r/GUI/CAD/LlmApi.hpp"
#include "slic3r/Utils/Http.hpp"

#include <wx/secretstore.h>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Slic3r { namespace GUI {

namespace {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

const char* kService = "OrcaCAD LLM";

std::string clip(std::string s, size_t n)
{
    if (s.size() > n) { s.resize(n); s += "\n...[cut]"; }
    return s;
}

std::string base64(const std::string& in)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned v = (unsigned char) in[i] << 16 | (i + 1 < in.size() ? (unsigned char) in[i + 1] << 8 : 0) | (i + 2 < in.size() ? (unsigned char) in[i + 2] : 0);
        out += t[v >> 18 & 63];
        out += t[v >> 12 & 63];
        out += i + 1 < in.size() ? t[v >> 6 & 63] : '=';
        out += i + 2 < in.size() ? t[v & 63] : '=';
    }
    return out;
}

// One request/reply on the app's MCP socket (plain JSON-RPC lines, method = tool name).
json sock_call(const std::string& path, const std::string& method, const json& params)
{
#ifdef _WIN32
    throw std::runtime_error("the API backends need the Unix MCP socket, which is not available on Windows yet");
#else
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("cannot open a socket");
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
    timeval tv{120, 0};
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX socket constants; <sys/socket.h> is included (the check wants a glibc-internal header)
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (::connect(fd, (sockaddr*) &a, sizeof a) != 0) { ::close(fd); throw std::runtime_error("the app's MCP socket is unreachable: " + path); }
    const std::string req = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}}.dump() + "\n";
    for (size_t off = 0; off < req.size();) {
        const ssize_t n = ::send(fd, req.data() + off, req.size() - off, 0);
        if (n <= 0) { ::close(fd); throw std::runtime_error("write to the MCP socket failed"); }
        off += size_t(n);
    }
    std::string buf;
    char        chunk[65536];
    while (buf.empty() || buf.back() != '\n') {
        const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0) break;
        buf.append(chunk, size_t(n));
    }
    ::close(fd);
    json r = json::parse(buf, nullptr, false);
    if (r.is_discarded()) throw std::runtime_error("the app's MCP socket sent an unreadable reply");
    return r;
#endif
}

// describe_tools -> {name, description, JSON schema}, the same mapping the stdio bridge uses. No "required" list: the app validates
// every call itself and answers with an error the model can read.
struct Tool { std::string name, description; json schema; };

std::vector<Tool> load_tools(const std::string& sock)
{
    std::vector<Tool> out;
    const json r = sock_call(sock, "describe_tools", json::object());
    if (!r.contains("result") || !r["result"].contains("tools")) throw std::runtime_error("the app returned no tool list");
    for (const json& t : r["result"]["tools"]) {
        json props = json::object();
        for (const json& p : t.value("params", json::array())) {
            std::string type = p.value("type", "string");
            if (type != "number" && type != "integer" && type != "string" && type != "boolean" && type != "array" && type != "object") type = "string";
            json s = {{"type", type}};
            std::string desc = p.value("description", "");
            if (p.contains("unit")) desc += (desc.empty() ? "" : " ") + std::string("in ") + p["unit"].dump();
            if (!desc.empty()) s["description"] = desc;
            if (p.contains("enum")) s["enum"] = p["enum"];
            if (type == "array") s["items"] = json::object();   // some providers reject an array without items
            props[p.value("name", "")] = s;
        }
        out.push_back({t.value("name", ""), t.value("summary", ""), {{"type", "object"}, {"properties", props}}});
    }
    return out;
}

// Result text for the model, and whether the app reported a failure.
std::pair<std::string, bool> run_tool(const std::string& sock, const std::string& name, const json& args)
{
    try {
        const json r = sock_call(sock, name, args.is_object() ? args : json::object());
        if (r.contains("error")) return {clip(r["error"].dump(), 30000), true};
        const json res = r.value("result", json());
        return {clip(res.dump(2), 30000), res.is_object() && res.value("ok", true) == false};
    } catch (const std::exception& e) {
        return {e.what(), true};
    }
}

// Conversations live in memory, keyed by the session id handed back to the panel.
std::mutex                 g_mutex;
std::map<std::string, json> g_sessions;   // provider-format message arrays
int                        g_next = 0;

std::string http_post(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body,
                      const std::function<void(std::function<void()>)>& set_canceller, std::string& err)
{
    Http http = Http::post(url);
    for (const auto& h : headers) http.header(h.first, h.second);
    http.header("Content-Type", "application/json");
    http.set_post_body(body).timeout_connect(20).timeout_max(600);
    auto prom = std::make_shared<std::promise<std::pair<bool, std::string>>>();
    auto fut  = prom->get_future();
    http.on_complete([prom](std::string b, unsigned) { prom->set_value({true, std::move(b)}); })
        .on_error([prom](std::string b, std::string e, unsigned status) {
            // Providers put the useful sentence in the body ({"error":{"message":...}}).
            const json j = json::parse(b, nullptr, false);
            std::string msg;
            if (j.is_object() && j.contains("error")) msg = j["error"].is_object() ? j["error"].value("message", j["error"].dump()) : j["error"].dump();
            prom->set_value({false, "HTTP " + std::to_string(status) + ": " + (msg.empty() ? e : msg)});
        });
    Http::Ptr running = http.perform();
    set_canceller([running]() { running->cancel(); });
    auto r = fut.get();
    set_canceller({});
    if (!r.first) err = r.second;
    return r.second;
}

struct Attachments { std::vector<std::pair<std::string, std::string>> images; /* mime, base64 */ std::string text; std::string cad; };

// The panel appends "Attached files...: \n- path" to the prompt for the CLI agents. Here there is no Read tool, so images become
// image blocks, text files are inlined, and CAD files stay as paths for import_step (the app reads those itself).
std::string split_attachments(const std::string& prompt, Attachments& att)
{
    const size_t m = prompt.find("\n\nAttached files.");
    if (m == std::string::npos) return prompt;
    std::string head = prompt.substr(0, m);
    std::istringstream in(prompt.substr(m));
    std::string        line, tail;
    while (std::getline(in, line)) {
        if (line.rfind("- /", 0) != 0) {
            if (line.rfind("First look at", 0) == 0) tail += "\n\n" + line;
            continue;
        }
        const fs::path p = line.substr(2);
        std::string    ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::ifstream f(p.string(), std::ios::binary);
        if (!f) continue;
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".webp") {
            if (data.size() < 8u * 1024 * 1024)
                att.images.push_back({ext == ".png" ? "image/png" : ext == ".gif" ? "image/gif" : ext == ".webp" ? "image/webp" : "image/jpeg", base64(data)});
        } else if (ext == ".txt" || ext == ".md" || ext == ".csv" || ext == ".json" || ext == ".yaml" || ext == ".yml" || ext == ".py" || ext == ".xml" || ext == ".log") {
            att.text += "\n\n--- " + p.filename().string() + " ---\n" + clip(data, 100000);
        } else {
            att.cad += "\n- " + p.string();
        }
    }
    if (!att.cad.empty()) head += "\n\nCAD files to import with the import_step tool (give it the path):" + att.cad;
    return head + att.text + tail;
}

void emit_tool(const std::function<void(const LlmEvent&)>& emit, const std::string& name, const json& args)
{
    LlmEvent e;
    e.kind = LlmEvent::Tool;
    e.text = name + "  " + clip(args.dump(), 200);
    emit(e);
}

void emit_text(const std::function<void(const LlmEvent&)>& emit, const std::string& t)
{
    if (t.empty()) return;
    LlmEvent e;
    e.kind = LlmEvent::Text;
    e.text = t;
    emit(e);
}

constexpr int kMaxSteps = 40;   // tool round-trips in one turn

} // namespace

bool llm_agent_is_api(const std::string& a) { return a == "anthropic" || a == "openai" || a == "ollama"; }

std::string llm_api_key(const std::string& provider)
{
    for (const char* v : {"ORCA_LLM_API_KEY", provider == "anthropic" ? "ANTHROPIC_API_KEY" : provider == "openai" ? "OPENAI_API_KEY" : "ORCA_LLM_UNSET"})
        if (const char* e = std::getenv(v); e && *e) return e;
    wxSecretStore store = wxSecretStore::GetDefault();
    wxString      user;
    wxSecretValue value;
    if (store.IsOk() && store.Load(wxString::FromUTF8(kService) + " " + provider, user, value)) return std::string(value.GetAsString().ToUTF8());
    return {};
}

bool llm_api_key_save(const std::string& provider, const std::string& key)
{
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk()) return false;
    const wxString svc = wxString::FromUTF8(kService) + " " + provider;
    if (key.empty()) return store.Delete(svc);
    return store.Save(svc, wxString::FromUTF8("api-key"), wxSecretValue(wxString::FromUTF8(key)));
}

bool llm_api_key_present(const std::string& provider) { return !llm_api_key(provider).empty(); }

std::string llm_api_run(const LlmApiRequest& rq, const std::function<void(const LlmEvent&)>& emit, const std::function<bool()>& cancelled,
                        const std::function<void(std::function<void()>)>& set_canceller)
{
    try {
        const std::vector<Tool> tools = load_tools(rq.sock);
        const bool anthropic = rq.provider == "anthropic";
        std::string base = rq.url;
        if (base.empty()) base = anthropic ? "https://api.anthropic.com" : rq.provider == "ollama" ? "http://localhost:11434/v1" : "https://api.openai.com/v1";
        while (!base.empty() && base.back() == '/') base.pop_back();
        if (rq.provider != "ollama" && rq.key.empty())
            return std::string("no API key: set it in the Assistant pane (Settings), or in the ") + (anthropic ? "ANTHROPIC_API_KEY" : "OPENAI_API_KEY") + " environment variable";
        if (rq.model.empty()) return "choose a model in the Assistant pane (Settings)";

        // Conversation so far
        std::string sid = rq.session_id;
        json        msgs = json::array();
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (auto it = g_sessions.find(sid); !sid.empty() && it != g_sessions.end()) msgs = it->second;
            else sid = "api-" + std::to_string(++g_next);
        }
        Attachments att;
        const std::string text = split_attachments(rq.prompt, att);
        if (anthropic) {
            json content = json::array();
            for (const auto& im : att.images) content.push_back({{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", im.first}, {"data", im.second}}}});
            content.push_back({{"type", "text"}, {"text", text}});
            msgs.push_back({{"role", "user"}, {"content", content}});
        } else if (att.images.empty()) {
            msgs.push_back({{"role", "user"}, {"content", text}});
        } else {
            json content = json::array({{{"type", "text"}, {"text", text}}});
            for (const auto& im : att.images) content.push_back({{"type", "image_url"}, {"image_url", {{"url", "data:" + im.first + ";base64," + im.second}}}});
            msgs.push_back({{"role", "user"}, {"content", content}});
        }

        json tool_defs = json::array();
        for (const Tool& t : tools)
            tool_defs.push_back(anthropic ? json{{"name", t.name}, {"description", t.description}, {"input_schema", t.schema}}
                                          : json{{"type", "function"}, {"function", {{"name", t.name}, {"description", t.description}, {"parameters", t.schema}}}});

        std::string final_text;
        std::string err;
        for (int step = 0; step < kMaxSteps; ++step) {
            if (cancelled()) return "stopped";
            json body;
            std::vector<std::pair<std::string, std::string>> hdr;
            std::string url;
            if (anthropic) {
                body = {{"model", rq.model}, {"max_tokens", 8192}, {"system", rq.system}, {"tools", tool_defs}, {"messages", msgs}};
                hdr  = {{"x-api-key", rq.key}, {"anthropic-version", "2023-06-01"}};
                url  = base + "/v1/messages";
            } else {
                json all = json::array({{{"role", "system"}, {"content", rq.system}}});
                for (const json& m : msgs) all.push_back(m);
                body = {{"model", rq.model}, {"messages", all}, {"tools", tool_defs}};
                if (!rq.key.empty()) hdr = {{"Authorization", "Bearer " + rq.key}};
                url = base + "/chat/completions";
            }
            const std::string reply = http_post(url, hdr, body.dump(), set_canceller, err);
            if (!err.empty()) return cancelled() ? "stopped" : err;
            const json r = json::parse(reply, nullptr, false);
            if (r.is_discarded()) return "the provider sent an unreadable reply";

            std::vector<std::pair<std::string, json>> calls;   // id, {name,args}
            std::string said;
            if (anthropic) {
                const json content = r.value("content", json::array());
                msgs.push_back({{"role", "assistant"}, {"content", content}});
                for (const json& b : content) {
                    if (b.value("type", "") == "text") said += b.value("text", "");
                    else if (b.value("type", "") == "tool_use") calls.push_back({b.value("id", ""), json{{"name", b.value("name", "")}, {"args", b.value("input", json::object())}}});
                }
            } else {
                if (!r.contains("choices") || r["choices"].empty()) return "the provider returned no answer: " + clip(reply, 300);
                const json m = r["choices"][0].value("message", json::object());
                json keep = {{"role", "assistant"}, {"content", m.contains("content") ? m["content"] : json("")}};
                if (m.contains("tool_calls") && m["tool_calls"].is_array() && !m["tool_calls"].empty()) keep["tool_calls"] = m["tool_calls"];
                msgs.push_back(keep);
                if (m.contains("content") && m["content"].is_string()) said = m["content"].get<std::string>();
                for (const json& tc : keep.value("tool_calls", json::array())) {
                    const json fn = tc.value("function", json::object());
                    json args = fn.contains("arguments") ? fn["arguments"] : json::object();
                    if (args.is_string()) { args = json::parse(args.get<std::string>(), nullptr, false); if (args.is_discarded()) args = json::object(); }
                    calls.push_back({tc.value("id", ""), json{{"name", fn.value("name", "")}, {"args", args}}});
                }
            }
            emit_text(emit, said);
            final_text = said;
            if (calls.empty()) break;

            json results = json::array();
            for (const auto& c : calls) {
                if (cancelled()) return "stopped";
                const std::string name = c.second["name"];
                emit_tool(emit, name, c.second["args"]);
                const auto out = run_tool(rq.sock, name, c.second["args"]);
                if (anthropic) results.push_back({{"type", "tool_result"}, {"tool_use_id", c.first}, {"content", out.first}, {"is_error", out.second}});
                else msgs.push_back({{"role", "tool"}, {"tool_call_id", c.first}, {"content", out.first}});
            }
            if (anthropic) msgs.push_back({{"role", "user"}, {"content", results}});
            if (step == kMaxSteps - 1) final_text += "\n(stopped after " + std::to_string(kMaxSteps) + " tool rounds)";
        }

        // Keep the conversation bounded: the oldest turns go first.
        while (msgs.size() > 60) msgs.erase(msgs.begin());
        auto starts_turn = [](const json& m) {
            if (m.value("role", "") != "user") return false;
            return m["content"].is_string() || (m["content"].is_array() && !m["content"].empty() && m["content"][0].value("type", "") != "tool_result");
        };
        while (!msgs.empty() && !starts_turn(msgs[0])) msgs.erase(msgs.begin());
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_sessions[sid] = msgs;
        }
        LlmEvent e;
        e.kind       = LlmEvent::Result;
        e.text       = final_text;
        e.session_id = sid;
        emit(e);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}

}} // namespace Slic3r::GUI
