#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>

#include <nlohmann/json.hpp>

#include "Http.hpp"

namespace Slic3r {

class AppConfig;   // libslic3r; used by AIProvider::config_from_app_config()

/// Configuration passed to AIProvider::create().
struct AIConfig
{
    std::string provider;   ///< Provider key: "openai", "anthropic", "none", etc.
    std::string api_key;
    std::string model;
    std::string base_url;   ///< Optional override for the API endpoint.
};

/// A single chat message (role + text content, plus optional images).
struct AIMessage
{
    std::string role;       ///< "system", "user", or "assistant"
    std::string content;

    /// Optional base64-encoded JPEG frames (no data-URI prefix) for vision
    /// models. When non-empty, providers send multimodal content parts.
    std::vector<std::string> images;
};

/// Describes a model returned by get_models().
struct AIModelInfo
{
    std::string id;
    std::string display_name;
};

/// Response returned by chat() / complete().
struct AIResponse
{
    bool        ok      { false };
    std::string content;          ///< The assistant reply (on success).
    std::string error;            ///< Human-readable error description (on failure).
    nlohmann::json raw;           ///< Full JSON payload from the provider.

    /// When the model answered with a tool/function call (see chat params
    /// "tools" + "tool_choice"), this holds the raw JSON arguments string of the
    /// first tool call. Empty if the model replied with prose instead — which,
    /// for a forced tool call, means the model is unsuitable for the task.
    std::string tool_call_arguments;
    std::string tool_call_name;
};

/**
 * Abstract base class for AI-provider back-ends.
 *
 * Mirror of PrintHost::get_print_host() pattern: callers obtain a concrete
 * instance through the static factory AIProvider::create() and then work
 * exclusively through the virtual interface below.
 *
 * Concrete provider implementations live in separate translation units
 * (separate tasks); this file defines only the interface and factory stub.
 */
class AIProvider
{
public:
    virtual ~AIProvider() = default;

    // --- Identity -----------------------------------------------------------

    /// Human-readable provider name (e.g. "OpenAI", "Anthropic").
    virtual const char *get_name() const = 0;

    // --- Core API -----------------------------------------------------------

    /**
     * Send a list of messages and return the assistant's response.
     *
     * @param messages  Conversation history (system + user + assistant turns).
     * @param params    Optional extra JSON parameters forwarded to the API
     *                  (e.g. {"temperature": 0.7, "max_tokens": 512}).
     * @return          AIResponse with ok==true on success.
     */
    virtual AIResponse chat(const std::vector<AIMessage> &messages,
                            const nlohmann::json         &params = {}) const = 0;

    /**
     * Convenience wrapper: single-turn completion from a plain prompt string.
     *
     * Default implementation wraps the prompt in a user message and calls chat().
     * Concrete providers may override for provider-specific single-turn endpoints.
     */
    virtual AIResponse complete(const std::string    &prompt,
                                const nlohmann::json &params = {}) const;

    // --- Diagnostics --------------------------------------------------------

    /**
     * Verify that the provider is reachable and the credentials are valid.
     *
     * @param error_msg  Populated with a human-readable error on failure.
     * @return           true on success.
     */
    virtual bool test_connection(std::string &error_msg) const = 0;

    /**
     * Return the list of models available for this provider/key combination.
     *
     * @param models     Populated on success.
     * @param error_msg  Populated with a human-readable error on failure.
     * @return           true on success.
     */
    virtual bool get_models(std::vector<AIModelInfo> &models,
                            std::string              &error_msg) const = 0;

    // --- Factory ------------------------------------------------------------

    /**
     * Instantiate the correct concrete AIProvider for the given AIConfig.
     *
     * Mirrors PrintHost::get_print_host(DynamicPrintConfig *).
     * Selects the implementation based on config.provider:
     *   "openai"     -> (future) OpenAIProvider
     *   "anthropic"  -> (future) AnthropicProvider
     *   "none" / ""  -> returns nullptr
     *   unknown      -> returns nullptr; callers should treat as disabled.
     *
     * @return  Heap-allocated concrete provider, or nullptr when disabled.
     */
    static AIProvider *create(const AIConfig &config);

    /**
     * Build an AIConfig from the "ai_slicer" section of AppConfig:
     *   ai_slicer/provider, ai_slicer/gateway_url (-> base_url),
     *   ai_slicer/api_key, ai_slicer/model.
     */
    static AIConfig config_from_app_config(const AppConfig &app_config);
};

} // namespace Slic3r
