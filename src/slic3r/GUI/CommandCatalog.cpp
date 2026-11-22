#include "CommandCatalog.hpp"

#include "ActionIds.hpp"
#include "CommandTypes.hpp"
#include "Mcp/McpArguments.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

void CommandCatalog::add(NativeCommand command)
{
    if (m_sealed)
        throw std::logic_error("The command catalog is sealed; " + command.key + " comes too late");
    if (m_index.count(command.key) != 0)
        throw std::logic_error("The command " + command.key + " is in the catalog already");
    m_index.emplace(command.key, m_commands.size());
    m_commands.push_back(std::move(command));
}

void CommandCatalog::add_source(std::unique_ptr<CommandSource> source)
{
    if (m_sealed)
        throw std::logic_error("The command catalog is sealed; a source comes too late");
    m_sources.push_back(std::move(source));
}

void CommandCatalog::leave_out(const std::string& key, std::string why, std::string code)
{
    if (m_sealed)
        throw std::logic_error("The command catalog is sealed; leaving out " + key + " comes too late");
    m_left_out[key] = {std::move(why), std::move(code)};
}

void CommandCatalog::seal() { m_sealed = true; }

const NativeCommand* CommandCatalog::find(const std::string& key) const
{
    const auto it = m_index.find(key);
    return it == m_index.end() ? nullptr : &m_commands[it->second];
}

std::shared_ptr<const NativeCommand> CommandCatalog::lookup(const std::string& key) const
{
    // The catalog owns its added commands, so the pointer shares no ownership.
    if (const NativeCommand* command = find(key))
        return std::shared_ptr<const NativeCommand>(std::shared_ptr<const NativeCommand>(), command);
    for (const std::unique_ptr<CommandSource>& source : m_sources)
        if (std::shared_ptr<const NativeCommand> command = source->find(key))
            return command;
    return nullptr;
}

std::vector<std::shared_ptr<const NativeCommand>> CommandCatalog::source_commands() const
{
    std::vector<std::shared_ptr<const NativeCommand>> out;
    for (const std::unique_ptr<CommandSource>& source : m_sources) {
        std::vector<std::shared_ptr<const NativeCommand>> held = source->all();
        out.insert(out.end(), std::make_move_iterator(held.begin()), std::make_move_iterator(held.end()));
    }
    return out;
}

std::vector<std::string> CommandCatalog::similar(const std::string& key, std::size_t limit) const
{
    std::vector<std::pair<std::size_t, std::string>> scored;
    for (const NativeCommand& c : m_commands)
        scored.emplace_back(c.key.find(key) != std::string::npos ? 0 : Mcp::edit_distance(key, c.key), c.key);
    std::sort(scored.begin(), scored.end());
    std::vector<std::string> out;
    for (const auto& [score, name] : scored)
        if (out.size() < limit && score <= std::max<std::size_t>(3, key.size() / 2))
            out.push_back(name);
    return out;
}

const std::string* CommandCatalog::left_out(const std::string& key) const
{
    const auto it = m_left_out.find(key);
    return it == m_left_out.end() ? nullptr : &it->second.why;
}

std::string CommandCatalog::unknown_command_code(const std::string& key) const
{
    const auto it = m_left_out.find(key);
    return it == m_left_out.end() ? "invalid_argument" : it->second.code;
}

std::string CommandCatalog::unknown_command(const std::string& key, bool suggest) const
{
    std::string message = "Unknown tool: " + key;
    // A key left out names the command the caller meant, so no other key is suggested.
    if (const std::string* why = left_out(key))
        return message + "; " + *why;
    if (suggest)
        if (const std::vector<std::string> close = similar(key); !close.empty())
            message += " (did you mean " + close.front() + "?)";
    return message;
}

namespace {

const char* guard_name(GuardClass guard)
{
    switch (guard) {
    case GuardClass::Inspect: return "inspect";
    case GuardClass::Recover: return "recover";
    case GuardClass::Mutate: return "mutate";
    }
    return "mutate";
}

const char* completion_name(CommandCompletion completion)
{
    switch (completion) {
    case CommandCompletion::Immediate: return "immediate";
    case CommandCompletion::Deferred: return "deferred";
    }
    return "immediate";
}

} // namespace

