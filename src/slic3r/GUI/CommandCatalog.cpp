#include "CommandCatalog.hpp"

#include "Mcp/McpTypes.hpp"

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

void CommandCatalog::leave_out(const std::string& key, std::string why)
{
    if (m_sealed)
        throw std::logic_error("The command catalog is sealed; leaving out " + key + " comes too late");
    m_left_out[key] = std::move(why);
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
    return it == m_left_out.end() ? nullptr : &it->second;
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

const char* guard_name(Mcp::GuardClass guard)
{
    switch (guard) {
    case Mcp::GuardClass::Inspect: return "inspect";
    case Mcp::GuardClass::Recover: return "recover";
    case Mcp::GuardClass::Mutate: return "mutate";
    }
    return "mutate";
}

const char* completion_name(CommandCompletion completion)
{
    switch (completion) {
    case CommandCompletion::None: return "none";
    case CommandCompletion::Export: return "export";
    }
    return "none";
}

} // namespace

nlohmann::json command_reference(const NativeCommand& command)
{
    nlohmann::json entry{{"key", command.key},
                         {"title", command.title},
                         {"group", command.group},
                         {"description", command.description},
                         {"input_schema", command.input_schema},
                         {"listed", command.listed},
                         {"in_palette", command.in_palette},
                         {"read_only", command.read_only},
                         {"destructive", command.destructive},
                         {"idempotent", command.idempotent},
                         {"open_world", command.open_world},
                         {"guard", guard_name(command.guard)},
                         {"completion", completion_name(command.completion)},
                         {"operations", command.operations}};
    if (!command.examples.is_null())
        entry["examples"] = command.examples;
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
