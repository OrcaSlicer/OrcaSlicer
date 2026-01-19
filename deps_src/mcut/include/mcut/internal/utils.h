/**
 * Copyright (c) 2021-2022 Floyd M. Chitalu.
 * All rights reserved.
 *
 * NOTE: This file is licensed under GPL-3.0-or-later (default).
 * A commercial license can be purchased from Floyd M. Chitalu.
 *
 * License details:
 *
 * (A)  GNU General Public License ("GPL"); a copy of which you should have
 *      recieved with this file.
 * 	    - see also: <http://www.gnu.org/licenses/>
 * (B)  Commercial license.
 *      - email: floyd.m.chitalu@gmail.com
 *
 * The commercial license options is for users that wish to use MCUT in
 * their products for comercial purposes but do not wish to release their
 * software products under the GPL license.
 *
 * Author(s)     : Floyd M. Chitalu
 */

#ifndef MCUT_UTILS_H_
#define MCUT_UTILS_H_

// check if c++11 is supported
#if __cplusplus >= 201103L || (defined(_MSC_VER) && _MSC_VER >= 1900)
#define MCUT_CXX11_IS_SUPPORTED
#elif !defined(__cplusplus) && !defined(_MSC_VER)
typedef char couldnt_parse_cxx_standard[-1]; ///< Error: couldn't parse standard
#endif

#if defined(_WIN64) || defined(_WIN32)
#define MCUT_BUILD_WINDOWS 1
#elif defined(__APPLE__)
#define MCUT_BUILD_APPLE 1
#elif defined(__linux__) || defined(__unix__)
#define MCUT_BUILD_LINUX 1
#endif // #if defined(_WIN64) || defined(_WIN32)

#define MCUT_MAKE_STRING__(s) #s
#define MCUT_MAKE_STRING_(s) MCUT_MAKE_STRING__(s)

#ifndef NDEBUG
#define MCUT_DEBUG_BUILD 1
#endif

// debug macros
#if defined(MCUT_DEBUG_BUILD)
//
// MCUT_DEBUG_BREAKPOINT
//
#if defined(MCUT_BUILD_WINDOWS)
#define MCUT_DEBUG_BREAKPOINT_() __debugbreak()
#else // #if defined(MCUT_BUILD_WINDOWS)
#define MCUT_DEBUG_BREAKPOINT_() std::abort()
#endif // #if defined(MCUT_BUILD_WINDOWS)

//
// MCUT_ASSERT
//
#define MCUT_ASSERT(a)                   \
    do {                                 \
        if (false == (a)) {              \
            std::fprintf(stderr,         \
                "Assertion failed: %s, " \
                "%d at \'%s\'\n",        \
                __FILE__,                \
                __LINE__,                \
                MCUT_MAKE_STRING_(a));   \
            MCUT_DEBUG_BREAKPOINT_();    \
        }                                \
    } while (0)

#define DEBUG_CODE_MASK(code) code
#else // #if defined(MCUT_DEBUG_BUILD)
//
// MCUT_ASSERT
//
#define MCUT_ASSERT(a) // do nothing
#define DEBUG_CODE_MASK(code) // do nothing
#endif // #if defined(MCUT_DEBUG_BUILD)

#include <fstream>
#include <iostream>
#include <sstream>

#if MCUT_BUILD_WINDOWS
#define EXCEPTION_THROWN throw()
#else
#define EXCEPTION_THROWN
#endif

#define PEDANTIC_SUBSCRIPT_ACCESS 1

#if defined(PEDANTIC_SUBSCRIPT_ACCESS)
#define SAFE_ACCESS(var, i) var.at(i)
#else
#define SAFE_ACCESS(var, i) var[i]
#endif

static inline int wrap_integer(int x, const int lo, const int hi)
{
    const int range_size = hi - lo + 1;

    if (x < lo) {
        x += range_size * ((lo - x) / range_size + 1);
    }

    return lo + (x - lo) % range_size;
}

class logger_t {

    std::stringstream m_buffer;
    bool m_verbose;
    std::string m_prepend;
    std::string m_reason_for_failure;

public:
    typedef std::ostream& (*ManipFn)(std::ostream&);
    typedef std::ios_base& (*FlagsFn)(std::ios_base&);

    logger_t()
        : m_buffer()
        , m_verbose(false)
        , m_prepend()
        , m_reason_for_failure()
    {
    }
    logger_t(const logger_t& other) = delete;
    logger_t& operator=(const logger_t& other) = delete;

    ~logger_t()
    {
    }

    std::string get_log_string()
    {
        return m_buffer.str();
    }

    void set_reason_for_failure(const std::string& msg)
    {
        if (m_reason_for_failure.empty()) // NOTE
            m_reason_for_failure = msg;
    }

    std::string get_reason_for_failure()
    {
        std::string s(m_reason_for_failure); // copy
        return s;
    }

    inline bool verbose()
    {
        return m_verbose;
    }

    inline void set_verbose(bool b)
    {
        m_verbose = b;
    }

    inline void reset()
    {
        m_prepend.clear();
    }

    inline void indent()
    {
        if (!verbose()) {
            return;
        }
        m_prepend.append("  ");
    }

    inline void unindent()
    {
        if (!verbose()) {
            return;
        }
        m_prepend.pop_back();
        m_prepend.pop_back();
    }

    template <class T> // int, double, strings, etc
    inline logger_t& operator<<(const T& output)
    {
        if (verbose()) {
            m_buffer << output;
        }
        return *this;
    }

    inline logger_t& operator<<(ManipFn manip) /// endl, flush, setw, setfill, etc.
    {
        if (verbose()) {
            manip(m_buffer);

            if (manip == static_cast<ManipFn>(std::flush) || manip == static_cast<ManipFn>(std::endl)) {
                this->flush();
            }
        }
        return *this;
    }

    inline logger_t& operator<<(FlagsFn manip) /// setiosflags, resetiosflags
    {
        if (verbose()) {
            manip(m_buffer);
        }
        return *this;
    }

    inline void flush()
    {
        if (!(verbose())) {
            return;
        }

#if 0 // dump log to terminal [immediately]
            std::cout << m_prepend << "::" << m_buffer.str();
            m_buffer.str(std::string());
            m_buffer.clear();
#endif
    }
};
template <typename T>
struct pair : std::pair<T, T> {
    pair(const T a, const T b)
        : std::pair<T, T>(a < b ? a : b, a < b ? b : a)
    {
    }
};

template <typename T>
pair<T> make_pair(const T a, const T b)
{
    return pair<T>(a, b);
}



// Threadsafe logging to console which prevents std::cerr from mixing strings when
// concatenating with the operator<< multiple time per string, across multiple
// threads.
#define log_msg(msg_str)                     \
    {                                        \
        std::stringstream ss;                \
        ss << msg_str << std::endl;          \
        std::cerr << ss.str() << std::flush; \
        }

// used to marked/label unused function parameters to prevent warnings
#define UNUSED(x) [&x] {}()

#endif // MCUT_UTILS_H_
