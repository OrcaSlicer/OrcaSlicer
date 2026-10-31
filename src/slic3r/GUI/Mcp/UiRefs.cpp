#include "UiRefs.hpp"

#include "McpTypes.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

std::string clean_imgui_label(const std::string& label)
{
    const auto hash = label.find("##");
    std::string out = hash == std::string::npos ? label : label.substr(0, hash);
    // Orca marks colored text and icons with control characters, and draws its icons as glyphs at
    // U+0800-U+085F (imconfig.h; UTF-8 E0 A0 80 to E0 A1 9F); drop them and the padding.
    std::string clean;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(out[i]);
        if (c == 0xE0 && i + 2 < out.size()) {
            const unsigned char c1 = static_cast<unsigned char>(out[i + 1]), c2 = static_cast<unsigned char>(out[i + 2]);
            if (c1 == 0xA0 || (c1 == 0xA1 && c2 <= 0x9F)) {
                i += 2;
                continue;
            }
        }
        if (c >= 0x20)
            clean.push_back(char(c));
    }
    const auto first = clean.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    return clean.substr(first, clean.find_last_not_of(' ') - first + 1);
}

std::string imgui_label_name(const std::string& label)
{
    const auto hash = label.find("##");
    if (hash == std::string::npos)
        return {};
    const auto start = label.find_first_not_of('#', hash);
    return start == std::string::npos ? std::string() : clean_imgui_label(label.substr(start));
}

std::string imgui_window_label(const std::string& window)
{
    return clean_imgui_label(window.substr(0, window.find('/')));
}

std::string format_snapshot(const std::vector<UiNode>& nodes, std::size_t omitted)
{
    std::string out;
    for (const UiNode& n : nodes) {
        out.append(std::size_t(n.depth) * 2, ' ');
        out += "[" + n.ref + "] " + n.kind;
        // One node per line: line breaks in multiline text show as \n.
        auto one_line = [](std::string s) {
            for (std::size_t at = s.find('\n'); at != std::string::npos; at = s.find('\n', at + 2))
                s.replace(at, 1, "\\n");
            return s;
        };
        if (!n.label.empty())
            out += " \"" + one_line(n.label) + "\"";
        if (!n.value.empty())
            out += " = \"" + one_line(n.value) + "\"";
        if (!n.cls.empty() && n.cls != n.kind)
            out += " (" + n.cls + ")";
        if (!n.name.empty())
            out += " name=" + n.name;
        if (!n.option.empty())
            out += " option=" + n.option;
        if (!n.window.empty())
            out += " in \"" + n.window + "\"";
        if (n.checked >= 0)
            out += n.checked ? " [checked]" : " [unchecked]";
        if (!n.enabled)
            out += " [disabled]";
        if (n.focused)
            out += " [focused]";
        if (!n.note.empty())
            out += " [" + n.note + "]";
        if (n.w > 0 && n.h > 0)
            out += " @" + std::to_string(n.x) + "," + std::to_string(n.y) + " " + std::to_string(n.w) + "x" + std::to_string(n.h);
        out += "\n";
    }
    if (omitted > 0)
        out += "... " + std::to_string(omitted) + " more nodes; narrow with root, query or a larger max_nodes (max_web_nodes for web pages)\n";
    return out;
}

namespace {

// What a menu label is matched and numbered by: lower case, trimmed, without the in-use marker and
// trailing dots ("...", or the ellipsis character some translations use).
std::string menu_key(std::string label, std::string current)
{
    for (std::string* s : {&label, &current})
        for (char& c : *s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
    const auto trim = [&label] {
        const auto first = label.find_first_not_of(' ');
        label = first == std::string::npos ? std::string() : label.substr(first, label.find_last_not_of(' ') - first + 1);
    };
    trim();
    if (!current.empty() && label.size() >= current.size() && label.compare(label.size() - current.size(), current.size(), current) == 0)
        label.resize(label.size() - current.size());
    for (;;) {
        if (!label.empty() && label.back() == '.')
            label.pop_back();
        else if (label.size() >= 3 && label.compare(label.size() - 3, 3, "\xE2\x80\xA6") == 0)
            label.resize(label.size() - 3);
        else
            break;
    }
    trim();
    return label;
}

// The `n`-th child of `parent` (counting from 1) whose label has `key`.
int menu_child(const std::vector<MenuEntry>& entries, int parent, const std::string& key, int n, const std::string& current)
{
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (entries[i].parent == parent && menu_key(entries[i].label, current) == key && --n == 0)
            return int(i);
    return -1;
}

} // namespace

void name_menu_entries(std::vector<MenuEntry>& entries, const std::string& current)
{
    std::map<std::pair<int, std::string>, int> seen;
    for (MenuEntry& e : entries) {
        const int         n     = ++seen[{e.parent, menu_key(e.label, current)}];
        const std::string label = n > 1 ? e.label + " #" + std::to_string(n) : e.label;
        e.path                  = e.parent < 0 ? label : entries[std::size_t(e.parent)].path + ">" + label;
    }
}

nlohmann::json menu_listing(const std::vector<MenuEntry>& entries, int under)
{
    nlohmann::json items = nlohmann::json::array(), disabled = nlohmann::json::array(), checked = nlohmann::json::array();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const MenuEntry& e = entries[i];
        int              p = e.parent;
        while (under >= 0 && p >= 0 && p != under)
            p = entries[std::size_t(p)].parent;
        if (e.submenu || (under >= 0 && p != under))
            continue;
        items.push_back(e.path);
        if (!e.enabled)
            disabled.push_back(e.path);
        if (e.checked)
            checked.push_back(e.path);
    }
    nlohmann::json out{{"items", std::move(items)}};
    if (!disabled.empty())
        out["disabled"] = std::move(disabled);
    if (!checked.empty())
        out["checked"] = std::move(checked);
    return out;
}

