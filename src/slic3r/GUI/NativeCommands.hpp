#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ActionRegistry.hpp" // for AppAction / AppActionRunResult

namespace Slic3r { namespace GUI {

// What a command's `param` means, for callers other than the palette (MCP, scripts). The palette
// keeps using `input` for its second-phase picker and never passes a path, so a command that takes
// one still opens its file dialog there.
enum class CommandParamType { None, Percent, Tab, Path, Index, Text };

struct CommandParam
{
    CommandParamType type = CommandParamType::None;
    std::string      help; // English, for agents
};

// Commands whose effect finishes after run() returns. Export: the G-code export runs in the
// background process, so an automated caller waits for its file.
enum class CommandCompletion { None, Export };

// A built-in speed-dial command: identity + how to run it. make_action() wraps a value as a thin
// AppAction for the registry, so this catalog is the single source of truth for the behaviour
// (runner => an owner method), the presentation (title/group/input), the tile pictogram
// (icon = an SVG base name under resources/images, "" for no icon), and what automation needs to
// call it (description, param, completion).
struct NativeCommand
{
    std::string key;
    std::string title; // untranslated English msgid; make_action() translates it for display
    std::string group; // likewise
    std::string input; // "percent"/"tab" or "" for immediate run
    std::string icon;  // SVG base name, or "" to render a blank tile
    std::function<AppActionRunResult(const std::string& param)> runner;
    std::string       description; // English, for agents; not shown in the palette
    CommandParam      param;
    CommandCompletion completion = CommandCompletion::None;
};

namespace NativeCommands {
// The full built-in command catalog. Built on first use and reused; it holds no translated text, so
// it stays valid across a UI language switch. UI thread only.
const std::vector<NativeCommand>& catalog();

// Dispatches `key` to its runner (unknown keys return a quiet Info). UI thread only.
AppActionRunResult run(const std::string& key, const std::string& param = {});

// The catalog entry for `key`, or nullptr.
const NativeCommand* find(const std::string& key);

const char* param_type_name(CommandParamType type);

// Materialises one catalog entry as a runnable AppAction, titled in the current UI language with the
// English title/group kept as search aliases. Keeps the catalog's identity, presentation and
// behaviour as the single source of truth; ActionRegistry only stores and dispatches the result.
// UI thread only.
std::unique_ptr<AppAction> make_action(const NativeCommand& command);
} // namespace NativeCommands

}} // namespace Slic3r::GUI
