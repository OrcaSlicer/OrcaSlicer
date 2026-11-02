#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <string>

namespace Slic3r {

// A Python script a person or an agent starts (run_python, a .py in the Run Script dialog or
// --mcp-script), or a plugin's run_python: code run on the plugin interpreter without being packaged
// as a plugin.
struct PythonRunRequest
{
    // UTF-8 text; a file's bytes go through decode_python_source() first. A coding declaration in it changes
    // nothing, as for any code string Python compiles.
    std::string    code;
    // Bound to the module-level name `args`.
    nlohmann::json args = nlohmann::json::object();
    // The audit identity the code runs under: kScriptIdentityPerson or kScriptIdentityAgent for a
    // person's or an agent's script, which every audited access is allowed to apart from the categorical
    // denies, or a plugin's key, whose grants then apply.
    std::string    identity;
    // Shown in tracebacks.
    std::string    filename = "<run_python>";
    // The run is interrupted as timed_out once it has run this long.
    std::chrono::milliseconds budget{std::chrono::minutes(5)};
    // Polled while the code runs, from another thread; true interrupts it as cancelled.
    std::function<bool()> cancelled;
};

struct PythonRunOutcome
{
    // finished, raised, cancelled, timed_out, or unavailable (no interpreter).
    std::string    status;
    // The value the code left in `result`, read once when the module body returned; null when it set
    // none or the run did not finish.
    nlohmann::json result;
    // What the run's own thread printed to stdout and stderr, in order. Threads the code starts print
    // to the app's streams instead.
    std::string    output;
    // The formatted traceback of a raised run.
    std::string    traceback;
    // Why a run did not finish, in one line.
    std::string    message;
};

// Runs `request.code` as a module body on the calling thread and returns how it ended. Holds an
// interpreter lease and the GIL for the run, under `request.identity`'s audit scope, in a fresh
// namespace where `args` is bound and `orca` is importable. Cancellation and the budget raise an
// exception in the running code at its next bytecode, again every second until it ends, so code
// blocked inside a C call stops once the call returns. A `result` that is not JSON (a dict with str
// keys, a list, a str, a finite number, a bool or None, nested) makes the run raised; SystemExit with
// code 0 or None finishes it.
PythonRunOutcome run_python_code(const PythonRunRequest& request);

// A .py file's bytes as UTF-8 text, decoded as Python reads a source file: by its BOM, else its PEP 263
// encoding declaration, else as UTF-8. Sets `encoding` to the codec used, which encode_python_source() takes
// to save the text back. False with `error` when the bytes are not text in that encoding.
bool decode_python_source(const std::string& bytes, std::string& text, std::string& encoding, std::string& error);
// UTF-8 `text` encoded in `encoding` (from decode_python_source(), or "utf-8" for a new file). False with
// `error` when a character has no form in it.
bool encode_python_source(const std::string& text, const std::string& encoding, std::string& bytes, std::string& error);

} // namespace Slic3r
