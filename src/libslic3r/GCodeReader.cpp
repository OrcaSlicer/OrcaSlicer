#include "GCodeReader.hpp"
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/cstdio.hpp>
#include <fstream>
#include <iostream>
#include <iomanip>
#include "Utils.hpp"

#include "LocalesUtils.hpp"

#include <Shiny/Shiny.h>
#include <fast_float/fast_float.h>

namespace Slic3r {

void GCodeReader::apply_config(const GCodeConfig &config)
{
    m_config = config;
}

void GCodeReader::apply_config(const DynamicPrintConfig &config)
{
    m_config.apply(config, true);
}

const char* GCodeReader::parse_line_internal(const char *ptr, const char *end, GCodeLine &gline, std::pair<const char*, const char*> &command)
{
    PROFILE_FUNC();

    assert(is_decimal_separator_point());
    
    // command and args
    const char *c = ptr;
    {
        PROFILE_BLOCK(command_and_args);
        // Skip the whitespaces.
        command.first = skip_whitespaces(c);
        // Skip the command.
        c = command.second = skip_word(command.first);
        // Up to the end of line or comment.
		while (! is_end_of_gcode_line(*c)) {
            // Skip whitespaces.
            c = skip_whitespaces(c);
			if (is_end_of_gcode_line(*c))
				break;
            // Check the name of the axis.
            Axis axis = NUM_AXES_WITH_UNKNOWN;
            switch (*c) {
            case 'X': axis = X; break;
            case 'Y': axis = Y; break;
            case 'Z': axis = Z; break;
            case 'F': axis = F; break;
            //BBS: add I and J axis
            case 'I': axis = I; break;
            case 'J': axis = J; break;
            case 'E': axis = E; break;
            case 'P': axis = P; break;
            default:
                if (*c >= 'A' && *c <= 'Z')
                	// Unknown axis, but we still want to remember that such a axis was seen.
                	axis = UNKNOWN_AXIS;
                break;
            }
            if (axis != NUM_AXES_WITH_UNKNOWN) {
                // Try to parse the numeric value.
                double v;
                auto [pend, ec] = fast_float::from_chars(++ c, end, v);
                if (pend != c && is_end_of_word(*pend)) {
                    // The axis value has been parsed correctly.
                    if (axis != UNKNOWN_AXIS)
	                    gline.m_axis[int(axis)] = float(v);
                    gline.m_mask |= 1 << int(axis);
                    c = pend;
                } else
                    // Skip the rest of the word.
                    c = skip_word(c);
            } else
                // Skip the rest of the word.
                c = skip_word(c);
        }
    }
    
    if (gline.has(E) && m_config.use_relative_e_distances)
        m_position[E] = 0;

    // Skip the rest of the line.
    for (; ! is_end_of_line(*c); ++ c);

    // Copy the raw string including the comment, without the trailing newlines.
    if (c > ptr) {
        PROFILE_BLOCK(copy_raw_string);
        gline.m_raw.assign(ptr, c);
    }

    // Skip the trailing newlines.
	if (*c == '\r')
		++ c;
	if (*c == '\n')
		++ c;

    if (m_verbose)
        std::cout << gline.m_raw << std::endl;

    return c;
}

void GCodeReader::update_coordinates(GCodeLine &gline, std::pair<const char*, const char*> &command)
{
    PROFILE_FUNC();
    if (*command.first == 'G') {
        int cmd_len = int(command.second - command.first);
        //BBS: add support of G2 and G3
        if ((cmd_len == 2 && (command.first[1] == '0' || command.first[1] == '1' || command.first[1] == '2' || command.first[1] == '3')) ||
            (cmd_len == 3 &&  command.first[1] == '9' && command.first[2] == '2')) {
            for (size_t i = 0; i < NUM_AXES; ++ i)
                if (gline.has(Axis(i)))
                    m_position[i] = gline.value(Axis(i));
        }
    }
}

template<typename ParseLineCallback, typename LineEndCallback>
bool GCodeReader::parse_file_raw_internal(const std::string &filename, ParseLineCallback parse_line_callback, LineEndCallback line_end_callback)
{
    FilePtr in{ boost::nowide::fopen(filename.c_str(), "rb") };

    // Read the input stream 64kB at a time, extract lines and process them.
    std::vector<char> buffer(65536 * 10, 0);
    // Line buffer.
    std::string gcode_line;
    size_t file_pos = 0;
    m_parsing = true;
    for (;;) {
        size_t cnt_read = ::fread(buffer.data(), 1, buffer.size(), in.f);
        if (::ferror(in.f))
            return false;
        bool eof       = cnt_read == 0;
        auto it        = buffer.begin();
        auto it_bufend = buffer.begin() + cnt_read;
        while (it != it_bufend || (eof && ! gcode_line.empty())) {
            // Find end of line.
            bool eol    = false;
            auto it_end = it;
            for (; it_end != it_bufend && ! (eol = *it_end == '\r' || *it_end == '\n'); ++ it_end)
                if (*it_end == '\n')
                    line_end_callback(file_pos + (it_end - buffer.begin()) + 1);
            // End of line is indicated also if end of file was reached.
            eol |= eof && it_end == it_bufend;
            if (eol) {
                if (gcode_line.empty())
                    parse_line_callback(&(*it), &(*it_end));
                else {
                    gcode_line.insert(gcode_line.end(), it, it_end);
                    parse_line_callback(gcode_line.c_str(), gcode_line.c_str() + gcode_line.size());
                    gcode_line.clear();
                }
                if (! m_parsing)
                    // The callback wishes to exit.
                    return true;
            } else
                gcode_line.insert(gcode_line.end(), it, it_end);
            // Skip EOL.
            it = it_end; 
            if (it != it_bufend && *it == '\r')
                ++ it;
            if (it != it_bufend && *it == '\n') {
                line_end_callback(file_pos + (it - buffer.begin()) + 1);
                ++ it;
            }
        }
        if (eof)
            break;
        file_pos += cnt_read;
    }
    return true;
}

template<typename ParseLineCallback, typename LineEndCallback>
bool GCodeReader::parse_file_internal(const std::string &filename, ParseLineCallback parse_line_callback, LineEndCallback line_end_callback)
{
    GCodeLine gline;    
    return this->parse_file_raw_internal(filename, 
        [this, &gline, parse_line_callback](const char *begin, const char *end) {
            gline.reset();

            const char* begin_new = begin;
            begin_new = skip_whitespaces(begin_new);
            if (std::toupper(*begin_new) == 'N')
                begin_new = skip_word(begin_new);
            begin_new = skip_whitespaces(begin_new);
            this->parse_line(begin_new, end, gline, parse_line_callback);
        }, 
        line_end_callback);
}

bool GCodeReader::parse_file(const std::string &file, callback_t callback)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":  before parse_file %1%") % file.c_str();
    auto ret = this->parse_file_internal(file, callback, [](size_t) {});
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":  finished parse_file %1%") % file.c_str();

    return ret;
}