const MenuEntry* find_menu_entry(const std::vector<MenuEntry>& entries, const std::string& path, const std::string& current,
                                 std::string& err, nlohmann::json& details)
{
    int at = -1;
    for (std::size_t start = 0;;) {
        const std::size_t end  = path.find('>', start);
        const std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        // The label itself first: a label may end in " #2" of its own.
        int found = menu_child(entries, at, menu_key(part, current), 1, current);
        if (const auto hash = part.rfind(" #"); found < 0 && hash != std::string::npos) {
            const std::string digits = part.substr(hash + 2);
            if (!digits.empty() && digits.size() <= 4 && digits.find_first_not_of("0123456789") == std::string::npos)
                found = menu_child(entries, at, menu_key(part.substr(0, hash), current), std::atoi(digits.c_str()), current);
        }
        if (found < 0) {
            err     = "The menu has no item \"" + path + "\"";
            details = menu_listing(entries);
            return nullptr;
        }
        at = found;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    const MenuEntry& e = entries[std::size_t(at)];
    if (e.submenu) {
        err     = "\"" + e.path + "\" is a submenu; name one of its items";
        details = menu_listing(entries, at);
        return nullptr;
    }
    if (!e.enabled) {
        err     = "\"" + e.path + "\" is disabled here";
        details = menu_listing(entries);
        return nullptr;
    }
    return &e;
}

std::vector<std::string> row_segments(const nlohmann::json& row)
{
    std::vector<std::string> out;
    if (row.is_array()) {
        for (const nlohmann::json& label : row)
            if (label.is_string())
                out.push_back(label.get<std::string>());
            else
                out.clear();
        if (out.size() == row.size() && !out.empty())
            return out;
    } else if (row.is_string() && !row.get<std::string>().empty()) {
        const std::string path = row.get<std::string>();
        for (std::size_t start = 0;;) {
            const std::size_t end = path.find('>', start);
            out.push_back(path.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos)
                return out;
            start = end + 1;
        }
    }
    throw ToolFailure("row is a label path, \"Cube>Cylinder\", or an array of labels");
}

nlohmann::json row_locator(const std::vector<std::string>& path)
{
    std::string joined;
    for (const std::string& label : path) {
        if (label.find('>') != std::string::npos)
            return path;
        joined += (joined.empty() ? "" : ">") + label;
    }
    return joined;
}

bool row_matches(const std::vector<std::string>& path, const std::vector<std::string>& wanted, const std::string& current)
{
    if (wanted.empty() || wanted.size() > path.size())
        return false;
    const std::size_t skip = path.size() - wanted.size();
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        const std::string& label = path[skip + i];
        const bool marked = !current.empty() && label.size() > current.size() &&
                            label.compare(label.size() - current.size(), current.size(), current) == 0;
        if (label != wanted[i] && !(marked && label.substr(0, label.size() - current.size()) == wanted[i]))
            return false;
    }
    return true;
}

namespace {

constexpr const char* kPlateIcons[kPlateIconCount] = {"plate", "delete", "orient", "arrange", "lock",
                                                      "settings", "name", "move_to_front", "filament_map", "imex_mode"};

} // namespace

int plate_icon_action(const std::string& name)
{
    for (int a = 0; a < kPlateIconCount; ++a)
        if (name == kPlateIcons[a])
            return a;
    return -1;
}

std::string plate_icon_name(int action) { return action >= 0 && action < kPlateIconCount ? kPlateIcons[action] : ""; }

std::pair<int, int> plate_icon_of(int picking_id) { return {picking_id / kPlateIconCount, picking_id % kPlateIconCount}; }

}}} // namespace Slic3r::GUI::Mcp
