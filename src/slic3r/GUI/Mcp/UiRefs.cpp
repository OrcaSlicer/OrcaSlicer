#include "UiRefs.hpp"

#include <cstddef>
#include <string>
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
        out += "... " + std::to_string(omitted) + " more nodes; narrow with root, query or a larger max_nodes\n";
    return out;
}

}}} // namespace Slic3r::GUI::Mcp
