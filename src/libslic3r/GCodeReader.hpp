#ifndef slic3r_GCodeReader_hpp_
#define slic3r_GCodeReader_hpp_

#include "libslic3r.h"
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include "PrintConfig.hpp"

namespace Slic3r {

class GCodeReader {
public:
    class GCodeLine {
    public:
        GCodeLine() { reset(); }
        void reset() { m_mask = 0; memset(m_axis, 0, sizeof(m_axis)); m_raw.clear(); }

        const std::string&      raw() const { return m_raw; }
        const std::string_view  cmd() const { 
            const char *cmd = GCodeReader::skip_whitespaces(m_raw.c_str());
            return std::string_view(cmd, GCodeReader::skip_word(cmd) - cmd);
        }
        const std::string_view  comment() const
            { size_t pos = m_raw.find(';'); return (pos == std::string::npos) ? std::string_view() : std::string_view(m_raw).substr(pos + 1); }

        // Return position in this->raw() string starting with the "axis" character.
        std::string_view axis_pos(char axis) const;
        void  clear() { m_raw.clear(); }
        bool  has(Axis axis) const { return (m_mask & (1 << int(axis))) != 0; }
        float value(Axis axis) const { return m_axis[axis]; }
        bool  has(char axis) const;
        bool  has_value(char axis, float &value) const;
        // Parse value of an axis from raw string starting at axis_pos.
        static bool has_value(std::string_view axis_pos, float &value);
        float new_X(const GCodeReader &reader) const { return this->has(X) ? this->x() : reader.x(); }
        float new_Y(const GCodeReader &reader) const { return this->has(Y) ? this->y() : reader.y(); }
        float new_Z(const GCodeReader &reader) const { return this->has(Z) ? this->z() : reader.z(); }
        float new_E(const GCodeReader &reader) const { return this->has(E) ? this->e() : reader.e(); }
        float new_F(const GCodeReader &reader) const { return this->has(F) ? this->f() : reader.f(); }
        float dist_X(const GCodeReader &reader) const { return this->has(X) ? (this->x() - reader.x()) : 0; }
        float dist_Y(const GCodeReader &reader) const { return this->has(Y) ? (this->y() - reader.y()) : 0; }
        float dist_Z(const GCodeReader &reader) const { return this->has(Z) ? (this->z() - reader.z()) : 0; }
        float dist_E(const GCodeReader &reader) const { return this->has(E) ? (this->e() - reader.e()) : 0; }
        float dist_XY(const GCodeReader &reader) const {
            float x = this->has(X) ? (this->x() - reader.x()) : 0;
            float y = this->has(Y) ? (this->y() - reader.y()) : 0;
            return sqrt(x*x + y*y);
        }
        bool cmd_is(const char *cmd_test)          const { return cmd_is(m_raw, cmd_test); }
        //BBS: modify to support G2 and G3
        bool extruding(const GCodeReader &reader)  const { return (this->cmd_is("G1") || this->cmd_is("G2") || this->cmd_is("G3")) && this->dist_E(reader) > 0; }
        bool retracting(const GCodeReader &reader) const { return (this->cmd_is("G1") || this->cmd_is("G2") || this->cmd_is("G3")) && this->dist_E(reader) < 0; }
        bool travel()     const { return (this->cmd_is("G1") || this->cmd_is("G2") || this->cmd_is("G3")) && ! this->has(E); }
        void set(const Axis axis, const float new_value, const int decimal_digits = 3);

        bool  has_x() const { return this->has(X); }
        bool  has_y() const { return this->has(Y); }
        bool  has_z() const { return this->has(Z); }
        bool  has_e() const { return this->has(E); }
        bool  has_f() const { return this->has(F); }
        // BBS: add I J P axis
        bool  has_i() const { return this->has(I); }
        bool  has_j() const { return this->has(J); }
        bool  has_p() const { return this->has(P); }

        bool  has_unknown_axis() const { return this->has(UNKNOWN_AXIS); }
        float x() const { return m_axis[X]; }
        float y() const { return m_axis[Y]; }
        float z() const { return m_axis[Z]; }
        float e() const { return m_axis[E]; }
        float f() const { return m_axis[F]; }
        // BBS: add I J P axis
        float i() const { return m_axis[I]; }
        float j() const { return m_axis[J]; }
        float p() const { return m_axis[P]; }

        static bool cmd_is(const std::string &gcode_line, const char *cmd_test) {
            const char *cmd = GCodeReader::skip_whitespaces(gcode_line.c_str());
            size_t len = strlen(cmd_test); 
            return strncmp(cmd, cmd_test, len) == 0 && GCodeReader::is_end_of_word(cmd[len]);
        }

        static bool cmd_starts_with(const std::string& gcode_line, const char* cmd_test) {
            return strncmp(GCodeReader::skip_whitespaces(gcode_line.c_str()), cmd_test, strlen(cmd_test)) == 0;
        }

