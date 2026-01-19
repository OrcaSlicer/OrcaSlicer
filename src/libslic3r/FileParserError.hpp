#ifndef slic3r_FileParserError_hpp_
#define slic3r_FileParserError_hpp_

#include "libslic3r.h"

#include <string>
#include <boost/filesystem/path.hpp>
#include <stdexcept>

namespace Slic3r {

// Generic file parser error, mostly copied from boost::property_tree::file_parser_error
class file_parser_error: public Slic3r::RuntimeError
{
public:
    file_parser_error(const std::string &msg, const std::string &file, unsigned long line = 0) :
        Slic3r::RuntimeError(format_what(msg, file, line)),
        m_message(msg), m_filename(file), m_line(line) {}
    file_parser_error(const std::string &msg, const boost::filesystem::path &file, unsigned long line = 0) :
        Slic3r::RuntimeError(format_what(msg, file.string(), line)),
        m_message(msg), m_filename(file.string()), m_line(line) {}
    // gcc 3.4.2 complains about lack of throw specifier on compiler
    // generated dtor
    ~file_parser_error() throw() {}

    // Get error message (without line and file - use what() to get full message)
    std::string message() const { return m_message; }
    // Get error filename
    std::string filename() const { return m_filename; }
    // Get error line number
    unsigned long line() const { return m_line; }

private:
    std::string     m_message;
    std::string     m_filename;
    unsigned long   m_line;

    // Format error message to be returned by Slic3r::RuntimeError::what()
    static std::string format_what(const std::string &msg, const std::string &file, unsigned long l)
    {
        std::stringstream stream;
        stream << (file.empty() ? "<unspecified file>" : file.c_str());
        if (l > 0)
            stream << '(' << l << ')';
        stream << ": " << msg;
        return stream.str();
    }
};

}; // Slic3r

#endif // slic3r_FileParserError_hpp_
