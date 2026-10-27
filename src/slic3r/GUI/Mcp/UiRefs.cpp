#include "UiRefs.hpp"

#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

std::string clean_imgui_label(const std::string& label)
{
    const auto hash = label.find("##");
    std::string out = hash == std::string::npos ? label : label.substr(0, hash);
    // Orca marks colored text and icons with control characters; drop them and the padding.
    std::string clean;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(out[i]);
        if (c >= 0x20)
            clean.push_back(char(c));
    }
    const auto first = clean.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    return clean.substr(first, clean.find_last_not_of(' ') - first + 1);
}

std::string format_snapshot(const std::vector<UiNode>& nodes, std::size_t omitted)
{
    std::string out;
    for (const UiNode& n : nodes) {
        out.append(std::size_t(n.depth) * 2, ' ');
        out += "[" + n.ref + "] " + n.kind;
        if (!n.label.empty())
            out += " \"" + n.label + "\"";
        if (!n.value.empty())
            out += " = \"" + n.value + "\"";
        if (!n.cls.empty() && n.cls != n.kind)
            out += " (" + n.cls + ")";
        if (!n.name.empty())
            out += " name=" + n.name;
        if (!n.option.empty())
            out += " option=" + n.option;
        if (n.checked >= 0)
            out += n.checked ? " [checked]" : " [unchecked]";
        if (!n.enabled)
            out += " [disabled]";
        if (n.focused)
            out += " [focused]";
        if (n.w > 0 && n.h > 0)
            out += " @" + std::to_string(n.x) + "," + std::to_string(n.y) + " " + std::to_string(n.w) + "x" + std::to_string(n.h);
        out += "\n";
    }
    if (omitted > 0)
        out += "... " + std::to_string(omitted) + " more nodes; narrow with root, query or a larger max_nodes\n";
    return out;
}

}}} // namespace Slic3r::GUI::Mcp