        static std::string extract_cmd(const std::string& gcode_line) {
            GCodeLine temp;
            temp.m_raw = gcode_line;
            const std::string_view cmd = temp.cmd();
            return { cmd.begin(), cmd.end() };
        }
    private:
        std::string      m_raw;
        float            m_axis[NUM_AXES];
        uint32_t         m_mask;
        friend class GCodeReader;
    };

    typedef std::function<void(GCodeReader&, const GCodeLine&)> callback_t;
    typedef std::function<void(GCodeReader&, const char*, const char*)> raw_line_callback_t;
    
    GCodeReader() : m_verbose(false) { this->reset(); }
    void reset() { memset(m_position, 0, sizeof(m_position)); }
    void apply_config(const GCodeConfig &config);
    void apply_config(const DynamicPrintConfig &config);
    const GCodeConfig& config() { return m_config; };

    template<typename Callback>
    void parse_buffer(const std::string &buffer, Callback callback)
    {
        const char *ptr = buffer.c_str();
        const char *end = ptr + buffer.size();
        GCodeLine gline;
        m_parsing = true;
        while (m_parsing && *ptr != 0) {
            gline.reset();
            ptr = this->parse_line(ptr, end, gline, callback);
        }
    }

    void parse_buffer(const std::string &buffer)
        { this->parse_buffer(buffer, [](GCodeReader&, const GCodeReader::GCodeLine&){}); }

    template<typename Callback>
    const char* parse_line(const char *ptr, const char *end, GCodeLine &gline, Callback &callback)
    {
        std::pair<const char*, const char*> cmd;
        const char *line_end = parse_line_internal(ptr, end, gline, cmd);
        callback(*this, gline);
        update_coordinates(gline, cmd);
        return line_end;
    }

    template<typename Callback>
    void parse_line(const std::string &line, Callback callback)
        { GCodeLine gline; this->parse_line(line.c_str(), line.c_str() + line.size(), gline, callback); }

    // Returns false if reading the file failed.
    bool parse_file(const std::string &file, callback_t callback);
    // Collect positions of line ends in the binary G-code to be used by the G-code viewer when memory mapping and displaying section of G-code
    // as an overlay in the 3D scene.
    bool parse_file(const std::string &file, callback_t callback, std::vector<size_t> &lines_ends);
    // Just read the G-code file line by line, calls callback (const char *begin, const char *end). Returns false if reading the file failed.
    bool parse_file_raw(const std::string &file, raw_line_callback_t callback);

    // To be called by the callback to stop parsing.
    void quit_parsing() { m_parsing = false; }

    float& x()       { return m_position[X]; }
    float  x() const { return m_position[X]; }
    float& y()       { return m_position[Y]; }
    float  y() const { return m_position[Y]; }
    float& z()       { return m_position[Z]; }
    float  z() const { return m_position[Z]; }
    float& e()       { return m_position[E]; }
    float  e() const { return m_position[E]; }
    float& f()       { return m_position[F]; }
    float  f() const { return m_position[F]; }
    // BBS: add I J axis
    float& i()       { return m_position[I]; }
    float  i() const { return m_position[I]; }
    float& j()       { return m_position[J]; }
    float  j() const { return m_position[J]; }

    GCodeConfig get_config() const
    { 
        return m_config;
    }

private:
    template<typename ParseLineCallback, typename LineEndCallback>
    bool        parse_file_raw_internal(const std::string &filename, ParseLineCallback parse_line_callback, LineEndCallback line_end_callback);
    template<typename ParseLineCallback, typename LineEndCallback>
    bool        parse_file_internal(const std::string &filename, ParseLineCallback parse_line_callback, LineEndCallback line_end_callback);

    const char* parse_line_internal(const char *ptr, const char *end, GCodeLine &gline, std::pair<const char*, const char*> &command);
    void        update_coordinates(GCodeLine &gline, std::pair<const char*, const char*> &command);

    static bool         is_whitespace(char c)           { return c == ' ' || c == '\t'; }
    static bool         is_end_of_line(char c)          { return c == '\r' || c == '\n' || c == 0; }
    static bool         is_end_of_gcode_line(char c)    { return c == ';' || is_end_of_line(c); }
    static bool         is_end_of_word(char c)          { return is_whitespace(c) || is_end_of_gcode_line(c); }
    static const char*  skip_whitespaces(const char *c) { 
        for (; is_whitespace(*c); ++ c)
            ; // silence -Wempty-body
        return c;
    }
    static const char*  skip_word(const char *c) { 
        for (; ! is_end_of_word(*c); ++ c)
            ; // silence -Wempty-body
        return c;
    }
    static const char*  axis_pos(const char *raw_str, char axis);

    GCodeConfig m_config;
    float       m_position[NUM_AXES];
    bool        m_verbose;
    // To be set by the callback to stop parsing.
    bool        m_parsing{ false };
};

} /* namespace Slic3r */

#endif /* slic3r_GCodeReader_hpp_ */
