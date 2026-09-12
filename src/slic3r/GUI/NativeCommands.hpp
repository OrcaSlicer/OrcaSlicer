#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ActionRegistry.hpp" // for AppAction / AppActionRunResult

namespace Slic3r { namespace GUI {

// A built-in speed-dial command: identity + how to run it. make_action() wraps a value as a thin
// AppAction for the registry, so this catalog is the single source of truth for the behaviour
// (runner => an owner method), the presentation (title/group/input), and the tile pictogram
// (icon = an SVG base name under resources/images, "" for no icon).
struct NativeCommand
{
    std::string key;
    std::string title;
    std::string group;
    std::string input; // "percent"/"tab" or "" for immediate run
    std::string icon;  // SVG base name, or "" to render a blank tile
    std::function<AppActionRunResult(const std::string& param)> runner;
};

namespace NativeCommands {
// The full built-in command catalog, built once on first use. UI thread only.
const std::vector<NativeCommand>& catalog();

// Dispatches `key` to its runner (unknown keys return a quiet Info). UI thread only.
AppActionRunResult run(const std::string& key, const std::string& param = {});

// Materialises one catalog entry as a runnable AppAction. Keeps the catalog's identity,
// presentation and behaviour as the single source of truth; ActionRegistry only stores and
// dispatches the result. UI thread only.
std::unique_ptr<AppAction> make_action(const NativeCommand& command);
} // namespace NativeCommands

}} // namespace Slic3r::GUI