const nlohmann::json* action_with_args(const NativeCommand& command, const nlohmann::json& args)
{
    if (!command.actions.is_array())
        return nullptr;
    // A call with no arguments gives none, null as {}, as Mcp::opened_item() reads it.
    const nlohmann::json given = Mcp::person_form(args) ? nlohmann::json::object() : args;
    // An action that collects a value is called with that value besides its own arguments, never with these alone.
    for (const nlohmann::json& action : command.actions)
        if (action.value("input", std::string()).empty() && action.at("args") == given)
            return &action;
    return nullptr;
}

nlohmann::json action_call_args(const nlohmann::json* action, const nlohmann::json& args)
{
    if (action == nullptr)
        return args;
    nlohmann::json    out   = action->at("args");
    const std::string input = action->value("input", std::string());
    // Without the value the action's args alone are given: action_run refuses such a call first (action_args_error()),
    // and the Speed Dial's second phase always collects the value.
    if (input.empty() || !args.is_object() || !args.contains(input))
        return out;
    nlohmann::json value = args.at(input);
    // The Speed Dial's tab picker sends the id of a page, and view_set's tab takes the word of a page that has one.
    if (input == "tab")
        for (const TabWord& w : kTabWords)
            if (value == w.page)
                value = w.word;
    out[action->value("input_arg", input)] = std::move(value);
    return out;
}

std::string call_title(const NativeCommand& command, const nlohmann::json& args)
{
    const nlohmann::json* action = action_with_args(command, args);
    return action != nullptr ? action->at("title").get<std::string>() : command.title;
}

std::pair<const NativeCommand*, const nlohmann::json*> find_command_action(const CommandCatalog& catalog, const std::string& key)
{
    if (const NativeCommand* command = catalog.find(key); command != nullptr && command->has_own_action)
        return {command, nullptr};
    for (const NativeCommand& command : catalog.commands())
        if (command.actions.is_array())
            for (const nlohmann::json& action : command.actions)
                if (action.at("key") == key)
                    return {&command, &action};
    return {nullptr, nullptr};
}

std::pair<std::string, const NativeCommand*> shown_title(const CommandCatalog& catalog, const NativeCommand& command,
                                                         const nlohmann::json& args)
{
    if (command.key == "action_run" && args.is_object() && args.value("id", nlohmann::json()).is_string()) {
        const auto [owner, action] = find_command_action(catalog, command_action_key(args.at("id").get<std::string>()));
        if (owner != nullptr)
            return {action != nullptr ? action->at("title").get<std::string>() : owner->title, owner};
    }
    return {call_title(command, args), &command};
}

nlohmann::json command_reference(const NativeCommand& command)
{
    nlohmann::json entry{{"key", command.key},
                         {"title", command.title},
                         {"group", command.group},
                         {"description", command.description},
                         {"input_schema", command.input_schema},
                         {"listed", command.listed},
                         {"has_own_action", command.has_own_action},
                         {"read_only", command.read_only},
                         {"destructive", command.destructive},
                         {"idempotent", command.idempotent},
                         {"open_world", command.open_world},
                         {"guard", guard_name(command.guard)},
                         {"completion", completion_name(command.completion)},
                         {"operations", command.operations}};
    for (const auto& [name, value] : {std::pair<const char*, const std::string*>{"heading", &command.heading},
                                      {"icon", &command.icon},
                                      {"input", &command.input},
                                      {"action_group", &command.action_group}})
        if (!value->empty())
            entry[name] = *value;
    if (!command.examples.is_null())
        entry["examples"] = command.examples;
    if (!command.actions.is_null())
        entry["actions"] = command.actions;
    return entry;
}

nlohmann::json catalog_reference(const CommandCatalog& catalog)
{
    nlohmann::json commands = nlohmann::json::array();
    for (const NativeCommand& command : catalog.commands())
        commands.push_back(command_reference(command));
    return {{"commands", std::move(commands)}};
}

}} // namespace Slic3r::GUI
