#ifndef slic3r_GUI_LlmApi_hpp_
#define slic3r_GUI_LlmApi_hpp_

// Direct-API backends for the Design tab assistant: Anthropic Messages, OpenAI-compatible chat completions (also Ollama's /v1).
// The app runs the tool loop itself against its own MCP socket, so these providers can only ever call the orca-cad tools: there
// is no shell, no file access of their own. Keys live in the OS keyring (or an environment variable). docs/CAD/SPEC-llm-panel.md

#include "slic3r/GUI/CAD/LlmAgent.hpp"

#include <functional>
#include <string>

namespace Slic3r { namespace GUI {

bool llm_agent_is_api(const std::string& agent);   // "anthropic" | "openai" | "ollama"

// Key for a provider: ORCA_LLM_API_KEY, the provider's usual variable (ANTHROPIC_API_KEY / OPENAI_API_KEY), then the OS keyring.
// UI thread only (the keyring may talk to a desktop service).
std::string llm_api_key(const std::string& provider);
bool        llm_api_key_save(const std::string& provider, const std::string& key);   // empty key removes it
bool        llm_api_key_present(const std::string& provider);

struct LlmApiRequest {
    std::string provider;     // anthropic | openai | ollama
    std::string url;          // base URL; empty = the provider's default
    std::string key;          // see llm_api_key
    std::string model;
    std::string sock;         // the app's MCP socket
    std::string prompt;
    std::string session_id;   // "" starts a conversation
    std::string system;
};

// Runs one turn on the calling (worker) thread. `emit` may be called from it; the caller marshals to the UI. Returns "" on
// success, else the reason. `set_canceller` receives a function that aborts the request in flight.
std::string llm_api_run(const LlmApiRequest& req, const std::function<void(const LlmEvent&)>& emit, const std::function<bool()>& cancelled,
                        const std::function<void(std::function<void()>)>& set_canceller);

}} // namespace Slic3r::GUI

#endif