bool GCodeReader::parse_file(const std::string &file, callback_t callback, std::vector<size_t> &lines_ends)
{
    lines_ends.clear();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":  before parse_file %1%") % file.c_str();
    auto ret = this->parse_file_internal(file, callback, [&lines_ends](size_t file_pos){ lines_ends.emplace_back(file_pos); });
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":  finished parse_file %1%") % file.c_str();

    return ret;
}

bool GCodeReader::parse_file_raw(const std::string &filename, raw_line_callback_t line_callback)
{
    return this->parse_file_raw_internal(filename,
        [this, line_callback](const char *begin, const char *end) { line_callback(*this, begin, end); }, 
        [](size_t){});
}

const char* GCodeReader::axis_pos(const char *raw_str, char axis)
{
    const char *c = raw_str;
    // Skip the whitespaces.
    c = skip_whitespaces(c);
    // Skip the command.
    c = skip_word(c);
    // Up to the end of line or comment.
    while (! is_end_of_gcode_line(*c)) {
        // Skip whitespaces.
        c = skip_whitespaces(c);
        if (is_end_of_gcode_line(*c))
            break;
        // Check the name of the axis.
        if (*c == axis)
            return c;
        // Skip the rest of the word.
        c = skip_word(c);
    }
    return nullptr;
}

