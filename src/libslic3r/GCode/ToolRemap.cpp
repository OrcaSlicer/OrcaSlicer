#include "ToolRemap.hpp"
#include "ZmodHeader.hpp"

#include <cctype>
#include <cstddef>
#include <string_view>

namespace Slic3r {

namespace {

// The physical tool for a `T<n>` token, or -1 when the token is not a tool number in the map.
int mapped_tool(std::string_view token, const std::map<int, int>& tool_map)
{
    if (token.size() < 2 || std::toupper(static_cast<unsigned char>(token[0])) != 'T' || token.size() > 7)
        return -1;
    int tool = 0;
    for (char c : token.substr(1)) {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return -1;
        tool = tool * 10 + (c - '0');
    }
    auto it = tool_map.find(tool);
    return it == tool_map.end() ? -1 : it->second;
}

bool is_command(std::string_view token, std::string_view command)
{
    if (token.size() != command.size())
        return false;
    for (size_t i = 0; i < token.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(token[i])) != command[i])
            return false;
    return true;
}

// Rewrites the code part of a line, the text before any comment.
std::string remap_code(const std::string& code, const std::map<int, int>& tool_map)
{
    std::string out;
    out.reserve(code.size() + 2);
    bool   tool_arguments = false; // inside an M104 / M109, whose T argument names a tool
    size_t pos            = 0;
    for (bool first = true; pos < code.size(); first = false) {
        const size_t start = code.find_first_not_of(" \t", pos);
        out.append(code, pos, (start == std::string::npos ? code.size() : start) - pos);
        if (start == std::string::npos)
            break;
        size_t end = code.find_first_of(" \t", start);
        if (end == std::string::npos)
            end = code.size();
        const std::string_view token(code.data() + start, end - start);
        if (first)
            tool_arguments = is_command(token, "M104") || is_command(token, "M109");
        // A tool change is a T<n> command standing alone; elsewhere T<n> is only a tool in M104 / M109.
        const int tool = (first || tool_arguments) ? mapped_tool(token, tool_map) : -1;
        if (tool >= 0) {
            out += token[0];
            out += std::to_string(tool);
        } else {
            out.append(token);
        }
        pos = end;
    }
    return out;
}

} // namespace

std::string remap_gcode_tools(const std::string&                       gcode,
                              const std::map<int, int>&                tool_map,
                              const std::function<void(std::string&)>& rewrite_comment)
{
    // A leading "; MD5:<hex>" line covers every byte after it; keep its line ending.
    size_t      body_start = 0;
    std::string md5_line_end;
    if (gcode.compare(0, 6, "; MD5:") == 0) {
        body_start   = gcode.find('\n');
        body_start   = body_start == std::string::npos ? gcode.size() : body_start + 1;
        md5_line_end = (body_start >= 2 && gcode[body_start - 2] == '\r') ? "\r\n" : "\n";
    }

    std::string body;
    body.reserve(gcode.size() - body_start + 64);
    for (size_t pos = body_start; pos < gcode.size();) {
        size_t line_end = gcode.find('\n', pos);
        line_end        = line_end == std::string::npos ? gcode.size() : line_end;
        size_t text_end = line_end;
        if (text_end > pos && gcode[text_end - 1] == '\r')
            --text_end;

        std::string  line(gcode, pos, text_end - pos);
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] == ';') {
            if (rewrite_comment)
                rewrite_comment(line);
        } else {
            const size_t comment = line.find(';');
            line = remap_code(line.substr(0, comment), tool_map) + (comment == std::string::npos ? "" : line.substr(comment));
        }
        body += line;
        body.append(gcode, text_end, (line_end < gcode.size() ? line_end + 1 : line_end) - text_end);
        pos = line_end + 1;
    }

    return md5_line_end.empty() ? body : "; MD5:" + md5_hex(body) + md5_line_end + body;
}

} // namespace Slic3r
