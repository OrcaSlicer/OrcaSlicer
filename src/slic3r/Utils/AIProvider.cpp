#include "AIProvider.hpp"

#include <sstream>
#include <utility>

#include <boost/log/trivial.hpp>

#include "libslic3r/AppConfig.hpp"

namespace Slic3r {

// ---------------------------------------------------------------------------
// Default non-pure implementations
// ---------------------------------------------------------------------------

AIResponse AIProvider::complete(const std::string    &prompt,
                                const nlohmann::json &params) const
{
    std::vector<AIMessage> messages;
    messages.push_back({"user", prompt});
    return chat(messages, params);
}

AIConfig AIProvider::config_from_app_config(const AppConfig &app_config)
{
    AIConfig cfg;
    cfg.provider = app_config.get("ai_slicer", "provider");
    cfg.base_url = app_config.get("ai_slicer", "gateway_url");
    cfg.api_key  = app_config.get("ai_slicer", "api_key");
    cfg.model    = app_config.get("ai_slicer", "model");
    return cfg;
}

// ---------------------------------------------------------------------------
// Concrete providers (internal — reachable only through AIProvider::create())
// ---------------------------------------------------------------------------

namespace {

// Result of one synchronous HTTP round-trip.
struct HttpResult
{
    bool        ok     { false };   ///< transport-level success (a response arrived)
    unsigned    status { 0 };
    std::string body;
    std::string error;              ///< transport error text (empty on ok)
};

// Run an already-configured Http request synchronously and collect the result.
HttpResult http_send(Http &&http)
{
    HttpResult r;
    http.on_complete([&r](std::string body, unsigned status) {
            r.ok = true; r.status = status; r.body = std::move(body);
        })
        .on_error([&r](std::string body, std::string error, unsigned status) {
            r.ok = false; r.status = status; r.body = std::move(body); r.error = std::move(error);
        })
        .perform_sync();
    return r;
}

std::string strip_trailing_slash(std::string s)
{
    while (! s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// Join an API base URL with a versioned path without doubling the "/v1" segment.
// OpenAI's base is given without it ("https://api.openai.com"), but many gateways
// are entered WITH it ("https://.../v1"). `path` is like "/chat/completions".
std::string api_url(const std::string &base, const std::string &path)
{
    std::string b = strip_trailing_slash(base);
    if (b.size() >= 3 && b.compare(b.size() - 3, 3, "/v1") == 0)
        return b + path;
    return b + "/v1" + path;
}

// Turn any failed HttpResult into a concise, user-facing message.
std::string http_error_message(const HttpResult &r)
{
    switch (r.status) {
        case 401: case 403: return "Authentication failed (" + std::to_string(r.status) +
                                   ") — check the API key and gateway URL.";
        case 404:           return "Endpoint not found (404) — check the gateway URL.";
        case 429:           return "Rate limited (429) — slow down or check your quota.";
        default: break;
    }
    if (! r.error.empty())
        return r.error;
    std::ostringstream os;
    os << "HTTP " << r.status;
    if (! r.body.empty())
        os << ": " << r.body.substr(0, 300);
    return os.str();
}

bool status_ok(unsigned s) { return s >= 200 && s < 300; }

// --- OpenAI, and any OpenAI-compatible gateway (Ollama, LiteLLM, Azure proxy...) ---
class OpenAIProvider : public AIProvider
{
public:
    OpenAIProvider(AIConfig cfg, std::string default_base, const char *name)
        : m_cfg(std::move(cfg))
        , m_name(name)
        , m_base(strip_trailing_slash(m_cfg.base_url.empty() ? std::move(default_base) : m_cfg.base_url))
    {}

    const char *get_name() const override { return m_name; }

    AIResponse chat(const std::vector<AIMessage> &messages,
                    const nlohmann::json         &params) const override
    {
        AIResponse res;
        nlohmann::json body = (params.is_object()) ? params : nlohmann::json::object();
        body["model"] = default_model();
        nlohmann::json msgs = nlohmann::json::array();
        for (const auto &m : messages) {
            if (m.images.empty()) {
                msgs.push_back({{"role", m.role}, {"content", m.content}});
            } else {
                // OpenAI multimodal: content is an array of text/image_url parts.
                nlohmann::json parts = nlohmann::json::array();
                if (! m.content.empty())
                    parts.push_back({{"type", "text"}, {"text", m.content}});
                for (const auto &img : m.images)
                    parts.push_back({{"type", "image_url"},
                                     {"image_url", {{"url", "data:image/jpeg;base64," + img}}}});
                msgs.push_back({{"role", m.role}, {"content", std::move(parts)}});
            }
        }
        body["messages"] = std::move(msgs);

        auto http = Http::post(api_url(m_base, "/chat/completions"));
        http.header("Authorization", "Bearer " + m_cfg.api_key);
        http.header("Content-Type", "application/json");
        http.set_post_body(body.dump());
        HttpResult r = http_send(std::move(http));

        if (! r.ok || ! status_ok(r.status)) { res.error = http_error_message(r); return res; }
        try {
            nlohmann::json j = nlohmann::json::parse(r.body);
            res.raw = j;
            const nlohmann::json &msg = j.at("choices").at(0).at("message");
            // Preferred path: a tool/function call (forced via the "tools" +
            // "tool_choice" chat params). Arguments are a JSON string.
            if (msg.contains("tool_calls") && msg["tool_calls"].is_array() && ! msg["tool_calls"].empty()) {
                const nlohmann::json &fn = msg["tool_calls"][0].at("function");
                res.tool_call_name      = fn.value("name", std::string());
                res.tool_call_arguments = fn.value("arguments", std::string());
            }
            // Plain assistant text (may be null when a tool was called).
            if (msg.contains("content") && msg["content"].is_string())
                res.content = msg["content"].get<std::string>();
            res.ok = true;
        } catch (const std::exception &e) {
            res.error = std::string("Could not parse response: ") + e.what();
        }
        return res;
    }

    bool test_connection(std::string &error_msg) const override
    {
        std::vector<AIModelInfo> models;
        return get_models(models, error_msg);
    }

    bool get_models(std::vector<AIModelInfo> &models, std::string &error_msg) const override
    {
        auto http = Http::get(api_url(m_base, "/models"));
        http.header("Authorization", "Bearer " + m_cfg.api_key);
        HttpResult r = http_send(std::move(http));
        if (! r.ok || ! status_ok(r.status)) { error_msg = http_error_message(r); return false; }
        try {
            nlohmann::json j = nlohmann::json::parse(r.body);
            for (const auto &m : j.value("data", nlohmann::json::array())) {
                std::string id = m.value("id", std::string());
                if (! id.empty()) models.push_back({id, id});
            }
            return true;
        } catch (const std::exception &e) {
            error_msg = std::string("Could not parse model list: ") + e.what();
            return false;
        }
    }

private:
    std::string default_model() const { return m_cfg.model.empty() ? "gpt-4o-mini" : m_cfg.model; }

    AIConfig    m_cfg;
    const char *m_name;
    std::string m_base;
};

// --- Anthropic (Messages API) ---
class AnthropicProvider : public AIProvider
{
public:
    explicit AnthropicProvider(AIConfig cfg)
        : m_cfg(std::move(cfg))
        , m_base(strip_trailing_slash(m_cfg.base_url.empty() ? "https://api.anthropic.com" : m_cfg.base_url))
    {}

    const char *get_name() const override { return "Anthropic"; }

    AIResponse chat(const std::vector<AIMessage> &messages,
                    const nlohmann::json         &params) const override
    {
        AIResponse res;
        nlohmann::json body = (params.is_object()) ? params : nlohmann::json::object();
        body["model"] = m_cfg.model.empty() ? "claude-3-5-sonnet-latest" : m_cfg.model;
        if (! body.contains("max_tokens"))
            body["max_tokens"] = 1024;

        // Anthropic keeps the system prompt as a top-level field, not a message role.
        nlohmann::json msgs = nlohmann::json::array();
        std::string system;
        for (const auto &m : messages) {
            if (m.role == "system") { system += (system.empty() ? "" : "\n") + m.content; continue; }
            if (m.images.empty()) {
                msgs.push_back({{"role", m.role}, {"content", m.content}});
            } else {
                // Anthropic multimodal: content is an array of text/image blocks.
                nlohmann::json parts = nlohmann::json::array();
                if (! m.content.empty())
                    parts.push_back({{"type", "text"}, {"text", m.content}});
                for (const auto &img : m.images)
                    parts.push_back({{"type", "image"},
                                     {"source", {{"type", "base64"},
                                                 {"media_type", "image/jpeg"},
                                                 {"data", img}}}});
                msgs.push_back({{"role", m.role}, {"content", std::move(parts)}});
            }
        }
        body["messages"] = std::move(msgs);
        if (! system.empty())
            body["system"] = system;

        auto http = Http::post(api_url(m_base, "/messages"));
        http.header("x-api-key", m_cfg.api_key);
        http.header("anthropic-version", "2023-06-01");
        http.header("Content-Type", "application/json");
        http.set_post_body(body.dump());
        HttpResult r = http_send(std::move(http));

        if (! r.ok || ! status_ok(r.status)) { res.error = http_error_message(r); return res; }
        try {
            nlohmann::json j = nlohmann::json::parse(r.body);
            res.raw = j;
            // content is an array of blocks: text blocks + optional tool_use.
            std::string text;
            for (const auto &block : j.value("content", nlohmann::json::array())) {
                const std::string btype = block.value("type", std::string());
                if (btype == "text")
                    text += block.value("text", std::string());
                else if (btype == "tool_use" && res.tool_call_arguments.empty()) {
                    res.tool_call_name      = block.value("name", std::string());
                    res.tool_call_arguments = block.contains("input") ? block["input"].dump() : std::string();
                }
            }
            res.content = std::move(text);
            res.ok      = true;
        } catch (const std::exception &e) {
            res.error = std::string("Could not parse response: ") + e.what();
        }
        return res;
    }

    bool test_connection(std::string &error_msg) const override
    {
        std::vector<AIModelInfo> models;
        return get_models(models, error_msg);
    }

    bool get_models(std::vector<AIModelInfo> &models, std::string &error_msg) const override
    {
        auto http = Http::get(api_url(m_base, "/models"));
        http.header("x-api-key", m_cfg.api_key);
        http.header("anthropic-version", "2023-06-01");
        HttpResult r = http_send(std::move(http));
        if (! r.ok || ! status_ok(r.status)) { error_msg = http_error_message(r); return false; }
        try {
            nlohmann::json j = nlohmann::json::parse(r.body);
            for (const auto &m : j.value("data", nlohmann::json::array())) {
                std::string id = m.value("id", std::string());
                if (! id.empty()) models.push_back({id, m.value("display_name", id)});
            }
            return true;
        } catch (const std::exception &e) {
            error_msg = std::string("Could not parse model list: ") + e.what();
            return false;
        }
    }

private:
    AIConfig    m_cfg;
    std::string m_base;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

AIProvider *AIProvider::create(const AIConfig &config)
{
    const std::string &provider = config.provider;

    if (provider.empty() || provider == "none")
        return nullptr;

    if (provider == "openai")
        return new OpenAIProvider(config, "https://api.openai.com", "OpenAI");

    if (provider == "anthropic")
        return new AnthropicProvider(config);

    if (provider == "compatible" || provider == "openai_compatible") {
        if (config.base_url.empty()) {
            BOOST_LOG_TRIVIAL(error)
                << "AIProvider::create: 'compatible' provider requires a gateway URL (base_url).";
            return nullptr;
        }
        return new OpenAIProvider(config, config.base_url, "OpenAI-compatible");
    }

    BOOST_LOG_TRIVIAL(warning)
        << "AIProvider::create: unknown provider '" << provider << "'; treating as disabled.";
    return nullptr;
}

} // namespace Slic3r