bool GCodeReader::GCodeLine::has(char axis) const
{
    const char *c = m_raw.c_str();
    // Skip the whitespaces.
    c = skip_whitespaces(c);
    // Skip the command.
    c = skip_word(c);
    // Up to the end of line or comment.
    while (! is_end_of_gcode_line(*c)) {
        // Skip whitespaces.
        c = skip_whitespaces(c);
        if (is_end_of_gcode_line(*c))
            break;
        // Check the name of the axis.
        if (*c == axis)
            return true;
        // Skip the rest of the word.
        c = skip_word(c);
    }
    return false;
}

std::string_view GCodeReader::GCodeLine::axis_pos(char axis) const
{
    const std::string &s = this->raw();
    const char *c = GCodeReader::axis_pos(this->raw().c_str(), axis);
    return c ? std::string_view{ c, s.size() - (c - s.data()) } : std::string_view();
}

bool GCodeReader::GCodeLine::has_value(std::string_view axis_pos, float &value)
{
    if (const char *c = axis_pos.data(); c) {
        // Try to parse the numeric value.
        double v = 0.;
        const char *end = axis_pos.data() + axis_pos.size();
        auto [pend, ec] = fast_float::from_chars(++ c, end, v);
        if (pend != c && is_end_of_word(*pend)) {
            // The axis value has been parsed correctly.
            value = float(v);
            return true;
        }
    }
    return false;
}

bool GCodeReader::GCodeLine::has_value(char axis, float &value) const
{
    assert(is_decimal_separator_point());
    const char *c = m_raw.c_str();
    // Skip the whitespaces.
    c = skip_whitespaces(c);
    // Skip the command.
    c = skip_word(c);
    // Up to the end of line or comment.
    while (! is_end_of_gcode_line(*c)) {
        // Skip whitespaces.
        c = skip_whitespaces(c);
        if (is_end_of_gcode_line(*c))
            break;
        // Check the name of the axis.
        if (*c == axis) {
            // Try to parse the numeric value.
            char   *pend = nullptr;
            double  v = strtod(++ c, &pend);
            if (pend != nullptr && is_end_of_word(*pend)) {
                // The axis value has been parsed correctly.
                value = float(v);
                return true;
            }
        }
        // Skip the rest of the word.
        c = skip_word(c);
    }
    return false;
}

void GCodeReader::GCodeLine::set(const Axis axis, const float new_value, const int decimal_digits)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(decimal_digits) << new_value;

    char match[3] = " X";
    if (int(axis) < 3)
        match[1] += int(axis);
    else if (axis == F)
        match[1] = 'F';
    //BBS： handle I and J axis
    else if (axis == I)
        match[1] = 'I';
    else if (axis == J)
        match[1] = 'J';
    else {
        assert(axis == E);
        match[1] = 'E';
    }

    if (this->has(axis)) {
        size_t pos = m_raw.find(match)+2;
        size_t end = m_raw.find(' ', pos+1);
        m_raw = m_raw.replace(pos, end-pos, ss.str());
    } else {
        size_t pos = m_raw.find(' ');
        if (pos == std::string::npos)
            m_raw += std::string(match) + ss.str();
        else
            m_raw = m_raw.replace(pos, 0, std::string(match) + ss.str());
    }
    m_axis[axis] = new_value;
    m_mask |= 1 << int(axis);
}

}
