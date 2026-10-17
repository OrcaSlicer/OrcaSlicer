#include "ZmodHeader.hpp"

#include "libslic3r/Config.hpp"

#include <openssl/md5.h>

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <vector>

namespace Slic3r {

namespace {

const std::string COLOR_DATA_KEY = "; zmod_color_data =";

// The preprocessor matches lines stripped and case-folded.
std::string folded(const std::string& line)
{
    const size_t first = line.find_first_not_of(" \t\r\n");
    const size_t last  = line.find_last_not_of(" \t\r\n");
    std::string  out   = first == std::string::npos ? std::string() : line.substr(first, last - first + 1);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

bool starts_with(const std::string& text, const std::string& prefix) { return text.compare(0, prefix.size(), prefix) == 0; }

// The values of a "; key = a;b;c" line.
std::vector<std::string> list_after_equals(const std::string& line)
{
    std::string value = folded(line.substr(line.find('=') + 1));
    std::vector<std::string> values;
    if (!value.empty())
        boost::split(values, value, boost::is_any_of(";"));
    return values;
}

} // namespace

bool printer_wants_zmod_header(const ConfigBase& printer_config)
{
    const auto* agent = printer_config.option<ConfigOptionString>("printer_agent");
    return agent != nullptr && agent->value == ZMOD_PRINTER_AGENT_ID;
}

std::string md5_hex(const std::string& data)
{
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_CTX       ctx;
    MD5_Init(&ctx);
    MD5_Update(&ctx, data.data(), data.size());
    MD5_Final(digest, &ctx);
    static const char hex[] = "0123456789abcdef";
    std::string       out;
    for (unsigned char byte : digest) {
        out += hex[byte >> 4];
        out += hex[byte & 0xF];
    }
    return out;
}

std::string add_zmod_header(const std::string& gcode)
{
    std::vector<std::string> lines; // each with its line ending
    for (size_t pos = 0; pos < gcode.size();) {
        const size_t end = std::min(gcode.find('\n', pos), gcode.size() - 1) + 1;
        lines.emplace_back(gcode, pos, end - pos);
        pos = end;
    }
    if (!lines.empty() && starts_with(folded(lines.front()), "; md5"))
        lines.erase(lines.begin());

    std::vector<int>         tools;
    int                      highest = -1;
    std::vector<std::string> colours, types;
    for (const std::string& raw : lines) {
        const std::string line = folded(raw);
        if (line.empty())
            continue;
        if (line[0] == 't') {
            std::string word = line.substr(0, line.find(';'));
            word             = folded(word.substr(0, word.find(' ')));
            if (word.size() > 1 && std::all_of(word.begin() + 1, word.end(), [](unsigned char c) { return std::isdigit(c); })) {
                const int tool = std::stoi(word.substr(1));
                if (std::find(tools.begin(), tools.end(), tool) == tools.end())
                    tools.push_back(tool);
                highest = std::max(highest, tool);
            }
        } else if (starts_with(line, "; filament_colour =")) {
            colours = list_after_equals(line);
        } else if (starts_with(line, "; filament_type =")) {
            types = list_after_equals(line);
        }
    }
    if (tools.empty()) {
        tools   = {0};
        highest = 0;
    }
    if (int(colours.size()) <= highest)
        colours.resize(highest + 1);
    if (int(types.size()) <= highest)
        types.resize(highest + 1);

    std::vector<std::string> tool_names;
    for (int tool : tools)
        tool_names.push_back(std::to_string(tool));
    const std::string color_data = COLOR_DATA_KEY + " " + boost::join(tool_names, ",") + "|" + boost::join(colours, ",") + "|" +
                                   boost::join(types, ",") + "\r\n";

    // Replace an existing line, placing the new one before the header's end (or first, without one).
    auto existing = std::find_if(lines.begin(), lines.end(), [](const std::string& l) { return starts_with(folded(l), COLOR_DATA_KEY); });
    if (existing != lines.end())
        lines.erase(existing);
    auto header_end = std::find_if(lines.begin(), lines.end(), [](const std::string& l) { return starts_with(folded(l), "; header_block_end"); });
    lines.insert(header_end == lines.end() ? lines.begin() : header_end, color_data);

    std::string body;
    body.reserve(gcode.size() + color_data.size());
    for (const std::string& line : lines)
        body += line;
    return "; MD5:" + md5_hex(body) + "\r\n" + body;
}

bool add_zmod_header_to_file(const std::string& path)
{
    std::string gcode;
    {
        boost::nowide::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        gcode.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string out_gcode = add_zmod_header(gcode);
    boost::nowide::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(out_gcode.data(), std::streamsize(out_gcode.size()));
    return bool(out);
}

void remap_zmod_color_data(std::string& line, const std::map<int, int>& tool_map)
{
    if (!starts_with(line, COLOR_DATA_KEY))
        return;
    std::vector<std::string> fields;
    const std::string        value = line.substr(COLOR_DATA_KEY.size());
    boost::split(fields, value, boost::is_any_of("|"));
    if (fields.size() != 3)
        return;
    std::vector<std::string> tools, colours, types;
    boost::split(tools, fields[0], boost::is_any_of(","));
    boost::split(colours, fields[1], boost::is_any_of(","));
    boost::split(types, fields[2], boost::is_any_of(","));

    std::vector<std::string> new_tools, new_colours, new_types;
    for (const std::string& token : tools) {
        const int  tool   = std::atoi(token.c_str());
        const auto mapped = tool_map.find(tool);
        const int  target = mapped == tool_map.end() ? tool : mapped->second;
        const auto name   = std::to_string(target);
        if (target < 0 || std::find(new_tools.begin(), new_tools.end(), name) != new_tools.end())
            continue;
        new_tools.push_back(name);
        if (size_t(target) >= new_colours.size()) {
            new_colours.resize(target + 1);
            new_types.resize(target + 1);
        }
        new_colours[target] = size_t(tool) < colours.size() ? colours[tool] : std::string();
        new_types[target]   = size_t(tool) < types.size() ? types[tool] : std::string();
    }
    line = COLOR_DATA_KEY + " " + boost::join(new_tools, ",") + "|" + boost::join(new_colours, ",") + "|" + boost::join(new_types, ",");
}

} // namespace Slic3r
